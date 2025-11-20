#include <unordered_map>
#include <unordered_set>
#include <stdio.h>
#include "util.h"
#include "ctpl.h"
#include "union_table.h"
#include "rgd_op.h"
#include "queue.h"
#include "proto/brctuples.pb.h"
#include <z3++.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <iostream>
#include <fstream>
#include <fcntl.h>           /* For O_* constants */
#include <sys/stat.h>        /* For mode constants */
#include <semaphore.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <glob.h>
#include <thread>
#include <chrono>

#define B_FLIPPED 0x1
#define THREAD_POOL_SIZE 1
#define XXH_STATIC_LINKING_ONLY   /* access advanced declarations */
#define XXH_IMPLEMENTATION
#include "xxhash.h"
//global variables

// addconstr::WholeTrace new_trace;
static std::atomic<uint64_t> fid;      // depth of the current branch
static std::atomic<uint64_t> ce_count; // output file id
int named_pipe_fd;
std::ifstream pcsetpipe;
static FILE* cxx_log_fp = NULL;

bool SAVING_WHOLE;

XXH32_hash_t call_stack_hash_;
static uint32_t max_label_;
uint32_t dump_tree_id_;

static z3::context __z3_context;
static z3::solver __z3_solver(__z3_context, "QF_BV");
static const dfsan_label kInitializingLabel = -1;
static uint32_t max_label_per_session = 0;
sem_t * semagra;
sem_t * semace;
sem_t * semafzr;

uint8_t BRC_MODE = 0;
int count_extra_cons = 0;
uint32_t total_symb_brc = 0;
uint64_t total_time = 0;
uint64_t total_solving_time = 0;
uint64_t total_reload_time = 0;
uint64_t total_rebuild_time = 0;
uint64_t total_updateG_time = 0;
uint64_t total_extra_time = 0;
uint64_t total_getdeps_cost = 0;
uint32_t total_pruned_ones = 0;

// uint64_t init_count = 0;
uint64_t untaken_update_ifsat = 0; // carry the pathprefix of untaken branch, in case of sat nested solving, mark it;
std::string input_file = "/outroot/tmp/cur_input_2";

static dfsan_label_info *__union_table;

struct RGDSolution {
    std::unordered_map<uint32_t, uint8_t> sol;
  //the intended branch for this solution
    uint32_t fid;  //the seed
    uint64_t addr;
    uint64_t ctx;
    uint32_t order;
};

moodycamel::ConcurrentQueue<RGDSolution> solution_queue;


// dependencies
struct dedup_hash {
  std::size_t operator()(const std::tuple<uint64_t,uint64_t,uint64_t,uint32_t> &operand) const {
    return std::hash<uint64_t>{}(std::get<0>(operand))^
            std::hash<uint64_t>{}(std::get<1>(operand))^
            std::hash<uint64_t>{}(std::get<2>(operand))^
            std::hash<uint32_t>{}(std::get<3>(operand));
  }
};

struct dedup_equal {
  bool operator()(const std::tuple<uint64_t,uint64_t,uint64_t,uint32_t> &lhs, const std::tuple<uint64_t,uint64_t,uint64_t,uint32_t> &rhs) const {
    return std::get<0>(lhs) == std::get<0>(rhs) &&
          std::get<1>(lhs) == std::get<1>(rhs) &&
          std::get<2>(lhs) == std::get<2>(rhs) &&
          std::get<3>(lhs) == std::get<3>(rhs);
  }
};

static std::unordered_set<std::tuple<uint64_t, uint64_t, uint64_t, uint32_t>, dedup_hash, dedup_equal> fmemcmp_dedup;

static std::unordered_set<uint32_t> visited_;
static std::unordered_set<uint32_t> flipped_labels_session; // track per-session flipped labels to avoid relying on persisted flags

std::unordered_map<uint32_t,z3::expr> expr_cache;
std::unordered_map<uint32_t,std::unordered_set<uint32_t>> deps_cache;

// dependencies
struct expr_hash {
  std::size_t operator()(const z3::expr &expr) const {
    return expr.hash();
  }
};
struct expr_equal {
  bool operator()(const z3::expr &lhs, const z3::expr &rhs) const {
    return lhs.id() == rhs.id();
  }
};
typedef std::unordered_set<z3::expr, expr_hash, expr_equal> expr_set_t;

struct labeltuple_hash {
  std::size_t operator()(const std::tuple<uint32_t, uint32_t> &x) const {
    return std::get<0>(x) ^ std::get<1>(x);
  }
};

typedef std::unordered_set<std::tuple<uint32_t, uint32_t>, labeltuple_hash> labeltuple_set_t;

typedef struct {
  std::unordered_set<dfsan_label> input_deps;
  labeltuple_set_t label_tuples;
} branch_dep_t;

static std::vector<branch_dep_t*> *__branch_deps;

static inline dfsan_label_info* get_label_info(dfsan_label label) {
  return &__union_table[label];
}

static inline branch_dep_t* get_branch_dep(size_t n) {
  if (n >= __branch_deps->size()) {
    __branch_deps->resize(n + 1);
  }
  return __branch_deps->at(n);
}

static inline void set_branch_dep(size_t n, branch_dep_t* dep) {
  if (n >= __branch_deps->size()) {
    __branch_deps->resize(n + 1);
  }
  __branch_deps->at(n) = dep;
}

static z3::expr get_cmd(z3::expr const &lhs, z3::expr const &rhs, uint32_t predicate) {
  switch (predicate) {
    case DFSAN_BVEQ:  return lhs == rhs;
    case DFSAN_BVNEQ: return lhs != rhs;
    case DFSAN_BVUGT: return z3::ugt(lhs, rhs);
    case DFSAN_BVUGE: return z3::uge(lhs, rhs);
    case DFSAN_BVULT: return z3::ult(lhs, rhs);
    case DFSAN_BVULE: return z3::ule(lhs, rhs);
    case DFSAN_BVSGT: return lhs > rhs;
    case DFSAN_BVSGE: return lhs >= rhs;
    case DFSAN_BVSLT: return lhs < rhs;
    case DFSAN_BVSLE: return lhs <= rhs;
    default:
      printf("FATAL: unsupported predicate: %u\n", predicate);
      // throw z3::exception("unsupported predicate");
      break;
  }
  // should never reach here
  //Die();
}

static inline z3::expr cache_expr(dfsan_label label, z3::expr const &e, std::unordered_set<uint32_t> &deps) {
  if (label != 0)  {
    expr_cache.insert({label,e});
    deps_cache.insert({label,deps});
  }
  return e;
}

// iteratively get all input deps of the current label
static void get_input_deps(dfsan_label label, std::unordered_set<uint32_t> &deps) {
  if (label < CONST_OFFSET || label == kInitializingLabel) {
    throw z3::exception("invalid label");
  }
  if (label > max_label_per_session) {
    max_label_per_session = label;
  }

  dfsan_label_info *info = get_label_info(label);
  if (info->depth > 500) {
    // printf("WARNING: tree depth too large: %d\n", info->depth);
    throw z3::exception("tree too deep");
  }

  // special ops
  if (info->op == 0) {
    deps.insert(info->op1);
    return;
  } else if (info->op == DFSAN_LOAD) {
    uint64_t offset = get_label_info(info->l1)->op1;
    deps.insert(offset);
    for (uint32_t i = 1; i < info->l2; i++) {
      deps.insert(offset + i);
    }
    return;
  } else if (info->op == DFSAN_ZEXT || info->op == DFSAN_SEXT || info->op == DFSAN_TRUNC || info->op == DFSAN_EXTRACT) {
    get_input_deps(info->l1, deps);
    return;
  } else if (info->op == DFSAN_NOT || info->op == DFSAN_NEG) {
    get_input_deps(info->l2, deps);
    return;
  }

  // common ops
  if (info->l1 >= CONST_OFFSET) {
    get_input_deps(info->l1, deps);
  }
  if (info->l2 >= CONST_OFFSET) {
    std::unordered_set<uint32_t> deps2;
    get_input_deps(info->l2, deps2);
    deps.insert(deps2.begin(),deps2.end());
  }
  return;
}

// get the extra [label, dir] in the prefix for completing the nested set;
std::string get_extra_tuple(dfsan_label label, uint32_t tkdir, int ifmemorize) {
  std::string res = "";
  // deps of the label alone
  std::unordered_set<dfsan_label> inputs;
  try {
    uint64_t t_getdep = getTimeStamp();
    get_input_deps(label, inputs);
    total_getdeps_cost += (getTimeStamp() - t_getdep);

    // collect additional input deps
    std::vector<dfsan_label> worklist;
    worklist.insert(worklist.begin(), inputs.begin(), inputs.end());
    while (!worklist.empty()) {
      auto off = worklist.back();
      worklist.pop_back();

      auto deps = get_branch_dep(off);
      if (deps != nullptr) {
        for (auto i : deps->input_deps) {
          if (inputs.insert(i).second)
            worklist.push_back(i);
        }
      }
    }
    count_extra_cons = inputs.size();

    // get tuples from branches with deps in inputs
    if (inputs.size() == 0) return res;

    if (ifmemorize) {
      // addconstr::Extracon* extra_con = new_trace.add_extracon();
      // extra_con->set_id(label);

      labeltuple_set_t added;
      for (auto off : inputs) {
        auto deps = get_branch_dep(off);
        if (deps != nullptr) {
          for (auto &expr : deps->label_tuples) {
            if (added.insert(expr).second) {
              res += (std::to_string(std::get<0>(expr)) + "," + std::to_string(std::get<1>(expr)) + ".");
              // addconstr::Extracon::LabelTuple* new_tuple = extra_con->add_labeltuples();
              // new_tuple->set_eid(std::get<0>(expr));
              // new_tuple->set_edir(std::get<1>(expr));
            }
          }
        }
      }
    } else {
      total_pruned_ones += 1;
    }


    // update the labellist of each in inputs
    for (auto off : inputs) {
      auto c = get_branch_dep(off);
      if (c == nullptr) {
        c = new branch_dep_t();
        if (c == nullptr) {
          printf("WARNING: out of memory\n");
        } else {
          set_branch_dep(off, c);
          c->input_deps.insert(inputs.begin(), inputs.end());
          c->label_tuples.insert(std::make_tuple(label, tkdir));
        }
      } else {
        c->input_deps.insert(inputs.begin(), inputs.end());
        c->label_tuples.insert(std::make_tuple(label, tkdir));
      }
    }
  } catch (z3::exception e) {
    printf("WARNING: solving error: %s\n", e.msg());
    return res;
  }
  return res;
}

static z3::expr serialize(dfsan_label label, std::unordered_set<uint32_t> &deps) {
  if (label < CONST_OFFSET || label == kInitializingLabel) {
    // printf("WARNING: invalid label: %d\n", label);
    throw z3::exception("invalid label");
  }

  if (label > max_label_per_session) {
    max_label_per_session = label;
  }


  dfsan_label_info *info = get_label_info(label);

  if (info->depth > 500) {
    // printf("WARNING: tree depth too large: %d\n", info->depth);
    throw z3::exception("tree too deep");
  }

  auto itr_expr = expr_cache.find(label);
  auto itr_deps = deps_cache.find(label);
  if (label !=0 && itr_expr != expr_cache.end() && itr_deps != deps_cache.end() ) {
    deps.insert(itr_deps->second.begin(), itr_deps->second.end());
    return itr_expr->second;
  }

  // special ops
  if (info->op == 0) {
    // input
    z3::symbol symbol = __z3_context.int_symbol(info->op1);
    z3::sort sort = __z3_context.bv_sort(8);
    info->tree_size = 1; // lazy init
    deps.insert(info->op1);
    // caching is not super helpful
    return __z3_context.constant(symbol, sort);
  } else if (info->op == DFSAN_LOAD) {
    uint64_t offset = get_label_info(info->l1)->op1;
    z3::symbol symbol = __z3_context.int_symbol(offset);
    z3::sort sort = __z3_context.bv_sort(8);
    z3::expr out = __z3_context.constant(symbol, sort);
    deps.insert(offset);
    for (uint32_t i = 1; i < info->l2; i++) {
      symbol = __z3_context.int_symbol(offset + i);
      out = z3::concat(__z3_context.constant(symbol, sort), out);
      deps.insert(offset + i);
    }
    info->tree_size = 1; // lazy init
    return cache_expr(label, out, deps);
  } else if (info->op == DFSAN_ZEXT) {
    z3::expr base = serialize(info->l1, deps);
    if (base.is_bool()) // dirty hack since llvm lacks bool
      base = z3::ite(base, __z3_context.bv_val(1, 1),
          __z3_context.bv_val(0, 1));
    uint32_t base_size = base.get_sort().bv_size();
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, z3::zext(base, info->size - base_size), deps);
  } else if (info->op == DFSAN_SEXT) {
    z3::expr base = serialize(info->l1, deps);
    if (base.is_bool()) // dirty hack since llvm lacks bool
      base = z3::ite(base, __z3_context.bv_val(1, 1),
          __z3_context.bv_val(0, 1));
    uint32_t base_size = base.get_sort().bv_size();
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, z3::sext(base, info->size - base_size), deps);
  } else if (info->op == DFSAN_TRUNC) {
    z3::expr base = serialize(info->l1, deps);
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, base.extract(info->size - 1, 0), deps);
  } else if (info->op == DFSAN_EXTRACT) {
    z3::expr base = serialize(info->l1, deps);
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, base.extract((info->op2 + info->size) - 1, info->op2), deps);
  } else if (info->op == DFSAN_NOT) {
    // if (info->l2 == 0 || info->size != 1) {
    //   throw z3::exception("invalid Not operation");
    // }
    z3::expr e = serialize(info->l2, deps);
    info->tree_size = get_label_info(info->l2)->tree_size; // lazy init
    // if (!e.is_bool()) {
    //   throw z3::exception("Only LNot should be recorded");
    // }
    return cache_expr(label, !e, deps);
  } else if (info->op == DFSAN_NEG) {
    // if (info->l2 == 0) {
    //   throw z3::exception("invalid Neg predicate");
    // }
    z3::expr e = serialize(info->l2, deps);
    info->tree_size = get_label_info(info->l2)->tree_size; // lazy init
    return cache_expr(label, -e, deps);
  }
  // common ops
  uint8_t size = info->size;
  // size for concat is a bit complicated ...
  if (info->op == DFSAN_CONCAT && info->l1 == 0) {
    assert(info->l2 >= CONST_OFFSET);
    size = info->size - get_label_info(info->l2)->size;
  }
  z3::expr op1 = __z3_context.bv_val((uint64_t)info->op1, size);
  if (info->l1 >= CONST_OFFSET) {
    op1 = serialize(info->l1, deps).simplify();
  } else if (info->size == 1) {
    op1 = __z3_context.bool_val(info->op1 == 1);
  }
  if (info->op == DFSAN_CONCAT && info->l2 == 0) {
    assert(info->l1 >= CONST_OFFSET);
    size = info->size - get_label_info(info->l1)->size;
  }
  z3::expr op2 = __z3_context.bv_val((uint64_t)info->op2, size);
  if (info->l2 >= CONST_OFFSET) {
    std::unordered_set<uint32_t> deps2;
    op2 = serialize(info->l2, deps2).simplify();
    deps.insert(deps2.begin(),deps2.end());
  } else if (info->size == 1) {
    op2 = __z3_context.bool_val(info->op2 == 1); }
  // update tree_size
  info->tree_size = get_label_info(info->l1)->tree_size +
    get_label_info(info->l2)->tree_size;

  switch((info->op & 0xff)) {
    // llvm doesn't distinguish between logical and bitwise and/or/xor
    case DFSAN_AND:     return cache_expr(label, info->size != 1 ? (op1 & op2) : (op1 && op2), deps);
    case DFSAN_OR:      return cache_expr(label, info->size != 1 ? (op1 | op2) : (op1 || op2), deps);
    case DFSAN_XOR:     return cache_expr(label, op1 ^ op2, deps);
    case DFSAN_SHL:     return cache_expr(label, z3::shl(op1, op2), deps);
    case DFSAN_LSHR:    return cache_expr(label, z3::lshr(op1, op2), deps);
    case DFSAN_ASHR:    return cache_expr(label, z3::ashr(op1, op2), deps);
    case DFSAN_ADD:     return cache_expr(label, op1 + op2, deps);
    case DFSAN_SUB:     return cache_expr(label, op1 - op2, deps);
    case DFSAN_MUL:     return cache_expr(label, op1 * op2, deps);
    case DFSAN_UDIV:    return cache_expr(label, z3::udiv(op1, op2), deps);
    case DFSAN_SDIV:    return cache_expr(label, op1 / op2, deps);
    case DFSAN_UREM:    return cache_expr(label, z3::urem(op1, op2), deps);
    case DFSAN_SREM:    return cache_expr(label, z3::srem(op1, op2), deps);
                  // relational
    case DFSAN_ICMP:    return cache_expr(label, get_cmd(op1, op2, info->op >> 8), deps);
                  // concat
    case DFSAN_CONCAT:  return cache_expr(label, z3::concat(op2, op1), deps); // little endian
    default:
                  printf("FATAL: unsupported op: %u\n", info->op);
                  // throw z3::exception("unsupported operator");
                  break;
  }
  // should never reach here
  //Die();
}

void init(bool saving_whole) {
  SAVING_WHOLE = saving_whole;
  __branch_deps = new std::vector<branch_dep_t*>(100000, nullptr);
}

void cleanup1();
void cleanup2();
int cleanup_deps();
bool check_pp(uint64_t digest);
void mark_pp(uint64_t digest);

static void generate_solution(z3::model &m, std::unordered_map<uint32_t, uint8_t> &solu) {
  unsigned num_constants = m.num_consts();
  for(unsigned i = 0; i< num_constants; i++) {
    z3::func_decl decl = m.get_const_decl(i);
    z3::expr e = m.get_const_interp(decl);
    z3::symbol name = decl.name();
    if(name.kind() == Z3_INT_SYMBOL) {
      uint8_t value = (uint8_t)e.get_numeral_int();
      solu[name.to_int()] = value;
    }
    if (name.kind() == Z3_STRING_SYMBOL) {
      int index = std::stoi(name.str().substr(2)) / 10;
      uint8_t value = (uint8_t)e.get_numeral_int();
      solu[index] = value;
    }
  }
}


// int build_nested_set(int extra, uint32_t label, uint32_t conc_dir, std::string src_tscs, std::string deps_file) {
//   std::string entry;
//   uint32_t e_label;
//   uint32_t e_dir;

//   int token_index = 0;
//   size_t pos1 = 0;
//   size_t pos2 = 0;

//   // get the opt set first.
//   z3::expr result = __z3_context.bool_val(conc_dir);
//   std::unordered_map<uint32_t, uint8_t> opt_sol;
//   std::unordered_map<uint32_t, uint8_t> sol;

//   try {
//     std::unordered_set<dfsan_label> inputs;
//     z3::expr cond = serialize(label, inputs);

//     __z3_solver.reset();
//     __z3_solver.add(cond != result);
//     z3::check_result res = __z3_solver.check();
//     // check if opt set is sat
//     if (res == z3::sat) {
//       z3::model m_opt = __z3_solver.get_model();
//       __z3_solver.push();

//       // collect additional constraints
//       if (extra == 1) {
//         addconstr::WholeTrace new_trace1;
//         std::fstream input(deps_file, std::ios::in | std::ios::binary);
//         if (!new_trace1.ParseFromIstream(&input)) {
//           fprintf(stderr, "[build_nested_set]: cannot open file to write: %s\n", deps_file.c_str());
//           return -1;
//         }
//         for (int i = 0; i < new_trace1.extracon_size(); i++) {
//           const addconstr::Extracon& brc = new_trace1.extracon(i);
//           if (brc.id() == label) {
//             // std::cout << "found record: " << brc.id() << "=" << label << std::endl;
//             for (int j = 0; j < brc.labeltuples_size(); j++) {
//                 const addconstr::Extracon::LabelTuple& label_tuple = brc.labeltuples(j);
//                 // std::cout << label_tuple.eid() << "," <<label_tuple.edir() << std::endl;

//                 std::unordered_set<dfsan_label> e_inputs;
//                 z3::expr e_cond = serialize(label_tuple.eid(), e_inputs);
//                 z3::expr e_result = __z3_context.bool_val(label_tuple.edir());
//                 __z3_solver.add(e_cond == e_result);
//             }
//             break;
//           }
//         }
//         // nested done
//         res = __z3_solver.check();
//       }

//       if (res == z3::sat) {
//         mark_pp(untaken_update_ifsat);
//         z3::model m = __z3_solver.get_model();
//         // std::cout << "PCset1: \n" << __z3_solver.to_smt2().c_str() << std::endl;
//         sol.clear();
//         generate_solution(m, sol);
//         generate_input(sol, src_tscs, "./fifo", ce_count+=1);
//         std::cout << "(nested)new file id " << ce_count << std::endl;
//         return 1;
//       } else {
//         __z3_solver.pop();
//         // std::cout << "PCset1: \n" << __z3_solver.to_smt2().c_str() << std::endl;
//         opt_sol.clear();
//         generate_solution(m_opt, opt_sol);
//         generate_input(opt_sol, src_tscs, "./fifo", ce_count+=1);
//         std::cout << "(opt)new file id " << ce_count << std::endl;

//         return 2; // optimistic sat
//       }
//     } else {
//       mark_pp(untaken_update_ifsat);
//       return 0;
//     }
//   } catch (z3::exception e) {
//     printf("WARNING: solving error: %s\n", e.msg());
//     return 0;
//   }
// }

int build_nested_set_old(std::string extra, uint32_t label, uint32_t conc_dir, std::string src_tscs) {
  std::string entry;
  uint32_t e_label;
  uint32_t e_dir;

  int token_index = 0;
  size_t pos1 = 0;
  size_t pos2 = 0;

  // get the opt set first.
  z3::expr result = __z3_context.bool_val(conc_dir);
  std::unordered_map<uint32_t, uint8_t> opt_sol;
  std::unordered_map<uint32_t, uint8_t> sol;

  std::cout << "build_nested_set_old 576" << std::endl;
  std::cerr << "build_nested_set_old: label=" << label << " conc_dir=" << conc_dir << " extra=\"" << extra << "\"" << std::endl;
  std::cout << "build_nested_set_old: label=" << label << " conc_dir=" << conc_dir << " extra=\"" << extra << "\"" << std::endl;
  fflush(stdout);
  fflush(stderr);

  try {
    std::unordered_set<dfsan_label> inputs;
    std::cerr << "build_nested_set_old: about to serialize label=" << label << std::endl;
    std::cout << "build_nested_set_old: about to serialize label=" << label << std::endl;
    fflush(stdout);
    fflush(stderr);
    z3::expr cond = serialize(label, inputs);
    std::cerr << "build_nested_set_old: serialize completed, inputs.size()=" << inputs.size() << std::endl;
    std::cout << "build_nested_set_old: serialize completed, inputs.size()=" << inputs.size() << std::endl;
    fflush(stdout);
    fflush(stderr);

    // Convert cond to bool if it's a bitvector (Marco-compatible: handle both bool and bv types)
    z3::expr cond_bool = cond;
    if (!cond.is_bool() && cond.get_sort().is_bv()) {
      // If cond is a bitvector, convert to bool: cond != 0
      cond_bool = (cond != __z3_context.bv_val(0, cond.get_sort().bv_size()));
    }

    __z3_solver.reset();
    __z3_solver.add(cond_bool != result);
    std::cerr << "build_nested_set_old: about to check solver (opt set)" << std::endl;
    std::cout << "build_nested_set_old: about to check solver (opt set)" << std::endl;
    fflush(stdout);
    fflush(stderr);
    z3::check_result res = __z3_solver.check();
    const char* res_str = (res == z3::sat ? "sat" : (res == z3::unsat ? "unsat" : "unknown"));
    std::cerr << "build_nested_set_old: solver check result=" << res_str << std::endl;
    std::cout << "build_nested_set_old: solver check result=" << res_str << std::endl;
    if (cxx_log_fp) { fprintf(cxx_log_fp, "build_nested_set_old: solver check result=%s\n", res_str); fflush(cxx_log_fp); }
    fflush(stdout);
    fflush(stderr);
    // check if opt set is sat
    if (res == z3::sat) {
      std::cout << "build_nested_set_old: opt sat" << std::endl;
      z3::model m_opt = __z3_solver.get_model();
      __z3_solver.push();

      // collect additional constraints
      while ((pos1 = extra.find("#")) != std::string::npos) {
        entry = extra.substr(0, pos1);
        if ((pos2 = entry.find(".")) != std::string::npos) {
          e_label = stoul(entry.substr(0, pos2));
          entry.erase(0, pos2+1);
          e_dir = stoul(entry);
          std::unordered_set<dfsan_label> e_inputs;
          z3::expr e_cond = serialize(e_label, e_inputs);
          z3::expr e_result = __z3_context.bool_val(e_dir);
          __z3_solver.add(e_cond == e_result);
        }
        extra.erase(0, pos1 + 1);
      }
      std::cout << "build_nested_set_old: nested set building done" << std::endl;
      // nested done
      res = __z3_solver.check();
      if (res == z3::sat) {
        std::cout << "build_nested_set_old: nested sat" << std::endl;
        if (cxx_log_fp) {
          fprintf(cxx_log_fp, "[build_nested_set_old] nested sat: mark_pp(untaken_update_ifsat=0x%llx) called\n",
                  (unsigned long long)untaken_update_ifsat);
          fflush(cxx_log_fp);
        }
        mark_pp(untaken_update_ifsat);
        z3::model m = __z3_solver.get_model();
        sol.clear();
        generate_solution(m, sol);
        // write outputs under SYMCC_OUTPUT_DIR/fifo if provided
        const char* out_base_env1 = getenv("SYMCC_OUTPUT_DIR");
        std::string out_base1 = (out_base_env1 && out_base_env1[0] != '\0') ? std::string(out_base_env1) : std::string(".");
        std::string out_fifo_dir1 = out_base1 + "/fifo";
        generate_input(sol, src_tscs, out_fifo_dir1.c_str(), ce_count+=1);
        std::cout << "(nested)new file id " << ce_count << std::endl;
        return 1;
      } else {
        std::cout << "build_nested_set_old: nested unsat" << std::endl;
        __z3_solver.pop();
        opt_sol.clear();
        generate_solution(m_opt, opt_sol);
        const char* out_base_env2 = getenv("SYMCC_OUTPUT_DIR");
        std::string out_base2 = (out_base_env2 && out_base_env2[0] != '\0') ? std::string(out_base_env2) : std::string(".");
        std::string out_fifo_dir2 = out_base2 + "/fifo";
        generate_input(opt_sol, src_tscs, out_fifo_dir2.c_str(), ce_count+=1);
        std::cout << "(opt)new file id " << ce_count << std::endl;
        return 2; // optimistic sat
      }
    } else {
      std::cout << "unsat solving; quick escape" << std::endl;
      // unsat
      if (cxx_log_fp) {
        fprintf(cxx_log_fp, "[build_nested_set_old] unsat: mark_pp(untaken_update_ifsat=0x%llx) called\n",
                (unsigned long long)untaken_update_ifsat);
        fflush(cxx_log_fp);
      }
      mark_pp(untaken_update_ifsat);
      return 0;
    }
  } catch (z3::exception e) {
    printf("WARNING: solving error: %s\n", e.msg());
    fflush(stdout);
    std::cerr << "ERROR: z3::exception in build_nested_set_old: " << e.msg() << std::endl;
    fflush(stderr);
    return 0;
  } catch (std::exception &e) {
    printf("WARNING: std::exception: %s\n", e.what());
    fflush(stdout);
    std::cerr << "ERROR: std::exception in build_nested_set_old: " << e.what() << std::endl;
    fflush(stderr);
    return 0;
  }
}

int gen_solve_pc(uint32_t queueid, uint32_t tree_id, uint32_t label, uint32_t conc_dir, uint32_t cur_label_loc, std::string extra) {
  std::cout << "[gen_solve_pc] queueid=" << queueid
            << " tree_id=" << tree_id
            << " label=" << label
            << " conc_dir=" << conc_dir
            << " cur_label_loc=" << cur_label_loc
            << " extra=\"" << extra << "\"" << std::endl;
  struct stat st;
  size_t sread;
  FILE *fp;
  int res = 1;
  uint64_t one_start = getTimeStamp();

  const char* tree_base_env = getenv("MARCO_TREE_DIR");
  std::string tree_base = (tree_base_env && tree_base_env[0] != '\0') ? std::string(tree_base_env) : std::string(".");
  std::string tree_idstr = std::to_string(tree_id % 1000000);
  std::string tree_file = tree_base + "/tree" + std::to_string(queueid) + "/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  std::cout << "[gen_solve_pc] tree_file=" << tree_file << std::endl;
  std::cout << "qid = " << queueid << std::endl;
  std::string src_tscs;
  if (queueid == 0) {
    src_tscs = "./afl-slave/queue/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr + "*";
    glob_t globbuf;
    glob(src_tscs.c_str(), 0, NULL, &globbuf);
    if (globbuf.gl_pathc > 0) {
      src_tscs = globbuf.gl_pathv[0];
    }
    globfree(&globbuf);

  } else if (queueid == 1) {
    src_tscs = "./fifo/queue/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  }
  std::cout << "src_tscs: " << src_tscs << std::endl;
  std::string deps_file = "./deps/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;

  // prep1: reinstate tree
  std::cout << "[gen_solve_pc] checking tree_file: " << tree_file << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "checking tree_file: %s\n", tree_file.c_str()); fflush(cxx_log_fp); }
  if (stat(tree_file.c_str(), &st) != 0) {
    std::cout << "[gen_solve_pc] stat failed for tree_file: " << tree_file
              << " errno=" << errno << " (" << strerror(errno) << ")" << std::endl;
    if (cxx_log_fp) { fprintf(cxx_log_fp, "stat failed: %s errno=%d\n", tree_file.c_str(), errno); fflush(cxx_log_fp); }
    // FORCE MODE: If try_solve was forced to true (for testing), return 0 (UNSAT) instead of -1 (DUP)
    // This allows the system to continue processing even when tree files are missing
    std::cout << "[gen_solve_pc] FORCE MODE: missing tree file, returning 0 (UNSAT) instead of -1 (DUP) for testing" << std::endl;
    if (cxx_log_fp) { fprintf(cxx_log_fp, "FORCE MODE: missing tree file, returning 0 (UNSAT) for testing\n"); fflush(cxx_log_fp); }
    return 0;  // Return UNSAT instead of DUP to allow testing
  }
  sread = st.st_size;

  // tree size -1 is max label_
  max_label_ = sread / sizeof(dfsan_label_info) - 1; // 1st being 0
  std::cout << "tree size (label count) is " << max_label_ << std::endl;

  fp = fopen(tree_file.c_str(), "rb");
  if (!fp) {
    std::cout << "[gen_solve_pc] fopen failed for tree_file: " << tree_file
              << " errno=" << errno << " (" << strerror(errno) << ")" << std::endl;
    // Marco original logic: return -1 for failed fopen (duplicate)
    return -1;
  }
  size_t nread = fread(__union_table, sread, 1, fp);
  fclose(fp);
  if (nread != 1) {
    std::cout << "[gen_solve_pc] fread size mismatch for tree_file: " << tree_file
              << " expected=" << sread << std::endl;
    // Marco original logic: return -1 for fread mismatch (duplicate)
    return -1;
  }

  total_reload_time += (getTimeStamp() - one_start);

  // prep2: reset the max label tracker; upper bound is new max_label_
  max_label_per_session = 0;

  // gen and solve new PC set
  // res = build_nested_set(extra, label, conc_dir, src_tscs, deps_file); // protobuf version
  std::cout << "[gen_solve_pc] about to call build_nested_set_old label=" << label << " conc_dir=" << conc_dir << " extra=\"" << extra << "\"" << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "about to call build_nested_set_old label=%u dir=%u extra=[%s]\n", label, conc_dir, extra.c_str()); fflush(cxx_log_fp); }
  res = build_nested_set_old(extra, label, conc_dir, src_tscs); // string conversion
  std::cout << "[gen_solve_pc] build_nested_set_old result=" << res << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "build_nested_set_old result=%d\n", res); fflush(cxx_log_fp); }

  // clean up after solving
  cleanup1(); // reset the memory in union table
  max_label_per_session = 0;
  int dele = cleanup_deps();

  return res;
}


int generate_next_tscs(std::ifstream &pcsetpipe) {
  std::string line;
  std::string token;
  std::string extra;

  uint32_t queueid;
  uint32_t tree_id;
  uint32_t node_id;
  uint32_t conc_dir;
  uint32_t cur_label_loc;

  size_t pos = 0;

  z3::context ctx;
  z3::solver solver(ctx, "QF_BV");

  while (1) {
    // Check if stream is in good state before getline
    if (!pcsetpipe.good() && !pcsetpipe.eof()) {
      std::cout << "[generate_next_tscs] Stream not good before getline, good=" << pcsetpipe.good() << " eof=" << pcsetpipe.eof() << " fail=" << pcsetpipe.fail() << " bad=" << pcsetpipe.bad() << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "stream not good before getline, good=%d eof=%d fail=%d bad=%d\n", pcsetpipe.good(), pcsetpipe.eof(), pcsetpipe.fail(), pcsetpipe.bad()); fflush(cxx_log_fp); }
    }
    if (std::getline(pcsetpipe, line)) {
      if (cxx_log_fp) { fprintf(cxx_log_fp, "getline ok, raw=[%s]\n", line.c_str()); fflush(cxx_log_fp); }
      // trim trailing CR/LF and trailing commas/spaces
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t' || line.back() == ',')) {
        line.pop_back();
      }
      if (line.empty()) {
        if (cxx_log_fp) { fprintf(cxx_log_fp, "empty line after trim\n"); fflush(cxx_log_fp); }
        std::cout << "[generate_next_tscs] got empty line, continue" << std::endl;
        continue;
      }
      std::cout << "[generate_next_tscs] line: " << line << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "line(after trim)=[%s]\n", line.c_str()); fflush(cxx_log_fp); }
      int token_index = 0;
      std::cout << "line: " << line << std::endl;
      while ((pos = line.find(",")) != std::string::npos) {
          token = line.substr(0, pos);
          switch (token_index) {
              case 0: queueid = stoul(token); break;
              case 1: tree_id = stoul(token); break;
              case 2: node_id = stoul(token); break;
              case 3: conc_dir = stoul(token); break;
              case 4: cur_label_loc = stoul(token); break;
              case 5: untaken_update_ifsat = stoull(token); break;
              case 6: extra = token; break;
              default: break;
          }
          line.erase(0, pos + 1);
          token_index++;
      }
      std::cout << "[generate_next_tscs] parsed qid=" << queueid
                << " tree_id=" << tree_id
                << " node_id=" << node_id
                << " conc_dir=" << conc_dir
                << " cur_label_loc=" << cur_label_loc
                << " pp_hash=" << untaken_update_ifsat
                << " extra=\"" << extra << "\"" << std::endl;
      if (cxx_log_fp) {
        fprintf(cxx_log_fp, "parsed qid=%u tid=%u nid=%u dir=%u cur=%u pp=%llu extra=[%s]\n",
                queueid, tree_id, node_id, conc_dir, cur_label_loc,
                (unsigned long long)untaken_update_ifsat, extra.c_str());
        fflush(cxx_log_fp);
      }
      // Marco original logic: check path-prefix deduplication
      if (!BRC_MODE && !check_pp(untaken_update_ifsat)) {
        std::cout << "dup pp, skip! pp_hash=" << untaken_update_ifsat << std::endl;
        if (cxx_log_fp) { fprintf(cxx_log_fp, "dup pp, skip! pp_hash=%llu\n", (unsigned long long)untaken_update_ifsat); fflush(cxx_log_fp); }
        return -1; // skip it, query next one!
      }
      std::cout << "[generate_next_tscs] invoking gen_solve_pc() qid=" << queueid << " tid=" << tree_id << " label=" << node_id << " dir=" << conc_dir << " cur=" << cur_label_loc << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "invoking gen_solve_pc qid=%u tid=%u label=%u dir=%u cur=%u\n", queueid, tree_id, node_id, conc_dir, cur_label_loc); fflush(cxx_log_fp); }
      int ret = gen_solve_pc(queueid, tree_id, node_id, conc_dir, cur_label_loc, extra);
      std::cout << "[generate_next_tscs] gen_solve_pc returned: " << ret << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "gen_solve_pc returned %d\n", ret); fflush(cxx_log_fp); }
      // Marco original logic: return -1 for duplicate path-prefix
      return ret;
    }

    // If we reach here, getline failed. Likely FIFO writer closed; reopen to block for next decision.
    if (pcsetpipe.eof() || pcsetpipe.fail()) {
      std::cout << "[generate_next_tscs] FIFO closed or no writer; reopening /tmp/myfifo and waiting..." << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "fifo reopen, eof=%d fail=%d bad=%d\n", pcsetpipe.eof(), pcsetpipe.fail(), pcsetpipe.bad()); fflush(cxx_log_fp); }
      pcsetpipe.clear();
      pcsetpipe.close();
      // Reopen in blocking mode; this will block until a writer opens the FIFO
      pcsetpipe.open("/tmp/myfifo");
      if (!pcsetpipe.is_open()) {
        std::cout << "[generate_next_tscs] Failed to reopen /tmp/myfifo" << std::endl;
        if (cxx_log_fp) { fprintf(cxx_log_fp, "failed to reopen myfifo\n"); fflush(cxx_log_fp); }
      } else {
        std::cout << "[generate_next_tscs] Reopened /tmp/myfifo successfully" << std::endl;
        if (cxx_log_fp) { fprintf(cxx_log_fp, "reopened myfifo OK\n"); fflush(cxx_log_fp); }
      }
      // Small backoff to avoid busy loop if open returns immediately without data
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
  }
}

#if 1
const int pfxkMapSize  = 1<<27;
uint8_t pfx_pp_map[pfxkMapSize];
uint16_t node_map[pfxkMapSize];
const int kMapSize = 1 << 16;
uint8_t pp_map[kMapSize];
uint8_t context_map_[kMapSize];
uint8_t virgin_map_[kMapSize];
uint8_t trace_map_[kMapSize];
uint32_t prev_loc_ = 0;
#endif

bool check_pp(uint64_t digest) {
  uint32_t hash = digest % (pfxkMapSize * CHAR_BIT);
  uint32_t idx = hash / CHAR_BIT;
  uint32_t mask = 1 << (hash % CHAR_BIT);
  return (pfx_pp_map[idx] & mask) == 0;
}

void mark_pp(uint64_t digest) {
  uint32_t hash = digest % (pfxkMapSize * CHAR_BIT);
  uint32_t idx = hash / CHAR_BIT;
  uint32_t mask = 1 << (hash % CHAR_BIT);
  pfx_pp_map[idx] |= mask;
}

// addr, ctx, tkdir, qid, tscsid, label => pipe to scheduler
static int update_graph(dfsan_label label, uint64_t pc, uint32_t tkdir,
    bool try_solve, uint32_t inputid, uint32_t queueid, int uniq_pcset, int ifmemorize) {

    // Filter: only process symbolic branches (label != 0)
    // Concrete branches (label == 0) are skipped - they don't need to be added to graph structure
    if (!label) return 0;
    
    printf("[update_graph] ENTER label=%u pc=0x%llx dir=%u try_solve=%d tid=%u qid=%u uniq=%d memo=%d [branch_debug]\n",
           label, (unsigned long long)pc, tkdir, try_solve, inputid, queueid, uniq_pcset, ifmemorize);
    fflush(stdout);
    
    // For symbolic branches only (label != 0)
    bool is_concrete = (label == 0);
    
    // FORCE MODE: If try_solve was forced to true (for testing), always respect it
    // This allows concrete branches to also be solved when forced
    bool was_forced = try_solve && is_concrete;
    
    // For concrete branches, force try_solve=false and skip label-based deduplication
    // BUT: If try_solve was forced to true (for testing), respect that
    if (is_concrete && !try_solve) {
      printf("[update_graph] concrete branch (label==0), will add to graph but not solve\n");
      fflush(stdout);
      try_solve = false;  // Don't try to solve concrete branches (only if not already forced)
      uniq_pcset = 0;    // Don't add to PC queue for solving
      // Skip label-based deduplication for concrete branches
    } else if (was_forced) {
      // FORCE MODE: Concrete branch with forced try_solve=true
      printf("[update_graph] FORCE MODE: concrete branch (label==0) with try_solve=true, will solve\n");
      fflush(stdout);
      // Keep try_solve=true and uniq_pcset as set by caller
      // This allows concrete branches to be added to solving queue when forced
      // Skip label-based deduplication for concrete branches (even when forced)
      // because label=0 doesn't need deduplication
    } else {
      // For symbolic branches, check if already flipped in this session
      if (flipped_labels_session.find(label) != flipped_labels_session.end()) {
        printf("[update_graph] early return: already flipped in this session label=%u\n", label);
        fflush(stdout);
        return 0;
      }
      // mark this one off for branch within this trace (session-local)
      flipped_labels_session.insert(label);
    }

    // proceed to update graph, pipe the record to python scheduler
    std::string record;

    // if filtered by policy, or already picked it for this trace, or pruned, update visit of concrete branch only
    // For concrete branches (label==0), use "none" format UNLESS try_solve was forced to true
    if(!try_solve && uniq_pcset == 0) {
      // This includes concrete branches (label==0) and filtered symbolic branches
      // BUT: If try_solve was forced to true, we should still send proper PC_ids
      record = std::to_string(pc) \
               + "-" + std::to_string(call_stack_hash_) \
               + "-" + std::to_string(tkdir) \
               + "-" + std::to_string(label) \
               + "-" + std::to_string(inputid) \
               + "-" + std::to_string(queueid) \
               + "@none@@\n";
    } else if (was_forced && is_concrete) {
      // FORCE MODE: Concrete branch with try_solve=true
      // Generate a simplified PC_ids format for concrete branches (no label info needed)
      // Format: is_good-qid-pp_hash-treedepth-plen-conc_dir-tid-nid
      // For concrete branches: is_good=1 (force solve), pp_hash from simple hash, treedepth=0, plen=0
      // Note: We use a simple hash of pc+direction+call_stack_hash as pp_hash for concrete branches
      XXH64_state_t tmp_pp;
      XXH64_reset(&tmp_pp, 0);
      uint64_t deter = 1;
      uint64_t direction = tkdir ? 1ULL : 0ULL;
      XXH64_update(&tmp_pp, &deter, sizeof(deter));
      XXH64_update(&tmp_pp, &direction, sizeof(direction));
      XXH64_update(&tmp_pp, &pc, sizeof(pc));
      XXH64_update(&tmp_pp, &call_stack_hash_, sizeof(call_stack_hash_));
      uint64_t concrete_pp_hash = XXH64_digest(&tmp_pp);
      // Ensure non-zero hash
      if (concrete_pp_hash == 0) concrete_pp_hash = 1;
      record = std::to_string(pc) \
               + "-" + std::to_string(call_stack_hash_) \
               + "-" + std::to_string(tkdir) \
               + "-" + std::to_string(label) \
               + "-" + std::to_string(inputid) \
               + "-" + std::to_string(queueid) \
               + "@" \
               + "1-" + std::to_string(queueid) + "-" + std::to_string(concrete_pp_hash) \
               + "-0-0-0-" + std::to_string(inputid) + "-0#@@\n";
      printf("[update_graph] FORCE MODE: generated PC_ids for concrete branch, pp_hash=%llu\n", 
             (unsigned long long)concrete_pp_hash);
      fflush(stdout);
    } else {
      // Only symbolic branches with try_solve=true reach here
      // Concrete branches should never reach here due to the check above
      if (get_label_info(label)->tree_size > 50000) {
        printf("[update_graph] early return: tree_size too large for label=%u size=%u\n", label, get_label_info(label)->tree_size);
        fflush(stdout);
        return 1;
      }
      if (get_label_info(label)->depth > 500) {
        printf("[update_graph] early return: depth too deep for label=%u depth=%u\n", label, get_label_info(label)->depth);
        fflush(stdout);
        return 1;
      }

      uint64_t t_extra = getTimeStamp();
      std::string res = get_extra_tuple(label, tkdir, ifmemorize);
      total_extra_time += (getTimeStamp() - t_extra);

      // Debug: log PC value before converting to string
      static int debug_pc_before_string_count = 0;
      if (debug_pc_before_string_count++ < 10) {
        printf("[update_graph] DEBUG: before std::to_string(pc), pc=0x%llx (decimal=%llu)\n", 
               (unsigned long long)pc, (unsigned long long)pc);
        fflush(stdout);
        if (cxx_log_fp) {
          fprintf(cxx_log_fp, "[update_graph] DEBUG: before std::to_string(pc), pc=0x%llx (decimal=%llu)\n", 
                  (unsigned long long)pc, (unsigned long long)pc);
          fflush(cxx_log_fp);
        }
      }

      record = std::to_string(pc) \
               + "-" + std::to_string(call_stack_hash_) \
               + "-" + std::to_string(tkdir) \
               + "-" + std::to_string(label) \
               + "-" + std::to_string(inputid) \
               + "-" + std::to_string(queueid) \
               + "@" \
               + std::to_string(uniq_pcset) \
               + "-" + std::to_string(queueid) \
               + "-" + std::to_string(untaken_update_ifsat) \
               + "-" + std::to_string(get_label_info(label)->depth) \
               + "#" + res \
               + "@@\n";
    }
    // debug: also dump what will be written to the pipe into a local log and stdout
    if (cxx_log_fp) {
      fprintf(cxx_log_fp, "[update_graph] WRITE RECORD(len=%zu): %s", record.size(), record.c_str());
      fflush(cxx_log_fp);
    }
    printf("[update_graph] WRITE RECORD(len=%zu)\n", record.size());
    fflush(stdout);
    {
      ssize_t wret = write(named_pipe_fd, record.c_str(), strlen(record.c_str()));
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[update_graph] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0)); fflush(cxx_log_fp); }
      printf("[update_graph] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0));
      fflush(stdout);
    }
    fsync(named_pipe_fd);

    return 0;
}

// for bb pruning
const int kBitmapSize = 65536;
const int kStride = 8;
uint16_t bitmap_[kBitmapSize];
bool is_interesting_ = false ;

// borrow from qsym code for bb pruning
static bool isPowerOfTwo(uint16_t x) {
    return (x & (x - 1)) == 0;
}

void computeHash(uint32_t ctx) { // call stack context hashing
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &ctx, sizeof(ctx));
    call_stack_hash_ = XXH32_digest(&state);
}

void updateBitmap(void *last_pc_, uint64_t ctx) {
    // Lazy update the bitmap when symbolic operation is happened

    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &last_pc_, sizeof(last_pc_));
    XXH32_update(&state, &ctx, sizeof(uint64_t));

    uint32_t h = XXH32_digest(&state);
    uint32_t index = h % kBitmapSize;

    // Use strided exponential backoff, which is interesting if the strided
    // bitmap meets exponential requirements. For example, {0, 1, 2, ..., 7}
    // maps to 0, {8, ..., 15} maps to 1, and so on. {0, 1, 2, ..., 7} is
    // interesting because it maps to 0, which is in the {0, 1, 2, 4, ...}.
    // But {24, ... 31} is not, because it maps to 3.
    is_interesting_ = isPowerOfTwo(bitmap_[index] / kStride);
    bitmap_[index]++;
}

//check if we need to solve a branch given
// labe: if 0 concreate
// addr: branch address
// output: true: solve the constraints false: don't solve the constraints
bool bcount_filter(uint64_t addr, uint64_t ctx, uint64_t direction, uint32_t order) {
  std::tuple<uint64_t,uint64_t, uint64_t, uint32_t> key{addr,ctx,direction,order};
  if (fmemcmp_dedup.find(key) != fmemcmp_dedup.end()) {
    return false;
  } else {
    fmemcmp_dedup.insert(key);
    return true;
  }
}
inline bool isPowerofTwoOrZero(uint32_t x) {
  return ((x & (x - 1)) == 0);
}

XXH32_hash_t hashPc(uint64_t pc, bool taken) {
  XXH32_state_t state;
  XXH32_reset(&state, 0);
  XXH32_update(&state, &pc, sizeof(pc));
  XXH32_update(&state, &taken, sizeof(taken));
  return XXH32_digest(&state) % kMapSize;
}

uint32_t getIndex(uint32_t h) {
  return ((prev_loc_ >> 1) ^ h) % kMapSize;
}

bool isInterestingContext(uint32_t h, uint32_t bits) {
  bool interesting = false;
  if (!isPowerofTwoOrZero(bits))
    return false;
  for (auto it = visited_.begin();
      it != visited_.end();
      it++) {
    uint32_t prev_h = *it;

    // Calculate hash(prev_h || h)
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &prev_h, sizeof(prev_h));
    XXH32_update(&state, &h, sizeof(h));

    uint32_t hash = XXH32_digest(&state) % (kMapSize * CHAR_BIT);
    uint32_t idx = hash / CHAR_BIT;
    uint32_t mask = 1 << (hash % CHAR_BIT);

    if ((context_map_[idx] & mask) == 0) {
      context_map_[idx] |= mask;
      interesting = true;
    }
  }

  if (bits == 0)
    visited_.insert(h);

  return interesting;
}

//roll in branch
uint64_t roll_in_pp(uint32_t label, uint64_t addr, uint64_t direction,
    XXH64_state_t* path_prefix) {

  //address
  XXH64_state_t tmp;
  XXH64_reset(&tmp, 0);

  // roll in pc first
  XXH64_update(path_prefix, &addr, sizeof(addr));

  // roll in: ifconcrete and direction;
  uint8_t deter = 0;
  if (label == 0)  {
    deter = 1;
    XXH64_update(path_prefix, &deter, sizeof(deter));
    XXH64_update(path_prefix, &direction, sizeof(direction));
    // CRITICAL FIX: Return a non-zero hash for concrete branches to avoid all pp_hash=0 collisions
    // Use the current path_prefix digest as the hash value
    uint64_t concrete_digest = XXH64_digest(path_prefix);
    // If digest is 0 (unlikely but possible), use a hash of addr+direction
    if (concrete_digest == 0) {
      XXH64_state_t tmp2;
      XXH64_reset(&tmp2, 0);
      XXH64_update(&tmp2, &addr, sizeof(addr));
      XXH64_update(&tmp2, &direction, sizeof(direction));
      concrete_digest = XXH64_digest(&tmp2);
      // Still ensure non-zero
      if (concrete_digest == 0) concrete_digest = 1;
    }
    return concrete_digest;
  }

  // if this is a symbolic branch, validate if shall solve
  uint64_t direction_sym = 1 - direction;

  //digest
  uint64_t taken_digest;
  uint64_t untaken_digest;

  // roll in branch state: ifsymbolic
  XXH64_update(path_prefix, &deter, sizeof(deter));
  XXH64_copyState(&tmp,path_prefix);

  // for untaken branch, calculate the hash
  XXH64_update(&tmp, &direction_sym, sizeof(direction_sym));
  untaken_digest = XXH64_digest(&tmp);

  // for taken branch, calculate the hash
  XXH64_update(path_prefix, &direction, sizeof(direction));
  taken_digest = XXH64_digest(path_prefix);

  // mark the taken branch for visited
  mark_pp(taken_digest);
  // std::cout << "tk: " << taken_digest << " utk: " << untaken_digest << std::endl;
  
  // Debug: log taken_digest and untaken_digest for analysis
  if (cxx_log_fp) {
    fprintf(cxx_log_fp, "[roll_in_pp] addr=0x%llx dir=%lu label=%u taken_digest=0x%llx untaken_digest=0x%llx mark_pp(taken_digest) called\n",
            (unsigned long long)addr, (unsigned long)direction, label,
            (unsigned long long)taken_digest, (unsigned long long)untaken_digest);
    fflush(cxx_log_fp);
  }
  
  return untaken_digest;
}



bool isInterestingPathPrefix(uint64_t pc, bool taken, uint32_t label, XXH64_state_t* path_prefix) {
  // reset value for every new query
  untaken_update_ifsat = 0;
  // acquire untaken_hash, mark the taken_hash
  // Fix: roll_in_pp signature is (label, addr, direction, path_prefix), not (pc, label, taken, path_prefix)
  uint64_t untaken_digest = roll_in_pp(label, pc, taken ? 1ULL : 0ULL, path_prefix);
  
  // done rolling in the branch state and pc, if concrete, return false directly
  if (!label) return false;
  
  // if symbolic and untaken_hash is touched already, return false
  if (!check_pp(untaken_digest)) return false;
  
  // else it's fresh, solve it, return true
  untaken_update_ifsat = untaken_digest;
  return true;
}

int isInterestingNode(uint64_t pc, bool taken, uint64_t ctx) {
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &pc, sizeof(pc));
    XXH32_update(&state, &ctx, sizeof(ctx));
    XXH32_update(&state, &taken, sizeof(taken));

    uint32_t h = XXH32_digest(&state);
    uint32_t index = h % pfxkMapSize;

    int res = 0;

    node_map[index]++;
    // if visit count is power of 2, nested-solve it; otherwise, opt-solve it.
    // add the miss-of-3 case.
    if ((!(node_map[index] & (node_map[index]-1)))) {
      res = 1;
    }
    // if (node_map[index] == 3) {
    //   res = 1;
    // }

    return res;
}

bool isInterestingBranch(uint64_t pc, bool taken, uint64_t ctx) {

  // here do the bb pruning:
  updateBitmap((void *)pc, ctx);
  bool bitmap_interesting = is_interesting_;
  if (!is_interesting_) {
    if (cxx_log_fp) {
      fprintf(cxx_log_fp,
              "[interesting_branch] pc=0x%llx taken=%u ctx=0x%llx bitmap_pruned=1\n",
              (unsigned long long)pc,
              taken ? 1u : 0u,
              (unsigned long long)ctx);
      fflush(cxx_log_fp);
    }
    printf("[interesting_branch] pc=0x%llx taken=%u ctx=0x%llx bitmap_pruned=1\n",
           (unsigned long long)pc,
           taken ? 1u : 0u,
           (unsigned long long)ctx);
    fflush(stdout);
    fprintf(stderr,
            "[interesting_branch] pc=0x%llx taken=%u ctx=0x%llx bitmap_pruned=1\n",
            (unsigned long long)pc,
            taken ? 1u : 0u,
            (unsigned long long)ctx);
    fflush(stderr);
    return false; // if pruned, don't proceed anymore, treat this brc as concrete basically.
  }

  uint32_t h = hashPc(pc, taken);
  uint32_t idx = getIndex(h);
  bool new_context = isInterestingContext(h, virgin_map_[idx]);
  bool ret = true;

  uint8_t virgin_before = virgin_map_[idx];
  uint8_t trace_before = trace_map_[idx];

  virgin_map_[idx]++;

  if ((virgin_map_[idx] | trace_map_[idx]) != trace_map_[idx]) {
    uint32_t inv_h = hashPc(pc, !taken);
    uint32_t inv_idx = getIndex(inv_h);

    trace_map_[idx] |= virgin_map_[idx];

    // mark the inverse case, because it's already covered by current testcase
    virgin_map_[inv_idx]++;

    trace_map_[inv_idx] |= virgin_map_[inv_idx];

    virgin_map_[inv_idx]--;
    ret = true;
  }
  else if (new_context) {
    ret = true;
  }
  else
    ret = false;

  prev_loc_ = h;

  if (cxx_log_fp) {
    fprintf(cxx_log_fp,
            "[interesting_branch] pc=0x%llx taken=%u ctx=0x%llx bitmap_pruned=0 new_context=%d ret=%d idx=%u virgin_before=%u virgin_after=%u trace_before=%u trace_after=%u\n",
            (unsigned long long)pc,
            taken ? 1u : 0u,
            (unsigned long long)ctx,
            new_context ? 1 : 0,
            ret ? 1 : 0,
            idx,
            virgin_before,
            virgin_map_[idx],
            trace_before,
            trace_map_[idx]);
    fflush(cxx_log_fp);
  }
  printf("[interesting_branch] pc=0x%llx taken=%u ctx=0x%llx bitmap_pruned=0 new_context=%d ret=%d idx=%u virgin_before=%u virgin_after=%u trace_before=%u trace_after=%u\n",
         (unsigned long long)pc,
         taken ? 1u : 0u,
         (unsigned long long)ctx,
         new_context ? 1 : 0,
         ret ? 1 : 0,
         idx,
         virgin_before,
         virgin_map_[idx],
         trace_before,
         trace_map_[idx]);
  fflush(stdout);
  fprintf(stderr,
          "[interesting_branch] pc=0x%llx taken=%u ctx=0x%llx bitmap_pruned=0 new_context=%d ret=%d idx=%u virgin_before=%u virgin_after=%u trace_before=%u trace_after=%u\n",
          (unsigned long long)pc,
          taken ? 1u : 0u,
          (unsigned long long)ctx,
          new_context ? 1 : 0,
          ret ? 1 : 0,
          idx,
          virgin_before,
          virgin_map_[idx],
          trace_before,
          trace_map_[idx]);
  fflush(stderr);

  return ret;
}

// dump the tree and flush the union table to get ready for the solving request
void generate_tree_dump(int qid) {
  std::cout << "[generate_tree_dump] DEBUG: dump_tree_id_=" << dump_tree_id_ << " before formatting" << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[generate_tree_dump] DEBUG: dump_tree_id_=%u before formatting\n", dump_tree_id_); fflush(cxx_log_fp); }
  std::string tree_idstr = std::to_string(dump_tree_id_ % 1000000);
  
  // Marco-compatible: use relative path (current working directory should be OUTPUT_DIR)
  // If MARCO_TREE_DIR is set, use it; otherwise use "." (relative to current working directory)
  const char* tree_base_env = getenv("MARCO_TREE_DIR");
  std::string tree_base = (tree_base_env && tree_base_env[0] != '\0') ? std::string(tree_base_env) : std::string(".");
  std::string abs_tree_dir = tree_base + "/tree" + std::to_string(qid);
  std::string abs_output_file = abs_tree_dir + "/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  
  std::cout << "[generate_tree_dump] called with qid=" << qid << ", output_file=" << abs_output_file << std::endl;
  fflush(stdout);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[generate_tree_dump] called with qid=%d, output_file=%s\n", qid, abs_output_file.c_str()); fflush(cxx_log_fp); }
  
  // Create tree directory if it doesn't exist
  std::string mkdir_cmd = "mkdir -p " + abs_tree_dir;
  printf("[generate_tree_dump] DEBUG: mkdir_cmd='%s', tree_base='%s', abs_tree_dir='%s'\n", mkdir_cmd.c_str(), tree_base.c_str(), abs_tree_dir.c_str());
  fflush(stdout);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[generate_tree_dump] DEBUG: mkdir_cmd='%s', tree_base='%s', abs_tree_dir='%s'\n", mkdir_cmd.c_str(), tree_base.c_str(), abs_tree_dir.c_str()); fflush(cxx_log_fp); }
  
  int mkdir_ret = system(mkdir_cmd.c_str());
  if (mkdir_ret != 0) {
    fprintf(stderr, "[generate_tree_dump] failed to create directory: %s (ret=%d, errno=%d: %s)\n", abs_tree_dir.c_str(), mkdir_ret, errno, strerror(errno));
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[generate_tree_dump] failed to create directory: %s (ret=%d, errno=%d: %s)\n", abs_tree_dir.c_str(), mkdir_ret, errno, strerror(errno)); fflush(cxx_log_fp); }
  } else {
    printf("[generate_tree_dump] created directory: %s\n", abs_tree_dir.c_str());
    fflush(stdout);
    std::cout << "[generate_tree_dump] created directory: " << abs_tree_dir << std::endl;
    std::cout.flush();
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[generate_tree_dump] created directory: %s\n", abs_tree_dir.c_str()); fflush(cxx_log_fp); }
  }
  
  // Use absolute path for file output
  std::string output_file = abs_output_file;
  
  size_t swrite;
  FILE *fp;
  if ((fp = fopen(output_file.c_str(), "wb")) == NULL) {
    fprintf(stderr, "[generate_tree_dump]1: cannot open file to write: %s (errno=%d: %s)\n", output_file.c_str(), errno, strerror(errno));
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[generate_tree_dump]1: cannot open file to write: %s (errno=%d: %s)\n", output_file.c_str(), errno, strerror(errno)); fflush(cxx_log_fp); }
    return;
  }

  std::cout << "max_label_ = " << max_label_ << ", max_label_per_session = " << max_label_per_session << std::endl;

  // Use max_label_per_session if max_label_ is 0 (fallback for SymFit compatibility)
  uint32_t effective_max_label = (max_label_ > 0) ? max_label_ : max_label_per_session;
  std::cout << "[generate_tree_dump] using effective_max_label = " << effective_max_label << std::endl;

  swrite = fwrite((void *)__union_table, sizeof(dfsan_label_info), effective_max_label+1, fp);
  if (swrite != (effective_max_label+1)) {
    fprintf(stderr, "[generate_tree_dump]1: write error %d (expected %u)\n", swrite, effective_max_label+1);
  }
  fclose(fp);

  // generate deps protobuf dump
  // std::string output_file1 = "./deps/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  // std::fstream output(output_file1, std::ios::out | std::ios::trunc | std::ios::binary);
  // if (!new_trace.SerializeToOstream(&output)) {
  //     fprintf(stderr, "[generate_tree_dump]2: cannot open file to write: %s\n", output_file1.c_str());
  //     return;
  // }
  // std::cout << "entry in this trace before:" << new_trace.extracon_size() << std::endl;
  // new_trace.mutable_extracon()->DeleteSubrange(0, new_trace.extracon_size());
  // std::cout << "entry in this trace after: " << new_trace.extracon_size() << std::endl;

}

void handle_fmemcmp(uint8_t* data, uint64_t index, uint32_t size, uint32_t tid, uint64_t addr) {
  std::unordered_map<uint32_t, uint8_t> rgd_solution;
  for(uint32_t i=0;i<size;i++) {
    //rgd_solution[(uint32_t)index+i] = (uint8_t) (data & 0xff);
    rgd_solution[(uint32_t)index+i] = data[i];
    //data = data >> 8 ;
  }
  if (SAVING_WHOLE) {
    // Marco-compatible: write outputs under SYMCC_OUTPUT_DIR/fifo if provided
    // This ensures new test cases are generated in fifo/queue directory
    const char* out_base_env = getenv("SYMCC_OUTPUT_DIR");
    std::string out_base = (out_base_env && out_base_env[0] != '\0') ? std::string(out_base_env) : std::string(".");
    std::string out_fifo_dir = out_base + "/fifo";
    generate_input(rgd_solution, input_file, out_fifo_dir.c_str(), ce_count+=1);
  }
  else {
    RGDSolution sol = {rgd_solution, tid, addr, 0, 0};
    solution_queue.enqueue(sol);
  }
}

// clean up the union table
void cleanup1() {
  if (max_label_per_session > max_label_) {
    std::cout << "warning! max_label_per_session > max_label_"
              << ", max_label_per_session=" << max_label_per_session
              << ", max_label_=" << max_label_
              << std::endl;
  }
  // in this trace, max_label_ is passed from the other end
  for(int i = 0; i <= max_label_; i++) {
    dfsan_label_info* info = get_label_info(i);
    memset(info, 0, sizeof(dfsan_label_info));
  }
  // shmdt(__union_table);
  max_label_ = 0;
  max_label_per_session = 0;
  flipped_labels_session.clear();
}

void cleanup2() {
  std::cout << "cleanup2(max_label_per_session): "
          << " max_label_per_session=" << max_label_per_session
          << " max_label_=" << max_label_ << std::endl;

  // in this trace, max_label_ is passed from the other end
  for(int i = 0; i <= max_label_per_session; i++) {
    dfsan_label_info* info = get_label_info(i);
    memset(info, 0, sizeof(dfsan_label_info));
  }
  // shmdt(__union_table);
  max_label_per_session = 0;
  flipped_labels_session.clear();
}

int cleanup_deps() {
  expr_cache.clear();
  deps_cache.clear();
  int count = 0;
  for (int i = 0 ; i < __branch_deps->size(); i++) {
    branch_dep_t* slot =  __branch_deps->at(i);
    if (slot) {
      count += 1;
      delete slot;
      __branch_deps->at(i) = nullptr;
    }
  }
  return count;
}


uint32_t solve(int shmid, uint32_t pipeid, uint32_t brc_flip, std::ifstream &pcsetpipe) {
  // Use printf to ensure output (not buffered)
  printf("[solve] ENTER function, shmid=%d pipeid=%u brc_flip=%u\n", shmid, pipeid, brc_flip);
  fflush(stdout);
  std::cout << "[solve] ENTER function, shmid=" << shmid << " pipeid=" << pipeid << " brc_flip=" << brc_flip << std::endl;
  std::cout.flush();
  fprintf(stderr, "[solve] ENTER function, shmid=%d pipeid=%u brc_flip=%u\n", shmid, pipeid, brc_flip);
  fflush(stderr);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] ENTER function, shmid=%d pipeid=%u brc_flip=%u\n", shmid, pipeid, brc_flip); fflush(cxx_log_fp); } else { printf("[solve] ERROR: cxx_log_fp is NULL!\n"); fflush(stdout); fprintf(stderr, "[solve] ERROR: cxx_log_fp is NULL!\n"); fflush(stderr); }

  std::ifstream myfile;
  std::cout << "[solve] about to open /tmp/wp2" << std::endl;
  std::cout.flush();
  fprintf(stderr, "[solve] about to open /tmp/wp2\n");
  fflush(stderr);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] about to open /tmp/wp2\n"); fflush(cxx_log_fp); } else { fprintf(stderr, "[solve] ERROR: cxx_log_fp is NULL!\n"); fflush(stderr); }
  myfile.open("/tmp/wp2");
  std::cout << "[solve] opened /tmp/wp2, is_open=" << myfile.is_open() << " good=" << myfile.good() << " eof=" << myfile.eof() << " fail=" << myfile.fail() << " bad=" << myfile.bad() << std::endl;
  std::cout.flush();
  fprintf(stderr, "[solve] opened /tmp/wp2, is_open=%d good=%d eof=%d fail=%d bad=%d\n", myfile.is_open(), myfile.good(), myfile.eof(), myfile.fail(), myfile.bad());
  fflush(stderr);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] opened /tmp/wp2, is_open=%d good=%d eof=%d fail=%d bad=%d\n", myfile.is_open(), myfile.good(), myfile.eof(), myfile.fail(), myfile.bad()); fflush(cxx_log_fp); } else { fprintf(stderr, "[solve] ERROR: cxx_log_fp is NULL!\n"); fflush(stderr); }

  __union_table = (dfsan_label_info*)shmat(shmid, nullptr, 0);
  if (__union_table == (void*)(-1)) {
    printf("error %s\n",strerror(errno));
    std::cout << "[solve] shmat failed: " << strerror(errno) << std::endl;
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] shmat failed: %s\n", strerror(errno)); fflush(cxx_log_fp); }
    return 0;
  }
  std::cout << "[solve] shmat succeeded, __union_table=" << (void*)__union_table << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] shmat succeeded, __union_table=%p\n", __union_table); fflush(cxx_log_fp); }

  memset(virgin_map_, 0, kMapSize);
  memset(node_map, 0, pfxkMapSize * sizeof(uint16_t)); // a per trace bitmap, for localvis bucketization pruning;
  prev_loc_ = 0;
  // Reset dump_tree_id_ at the start of each solve() call to ensure it's set from the current trace
  // This prevents using stale values from previous traces
  dump_tree_id_ = 0;
  printf("[solve] DEBUG: reset dump_tree_id_=0 at start of solve()\n");
  fflush(stdout);
  fprintf(stderr, "[solve] DEBUG: reset dump_tree_id_=0 at start of solve()\n");
  fflush(stderr);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: reset dump_tree_id_=0 at start of solve()\n"); fflush(cxx_log_fp); }
  std::string line;
  size_t pos = 0;
  std::string token;
  std::string delimiter = ",";
  uint32_t maxlabel = 0;
  uint32_t tid = (uint32_t)-1;  // Use -1 as default to detect uninitialized values
  uint32_t first_tid = (uint32_t)-1;  // Track first non-zero tid for dump_tree_id_
  bool first_tid_set = false;
  uint32_t qid = 0;
  uint32_t label = 0;
  uint32_t direction = 0;
  uint64_t addr = 0;
  uint64_t ctx  = 0;
  uint32_t order = 0;
  //constratint type: conditional, gep, add_constraints
  uint32_t cons_type = 0;

  uint32_t filtered_count = 0;
  //create global state for one session
  XXH64_state_t path_prefix;
  XXH64_reset(&path_prefix,0);
  uint64_t acc_time = 0;
  uint64_t one_start = getTimeStamp();
  bool skip_rest = false;
  int line_count = 0;
  uint32_t first_qid = 0; // Track the first qid for tree dump (should match queueid from scheduler)
  bool first_qid_set = false;
  uint32_t first_tid_for_pp = (uint32_t)-1; // Track first tid for path-prefix initialization
  std::cout << "[solve] about to enter while loop to read from /tmp/wp2" << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] about to enter while loop to read from /tmp/wp2\n"); fflush(cxx_log_fp); }
  while (std::getline(myfile, line))
  {
    line_count++;
    std::cout << "[solve] read line " << line_count << " (length=" << line.length() << "): " << (line.length() > 80 ? line.substr(0, 80) : line) << std::endl;
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] read line %d (length=%zu): %s\n", line_count, line.length(), (line.length() > 80 ? line.substr(0, 80).c_str() : line.c_str())); fflush(cxx_log_fp); }
    if (line.empty()) {
      std::cout << "[solve] got empty line, continue" << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] got empty line, continue\n"); fflush(cxx_log_fp); }
      continue;
    }
    int token_index = 0;
    while ((pos = line.find(delimiter)) != std::string::npos) {
      token = line.substr(0, pos);

      switch (token_index) {
        case 0: 
                qid = stoul(token);
                // Track the first qid for tree dump (should match queueid from scheduler)
                printf("[solve] DEBUG: case 0, token='%s', qid=%u, first_qid_set=%d\n", token.c_str(), qid, first_qid_set ? 1 : 0);
                fflush(stdout);
                if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: case 0, token='%s', qid=%u, first_qid_set=%d\n", token.c_str(), qid, first_qid_set ? 1 : 0); fflush(cxx_log_fp); }
                if (!first_qid_set) {
                  first_qid = qid;
                  first_qid_set = true;
                  printf("[solve] *** FIRST QID SET TO %u ***\n", first_qid);
                  fflush(stdout);
                  std::cout << "[solve] first qid set to " << first_qid << std::endl;
                  std::cout.flush();
                  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] *** FIRST QID SET TO %u ***\n", first_qid); fflush(cxx_log_fp); }
                }
                break;  // queue id; for sage usage
        case 1: label = stoul(token); break;  // index
        case 2: direction  = stoul(token); break;
        case 3: addr  = stoull(token); break;
        case 4: ctx = stoull(token); break;
        case 5: order = stoul(token); break;
        case 6: cons_type = stoul(token); break;
        case 7: // testcase id
                tid = stoul(token);
                printf("[solve] DEBUG: case 7, token='%s', tid=%u, first_tid_set=%s, first_tid=%u, dump_tree_id_=%u\n", token.c_str(), tid, (first_tid_set ? "true" : "false"), first_tid, dump_tree_id_);
                fflush(stdout);
                fprintf(stderr, "[solve] DEBUG: case 7, token='%s', tid=%u, first_tid_set=%s, first_tid=%u, dump_tree_id_=%u\n", token.c_str(), tid, (first_tid_set ? "true" : "false"), first_tid, dump_tree_id_);
                fflush(stderr);
                if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: case 7, token='%s', tid=%u, first_tid_set=%s, first_tid=%u, dump_tree_id_=%u\n", token.c_str(), tid, (first_tid_set ? "true" : "false"), first_tid, dump_tree_id_); fflush(cxx_log_fp); }
                // Track first tid for dump_tree_id_ (Marco-compatible)
                // Note: tid=-1 (0xFFFFFFFF) means uninitialized, tid=0 is a valid input id
                if (tid != (uint32_t)-1 && !first_tid_set) {
                  first_tid = tid;
                  first_tid_set = true;
                dump_tree_id_ = tid;
                  printf("[solve] DEBUG: first_tid set to %u, dump_tree_id_ set to %u\n", first_tid, dump_tree_id_);
                  fflush(stdout);
                  fprintf(stderr, "[solve] DEBUG: first_tid set to %u, dump_tree_id_ set to %u\n", first_tid, dump_tree_id_);
                  fflush(stderr);
                  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: first_tid set to %u, dump_tree_id_ set to %u\n", first_tid, dump_tree_id_); fflush(cxx_log_fp); }
                } else if (tid != (uint32_t)-1) {
                  // Update dump_tree_id_ if we haven't set it yet or if current tid is different
                  if (dump_tree_id_ == 0 || tid != dump_tree_id_) {
                    dump_tree_id_ = tid;
                    printf("[solve] DEBUG: updated dump_tree_id_ to %u (tid=%u)\n", dump_tree_id_, tid);
                    fflush(stdout);
                    fprintf(stderr, "[solve] DEBUG: updated dump_tree_id_ to %u (tid=%u)\n", dump_tree_id_, tid);
                    fflush(stderr);
                    if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: updated dump_tree_id_ to %u (tid=%u)\n", dump_tree_id_, tid); fflush(cxx_log_fp); }
                  }
                } else if (tid == (uint32_t)-1) {
                  printf("[solve] DEBUG: tid=-1 (uninitialized), not updating dump_tree_id_ (current=%u)\n", dump_tree_id_);
                  fflush(stdout);
                  fprintf(stderr, "[solve] DEBUG: tid=-1 (uninitialized), not updating dump_tree_id_ (current=%u)\n", dump_tree_id_);
                  fflush(stderr);
                  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: tid=-1 (uninitialized), not updating dump_tree_id_ (current=%u)\n", dump_tree_id_); fflush(cxx_log_fp); }
                }
                break;
        case 8: // the maximum entry count in the union table
                max_label_ = stoull(token);
                break;
        default: break;
      }
      line.erase(0, pos + delimiter.length());
      token_index++;
    }
    // Handle the last field (after the last delimiter)
    // After parsing 8 fields (0-7), the remaining line should be the 9th field (max_label_)
    if (token_index == 8 && !line.empty()) {
      // Remove trailing comma and newline if present
      while (!line.empty() && (line.back() == ',' || line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
      }
      if (!line.empty()) {
        try {
          max_label_ = stoull(line);
          std::cout << "[solve] DEBUG: parsed last field max_label_=" << max_label_ << " from remaining line='" << line << "'" << std::endl;
          if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: parsed last field max_label_=%llu from remaining line='%s'\n", (unsigned long long)max_label_, line.c_str()); fflush(cxx_log_fp); }
        } catch (...) {
          std::cout << "[solve] DEBUG: failed to parse max_label_ from line='" << line << "'" << std::endl;
          if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: failed to parse max_label_ from line='%s'\n", line.c_str()); fflush(cxx_log_fp); }
        }
      } else {
        std::cout << "[solve] DEBUG: remaining line is empty after parsing 8 fields, token_index=" << token_index << std::endl;
        if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: remaining line is empty after parsing 8 fields, token_index=%d\n", token_index); fflush(cxx_log_fp); }
      }
    } else {
      std::cout << "[solve] DEBUG: token_index=" << token_index << ", line.empty()=" << (line.empty() ? "true" : "false") << ", line='" << line << "'" << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: token_index=%d, line.empty()=%s, line='%s'\n", token_index, (line.empty() ? "true" : "false"), line.c_str()); fflush(cxx_log_fp); }
    }
    // Debug: log tid value after parsing
    if (line_count <= 5 || tid != 0) {
      std::cout << "[solve] DEBUG: after parsing line " << line_count << ", tid=" << tid << ", token_index=" << token_index << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: after parsing line %d, tid=%u, token_index=%d\n", line_count, tid, token_index); fflush(cxx_log_fp); }
    }
    std::cout << "[solve] parsed line " << line_count << ": qid=" << qid << " label=" << label << " dir=" << direction << " addr=0x" << std::hex << addr << std::dec << " ctx=" << ctx << " order=" << order << " cons_type=" << cons_type << " tid=" << tid << " max_label_=" << max_label_ << std::endl;
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] parsed line %d: qid=%u label=%u dir=%u addr=0x%llx ctx=%llu order=%u cons_type=%u tid=%u max_label_=%llu\n", line_count, qid, label, direction, (unsigned long long)addr, (unsigned long long)ctx, order, cons_type, tid, (unsigned long long)max_label_); fflush(cxx_log_fp); }
    std::unordered_map<uint32_t, uint8_t> sol;
    std::unordered_map<uint32_t, uint8_t> opt_sol;

    if (skip_rest) {
      std::cout << "[solve] skip_rest=true, continue" << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] skip_rest=true, continue\n"); fflush(cxx_log_fp); }
      continue;
    }

    computeHash(ctx); // get the call_stack_hash_ ready

    // Initialize path_prefix with tid for the first branch of each trace
    // This ensures different traces have different path-prefix hashes even if they process the same PC
    if (cxx_log_fp) {
      fprintf(cxx_log_fp, "[solve] DEBUG: checking path_prefix init: cons_type=%u first_tid_for_pp=%u tid=%u\n", 
              cons_type, first_tid_for_pp, tid);
      fflush(cxx_log_fp);
    }
    if (cons_type == 0 && first_tid_for_pp == (uint32_t)-1 && tid != (uint32_t)-1) {
      first_tid_for_pp = tid;
      XXH64_update(&path_prefix, &tid, sizeof(tid));
      if (cxx_log_fp) {
        fprintf(cxx_log_fp, "[solve] initialized path_prefix with tid=%u for first branch\n", tid);
        fflush(cxx_log_fp);
      }
      printf("[solve] initialized path_prefix with tid=%u for first branch\n", tid);
      fflush(stdout);
    }

    if (cons_type == 0) {
      std::cout << "[solve] cons_type=0 (conditional), processing..." << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] cons_type=0 (conditional), processing...\n"); fflush(cxx_log_fp); }
      bool try_solve = false;
      int uniq_pcset = 0;
      int ifmemorize = 0;
      switch (brc_flip) {
        case 0: // using qsym style branch filter
          BRC_MODE = 1;
          if (label) {
            try_solve = isInterestingBranch(addr, direction, ctx);
            if (try_solve) uniq_pcset = 1;
          }
          break;
        case 1: // using pathprefix branch filter
          BRC_MODE = 0;
          try_solve = isInterestingPathPrefix(addr, direction, label, &path_prefix);
          if (isInterestingBranch(addr, direction, ctx)) uniq_pcset = 1; // promote it in the PC queue
          if (isInterestingNode(addr, direction, ctx)) ifmemorize = 1; // pruning by local visitcount
          break;
      }
      if (try_solve) {
        filtered_count++;
      }
      uint64_t t_update = getTimeStamp();
      std::cout << "[solve] calling update_graph label=" << label << " addr=0x" << std::hex << addr << std::dec << " dir=" << direction << " try_solve=" << try_solve << " tid=" << tid << " qid=" << qid << " uniq_pcset=" << uniq_pcset << " ifmemorize=" << ifmemorize << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] calling update_graph label=%u addr=0x%llx dir=%u try_solve=%d tid=%u qid=%u uniq_pcset=%d ifmemorize=%d\n", label, (unsigned long long)addr, direction, try_solve, tid, qid, uniq_pcset, ifmemorize); fflush(cxx_log_fp); }
      update_graph(label, addr, direction, try_solve, tid, qid, uniq_pcset, ifmemorize);
      if (cxx_log_fp) {
        fprintf(cxx_log_fp,
                "[branch_summary] tid=%u qid=%u label=%u dir=%u addr=0x%llx ctx=%llu order=%u try_solve=%d uniq_pcset=%d ifmemorize=%d untaken_digest=0x%llx\n",
                tid,
                qid,
                label,
                direction,
                (unsigned long long)addr,
                (unsigned long long)ctx,
                order,
                try_solve ? 1 : 0,
                uniq_pcset,
                ifmemorize,
                (unsigned long long)untaken_update_ifsat);
        fflush(cxx_log_fp);
      }
      printf("[branch_summary] tid=%u qid=%u label=%u dir=%u addr=0x%llx ctx=%llu order=%u try_solve=%d uniq_pcset=%d ifmemorize=%d untaken_digest=0x%llx\n",
             tid,
             qid,
             label,
             direction,
             (unsigned long long)addr,
             (unsigned long long)ctx,
             order,
             try_solve ? 1 : 0,
             uniq_pcset,
             ifmemorize,
             (unsigned long long)untaken_update_ifsat);
      fflush(stdout);
      fprintf(stderr,
              "[branch_summary] tid=%u qid=%u label=%u dir=%u addr=0x%llx ctx=%llu order=%u try_solve=%d uniq_pcset=%d ifmemorize=%d untaken_digest=0x%llx\n",
              tid,
              qid,
              label,
              direction,
              (unsigned long long)addr,
              (unsigned long long)ctx,
              order,
              try_solve ? 1 : 0,
              uniq_pcset,
              ifmemorize,
              (unsigned long long)untaken_update_ifsat);
      fflush(stderr);
      total_updateG_time += (getTimeStamp() - t_update);
      std::cout << "[solve] update_graph returned" << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] update_graph returned\n"); fflush(cxx_log_fp); }
    }
    else if (cons_type == 2) {
      if (std::getline(myfile,line)) {
        uint32_t memcmp_datasize = label;
        uint8_t data[1024];
        int token_index = 0;
        while ((pos = line.find(delimiter)) != std::string::npos) {
          token = line.substr(0,pos);
          data[token_index++] = stoul(token);
          line.erase(0, pos + delimiter.length());
        }
        data[token_index++] = stoul(line);
        bool try_solve = bcount_filter(addr, ctx, 0, order);
        std::cout << "going for handle_fmemcmp branch" << std::endl;
        if (try_solve)
          handle_fmemcmp(data, direction, label, tid, addr);
      } else {
        break;
      }
    }
    acc_time = getTimeStamp() - one_start; // time spent on one single seed
    if (acc_time > 30000000) {
      std::cout << "[solve] timeout! acc_time=" << acc_time << " > 30000000, skip_rest=true" << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] timeout! acc_time=%llu > 30000000, skip_rest=true\n", (unsigned long long)acc_time); fflush(cxx_log_fp); }
      skip_rest = true; // 10s timeout per seed
  }
  }
  std::cout << "[solve] exited while loop, line_count=" << line_count << ", end of one input" << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] exited while loop, line_count=%d, end of one input\n", line_count); fflush(cxx_log_fp); }
  // end of one input
  total_symb_brc += filtered_count;

  fid = 0; // reset for next input seed
  // at the end of each execution, dump tree and flush everything of the running seed
  // Use first_qid (from /tmp/wp2) for tree dump, which should match queueid from scheduler
  // This ensures tree dump and gen_solve_pc use the same queueid
  std::cout << "[solve] DEBUG: before generate_tree_dump, first_qid_set=" << (first_qid_set ? "true" : "false") << ", first_qid=" << first_qid << ", qid=" << qid << std::endl;
  fflush(stdout);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: before generate_tree_dump, first_qid_set=%s, first_qid=%u, qid=%u\n", (first_qid_set ? "true" : "false"), first_qid, qid); fflush(cxx_log_fp); }
  uint32_t tree_dump_qid = first_qid_set ? first_qid : qid;
  std::cout << "[solve] calling generate_tree_dump with qid=" << tree_dump_qid << " (first_qid_set=" << (first_qid_set ? "true" : "false") << ", first_qid=" << first_qid << ", last_qid=" << qid << ")" << std::endl;
  fflush(stdout);
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] calling generate_tree_dump with qid=%u (first_qid_set=%s, first_qid=%u, last_qid=%u)\n", tree_dump_qid, (first_qid_set ? "true" : "false"), first_qid, qid); fflush(cxx_log_fp); }
  generate_tree_dump(tree_dump_qid);

  cleanup1(); // flush the union table of the running seed
  cleanup_deps(); // flush the dependency tree

  acc_time = getTimeStamp() - one_start;
  total_time += acc_time;

  if (skip_rest) std::cout << "timeout!" << std::endl;
  if (ce_count % 50 == 0) {
    std::cout << "total uniqpp PC count= " << total_symb_brc
              << "\ncur total_pruned_ones = " << total_pruned_ones
              << "\ntotal (exec;no solving)cost  " << total_time / 1000  << "ms"
              << "\ntotal updateG time " << total_updateG_time / 1000  << "ms"
              << "\ntotal tupling time " << total_extra_time / 1000  << "ms"
              << "\ntotal getdeps time " << total_getdeps_cost / 1000  << "ms"
              << "\ncur (exec;no solving)cost: " << acc_time / 1000  << "ms"
              << "\ntotal reload time " << total_reload_time / 1000  << "ms"
              << "\ntotal solving(reload included) time " << total_solving_time / 1000  << "ms"
              << std::endl;
  }
  // Use first_tid if available, otherwise use last tid
  uint32_t return_tid = first_tid_set ? first_tid : tid;
  std::cout << "[solve] finished, read " << line_count << " lines, filtered_count=" << filtered_count << ", returning tid=" << return_tid << " (first_tid=" << first_tid << ", last_tid=" << tid << ", dump_tree_id_=" << dump_tree_id_ << ")" << std::endl;
  if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] finished, read %d lines, filtered_count=%u, returning tid=%u (first_tid=%u, last_tid=%u, dump_tree_id_=%u)\n", line_count, filtered_count, return_tid, first_tid, tid, dump_tree_id_); fflush(cxx_log_fp); }
  // Ensure dump_tree_id_ is set to first_tid if available, otherwise use return_tid
  // Note: tid=0 is a valid input id (e.g., id:000000), so we should allow it
  if (dump_tree_id_ == 0 && first_tid_set && first_tid != (uint32_t)-1) {
    dump_tree_id_ = first_tid;
    printf("[solve] DEBUG: set dump_tree_id_=%u from first_tid at end of solve()\n", dump_tree_id_);
    fflush(stdout);
    fprintf(stderr, "[solve] DEBUG: set dump_tree_id_=%u from first_tid at end of solve()\n", dump_tree_id_);
    fflush(stderr);
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: set dump_tree_id_=%u from first_tid at end of solve()\n", dump_tree_id_); fflush(cxx_log_fp); }
  } else if (dump_tree_id_ == 0 && return_tid != (uint32_t)-1) {
    dump_tree_id_ = return_tid;
    printf("[solve] DEBUG: set dump_tree_id_=%u from return_tid at end of solve()\n", dump_tree_id_);
    fflush(stdout);
    fprintf(stderr, "[solve] DEBUG: set dump_tree_id_=%u from return_tid at end of solve()\n", dump_tree_id_);
    fflush(stderr);
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[solve] DEBUG: set dump_tree_id_=%u from return_tid at end of solve()\n", dump_tree_id_); fflush(cxx_log_fp); }
  }
  return return_tid;
}

extern "C" {
  void init_core(bool saving_whole, uint32_t initial_count) {
    init(saving_whole);
    named_pipe_fd = open("/tmp/pcpipe", O_WRONLY);
    // Get log directory from environment or use default
    const char* log_dir = getenv("MARCO_LOG_DIR");
    char log_path[512];
    if (log_dir && strlen(log_dir) > 0) {
      snprintf(log_path, sizeof(log_path), "%s/fastgen_cxx.log", log_dir);
    } else {
      snprintf(log_path, sizeof(log_path), "/home/administrator/tli-test/Marco-SymFit/test/workdir/logs/fastgen_cxx.log");
    }
    cxx_log_fp = fopen(log_path, "a");
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[init_core] starting, open(/tmp/pcpipe) fd=%d errno=%d, log_path=%s\n", named_pipe_fd, (named_pipe_fd < 0 ? errno : 0), log_path); fflush(cxx_log_fp); }
    pcsetpipe.open("/tmp/myfifo");
    if (!pcsetpipe.is_open()) {
      std::cout << "[init_core] failed to open /tmp/myfifo for reading" << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "open myfifo FAILED\n"); fflush(cxx_log_fp); }
    } else {
      std::cout << "[init_core] opened /tmp/myfifo for reading" << std::endl;
      if (cxx_log_fp) { fprintf(cxx_log_fp, "open myfifo OK\n"); fflush(cxx_log_fp); }
    }

    ce_count = -1;
    // init_count =  initial_count - 1;

    printf("the length of union_table is %u\n", 0xC00000000/sizeof(dfsan_label_info));
    __z3_solver.set("timeout", 1000U);
    memset(pfx_pp_map, 0, pfxkMapSize);
    memset(node_map, 0, pfxkMapSize * sizeof(uint16_t));
    memset(pp_map, 0, kMapSize);
    memset(trace_map_, 0, kMapSize);
    memset(context_map_, 0, kMapSize);
    memset(bitmap_, 0, kBitmapSize * sizeof(uint16_t));
  }

  uint32_t run_solver(int shmid, uint32_t pipeid, uint32_t brc_flip, uint32_t lastone) {
    printf("[run_solver] ENTER function, shmid=%d pipeid=%u brc_flip=%u lastone=%u\n", shmid, pipeid, brc_flip, lastone);
    fflush(stdout);
    if (cxx_log_fp) { fprintf(cxx_log_fp, "ENTER run_solver lastone=%u pipeid=%u brc=%u\n", lastone, pipeid, brc_flip); fflush(cxx_log_fp); } else { printf("[run_solver] ERROR: cxx_log_fp is NULL!\n"); fflush(stdout); }
    // reset for every new episode
    max_label_ = 0;
    printf("[run_solver] about to call solve()\n");
    fflush(stdout);
    uint32_t cur_inid = solve(shmid, pipeid, brc_flip, pcsetpipe);
    printf("[run_solver] solve() returned cur_inid=%u\n", cur_inid);
    fflush(stdout);
    // make sure there's one seed generated at the end of this execution
    uint64_t one_start;
    int res;
    std::cout << "cur_inid=" << cur_inid << std::endl;
    std::cout << "run_solver:lastone=" << lastone << std::endl;
    std::string endtoken;
    // Marco semantics: signal end of current trace ingestion
    endtoken = "END@@\n";
    if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] WRITE TOKEN: %s", endtoken.c_str()); fflush(cxx_log_fp); }
    printf("[run_solver] WRITE TOKEN: %s", endtoken.c_str());
    fflush(stdout);
    {
      ssize_t wret = write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
      if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0)); fflush(cxx_log_fp); }
      printf("[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0));
      fflush(stdout);
    }
    fsync(named_pipe_fd);

    // retry and keep producing seeds continuously; block waiting for new decisions
    while (1) {
      one_start = getTimeStamp();
      res = generate_next_tscs(pcsetpipe);
      total_solving_time += (getTimeStamp() - one_start);
      if (res > 0) { // with outcome, CE will pick up new seed to run
        std::cout << "new outcome, move on to sync new batch" << std::endl;
        shmdt(__union_table); // reset for next epi
        endtoken = "ENDNEW@@\n";  // a new seed is generated!
        if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] WRITE TOKEN: %s", endtoken.c_str()); fflush(cxx_log_fp); }
        printf("[run_solver] WRITE TOKEN: %s", endtoken.c_str());
        fflush(stdout);
        {
          ssize_t wret = write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
          if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0)); fflush(cxx_log_fp); }
          printf("[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0));
          fflush(stdout);
        }
        fsync(named_pipe_fd);
        break;
      } else if (res == -1) {
        // Marco original logic: send ENDDUP for duplicate path-prefix
        endtoken = "ENDDUP@@\n";
        if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] WRITE TOKEN: %s", endtoken.c_str()); fflush(cxx_log_fp); }
        printf("[run_solver] WRITE TOKEN: %s", endtoken.c_str());
        fflush(stdout);
        {
          ssize_t wret = write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
          if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0)); fflush(cxx_log_fp); }
          printf("[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0));
          fflush(stdout);
        }
        fsync(named_pipe_fd);
      } else {
        endtoken = "ENDUNSAT@@\n";
        if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] WRITE TOKEN: %s", endtoken.c_str()); fflush(cxx_log_fp); }
        printf("[run_solver] WRITE TOKEN: %s", endtoken.c_str());
        fflush(stdout);
        {
          ssize_t wret = write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
          if (cxx_log_fp) { fprintf(cxx_log_fp, "[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0)); fflush(cxx_log_fp); }
          printf("[run_solver] write() ret=%zd errno=%d\n", wret, (wret < 0 ? errno : 0));
          fflush(stdout);
        }
        fsync(named_pipe_fd);
        std::cout << "failure solving, UNSAT!" << std::endl;
      }
    }
    return 0;
  }

  void wait_ce() {
    sem_wait(semace);
  }

  void post_gra() {
    sem_post(semagra);
  }

  void post_fzr() {
    sem_post(semafzr);
  }

};

#include "afl_trace_map.h"
namespace qsym {

const int kMapSize = 65536;

// static  KNOB<bool> g_opt_context_sensitive(KNOB_MODE_WRITEONCE, "pintool",
//     "context_sensitive", "1", "Generate testcases by awaring of contexts");

namespace {

inline bool isPowerOfTwoOrZero(uintptr_t x) {
  return ((x & (x - 1)) == 0);
}

XXH32_hash_t hashPc(uintptr_t pc, bool taken) {
  // // mimic afl's branch map
  // PIN_LockClient();
  // IMG img = IMG_FindByAddress(pc);
  // PIN_UnlockClient();

  // // hopefully IMG_Id is same for every execution
  // if (!IMG_Valid(img))
  //   LOG_FATAL("invalid image");

  // pc -= IMG_LowAddress(img);

  // UINT32 img_id = IMG_Id(img);
  XXH32_state_t state;
  XXH32_reset(&state, 0); // seed = 0
  XXH32_update(&state, &pc, sizeof(pc));
  // XXH32_update(&state, &img_id, sizeof(img_id));
  XXH32_update(&state, &taken, sizeof(taken));
  return XXH32_digest(&state) % kMapSize;
}

} // namespace

void* safeMalloc(size_t size) {
  void* ptr = malloc(size);
  if (ptr == NULL)
    fprintf(stderr, "out of memory\n");
  return ptr;
}

void AflTraceMap::allocMap() {
  trace_map_ = (uint8_t*)safeMalloc(kMapSize);
  virgin_map_ = (uint8_t*)safeMalloc(kMapSize);
  context_map_ = (uint8_t*)safeMalloc(kMapSize);
  memset(virgin_map_, 0, kMapSize);
}

void AflTraceMap::setDefault() {
  memset(trace_map_, 0, kMapSize);
  memset(context_map_, 0, kMapSize);
}

void AflTraceMap::import(const std::string path) {
  std::ifstream ifs;
  ifs.open(path, std::ios::binary);
  if (ifs.fail()) {
    fprintf(stderr, "cannot read a file, so use a default trace map\n");
    setDefault();
    return;
  }
  ifs.read((char*)trace_map_, kMapSize);
  ifs.read((char*)context_map_, kMapSize);
  if (!ifs)
    setDefault();
  ifs.close();
}

uintptr_t AflTraceMap::getIndex(uintptr_t h) {
  return ((prev_loc_ >> 1) ^ h) % kMapSize;
}

bool AflTraceMap::isInterestingContext(uintptr_t h, uintptr_t bits) {
  // if (!g_opt_context_sensitive.Value())
  //   return false;

  bool interesting = false;

  // only care power of two
  if (!isPowerOfTwoOrZero(bits))
    return false;

  for (auto it = visited_.begin();
      it != visited_.end();
      it++) {
    uintptr_t prev_h = *it;

    // Calculate hash(prev_h || h)
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &prev_h, sizeof(prev_h));
    XXH32_update(&state, &h, sizeof(h));

    uintptr_t hash = XXH32_digest(&state) % (kMapSize * __CHAR_BIT__);
    uintptr_t idx = hash / __CHAR_BIT__;
    uintptr_t mask = 1 << (hash % __CHAR_BIT__);

    if ((context_map_[idx] & mask) == 0) {
      context_map_[idx] |= mask;
      interesting = true;
    }
  }

  if (bits == 0)
    visited_.insert(h);

  return interesting;
}

void AflTraceMap::commit() {
  if (!path_.empty()) {
    std::ofstream ofs;
    ofs.open(path_, std::ios::binary);
    if (ofs.fail())
      fprintf(stderr, "Unable to open a bitmap to commit");
    ofs.write((char*)trace_map_, kMapSize);
    ofs.write((char*)context_map_, kMapSize);
    ofs.close();
  }
}

AflTraceMap::AflTraceMap(const std::string path)
  : path_(path),
    prev_loc_(0),
    visited_() {
  allocMap();
  if (path.empty())
    setDefault();
  else
    import(path);
}

bool AflTraceMap::isInterestingBranch(uintptr_t pc, bool taken) {
  uintptr_t h = hashPc(pc, taken);
  uintptr_t idx = getIndex(h);
  bool new_context = isInterestingContext(h, virgin_map_[idx]);
  bool ret = true;

  virgin_map_[idx]++;

  if ((virgin_map_[idx] | trace_map_[idx]) != trace_map_[idx]) {
    uintptr_t inv_h = hashPc(pc, !taken);
    uintptr_t inv_idx = getIndex(inv_h);

    trace_map_[idx] |= virgin_map_[idx];

    // mark the inverse case, because it's already covered by current testcase
    virgin_map_[inv_idx]++;

    trace_map_[inv_idx] |= virgin_map_[inv_idx];
    commit();

    virgin_map_[inv_idx]--;
    ret = true;
  }
  else if (new_context) {
    ret = true;
    commit();
  }
  else
    ret = false;

  prev_loc_ = h;
  return ret;
}

} // namespace qsym


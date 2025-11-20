#ifndef QSYM_CALL_STACK_MANAGER_H
#define QSYM_CALL_STACK_MANAGER_H

#include <cstdint>
#include <vector>

// #include "common.h"

#define XXH_STATIC_LINKING_ONLY
#include "third_party/xxhash/xxhash.h"
namespace qsym {
  class CallStackManager {
  public:
    CallStackManager();
    ~CallStackManager();

    void visitCall(uintptr_t pc);
    void visitRet(uintptr_t pc);
    void visitBasicBlock(uintptr_t pc);
    void updateBitmap();
    bool isInteresting() { return is_interesting_; }

  private:
    std::vector<uintptr_t> call_stack_;
    XXH32_hash_t call_stack_hash_;
    bool is_interesting_;
    uint16_t *bitmap_;
    uint32_t last_index_;
    bool pending_;
    uintptr_t last_pc_;

    void computeHash();
  };

  extern CallStackManager g_call_stack_manager;

} // namespace qsym

#endif // QSYM_CALL_STACK_MANAGER_H

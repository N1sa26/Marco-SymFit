#ifndef __AFL_TRACE_MAP_H__
#define __AFL_TRACE_MAP_H__

#define FFL(_b) (0xffULL << ((_b) << 3))
#define FF(_b)  (0xff << ((_b) << 3))

#include <cstring>
#include <fstream>
#include <iostream>
#include <set>

// #include "common.h"
#define XXH_STATIC_LINKING_ONLY
#include "third_party/xxhash/xxhash.h"

namespace qsym {
class AflTraceMap {

private:
  std::string path_;
  uintptr_t prev_loc_;
  uint8_t *trace_map_;
  uint8_t *virgin_map_;
  uint8_t *context_map_;
  std::set<uintptr_t> visited_;

  void allocMap();
  void setDefault();
  void import(const std::string path);
  void commit();
  uintptr_t getIndex(uintptr_t h);
  bool isInterestingContext(uintptr_t h, uintptr_t bits);

public:
  AflTraceMap(const std::string path);
  bool isInterestingBranch(uintptr_t pc, bool taken);
};
} // namespace qsym
#endif // __AFL_TRACE_MAP_H__

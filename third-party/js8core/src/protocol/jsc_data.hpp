#pragma once

#include <cstdint>

// Offsets into one char blob, not pointers: pointer tables in a PIE binary
// are relocated at start, ~6 MB of private RAM
struct JscTuple {
  std::uint32_t off;
  int size;
  int index;
};

extern char const JSC_CHARS[];

inline char const* jsc_str(JscTuple const& t) { return JSC_CHARS + t.off; }

extern JscTuple const JSC_MAP[262144];
extern JscTuple const JSC_LIST[262144];
extern int const JSC_PREFIX_SIZE;
extern JscTuple const JSC_PREFIX[103];
extern std::uint32_t const JSC_SIZE;

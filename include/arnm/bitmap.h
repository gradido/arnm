#ifndef ARNM_ARENA_BITMAP_H
#define ARNM_ARENA_BITMAP_H

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// add in program used builtin functions plattform independently usable
// https://gcc.gnu.org/onlinedocs/gcc/Bit-Operation-Builtins.html Returns the number of trailing
// 0-bits in bitmap, starting at the least significant bit position. If x is 0, the result is
// undefined.
static inline int arnm_ctz(unsigned int bitmap) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanForward(&index, bitmap);
  return index;
#else
  return __builtin_ctz(bitmap);
#endif
}
// Returns the number of trailing 0-bits in bitmap, starting at the least significant bit position.
// If x is 0, the result is undefined.
static inline int arnm_ctzll(unsigned long long bitmap) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanForward64(&index, bitmap);
  return index;
#else
  return __builtin_ctzll(bitmap);
#endif
}

#ifdef __cplusplus
}
#endif

#endif // ARNM_ARENA_BITMAP_H

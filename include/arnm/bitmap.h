#ifndef ARNM_ARENA_BITMAP_H
#define ARNM_ARENA_BITMAP_H

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_bitmap arnm_bitmap
 * @brief Bit scans, spelled the same way on every toolchain.
 *
 * Finding the lowest set bit is one instruction on every architecture arnm targets and a
 * different spelling on every compiler: gcc and clang carry it as `__builtin_ctz`, MSVC as
 * `_BitScanForward`. These wrappers hold that difference in one place, so a caller that walks a
 * mask never learns which toolchain it was built with.
 *
 * See the gcc reference for the builtins underneath:
 * https://gcc.gnu.org/onlinedocs/gcc/Bit-Operation-Builtins.html
 *
 * Both return the index itself rather than a result code, because that is what a caller walking
 * a mask wants in a loop header and every result code would have to be unpacked out of one
 * first. What that costs is the empty mask: there is no index to answer with and no room to say
 * so, and the answer is undefined rather than wrong in a particular way. Test the mask first --
 * a caller that loops over one is testing it anyway to know whether to loop at all.
 * @{
 */

/*
 * The 64 bit scan is the one that is not everywhere. MSVC exposes it as `_BitScanForward64`,
 * which its own documentation limits to x64 and ARM64, so a 32 bit MSVC target has no 64 bit
 * scan intrinsic to reach for at all. gcc and clang carry `__builtin_ctzll` on every target
 * they support, 32 bit ones included, lowering it to a helper call where the hardware has no
 * single instruction.
 *
 * That case is refused at compile time rather than at run time. A scan that answers an index
 * has no spare value to report a missing implementation with, and a build that quietly dropped
 * the function would fail at the call site with nothing said about why. The line below says it
 * once, in the place that knows.
 */
#if defined(_MSC_VER) && !defined(_WIN64)
#error                                                                                             \
    "arnm_ctzll needs a 64 bit scan; _BitScanForward64 is x64/ARM64 only. Build for x64/ARM64, or add a 32 bit fallback here (two _BitScanForward passes, low word then high)."
#endif

/**
 * @brief The position of the lowest set bit of a 32 bit mask.
 *
 * Counts the zeros below it, so the answer is the index of that bit and not a count of anything
 * the caller has to convert.
 *
 * @param[in] bitmap Mask to scan; must have at least one bit set.
 * @return The index of the lowest set bit, 0 to 31. Undefined where @p bitmap is 0 -- the
 *         builtins underneath say the same, and no value here could mean "none" without also
 *         being a valid index.
 * @whisper The first light in a row of dark windows
 */
static inline int arnm_ctz(unsigned int bitmap) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanForward(&index, (unsigned long)bitmap);
  return (int)index;
#else
  return __builtin_ctz(bitmap);
#endif
}

/**
 * @brief The position of the lowest set bit of a 64 bit mask.
 *
 * As @ref arnm_ctz(), one word wider. A target without a 64 bit scan never reaches this: the
 * `#error` above stops the build instead.
 *
 * @param[in] bitmap Mask to scan; must have at least one bit set.
 * @return The index of the lowest set bit, 0 to 63. Undefined where @p bitmap is 0.
 * @whisper The first light in a longer row
 */
static inline int arnm_ctzll(unsigned long long bitmap) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanForward64(&index, (unsigned __int64)bitmap);
  return (int)index;
#else
  return __builtin_ctzll(bitmap);
#endif
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_ARENA_BITMAP_H

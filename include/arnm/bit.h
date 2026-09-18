#ifndef ARNM_BIT_H
#define ARNM_BIT_H

#include "arnm/bitmap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_bit arnm_bit
 * @brief Powers of two: build one, round up to one, find its exponent, test for one.
 *
 * Grades, bucket sizes and ladders in arnm are all powers of two, and the same few lines of bit
 * arithmetic kept turning up in each of them. They live here once, spelled out and checked
 * once, so a module that needs one reads as what it does rather than how.
 *
 * None of these return a result code. The limits are in each description, and a caller that
 * cannot guarantee them checks before calling -- these sit on hot paths, and a check that every
 * caller already made would be paid on every call.
 *
 * @{
 */

/**
 * @brief 2^@p exponent as a uint32_t.
 * @param[in] exponent 0 to 31; 32 and above is undefined, as the shift underneath is.
 */
static inline uint32_t arnm_pow2_u32(uint8_t exponent) {
  return (uint32_t)1u << exponent;
}

/**
 * @brief @p value * 2^@p exponent, narrowed to a uint32_t.
 * @param[in] value    Factor; the product must fit a uint32_t, the bits above are dropped.
 * @param[in] exponent Below the width of size_t (64 or 32 bits, depending on the target), and
 *                     small enough for the product to fit. At the width and above the shift
 *                     underneath is undefined; nothing here checks, the caller has already.
 */
static inline uint32_t arnm_mul_pow2_u32(size_t value, uint8_t exponent) {
  return (uint32_t)(value << exponent);
}

/**
 * @brief 2^@p exponent as a uint16_t.
 * @param[in] exponent 0 to 15. 16 to 31 give 0, the bit falling off the uint16_t; 32 and above
 *                     is undefined, as the shift underneath is.
 */
static inline uint16_t arnm_pow2_u16(uint8_t exponent) {
  return (uint16_t)(1u << exponent);
}

/**
 * @brief The smallest power of two at or above @p value: 13 gives 16, 16 gives 16, 17 gives 32.
 *
 * Branchless: every bit below the highest set one is filled in, which leaves a run of ones
 * ending where the value did, and one more turns that run into the next power of two.
 *
 * @param[in] value 1 to 2^31. Above 2^31 the answer would need a 33rd bit and comes back as 0;
 *                  0 comes back as 0 as well.
 */
static inline uint32_t arnm_ceil_power_of_two(uint32_t value) {
  value--;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  return value + 1u;
}

/**
 * @brief The exponent of the smallest power of two at or above @p value: 13 gives 4, as 16 is
 *        2^4.
 *
 * Defined for every input: 0 and 1 give 0, and anything above 2^31 gives 32 -- one past every
 * exponent a uint32_t holds, so a caller comparing against its own largest exponent refuses it
 * without a separate check.
 *
 * @param[in] value Any value.
 */
static inline uint8_t arnm_log2_power_of_two(uint32_t value) {
  return value <= 1u ? 0u : (uint8_t)(32u - (uint32_t)arnm_clz(value - 1u));
}

/**
 * @brief Whether @p size is an exact power of two. 0 is not one.
 * @param[in] size Any value.
 */
static inline bool arnm_is_power_of_two(uint32_t size) {
  return size && (size & (size - 1u)) == 0u;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_BIT_H

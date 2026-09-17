#include "bitmap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

static inline uint32_t pow2_u32(uint8_t exponent) {
    return (uint32_t)1u << exponent;
}

// scale a value with power of 2, value * 2^exponent
static inline uint32_t mul_pow2_u32(size_t value, uint8_t exponent) {
    return (uint32_t)(value << exponent);
}

static inline uint16_t pow2_u16(uint8_t exponent) {
    return (uint16_t)(1u << exponent);
}

/**
 * Returns the smallest power of two >= value.
 *
 * For example: 13 -> 16, 16 -> 16, 17 -> 32.
 *
 * @p value must be > 0 and <= 2^31.
 */
static inline uint32_t ceil_power_of_two(uint32_t value) {
  value--;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  return value + 1u;
}

/**
 * Returns the exponent of the smallest power of two >= value.
 *
 * For example: 13 -> 4, because 16 is the smallest power of two >= 13
 * and 16 = 2^4.
 *
 * @p value must be > 0 and <= 2^31.
 */
static inline uint8_t log2_power_of_two(uint32_t value) {
  return (uint8_t)arnm_ctz(ceil_power_of_two(value));
}

/**
 * Returns whether @p size is an exact power of two.
 *
 * @p size must be > 0.
 */
static inline bool is_power_of_two(uint32_t size) {
  return size && (size & (size - 1u)) == 0u;
}

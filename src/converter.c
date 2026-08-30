#include "arnm/converter.h"
#include "arnm/memory.h"
#include "arnm/result.h"
#include <stdbool.h>
#include <stdint.h>

#include <assert.h>
#include <string.h>

/**
 * @brief Compute the number of decimal digits of a uint64_t value.
 *
 * This function returns the length of the decimal representation of `v`
 * without converting it to a string.
 *
 * Implementation details:
 * - Uses a fully unrolled decision tree of comparisons against powers of 10.
 * - Avoids loops, divisions, and memory lookups.
 * - Runs in O(1) time with a small, fixed number of branches.
 *
 * Rationale:
 * - A naive implementation (e.g., repeated division by 10 or scanning a powers[] array)
 *   introduces loops, branch dependencies, and potential cache access.
 * - This version minimizes branch depth and allows the CPU branch predictor
 *   to perform efficiently, making it faster in hot paths.
 *
 * Range:
 * - Supports full uint64_t range [0, UINT64_MAX].
 * - Maximum return value is 20: UINT64_MAX is 18446744073709551615, twenty digits wide.
 *
 * Notes:
 * - The structure may look unusual, but it is intentionally optimized for performance.
 * - Any modification should preserve the exact boundary conditions (powers of 10),
 *   otherwise subtle off-by-one errors may occur.
 */
uint8_t arnm_uint64_to_string_size(uint64_t value) {
  if (value < 100000000ULL) {
    if (value < 10000ULL) {
      if (value < 100ULL) return value < 10 ? 1 : 2;
      return value < 1000ULL ? 3 : 4;
    }
    if (value < 1000000ULL) { return value < 100000ULL ? 5 : 6; }
    return value < 10000000ULL ? 7 : 8;
  }

  if (value < 1000000000000ULL) {
    if (value < 10000000000ULL) { return value < 1000000000ULL ? 9 : 10; }
    return value < 100000000000ULL ? 11 : 12;
  }

  if (value < 10000000000000000ULL) {
    if (value < 100000000000000ULL) { return value < 10000000000000ULL ? 13 : 14; }
    return value < 1000000000000000ULL ? 15 : 16;
  }

  if (value < 100000000000000000ULL) { return 17; }
  if (value < 1000000000000000000ULL) { return 18; }
  // 2^64 - 1 is twenty digits wide, so the ladder has to reach twenty. Stopping at nineteen
  // does not merely misreport the length: the writer fills the buffer from the back and would
  // leave its first digit unwritten.
  return value < 10000000000000000000ULL ? 19 : 20;
}

/*
 * |v| as an unsigned value. `v * -1` is undefined for INT64_MIN, because +2^63 has no int64_t
 * to live in -- the two's complement range is asymmetric. Negating after the conversion works
 * for every input: the conversion of a negative value to uint64_t is defined as v + 2^64, and
 * unsigned subtraction wraps by definition, so the result is exactly |v|.
 */
static inline uint64_t int64_to_abs_u64(int64_t v) {
  return v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
}

uint8_t arnm_int64_to_string_size(int64_t v) {
  uint8_t str_size = arnm_uint64_to_string_size(int64_to_abs_u64(v));
  if (v < 0) {
    str_size++; // add one place for minus in front of number string
  }
  return str_size;
}

uint8_t arnm_uint64_to_string_known_string_size(char *buffer, uint64_t value, uint8_t stringSize) {
  if (value == 0) {
    if (stringSize < 1) {
      return 1; // return required size without null terminator
    }
    buffer[0] = '0';
    buffer[1] = '\0';
    return 1;
  }
  uint64_t temp = value;
  int cursor = stringSize;
  buffer[cursor] = '\0';

  static const char DIGIT_TABLE[201] = "00010203040506070809"
                                       "10111213141516171819"
                                       "20212223242526272829"
                                       "30313233343536373839"
                                       "40414243444546474849"
                                       "50515253545556575859"
                                       "60616263646566676869"
                                       "70717273747576777879"
                                       "80818283848586878889"
                                       "90919293949596979899";

  // process 2 digits at a time
  while (temp >= 100) {
    if (cursor < 2) {
      return arnm_uint64_to_string_size(value); // return required size without null terminator
    }
    uint64_t q = temp / 100;
    uint64_t r = temp - q * 100;
    buffer[--cursor] = DIGIT_TABLE[r * 2 + 1];
    buffer[--cursor] = DIGIT_TABLE[r * 2];
    temp = q;
  }

  // last 1 or 2 digits
  if (temp < 10) {
    if (cursor < 1) {
      return arnm_uint64_to_string_size(value); // return required size without null terminator
    }
    buffer[--cursor] = '0' + (char)temp;
  } else {
    if (cursor < 2) {
      return arnm_uint64_to_string_size(value); // return required size without null terminator
    }
    buffer[--cursor] = DIGIT_TABLE[temp * 2 + 1];
    buffer[--cursor] = DIGIT_TABLE[temp * 2];
  }
  return stringSize; // return number of characters written, not counting null terminator
}

uint8_t arnm_int64_to_string_known_string_size(char *buffer, int64_t value, uint8_t stringSize) {
  if (value >= 0) {
    return arnm_uint64_to_string_known_string_size(buffer, (uint64_t)value, stringSize);
  } else {
    buffer[0] = '-';
    return arnm_uint64_to_string_known_string_size(
               &buffer[1], int64_to_abs_u64(value), stringSize - 1
           ) +
           1;
  }
}
// for easy use, one call

uint8_t arnm_uint64_to_string(char *buffer, uint8_t bufferSize, uint64_t value) {
  uint8_t requiredSize = arnm_uint64_to_string_size(value);
  if (bufferSize < requiredSize + 1) {
    // better safe then sorry
    if (bufferSize) { buffer[0] = '\0'; }
    return requiredSize; // return required size without null terminator
  }
  return arnm_uint64_to_string_known_string_size(buffer, value, requiredSize);
}

uint8_t arnm_int64_to_string(char *buffer, uint8_t bufferSize, int64_t value) {
  uint8_t requiredSize = arnm_int64_to_string_size(value);
  if (bufferSize < requiredSize + 1) {
    // better safe then sorry
    if (bufferSize) { buffer[0] = '\0'; }
    return requiredSize; // return required size without null terminator
  }
  return arnm_int64_to_string_known_string_size(buffer, value, requiredSize);
}

/*
 * Bytes to text and back.
 *
 * Both directions compute their digits rather than looking them up. A table would be a gather
 * no vectoriser can follow, while a comparison and an add per nibble lets the compiler fold the
 * conditional into a select and run the loop a vector register at a time. Neither is constant
 * time -- the scalar remainder beside the vector body branches on the nibble, and an
 * unoptimised build has no vector body at all -- so neither belongs on secret material. See the
 * warning on the group in converter.h.
 */

arnm_result arnm_binary_to_hex(char *result_buffer, const arnm_memory_block *data) {
  if (!result_buffer || !data || !data->data) { return ARNM_ERROR_NULL_POINTER; }
  // an empty block is a parameter the caller can fix, not a pointer they forgot
  if (!data->size) { return ARNM_ERROR_INVALID_PARAM; }

  // Staying in uint8_t is what lets the vectoriser in: the same expression written over int
  // costs a sign extension per element and loses it.
  // Read out of the block before the loop: result_buffer is a char pointer, which is allowed to
  // alias anything, so a store through it forces the compiler to assume data->size and
  // data->data may have changed. Reloading them every iteration is what stops it vectorising.
  const uint8_t *bytes = data->data;
  const size_t count = data->size;

  for (size_t i = 0; i < count; ++i) {
    uint8_t high = (uint8_t)(bytes[i] >> 4);
    uint8_t low = (uint8_t)(bytes[i] & 0x0F);
    result_buffer[i * 2] = (char)(uint8_t)(high + (high < 10 ? 48 : 87));
    result_buffer[i * 2 + 1] = (char)(uint8_t)(low + (low < 10 ? 48 : 87));
  }
  result_buffer[count * 2] = '\0';
  return ARNM_SUCCESS;
}

/**
 * @brief The standard alphabet, indexed by the six bit group it encodes.
 *
 * A table and not the arithmetic, which was tried and measured and is the slower of the two.
 * Written down because the arithmetic is the obvious idea and the reasoning for it is sound
 * right up to the point where someone runs it:
 *
 * The five runs of the alphabet are not contiguous, so computing a character is four compares
 * walking an offset -- and a compare chain is something a vectoriser can turn into blends,
 * where a table load is where auto vectorisation stops. That argument predicts the arithmetic
 * wins. It does not. Swapping only this function, same loop, same build:
 *
 *   CMake Release, one buffer converted over and over
 *     64 bytes    33 ns table   61 ns arithmetic
 *     512 bytes  241 ns table  479 ns arithmetic
 *     4096 bytes 1.9 us table  5.4 us arithmetic
 *
 *   zig ReleaseFast, payloads cycled past the size of L1
 *     1024 bytes 694 ns table  684 ns arithmetic
 *
 * So: two times slower where it is measured hot, and level where the loop is waiting on the
 * payload anyway. The table is 64 bytes and stays in L1 across any loop that encodes more than
 * one block, which is why removing it buys a caller less than it looks like it should.
 *
 * @see arnm_binary_from_base64(), whose table survived the same experiment by a wider margin.
 */
static const char BASE64_ALPHABET[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief The way back, indexed by the character: its six bit value, or 0xFF for anything else.
 *
 * Written out rather than searched, so a character is one load and one compare no matter where
 * in the alphabet it sits, and everything outside answers with a value no group can hold.
 */
static const uint8_t BASE64_VALUE[256] = {
    /* 0x00 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0x10 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0x20 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    62,
    255,
    255,
    255,
    63,
    /* 0x30 */ 52,
    53,
    54,
    55,
    56,
    57,
    58,
    59,
    60,
    61,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0x40 */ 255,
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    /* 0x50 */ 15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    255,
    255,
    255,
    255,
    255,
    /* 0x60 */ 255,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    40,
    /* 0x70 */ 41,
    42,
    43,
    44,
    45,
    46,
    47,
    48,
    49,
    50,
    51,
    255,
    255,
    255,
    255,
    255,
    /* 0x80 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0x90 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0xA0 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0xB0 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0xC0 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0xD0 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0xE0 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    /* 0xF0 */ 255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255,
    255
};

/** @brief What BASE64_VALUE answers for a character the alphabet does not have. */
#define BASE64_NO_VALUE 255u

arnm_result arnm_binary_to_base64(char *result_buffer, const arnm_memory_block *data) {
  if (!result_buffer || !data || !data->data) { return ARNM_ERROR_NULL_POINTER; }
  if (!data->size) { return ARNM_ERROR_INVALID_PARAM; }

  const uint8_t *bytes = data->data;
  const uint32_t count = data->size;
  const uint32_t groups = count / 3u;

  // Driven by the group index rather than by two indices walking at 3 and at 4. It reads no
  // worse and it is the form the arithmetic variant above needed to be halfway competitive --
  // with induction variables that one measured 1.7 us against 0.68 here. The table does not
  // care either way, but the shape is kept so a future attempt at the arithmetic starts from
  // the loop that gives it its best showing rather than its worst.
  for (uint32_t k = 0; k < groups; ++k) {
    const uint8_t *in = bytes + (size_t)k * 3u;
    const uint32_t group = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | (uint32_t)in[2];
    char *out = result_buffer + (size_t)k * 4u;
    out[0] = BASE64_ALPHABET[(group >> 18) & 0x3Fu];
    out[1] = BASE64_ALPHABET[(group >> 12) & 0x3Fu];
    out[2] = BASE64_ALPHABET[(group >> 6) & 0x3Fu];
    out[3] = BASE64_ALPHABET[group & 0x3Fu];
  }

  uint32_t written = groups * 4u;
  const uint32_t rest = count - groups * 3u;
  if (rest) {
    // the missing bytes are read as zeros, which is what makes the last characters land on the
    // same bit boundaries the whole groups use
    const uint8_t *in = bytes + (size_t)groups * 3u;
    const uint32_t group = ((uint32_t)in[0] << 16) | (rest > 1u ? ((uint32_t)in[1] << 8) : 0u);
    result_buffer[written] = BASE64_ALPHABET[(group >> 18) & 0x3Fu];
    result_buffer[written + 1u] = BASE64_ALPHABET[(group >> 12) & 0x3Fu];
    result_buffer[written + 2u] = (rest > 1u) ? BASE64_ALPHABET[(group >> 6) & 0x3Fu] : '=';
    result_buffer[written + 3u] = '=';
    written += 4u;
  }

  result_buffer[written] = '\0';
  return ARNM_SUCCESS;
}

/** @brief What the padding at the end of @p base64 takes off the byte count. */
static size_t base64_padding(const char *base64, size_t length) {
  if ('=' != base64[length - 1u]) { return 0; }
  return ('=' == base64[length - 2u]) ? 2u : 1u;
}

/*
 * Kept out of line, which is a performance decision and not a style one.
 *
 * Two entry points share this loop, and letting the compiler inline it into both is what the
 * obvious reading of "static helper" gets. Measured on bench_binaryToString, that reading costs:
 * inlined twice, the hot loop comes out with one more load per group hoisted into the path every
 * group takes, and the 1024 byte row moves from 580 ns to 650. One copy, called twice, measures
 * where the loop measured before it was shared -- 573 ns -- and the call it adds does not show
 * even on the 16 byte row, where a call is the largest share of the work there is.
 *
 * The attribute is a hint and every compiler that does not have it is still correct, only
 * possibly back to the inlined figure.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ARNM_CONVERTER_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define ARNM_CONVERTER_NOINLINE __declspec(noinline)
#else
#define ARNM_CONVERTER_NOINLINE
#endif

/**
 * @brief The loop both decoders are, with the two buffers named apart.
 *
 * @p result_buffer may be @p base64 itself, which is what makes the in place decode this same
 * loop rather than a second one: a group is read whole into @c group before any of its bytes is
 * written, and the three bytes of group `i` land at `3i` while its characters sat at `4i`, so
 * every write goes behind the read that is already done. Nothing is read after it is written,
 * at any length, and the padding check reads no further than the group it is in.
 *
 * The two pointers are deliberately not `restrict`. They are character types, so a compiler may
 * not assume they are apart anyway, and saying it here would be the one place the in place call
 * turns into undefined behaviour rather than the intended one.
 *
 * @param[out] result_buffer Where the bytes go; @p binary_size of them, or zeros on a refusal.
 * @param[in]  base64        The characters; @p length of them, a multiple of four, not empty.
 * @param[in]  length        Characters to read.
 * @param[in]  binary_size   Bytes the padding says those characters carry.
 * @retval ARNM_SUCCESS             @p binary_size bytes written.
 * @retval ARNM_ERROR_DECODE_FAILED A character outside the alphabet, or padding where none
 *                                  belongs. The output is zeroed, which for the in place call
 *                                  is the front of the string it was handed.
 */
ARNM_CONVERTER_NOINLINE static arnm_result base64_decode_into(
    uint8_t *result_buffer, const char *base64, size_t length, size_t binary_size
) {
  size_t out = 0;
  for (size_t i = 0; i < length; i += 4u) {
    uint32_t group = 0;
    for (size_t k = 0; k < 4u; ++k) {
      const uint8_t value = BASE64_VALUE[(unsigned char)base64[i + k]];
      if (BASE64_NO_VALUE == value) {
        // only the last group may be padded, and only in its last two places
        const bool is_pad = '=' == base64[i + k] && i + 4u == length && k >= 2u &&
                            (k == 3u || '=' == base64[i + 3u]);
        if (!is_pad) {
          memset(result_buffer, 0, binary_size);
          return ARNM_ERROR_DECODE_FAILED;
        }
        continue; // the six bits it would have carried stay zero
      }
      group |= (uint32_t)value << (18u - 6u * k);
    }
    // written through the size rather than through the group count, so the bytes the padding
    // stands for are never stored
    if (out < binary_size) { result_buffer[out++] = (uint8_t)((group >> 16) & 0xFFu); }
    if (out < binary_size) { result_buffer[out++] = (uint8_t)((group >> 8) & 0xFFu); }
    if (out < binary_size) { result_buffer[out++] = (uint8_t)(group & 0xFFu); }
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_binary_from_base64(
    uint8_t *result_buffer, uint32_t *out_size, const char *base64
) {
  if (!result_buffer || !out_size || !base64) { return ARNM_ERROR_NULL_POINTER; }

  const size_t length = strlen(base64);
  if (0 == length) {
    *out_size = 0;
    return ARNM_SUCCESS;
  }
  // four characters make three bytes, so a length that is not a multiple of four cannot be
  // base64 -- refused before anything is written, so the buffer is left as the caller had it
  if (length % 4u) { return ARNM_ERROR_INVALID_PARAM; }

  // how much the padding takes off, read before the loop so the output size is known up front
  const size_t binary_size = (length / 4u) * 3u - base64_padding(base64, length);

  const arnm_result result = base64_decode_into(result_buffer, base64, length, binary_size);
  if (ARNM_SUCCESS != result) { return result; }

  *out_size = (uint32_t)binary_size;
  return ARNM_SUCCESS;
}

arnm_result arnm_binary_from_base64_insitu(char *base64, uint32_t length, uint32_t *out_size) {
  if (!base64 || !out_size) { return ARNM_ERROR_NULL_POINTER; }

  if (0 == length) {
    *out_size = 0;
    return ARNM_SUCCESS;
  }
  if (length % 4u) { return ARNM_ERROR_INVALID_PARAM; }

  const size_t binary_size = ((size_t)length / 4u) * 3u - base64_padding(base64, length);

  // the same buffer as both arguments; see base64_decode_into() for why the loop survives that
  const arnm_result result =
      base64_decode_into((uint8_t *)base64, base64, (size_t)length, binary_size);
  if (ARNM_SUCCESS != result) { return result; }

  *out_size = (uint32_t)binary_size;
  return ARNM_SUCCESS;
}

arnm_result arnm_base64_binary_size(const char *base64, uint32_t length, uint32_t *out_size) {
  if (!base64 || !out_size) { return ARNM_ERROR_NULL_POINTER; }
  // four characters make three bytes, so anything else was never base64
  if (length % 4u) { return ARNM_ERROR_DECODE_FAILED; }
  if (0 == length) {
    *out_size = 0;
    return ARNM_SUCCESS;
  }

  uint32_t padding = 0;
  if ('=' == base64[length - 1u]) { padding = ('=' == base64[length - 2u]) ? 2u : 1u; }
  *out_size = ARNM_BASE64_BINARY_SIZE(length) - padding;
  return ARNM_SUCCESS;
}

arnm_result arnm_binary_from_hex_with_known_hex_size(
    uint8_t *result_buffer, const char *hex, size_t hex_size
) {
  if (!result_buffer || !hex) return ARNM_ERROR_NULL_POINTER;
  if (!hex_size) return ARNM_ERROR_INVALID_PARAM;
  size_t bin_size = hex_size / 2;
  // two characters make one byte, so an odd length cannot be hex -- the division above dropped
  // the stray character and multiplying back reveals it
  if (bin_size * 2 != hex_size) { return ARNM_ERROR_INVALID_PARAM; }

  // Same reasoning as the encoding direction, with the validity test folded in. Clearing bit 5
  // maps a lower case letter onto its upper case twin, so one range check covers both; a digit
  // is its own range. The nibble itself falls out of (c & 0xF) + 9 * (c >> 6), since bit 6 is
  // set for letters and clear for digits. Invalid characters produce a value here as well --
  // that is what makes the loop branchless -- and the verdict below throws it away.
  uint8_t invalid = 0;
  for (size_t i = 0; i < bin_size; ++i) {
    uint8_t high_char = (uint8_t)hex[i * 2];
    uint8_t low_char = (uint8_t)hex[i * 2 + 1];
    uint8_t high_letter = (uint8_t)(high_char & 0xDF);
    uint8_t low_letter = (uint8_t)(low_char & 0xDF);

    invalid |= (uint8_t)(1u - (unsigned)(((high_char >= '0') & (high_char <= '9')) |
                                         ((high_letter >= 'A') & (high_letter <= 'F'))));
    invalid |= (uint8_t)(1u - (unsigned)(((low_char >= '0') & (low_char <= '9')) |
                                         ((low_letter >= 'A') & (low_letter <= 'F'))));

    uint8_t high = (uint8_t)((high_char & 0x0F) + 9u * (unsigned)(high_char >> 6));
    uint8_t low = (uint8_t)((low_char & 0x0F) + 9u * (unsigned)(low_char >> 6));
    result_buffer[i] = (uint8_t)((high << 4) | low);
  }

  // Half converted bytes are worth less than nothing to a caller who overlooks the result code,
  // so the failure path clears them. It costs nothing where it matters: this runs only when the
  // string was already rejected.
  if (invalid) {
    memset(result_buffer, 0, bin_size);
    return ARNM_ERROR_DECODE_FAILED;
  }
  return ARNM_SUCCESS;
}

/*
 * A uuid in its canonical 8-4-4-4-12 form.
 *
 * Both directions go through the hex pair above and do nothing of their own but move the
 * separators. That used to be three lookup tables here -- 512 bytes to write a byte pair, 256
 * to read one and 16 for the scattered positions -- on the reasoning that a run broken by four
 * dashes is not a run a vectoriser can help with.
 *
 * The dashes turn out to be the wrong thing to look at: the sixteen bytes are contiguous and
 * only their text is not, so hexing them whole and then placing five stretches of it costs less
 * than converting them one at a time ever did. Measured over bench_binaryToString, both builds
 * agreeing: writing a uuid went from 6.0 ns to 3.7 in zig ReleaseFast and from 6.8 to 3.7 under
 * CMake, and reading one came out level either way. What the tables were buying was nothing,
 * and their 784 bytes of L1 are a caller's again.
 *
 * Neither direction is constant time -- the same warning on the group in converter.h covers
 * them, and now covers them for the same reason it covers the hex pair itself.
 */

static_assert(ARNM_UUID_BINARY_SIZE == 16, "uuid binary size does not match 16 bytes");
static_assert(
    ARNM_UUID_STRING_LENGTH == 36, "the 8-4-4-4-12 form is 36 characters, terminator aside"
);

arnm_result arnm_uuid_from_string(uint8_t *uuid, const char *uuid_string) {
  if (!uuid || !uuid_string) { return ARNM_ERROR_NULL_POINTER; }
  if (strlen(uuid_string) != ARNM_UUID_STRING_LENGTH) { return ARNM_ERROR_INVALID_PARAM; }
  if (uuid_string[8] != '-' || uuid_string[13] != '-' || uuid_string[18] != '-' ||
      uuid_string[23] != '-') {
    memset(uuid, 0, ARNM_UUID_BINARY_SIZE);
    return ARNM_ERROR_DECODE_FAILED;
  }
  char hex[ARNM_UUID_BINARY_SIZE * 2u + 1u];
  memcpy(hex, uuid_string, 8u);
  memcpy(hex + 8, uuid_string + 9, 4u);
  memcpy(hex + 12, uuid_string + 14, 4u);
  memcpy(hex + 16, uuid_string + 19, 4u);
  memcpy(hex + 20, uuid_string + 24, 12u);
  hex[ARNM_UUID_BINARY_SIZE * 2u] = '\0';
  return arnm_binary_from_hex(uuid, hex);
}

void arnm_uuid_to_string(char *result_buffer, const uint8_t uuid[ARNM_UUID_BINARY_SIZE]) {
  char hex[ARNM_UUID_BINARY_SIZE * 2u + 1u];
  const arnm_memory_block block = {
      (uint8_t *)(uintptr_t)(const void *)uuid, (uint32_t)ARNM_UUID_BINARY_SIZE
  };
  (void)arnm_binary_to_hex(hex, &block);
  memcpy(result_buffer, hex, 8u);
  result_buffer[8] = '-';
  memcpy(result_buffer + 9, hex + 8, 4u);
  result_buffer[13] = '-';
  memcpy(result_buffer + 14, hex + 12, 4u);
  result_buffer[18] = '-';
  memcpy(result_buffer + 19, hex + 16, 4u);
  result_buffer[23] = '-';
  memcpy(result_buffer + 24, hex + 20, 12u);
  result_buffer[ARNM_UUID_STRING_LENGTH] = '\0';
}

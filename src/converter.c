#include "arnm/converter.h"
#include "arnm/memory.h"
#include "arnm/result.h"
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
 * - Maximum return value is 19 (since UINT64_MAX < 10^20).
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

  return value < 100000000000000000ULL ? 17 : (value < 1000000000000000000ULL ? 18 : 19);
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
  int len = stringSize;
  int cursor = len;
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
  return len; // return number of characters written, not counting null terminator
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
  size_t requiredSize = arnm_int64_to_string_size(value);
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

arnm_result arnm_binary_from_hex(uint8_t *result_buffer, const char *hex) {
  if (!result_buffer || !hex) { return ARNM_ERROR_NULL_POINTER; }
  size_t hex_size = strlen(hex);
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
 * Sixteen bytes at fixed, scattered positions: not a run a vectoriser can help with, so both
 * directions read lookup tables where the bulk conversions above compute their digits. Neither
 * is constant time either -- the same warning on the group in converter.h covers them.
 */

static_assert(ARNM_UUID_BINARY_SIZE == 16, "uuid binary size does not match 16 bytes");
static_assert(
    ARNM_UUID_STRING_LENGTH == 36, "the 8-4-4-4-12 form is 36 characters, terminator aside"
);

static const uint8_t UUID_HEX_VALUE[256] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* Both characters of every byte value, so one two byte copy per byte replaces a pair of nibble
   lookups and lands them at their final position in one go. */
static const char UUID_HEX_PAIR[256][2] = {
    {'0', '0'}, {'0', '1'}, {'0', '2'}, {'0', '3'}, {'0', '4'}, {'0', '5'}, {'0', '6'}, {'0', '7'},
    {'0', '8'}, {'0', '9'}, {'0', 'a'}, {'0', 'b'}, {'0', 'c'}, {'0', 'd'}, {'0', 'e'}, {'0', 'f'},
    {'1', '0'}, {'1', '1'}, {'1', '2'}, {'1', '3'}, {'1', '4'}, {'1', '5'}, {'1', '6'}, {'1', '7'},
    {'1', '8'}, {'1', '9'}, {'1', 'a'}, {'1', 'b'}, {'1', 'c'}, {'1', 'd'}, {'1', 'e'}, {'1', 'f'},
    {'2', '0'}, {'2', '1'}, {'2', '2'}, {'2', '3'}, {'2', '4'}, {'2', '5'}, {'2', '6'}, {'2', '7'},
    {'2', '8'}, {'2', '9'}, {'2', 'a'}, {'2', 'b'}, {'2', 'c'}, {'2', 'd'}, {'2', 'e'}, {'2', 'f'},
    {'3', '0'}, {'3', '1'}, {'3', '2'}, {'3', '3'}, {'3', '4'}, {'3', '5'}, {'3', '6'}, {'3', '7'},
    {'3', '8'}, {'3', '9'}, {'3', 'a'}, {'3', 'b'}, {'3', 'c'}, {'3', 'd'}, {'3', 'e'}, {'3', 'f'},
    {'4', '0'}, {'4', '1'}, {'4', '2'}, {'4', '3'}, {'4', '4'}, {'4', '5'}, {'4', '6'}, {'4', '7'},
    {'4', '8'}, {'4', '9'}, {'4', 'a'}, {'4', 'b'}, {'4', 'c'}, {'4', 'd'}, {'4', 'e'}, {'4', 'f'},
    {'5', '0'}, {'5', '1'}, {'5', '2'}, {'5', '3'}, {'5', '4'}, {'5', '5'}, {'5', '6'}, {'5', '7'},
    {'5', '8'}, {'5', '9'}, {'5', 'a'}, {'5', 'b'}, {'5', 'c'}, {'5', 'd'}, {'5', 'e'}, {'5', 'f'},
    {'6', '0'}, {'6', '1'}, {'6', '2'}, {'6', '3'}, {'6', '4'}, {'6', '5'}, {'6', '6'}, {'6', '7'},
    {'6', '8'}, {'6', '9'}, {'6', 'a'}, {'6', 'b'}, {'6', 'c'}, {'6', 'd'}, {'6', 'e'}, {'6', 'f'},
    {'7', '0'}, {'7', '1'}, {'7', '2'}, {'7', '3'}, {'7', '4'}, {'7', '5'}, {'7', '6'}, {'7', '7'},
    {'7', '8'}, {'7', '9'}, {'7', 'a'}, {'7', 'b'}, {'7', 'c'}, {'7', 'd'}, {'7', 'e'}, {'7', 'f'},
    {'8', '0'}, {'8', '1'}, {'8', '2'}, {'8', '3'}, {'8', '4'}, {'8', '5'}, {'8', '6'}, {'8', '7'},
    {'8', '8'}, {'8', '9'}, {'8', 'a'}, {'8', 'b'}, {'8', 'c'}, {'8', 'd'}, {'8', 'e'}, {'8', 'f'},
    {'9', '0'}, {'9', '1'}, {'9', '2'}, {'9', '3'}, {'9', '4'}, {'9', '5'}, {'9', '6'}, {'9', '7'},
    {'9', '8'}, {'9', '9'}, {'9', 'a'}, {'9', 'b'}, {'9', 'c'}, {'9', 'd'}, {'9', 'e'}, {'9', 'f'},
    {'a', '0'}, {'a', '1'}, {'a', '2'}, {'a', '3'}, {'a', '4'}, {'a', '5'}, {'a', '6'}, {'a', '7'},
    {'a', '8'}, {'a', '9'}, {'a', 'a'}, {'a', 'b'}, {'a', 'c'}, {'a', 'd'}, {'a', 'e'}, {'a', 'f'},
    {'b', '0'}, {'b', '1'}, {'b', '2'}, {'b', '3'}, {'b', '4'}, {'b', '5'}, {'b', '6'}, {'b', '7'},
    {'b', '8'}, {'b', '9'}, {'b', 'a'}, {'b', 'b'}, {'b', 'c'}, {'b', 'd'}, {'b', 'e'}, {'b', 'f'},
    {'c', '0'}, {'c', '1'}, {'c', '2'}, {'c', '3'}, {'c', '4'}, {'c', '5'}, {'c', '6'}, {'c', '7'},
    {'c', '8'}, {'c', '9'}, {'c', 'a'}, {'c', 'b'}, {'c', 'c'}, {'c', 'd'}, {'c', 'e'}, {'c', 'f'},
    {'d', '0'}, {'d', '1'}, {'d', '2'}, {'d', '3'}, {'d', '4'}, {'d', '5'}, {'d', '6'}, {'d', '7'},
    {'d', '8'}, {'d', '9'}, {'d', 'a'}, {'d', 'b'}, {'d', 'c'}, {'d', 'd'}, {'d', 'e'}, {'d', 'f'},
    {'e', '0'}, {'e', '1'}, {'e', '2'}, {'e', '3'}, {'e', '4'}, {'e', '5'}, {'e', '6'}, {'e', '7'},
    {'e', '8'}, {'e', '9'}, {'e', 'a'}, {'e', 'b'}, {'e', 'c'}, {'e', 'd'}, {'e', 'e'}, {'e', 'f'},
    {'f', '0'}, {'f', '1'}, {'f', '2'}, {'f', '3'}, {'f', '4'}, {'f', '5'}, {'f', '6'}, {'f', '7'},
    {'f', '8'}, {'f', '9'}, {'f', 'a'}, {'f', 'b'}, {'f', 'c'}, {'f', 'd'}, {'f', 'e'}, {'f', 'f'},
};

/* Where each byte's first hex character sits in the 8-4-4-4-12 layout; the second follows
   directly after it. The four separators sit at 8, 13, 18 and 23. Driving the loops from this
   table is what removes the per-character branching a discovering parser needs: the format is
   fixed, so the positions never have to be looked for while reading. */
static const uint8_t UUID_HEX_POS[ARNM_UUID_BINARY_SIZE] = {0,  2,  4,  6,  9,  11, 14, 16,
                                                            19, 21, 24, 26, 28, 30, 32, 34};

arnm_result arnm_uuid_from_string(uint8_t *uuid, const char *uuid_string) {
  if (!uuid || !uuid_string) { return ARNM_ERROR_NULL_POINTER; }
  if (strlen(uuid_string) != ARNM_UUID_STRING_LENGTH) { return ARNM_ERROR_INVALID_PARAM; }

  // The separators are checked by position, not merely counted. Skipping any dash wherever it
  // appeared lets a 36 character string carry fewer than four of them, and every missing dash
  // would turn two characters into an extra output byte: an all hex string of the right length
  // then writes 18 bytes into these 16.
  if (uuid_string[8] != '-' || uuid_string[13] != '-' || uuid_string[18] != '-' ||
      uuid_string[23] != '-') {
    memset(uuid, 0, ARNM_UUID_BINARY_SIZE);
    return ARNM_ERROR_DECODE_FAILED;
  }

  // Decoding writes straight into the caller's buffer and the verdict is settled once at the
  // end: a bad digit shows up as 0xFF, whose high nibble survives the OR no matter what else
  // the string held. Nothing branches on the data in between.
  unsigned invalid = 0;
  for (size_t k = 0; k < ARNM_UUID_BINARY_SIZE; ++k) {
    unsigned high = UUID_HEX_VALUE[(unsigned char)uuid_string[UUID_HEX_POS[k]]];
    unsigned low = UUID_HEX_VALUE[(unsigned char)uuid_string[UUID_HEX_POS[k] + 1]];
    invalid |= high | low;
    uuid[k] = (uint8_t)((high << 4) | low);
  }

  // Half decoded bytes are worth less than nothing to a caller who ignores the result code, so
  // the failure path clears them. It costs nothing where it matters: this runs only when the
  // string was already rejected.
  if (invalid & 0xF0u) {
    memset(uuid, 0, ARNM_UUID_BINARY_SIZE);
    return ARNM_ERROR_DECODE_FAILED;
  }
  return ARNM_SUCCESS;
}

void arnm_uuid_to_string(char *result_buffer, const uint8_t uuid[ARNM_UUID_BINARY_SIZE]) {
  // Writes each byte where it belongs immediately. Formatting all 32 characters into a scratch
  // buffer and reassembling them around the separators afterwards walks the result twice for
  // the same output.
  for (size_t k = 0; k < ARNM_UUID_BINARY_SIZE; ++k) {
    memcpy(result_buffer + UUID_HEX_POS[k], UUID_HEX_PAIR[uuid[k]], 2);
  }
  result_buffer[8] = '-';
  result_buffer[13] = '-';
  result_buffer[18] = '-';
  result_buffer[23] = '-';
  result_buffer[ARNM_UUID_STRING_LENGTH] = '\0';
}

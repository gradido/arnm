#include "arnm/duration.h"

#include <stdint.h>
#include <string.h>

#include "arnm/converter.h"

int arnm_duration_string(
    char *buffer, size_t buffer_size, arnm_duration duration, uint8_t precision
) {
  uint64_t ns = (uint64_t)duration;

  uint64_t divisor;
  const char *suffix;

  if (duration < 0) {
    return -1; // negative durations are not supported
  }
  if (precision > 15) {
    precision = 15; // limit precision to 15 to avoid overflow in fractional part
  }

  // --- unit selection (branch tree, kein array) ---
  if (ns < 1000ULL) {
    divisor = 1ULL;
    suffix = " ns";
  } else if (ns < 1000000ULL) {
    divisor = 1000ULL;
    suffix = " us";
  } else if (ns < 1000000000ULL) {
    divisor = 1000000ULL;
    suffix = " ms";
  } else if (ns < 60000000000ULL) {
    divisor = 1000000000ULL;
    suffix = " s";
  } else if (ns < 3600000000000ULL) {
    divisor = 60ULL * 1000000000ULL;
    suffix = " min";
  } else if (ns < 86400000000000ULL) {
    divisor = 60ULL * 60ULL * 1000000000ULL;
    suffix = " h";
  } else {
    divisor = 24ULL * 60ULL * 60ULL * 1000000000ULL;
    suffix = " days";
  }

  double decimalValue = (double)ns / (double)divisor;
  int64_t integerPart = (int64_t)decimalValue;
  int64_t fractionalPart = (int64_t)((decimalValue - (double)integerPart) * 1000000000000000ULL);

  uint8_t int_size = arnm_int64_to_string_size(integerPart);
  uint8_t suffix_len = (uint8_t)strlen(suffix);
  // +2 for the '.' and the terminator, +precision for the fractional part. A '-' is not among
  // them: arnm_int64_to_string_size() counts the sign inside int_size already. Widened to
  // size_t before the comparison: every term is a uint8_t and promotes to int, which
  // buffer_size would then be compared against across the sign boundary.
  size_t needed = (size_t)int_size + 2u + precision + suffix_len;
  if (buffer_size < needed) {
    return int_size + 1 + precision + suffix_len; // return required size without null terminator
  }

  int written = arnm_int64_to_string_known_string_size(buffer, integerPart, int_size);
  // --- fractional part ---
  if (precision > 0 && divisor > 1) {
    buffer[written++] = '.';
    uint8_t fractionalPartSize = 0;
    if (fractionalPart) { fractionalPartSize = arnm_int64_to_string_size(fractionalPart); }
    // 15 = max fractional part size (15 zeros near the double boundary)
    uint8_t zerosBeforeCount = 15 - fractionalPartSize;
    if (zerosBeforeCount > precision) { zerosBeforeCount = precision; }
    if (zerosBeforeCount > 0) {
      memset(buffer + written, '0', zerosBeforeCount);
      written += zerosBeforeCount;
    }
    uint8_t restNumbers = precision - zerosBeforeCount;
    if (restNumbers) {
      char tempBuffer[20]; // enough to hold fractional part
      arnm_int64_to_string_known_string_size(tempBuffer, fractionalPart, fractionalPartSize);
      memcpy(buffer + written, tempBuffer, restNumbers);
      written += restNumbers;
    }
  }

  // --- suffix ---
  memcpy(buffer + written, suffix, suffix_len);
  written += suffix_len;

  buffer[written] = '\0';
  return written;
}

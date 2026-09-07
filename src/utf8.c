#include "arnm/utf8.h"

#include <stdint.h>
#include <string.h>

/*
 * The lead byte says how long the sequence is and, for four of the ranges, narrows what its
 * first continuation may be. Those two narrowings are the whole of what separates this from a
 * decoder that accepts anything decodable:
 *
 *   E0 A0..BF   without the floor, E0 80 80 would be U+0000 written in three bytes
 *   ED 80..9F   without the ceiling, ED A0 80 would be U+D800, half of a surrogate pair
 *   F0 90..BF   the same floor one level up
 *   F4 80..8F   the ceiling at U+10FFFF, which is where Unicode ends
 *
 * C0 and C1 cannot lead anything but an overlong two byte form, and F5 and up cannot lead
 * anything inside Unicode, so both are refused as lead bytes rather than checked later. The code
 * point itself is never assembled: nothing here needs its value, only its shape.
 */

size_t arnm_utf8_valid_length(const char *text, size_t length) {
  if (!text) { return 0; }
  const uint8_t *bytes = (const uint8_t *)text;
  size_t index = 0;

  while (index < length) {
    if (bytes[index] < 0x80u) {
      /*
       * ASCII in machine words. The mask answers "is any of these eight bytes not ASCII" in one
       * test, and a word that fails it is not examined here -- the byte loop below walks up to
       * the offending byte, whichever end of the word the machine put it at. memcpy and not a
       * cast, because a uint64_t read through a char pointer is neither aligned nor allowed;
       * every compiler this library is built with turns it into the single load it is.
       */
      while (length - index >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, bytes + index, sizeof(word));
        if (word & 0x8080808080808080ULL) { break; }
        index += sizeof(uint64_t);
      }
      while (index < length && bytes[index] < 0x80u) { ++index; }
      continue;
    }

    const uint8_t lead = bytes[index];
    size_t continuations;
    uint8_t first_low;
    uint8_t first_high;

    if (lead >= 0xC2u && lead <= 0xDFu) {
      continuations = 1;
      first_low = 0x80u;
      first_high = 0xBFu;
    } else if (lead == 0xE0u) {
      continuations = 2;
      first_low = 0xA0u;
      first_high = 0xBFu;
    } else if (lead == 0xEDu) {
      continuations = 2;
      first_low = 0x80u;
      first_high = 0x9Fu;
    } else if ((lead >= 0xE1u && lead <= 0xECu) || lead == 0xEEu || lead == 0xEFu) {
      continuations = 2;
      first_low = 0x80u;
      first_high = 0xBFu;
    } else if (lead == 0xF0u) {
      continuations = 3;
      first_low = 0x90u;
      first_high = 0xBFu;
    } else if (lead >= 0xF1u && lead <= 0xF3u) {
      continuations = 3;
      first_low = 0x80u;
      first_high = 0xBFu;
    } else if (lead == 0xF4u) {
      continuations = 3;
      first_low = 0x80u;
      first_high = 0x8Fu;
    } else {
      // 0x80..0xC1 and 0xF5..0xFF lead nothing: a stray continuation, an overlong two byte form,
      // or a code point past the end of Unicode
      return index;
    }

    // the sequence has to fit: a run that ends inside one is invalid at the lead, not at the end
    if (length - index <= continuations) { return index; }
    if (bytes[index + 1u] < first_low || bytes[index + 1u] > first_high) { return index; }
    for (size_t step = 2u; step <= continuations; ++step) {
      if ((bytes[index + step] & 0xC0u) != 0x80u) { return index; }
    }
    index += continuations + 1u;
  }

  return length;
}

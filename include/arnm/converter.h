#ifndef ARNM_CONVERTER_H
#define ARNM_CONVERTER_H

#include "arnm/memory_block.h"
#include "arnm/result.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_converter arnm_converter
 * @brief Numbers and raw bytes rendered as text, and read back.
 *
 * Two families live here. The first turns a uint64_t or an int64_t into decimal digits with the
 * LR-algorithm, and measures how many places that will take before a single one is written. The
 * second carries bytes across to lowercase hex and back, whole blocks of them or a uuid in its
 * canonical 8-4-4-4-12 form. Both are built for hot paths: no allocation, no format string
 * parsing, every destination sized by the caller.
 *
 * @warning Neither the hex pair nor the base64 pair runs in constant time, so none of them
 * belongs on secret material. The base64 pair reads a lookup table both ways, which is a measured
 * choice and not a shortcut: computing the characters instead was tried in both directions and
 * came out slower in both -- twice over for the encoder, and worse for the decoder, which has
 * to answer "is this base64 at all" once per character. `converter.c` carries the figures at
 * each table. libsodium's pair is the constant time one and is around eight times slower for
 * it. Payloads that are already encrypted or already public are
 * what this is for, and for those the timing carries nothing. That is not a matter of how it is
 * written: arnm_binary_to_hex() computes each digit instead of looking it up, and in an optimised
 * build its vectorised body really is branchless -- but the scalar path beside it, which takes the
 * remainder and takes short inputs whole, compiles to a compare and a jump on the nibble. Rewriting
 * the conditional as an arithmetic mask does not move it; the compiler turns that back into a
 * branch as well, and an unoptimised build has no vector path at all. The base64 pair reads lookup
 * tables on top of that; the uuid pair is this same hex pair with the separators moved, and
 * inherits exactly this. Keys, seeds and passphrases belong in a crypto library's constant time
 * conversion -- arnm links none and offers none. Hashes, transaction ids, public keys, uuids and
 * anything else already public are exactly what these are for.
 * @{
 */

/**
 * @brief Transform a uint64_t into its string representation using the LR-algorithm.
 *
 * The LR-algorithm streams digits efficiently through optimized processing. See:
 * https://medium.com/data-science/34-faster-integer-to-string-conversion-algorithm-c72453d25352
 *
 * @param[out] buffer      Destination buffer receiving the resulting string.
 * @param[in]  bufferSize  Size of buffer (must contain result + null terminator).
 * @param[in]  value       The uint64_t value to transform.
 *
 * @return
 *   Characters written (excluding '\0'). If buffer is too small, returns
 *   the length that would have been needed -- a hint for the caller.
 *
 * @whisper Number becomes word, digit by digit
 */
uint8_t arnm_uint64_to_string(char *buffer, uint8_t bufferSize, uint64_t value);
/**
 * @brief As arnm_uint64_to_string(), for a signed value.
 *
 * A negative number is written with a leading '-', which counts towards the return value and
 * towards the space @p buffer has to have.
 *
 * @param[out] buffer     Destination buffer receiving the resulting string.
 * @param[in]  bufferSize Size of buffer (must contain result + sign + null terminator).
 * @param[in]  value      The int64_t value to transform.
 * @return Characters written (excluding '\0'), or the length that would have been needed when
 *         @p buffer is too small.
 * @whisper The same digits, and the sign that leads them
 */
uint8_t arnm_int64_to_string(char *buffer, uint8_t bufferSize, int64_t value);

/**
 * @brief Convert a uint64_t to string, when its length is already known.
 *
 * Optimized for hot paths: skips redundant size calculation when length flows in
 * from a prior call to arnm_uint64_to_string_size. Reduces overhead in tight loops.
 *
 * @param[out] buffer      Destination buffer, must be at least stringSize + 1 bytes.
 * @param[in]  value       The uint64_t value to transform.
 * @param[in]  stringSize  Pre-calculated string length (from arnm_uint64_to_string_size).
 *
 * @return
 *   Number of characters written (excluding '\0').
 *
 * @note Caller is responsible for ensuring buffer space is adequate.
 *
 * @whisper When size is known, conversion becomes a smooth stride
 */
uint8_t arnm_uint64_to_string_known_string_size(char *buffer, uint64_t value, uint8_t stringSize);
/**
 * @brief As arnm_uint64_to_string_known_string_size(), for a signed value.
 *
 * @param[out] buffer     Destination buffer, at least @p stringSize + 1 bytes.
 * @param[in]  value      The int64_t value to transform.
 * @param[in]  stringSize Pre-calculated length from arnm_int64_to_string_size(), sign included.
 * @return Number of characters written (excluding '\0').
 * @note Caller is responsible for ensuring buffer space is adequate.
 */
uint8_t arnm_int64_to_string_known_string_size(char *buffer, int64_t value, uint8_t stringSize);

/**
 * @brief Measure the length of a uint64_t's string representation.
 *
 * Calculates exactly how many character-places a number will need before
 * conversion. Useful for pre-allocating buffers or coordinating with
 * arnm_uint64_to_string_known_string_size to avoid double-calculation.
 *
 * @param[in] value  The uint64_t value to measure.
 *
 * @return
 *   String length in characters (excluding null terminator).
 *
 * @whisper Know the shape before filling the space
 */
uint8_t arnm_uint64_to_string_size(uint64_t value);
/**
 * @brief As arnm_uint64_to_string_size(), for a signed value.
 *
 * @param[in] value The int64_t value to measure.
 * @return String length in characters (excluding null terminator), counting the '-' of a
 *         negative value.
 */
uint8_t arnm_int64_to_string_size(int64_t value);

/**
 * @brief Write @p data as lowercase hex into a buffer the caller sized.
 *
 * Each byte becomes two characters in order, and a terminator closes the run. Nothing is
 * allocated and nothing is remembered: the bytes flow through and the buffer holds what is
 * left.
 *
 * @param[out] result_buffer Expected to hold data->size * 2 + 1 bytes. Not checkable from
 *                           here -- sizing it is the caller's part of the contract.
 * @param[in]  data          Block to encode; not NULL and not empty.
 * @retval ARNM_SUCCESS             Hex written, terminator included.
 * @retval ARNM_ERROR_NULL_POINTER  @p result_buffer, @p data or its data pointer is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p data holds no bytes.
 * @note Not constant time; see the warning on this group.
 * @whisper Every byte says its name twice, in the same quiet alphabet
 */
arnm_result arnm_binary_to_hex(char *result_buffer, const arnm_memory_block *data);

/**
 * @brief Read a hex string back into the bytes it spells.
 *
 * Both digit cases are accepted. Nothing is skipped: a separator between the bytes makes the
 * string undecodable rather than being ignored.
 *
 * @param[out] result_buffer Expected to hold strlen(hex) / 2 bytes. Those bytes are set to all
 *                           zeros when the string turns out not to be hex, so a caller that
 *                           overlooks the result code never reads half converted bytes. Only
 *                           what this call decoded is cleared; whatever the buffer held before
 *                           belongs to the caller and is left alone.
 * @param[in]  hex           Null terminated string of an even number of hex digits. Empty is
 *                           allowed and writes nothing.
 * @retval ARNM_SUCCESS             strlen(hex) / 2 bytes written.
 * @retval ARNM_ERROR_NULL_POINTER  @p result_buffer or @p hex is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p hex has an odd number of characters. Refused before
 *                                     anything is written, so @p result_buffer is left exactly
 *                                     as the caller had it -- there is nothing of this call's
 *                                     making in it to clear.
 * @retval ARNM_ERROR_DECODE_FAILED @p hex holds a character that is not a hex digit. The
 *                                     strlen(hex) / 2 bytes are zeroed.
 * @note Not constant time; see the warning on this group.
 * @whisper Two characters settle back into the one byte they came from
 */
arnm_result arnm_binary_from_hex(uint8_t *result_buffer, const char *hex);

/**
 * @brief Characters the base64 of @p size bytes takes, terminator not counted.
 *
 * Three bytes become four characters, and a last group of one or two is padded out to four with
 * `=`. The figure is therefore exact and not a bound, which is what lets a caller size a buffer
 * from it and a writer count a field before it exists.
 */
#define ARNM_BASE64_STRING_LENGTH(size) ((((size) + 2u) / 3u) * 4u)

/** @brief Bytes the base64 string of @p length characters can decode to, at most. */
#define ARNM_BASE64_BINARY_SIZE(length) (((length) / 4u) * 3u)

/**
 * @brief Write @p data as base64 into a buffer the caller sized.
 *
 * The standard alphabet -- `A-Z`, `a-z`, `0-9`, `+`, `/` -- padded to a multiple of four with
 * `=`. That is what `atob()` in a browser reads and what every base64 tool means when it says
 * base64 with no further word; the URL safe alphabet is a different one and is not written here.
 *
 * Four characters where hex needs six, so a payload that travels as text costs a third less.
 * Reach for hex instead where a person will compare the value against another tool's output --
 * a key, a hash, a transaction id.
 *
 * @param[out] result_buffer Expected to hold ARNM_BASE64_STRING_LENGTH(data->size) + 1 bytes.
 *                           Not checkable from here -- sizing it is the caller's part of the
 *                           contract.
 * @param[in]  data          Block to encode; not NULL and not empty.
 * @retval ARNM_SUCCESS             Base64 written, terminator included.
 * @retval ARNM_ERROR_NULL_POINTER  @p result_buffer, @p data or its data pointer is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p data holds no bytes.
 * @note Not constant time; see the warning on this group.
 * @whisper Three bytes fold into four letters, and the last group is made whole
 */
arnm_result arnm_binary_to_base64(char *result_buffer, const arnm_memory_block *data);

/**
 * @brief Read a base64 string back into the bytes it spells.
 *
 * The standard alphabet and nothing beside it: whitespace, a newline or a character from the
 * URL safe alphabet makes the string undecodable rather than being skipped. Padding is required
 * and is checked -- a string whose length is not a multiple of four is refused, and so is a `=`
 * anywhere but in the last group.
 *
 * @param[out] result_buffer Expected to hold ARNM_BASE64_BINARY_SIZE(strlen(base64)) bytes.
 *                           Those bytes are set to all zeros when the string turns out not to
 *                           be base64, so a caller that overlooks the result code never reads
 *                           half converted bytes.
 * @param[out] out_size      Receives the bytes actually written, which the padding decides;
 *                           not NULL. Untouched unless the call succeeds.
 * @param[in]  base64        Null terminated string. Empty is allowed and writes nothing.
 * @retval ARNM_SUCCESS             @p out_size bytes written.
 * @retval ARNM_ERROR_NULL_POINTER  @p result_buffer, @p out_size or @p base64 is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p base64 has a length that is not a multiple of four.
 *                                     Refused before anything is written.
 * @retval ARNM_ERROR_DECODE_FAILED @p base64 holds a character the alphabet does not have, or
 *                                     padding where none belongs. The output is zeroed.
 * @note Not constant time; see the warning on this group.
 * @whisper Four letters settle back into the three bytes they came from
 */
arnm_result arnm_binary_from_base64(uint8_t *result_buffer, uint32_t *out_size, const char *base64);

/** @brief Bytes a uuid occupies in binary form. */
#define ARNM_UUID_BINARY_SIZE 16

/** @brief Characters of the canonical 8-4-4-4-12 form, terminator not counted.
 *
 *  A buffer for arnm_uuid_to_string() needs one more than this.
 */
#define ARNM_UUID_STRING_LENGTH 36

/**
 * @brief Parse the canonical 8-4-4-4-12 form into 16 bytes.
 *
 * The layout is fixed, so nothing has to be discovered while reading: each byte is picked up
 * from the position the format assigns it, and the verdict settles once at the end rather than
 * at every digit.
 *
 * @param[out] uuid        Expected to be @ref ARNM_UUID_BINARY_SIZE bytes. Set to all zeros
 *                         on ARNM_ERROR_DECODE_FAILED, so a caller that overlooks the result
 *                         code never reads half decoded bytes. A length that is wrong is caught
 *                         before any of it is written and leaves it as the caller had it.
 * @param[in]  uuid_string Expected to be exactly @ref ARNM_UUID_STRING_LENGTH characters long
 *                         plus its terminator, with the separators at index 8, 13, 18 and 23.
 *                         Both digit cases are accepted.
 * @retval ARNM_SUCCESS             16 bytes written.
 * @retval ARNM_ERROR_NULL_POINTER  @p uuid or @p uuid_string is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p uuid_string is not 36 characters long. Refused before
 *                                     anything is written, so @p uuid is untouched.
 * @retval ARNM_ERROR_DECODE_FAILED A separator is missing or misplaced, or a character where
 *                                     a hex digit belongs is not one. @p uuid is zeroed.
 * @note Not constant time; see the warning on this group.
 * @whisper Thirty-six characters fold back into sixteen bytes
 */
arnm_result arnm_uuid_from_string(uint8_t *uuid, const char *uuid_string);

/**
 * @brief Write 16 bytes as the canonical 8-4-4-4-12 form.
 *
 * Each byte lands at its final position straight away, separators and terminator after it.
 * Nothing is validated: any 16 bytes are a uuid as far as this is concerned, and the version
 * and variant fields are the caller's business.
 *
 * @param[out] result_buffer Expected to hold @ref ARNM_UUID_STRING_LENGTH + 1 bytes,
 *                           terminator included; not NULL.
 * @param[in]  uuid          The 16 bytes to render; not NULL.
 * @note Not constant time; see the warning on this group.
 * @whisper Sixteen bytes settle into the shape the world reads them by
 */
void arnm_uuid_to_string(char *result_buffer, const uint8_t uuid[ARNM_UUID_BINARY_SIZE]);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif // ARNM_CONVERTER_H

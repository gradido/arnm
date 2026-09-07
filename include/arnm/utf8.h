#ifndef ARNM_UTF8_H
#define ARNM_UTF8_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup arnm_utf8 arnm_utf8
 * @brief One question about a run of bytes: is it UTF-8, and if not, where does it stop being it.
 *
 * arnm sits at the bottom of a chain, and the two halves of its JSON pair are not symmetric
 * about this. @ref arnm_json_reader refuses a document whose bytes are not UTF-8; yyjson is
 * built here with its validation on. @ref arnm_json_writer does not, on the path nearly every
 * field takes: a string added the ordinary way is marked as needing no escaping, and a string
 * that needs no escaping is copied without being walked.
 *
 * | | validation off | on |
 * |---|---|---|
 * | ascii, per string read | 17.2 ns | 17.8 ns |
 * | ascii, per string written | 25.2 ns | 25.2 ns |
 * | multi byte, per string read | 14.8 ns | 17.6 ns |
 * | multi byte, per string written | 28.9 ns | 34.9 ns |
 *
 * Measured against yyjson compiled both ways, 64 strings per document. Free on ASCII, about a
 * fifth on anything else -- which is why the reader carries it and why turning the writer's
 * borrowing off to gain it would be the wrong trade.
 *
 * So the writing side is this call's, and it belongs where the untrusted bytes arrive rather
 * than at the write, which fails for the whole document and names no field:
 *
 * @code
 * // at the edge, once per request, before anything else looks at it
 * if (!arnm_utf8_is_valid(body, body_length)) { return reject(request); }
 * @endcode
 *
 * ### Why it matters that the writing side does not check
 *
 * A JSON document holding malformed UTF-8 is not JSON. What happens to it is decided far from
 * here and differently every time: a browser's `JSON.parse` throws, a Postgres `text` column
 * refuses the row, a log shipper drops the line or replaces the bytes, and a strict parser on
 * the far side of a queue rejects a message that was already acknowledged. None of those
 * failures name the field they came from, and the writer that let the bytes through is by then
 * several hops away.
 *
 * The bytes to be careful with are the ones a caller did not produce: a request body, a header,
 * a filename, a database column filled by someone else, anything decoded from base64. A literal
 * in the source, a number formatted by @ref arnm_converter, a uuid or a hex digest cannot fail
 * this and should not be walked.
 *
 * @note This validates. It does not repair, replace or normalize -- see the note on
 *       transformations in the writer: what a caller did not ask for does not happen.
 * @{
 */

/**
 * @brief How many bytes from the front of @p text are well formed UTF-8.
 *
 * Answers both questions a caller has in one number: whether the run is valid, by comparing it
 * against @p length, and where the first bad byte is, by being its offset.
 *
 * What is rejected is what the standard rejects, not merely what decodes to something: an
 * overlong encoding (`C0 80` for NUL), a surrogate half (`ED A0 80`), a code point above
 * U+10FFFF (`F5` and up), a continuation byte with no lead, and a sequence the run ends in the
 * middle of. A NUL byte is none of those and passes -- UTF-8 says nothing about where a string
 * ends.
 *
 * @param[in] text   Bytes to walk. NULL answers 0, which reads as "invalid at 0" for a non
 *                   empty run and as "all of it" for an empty one.
 * @param[in] length Bytes in @p text. 0 answers 0 without reading anything.
 * @return The offset of the first byte that is not part of a well formed sequence, which is
 *         @p length when there is none.
 * @note Runs over ASCII eight bytes at a time and drops to one at a time only where a byte has
 *       its high bit set, so text that is mostly ASCII costs close to nothing: 34 GB/s over
 *       ASCII, 2.7 GB/s over German text, 1.5 GB/s over Japanese. A 4 KiB request body is
 *       therefore about 0.1 us of mostly ASCII and under 3 us if every character of it is
 *       outside the BMP -- against a request that will be parsed, dispatched and answered.
 * @note Scalar, and portable C11. A vectorised validator reaches ten times the multi byte
 *       figure and needs intrinsics per architecture; that is a trade for a caller who
 *       validates megabytes, and this library does not carry it.
 * @whisper Reading the river until the water changes
 */
size_t arnm_utf8_valid_length(const char *text, size_t length);

/**
 * @brief Is @p text well formed UTF-8 from end to end?
 *
 * @ref arnm_utf8_valid_length() asked as the yes or no a call site usually wants. Where the
 * answer is no and the offset would help -- a log line naming the position, a parser reporting
 * a column -- ask the other one; it is the same walk.
 *
 * @param[in] text   Bytes to walk; NULL is valid only for an empty run.
 * @param[in] length Bytes in @p text. 0 is valid: an empty run is well formed.
 * @return true when every byte belongs to a well formed sequence.
 * @whisper The whole river, or the place it turns
 */
static inline bool arnm_utf8_is_valid(const char *text, size_t length) {
  return arnm_utf8_valid_length(text, length) == length;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_UTF8_H

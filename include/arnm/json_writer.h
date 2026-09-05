#ifndef ARNM_JSON_WRITER_H
#define ARNM_JSON_WRITER_H

#include "arnm/memory.h"
#include "arnm/memory_block.h"
#include "arnm/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C11 static assert fallback; in C++ the keyword is already there */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/**
 * @defgroup arnm_json_writer arnm_json_writer
 * @brief A JSON document built one field at a time and written into an arena.
 *
 * The mirror of @ref arnm_json_reader, and the same three habits: fields go in one line each,
 * strings are borrowed rather than copied, and nothing is checked until the end. The serializer
 * underneath is yyjson; nothing of it reaches this header, so a consumer includes
 * `arnm/json_writer.h` and links against arnm alone.
 *
 * ### The shape a mapper takes
 *
 * @code
 * arnm_json_writer writer;
 * arnm_json_writer_init(&writer, scratch, ARNM_JSON_WRITE_DEFAULT, NULL);
 *
 * arnm_json_writer_add_string(&writer, "host", config.host);
 * arnm_json_writer_add_uint64(&writer, "port", config.port);
 * arnm_json_writer_add_bool(&writer, "debug", config.debug);
 *
 * arnm_memory_block text;
 * if (ARNM_SUCCESS == arnm_json_writer_write(&writer, output, &text, NULL)) {
 *   send(text.data);                       // NUL terminated, text.size bytes reserved
 *   arnm_memory_block_free(&text, output);
 * }
 * @endcode
 *
 * No `begin` is needed for the common case: the first field opens a document whose root is an
 * object. @ref arnm_json_writer_begin_array() opens one whose root is an array, and either
 * `begin` starts over, which is how one writer serves one payload after another.
 *
 * Like the reader, the writer keeps the **first** error and the field name it happened at.
 * Every later call after that does nothing at all, so a struct is written without a test
 * between the lines and asked about once -- through @ref arnm_json_writer_status(), or simply
 * by reading what @ref arnm_json_writer_write() answers, since a writer carrying an error
 * refuses to write.
 *
 * ### Nesting, and why it counts its own levels
 *
 * @code
 * arnm_json_writer_open_object(&writer, "address");
 * arnm_json_writer_add_string(&writer, "city", config.city);
 * arnm_json_writer_close(&writer);
 *
 * arnm_json_writer_open_array(&writer, "peers");
 * for (uint32_t i = 0; i < config.peer_count; ++i) {
 *   arnm_json_writer_open_object(&writer, NULL);          // NULL key: an array element
 *   arnm_json_writer_add_string(&writer, "name", config.peers[i].name);
 *   arnm_json_writer_close(&writer);
 * }
 * arnm_json_writer_close(&writer);
 * @endcode
 *
 * The reader hands its caller a value to give back at `leave`, because the tree it walks
 * already exists and holds every way home. A writer builds the tree as it goes and knows its
 * own place in it, so @ref arnm_json_writer_close() needs no argument -- and the levels it
 * keeps are the reason @ref ARNM_JSON_WRITER_MAX_DEPTH is a number rather than a promise.
 *
 * A key of NULL means "an element of the current array", exactly as a NULL key means "the value
 * itself" on the reading side. Naming a key inside an array, or leaving it out inside an
 * object, is @ref ARNM_ERROR_INVALID_PARAM.
 *
 * ### Strings are borrowed
 *
 * `arnm_json_writer_add_string()` keeps the pointer it is given and nothing else -- no copy, no
 * length prefix, no allocation. Every string, and every key, therefore has to stay where it is
 * until @ref arnm_json_writer_write() has run. That is what makes writing a struct cost almost
 * nothing: the payload is read once, at the end, straight out of the caller's own memory.
 *
 * Where a value will not stand still that long -- a formatted number in a local buffer, a name
 * from a string builder about to be reused -- `arnm_json_writer_add_string_copy()` takes a copy
 * into the writer's own allocator, and that copy lives exactly as long as the document does.
 *
 * ### Where the memory comes from
 *
 * The allocator named at @ref arnm_json_writer_init() carries the document being built: the
 * values, the copied strings, nothing else. The written text comes from the allocator handed to
 * @ref arnm_json_writer_write(), which may be the same one or a different one, and belongs to
 * the caller from then on. NULL is the host in both places, as everywhere else in arnm.
 *
 * @note Nothing here is thread safe. One writer belongs to one thread at a time.
 * @{
 */

/**
 * @brief Bytes the opaque writer state occupies.
 *
 * Larger than a reader's, and all of the difference is the level stack: a writer has to
 * remember the containers it has open, where a reader only ever remembers where it stands. The
 * implementation pins this against its real layout with a `static_assert`.
 */
#define ARNM_JSON_WRITER_SIZE 328

/**
 * @brief Containers that may stand open at once, the root included.
 *
 * Deep enough for any struct written by hand, and a fixed number rather than an allocation
 * because a writer that reaches for memory in the middle of a field is a writer that can fail
 * in the middle of a field. Opening past this records @ref ARNM_ERROR_RESOURCE_EXHAUSTED and
 * writes nothing.
 */
#define ARNM_JSON_WRITER_MAX_DEPTH 16

/**
 * @brief Bytes of the failing field name the writer keeps, terminator included.
 *
 * A key longer than this is recorded truncated rather than not at all.
 */
#define ARNM_JSON_WRITER_FIELD_NAME_SIZE 64

/**
 * @brief Longest text yyjson can render one number as, terminator excluded.
 *
 * The ceiling belongs to fixed point notation and not to the exponents, which is the opposite
 * of what the extremes suggest. `-1.7976931348623157e+308` is twenty-four characters and so is
 * every other number written with an exponent, sign and all. But a number whose decimal point
 * falls just left of the first significant digit is written out in full, and seventeen
 * significant digits behind `-0.00000` is twenty-five -- `-0.0000018498776203445192` is one such
 * `double`, and nothing a `double` can hold is longer than that.
 *
 * @ref arnm_json_writer_size() charges this for every real number it cannot know the length of
 * in advance, which is where its answer stops being exact.
 */
#define ARNM_JSON_WRITER_MAX_NUMBER_TEXT 25

// ********** flags *******************

/*
 * Two switches are missing from this list, and their bits are missing with them.
 *
 * yyjson is compiled here with YYJSON_DISABLE_NON_STANDARD, which removes the code behind every
 * one of its YYJSON_WRITE_ALLOW_* switches rather than merely defaulting them off. A flag for
 * `Infinity`/`NaN` or for malformed UTF-8 would therefore be a bit this writer could accept,
 * translate and hand to a serializer that no longer has anything to do with it -- accepted at
 * init, and then quietly without effect at every write. Both were removed for that reason: a
 * switch that cannot be honoured is worse than an absent one, because the caller has no way to
 * find out.
 *
 * What that leaves is a writer that refuses a non-finite number outright, unless
 * @ref ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL is asked for -- `null` is standard JSON, so that one
 * survives the build and is the only way to get such a value into the output at all.
 *
 * The other one moved the other way. The same build also sets YYJSON_DISABLE_UTF8_VALIDATION,
 * so a string is never checked and malformed bytes are always written through. That is what the
 * removed ALLOW_INVALID_UNICODE used to ask for, and it is now simply how this writer behaves;
 * a caller that must emit well formed text has to check its own input.
 *
 * Bits 4 and 6 stay empty rather than being closed up. A caller that still passes one of the
 * removed values is answered with ARNM_ERROR_INVALID_PARAM at init, which is what a renumbering
 * would have turned into a different flag being silently applied instead.
 *
 * Turning either back on is a build change and not a call site one -- drop the macro in
 * build.zig and CMakeLists.txt, and the flag that belongs to it can come back here.
 */

/** @brief Bit set of `ARNM_JSON_WRITE_*`, handed to @ref arnm_json_writer_init(). */
typedef uint32_t arnm_json_write_flags;

/** @brief Minified: no space anywhere the format does not require one. */
#define ARNM_JSON_WRITE_DEFAULT ((arnm_json_write_flags)0u)
/** @brief One value per line, four spaces of indent per level. */
#define ARNM_JSON_WRITE_PRETTY ((arnm_json_write_flags)(1u << 0))
/** @brief As @ref ARNM_JSON_WRITE_PRETTY, two spaces per level. */
#define ARNM_JSON_WRITE_PRETTY_TWO_SPACES ((arnm_json_write_flags)(1u << 1))
/** @brief Escape every non-ASCII character as `\\uXXXX` instead of copying its UTF-8. */
#define ARNM_JSON_WRITE_ESCAPE_UNICODE ((arnm_json_write_flags)(1u << 2))
/** @brief Escape `/` as `\\/`, which some embedders of JSON in HTML ask for. */
#define ARNM_JSON_WRITE_ESCAPE_SLASHES ((arnm_json_write_flags)(1u << 3))
/* bit 4 was ALLOW_INF_AND_NAN, which this build cannot honour; left empty on purpose */
/** @brief Write a non-finite number as `null`. Standard JSON, lost information. */
#define ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL ((arnm_json_write_flags)(1u << 5))
/* bit 6 was ALLOW_INVALID_UNICODE, which this build does unconditionally; left empty on purpose */
/** @brief End the text with a newline, ahead of the terminator. */
#define ARNM_JSON_WRITE_NEWLINE_AT_END ((arnm_json_write_flags)(1u << 7))

// ********** the writer *******************

/**
 * @brief A writer: one allocator, one document being built, and the first error it saw.
 *
 * Opaque by construction -- the bytes carry a layout that lives entirely in `json_writer.c`.
 * The union is not a choice between members but an alignment floor.
 *
 * Not usable until @ref arnm_json_writer_init() or @ref arnm_json_writer_create(). A zeroed
 * writer reads as one that was never initialized and refuses every call with
 * @ref ARNM_ERROR_NOT_INITIALIZED.
 */
typedef struct arnm_json_writer {
  union {
    uint8_t bytes[ARNM_JSON_WRITER_SIZE]; /**< Opaque; never read these directly. */
    void *alignment_pointer;              /**< Never read. Present for its alignment alone. */
    uint64_t alignment_integer;           /**< Never read. Present for its alignment alone. */
  } opaque;                               /**< The storage itself. Never named by a caller. */
} arnm_json_writer;

/**
 * @brief What a document is expected to hold, so its pools are opened at that size once.
 *
 * The document is built in two pools that start small -- a few hundred bytes each -- and double
 * every time they run out, keeping every chunk they ever opened. A document that needs four
 * chunks therefore pays for all four, and the three it outgrew are dead weight until it is
 * released. Saying up front how big it will be replaces the whole series with one chunk.
 *
 * Both figures are hints and neither has to be right. Too low costs one extra chunk, which is
 * where the growth would have started anyway; too high reserves room the document does not use,
 * and one past what the allocator behind the writer could ever hand out is dropped rather than
 * attempted -- a hint never turns into a write that does not happen.
 * A writer that builds documents of one shape can afford to be exact about it; one that does
 * not should leave the hint out and let the pools grow, because a hint sized for the largest
 * document is paid by every small one.
 *
 * @see arnm_json_writer_init()
 */
typedef struct arnm_json_writer_hint {
  /** Values the document will hold, every key counted as one of them. 0 says nothing.
   *
   *  A scalar member of an object is two: the key and the value. A member whose value is an
   *  object or an array is two as well, plus whatever that container holds. An element of an
   *  array is one, plus its contents. The container itself is one.
   */
  uint32_t values;
  /** Bytes the copied strings will take, each one's terminator counted. 0 says nothing.
   *
   *  Only what is copied lands here: @ref arnm_json_writer_add_string_copy(),
   *  @ref arnm_json_writer_add_hex() and @ref arnm_json_writer_add_uuid(). A borrowed string
   *  and every key cost nothing, whatever their length.
   */
  uint32_t string_bytes;
} arnm_json_writer_hint;

/**
 * @brief Prepare a writer in storage the caller owns. Allocates nothing.
 *
 * The allocator and the flags are only remembered here; the first field is what draws from the
 * one and opens a document under the other.
 *
 * @param[out]    writer    Storage to initialize; not NULL. Every field is written and none is
 *                          read, so uninitialized storage is a valid input.
 * @param[in,out] allocator Where the document being built comes from, or NULL for the host.
 *                          Kept for the writer's whole life.
 * @param[in]     flags     Bit set of `ARNM_JSON_WRITE_*`, or @ref ARNM_JSON_WRITE_DEFAULT.
 * @param[in]     hint      How big the documents this writer builds will be, or NULL to let
 *                          the pools grow on their own. Copied here, so it does not have to
 *                          outlive the call, and applied to every document the writer begins.
 * @retval ARNM_SUCCESS             Ready, holding no document.
 * @retval ARNM_ERROR_NULL_POINTER  @p writer is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p flags holds a bit this header does not define; @p writer
 *                                  is left untouched and stays uninitialized.
 * @warning Calling this on a writer that still holds a document strands it. Use
 *          @ref arnm_json_writer_release() first.
 * @warning An initialized writer may not be moved or copied to another address. The document
 *          reaches its allocator back through the writer it was built by.
 * @whisper An empty page, and the ink already chosen
 */
arnm_result arnm_json_writer_init(
    arnm_json_writer *writer,
    arnm *allocator,
    arnm_json_write_flags flags,
    const arnm_json_writer_hint *hint
);

/**
 * @brief Carve a writer out of @p allocator and initialize it against that same allocator.
 *
 * @param[in,out] allocator Where the state's @ref ARNM_JSON_WRITER_SIZE bytes and the document
 *                          come from, or NULL for the host.
 * @param[in]     flags     As @ref arnm_json_writer_init().
 * @param[in]     hint      As @ref arnm_json_writer_init(), NULL for none.
 * @return The new writer, or NULL if @p allocator had no room or @p flags holds an unknown bit.
 * @note Give it back with @ref arnm_json_writer_destroy().
 */
arnm_json_writer *arnm_json_writer_create(
    arnm *allocator, arnm_json_write_flags flags, const arnm_json_writer_hint *hint
);

/**
 * @brief Let go of the document, keep the writer.
 *
 * Everything the building allocated goes back to the allocator it came from; the writer itself
 * survives and can start another document. The recorded error survives too, so a status may
 * still be asked for afterwards; the next `begin` is what clears it.
 *
 * @param[in,out] writer Writer to empty; NULL is a no-op, and so is one holding nothing.
 * @retval ARNM_SUCCESS Released, or there was nothing to release.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED The document was released and an arena kept
 *                     part of its bytes until that arena resets.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p writer was never initialized.
 * @warning Text already written is untouched -- it was never the document's to hold.
 * @whisper The scaffolding comes down, the building stands
 */
arnm_result arnm_json_writer_release(arnm_json_writer *writer);

/**
 * @brief @ref arnm_json_writer_release(), then the writer's own bytes.
 *
 * @param[in,out] writer    From @ref arnm_json_writer_create(), never stack or static storage;
 *                          may be NULL.
 * @param[in,out] allocator The allocator @p writer was carved from, NULL for the host.
 * @retval ARNM_SUCCESS Released and given back, or @p writer was NULL.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Everything was released; an arena kept some
 *                     of it until it resets.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p writer was never initialized.
 */
arnm_result arnm_json_writer_destroy(arnm_json_writer *writer, arnm *allocator);

// ********** starting a document *******************

/**
 * @brief Throw away what was written and start a document whose root is an object.
 *
 * Not needed before the first field -- an object root is what a writer opens on its own. It is
 * needed to write a second payload through the same writer, and that is what it is for: the
 * recorded error goes back to @ref ARNM_SUCCESS, every open level is closed, and the previous
 * document is released.
 *
 * @param[in,out] writer Writer to start over; not NULL and initialized.
 * @retval ARNM_SUCCESS               Ready, holding an empty object.
 * @retval ARNM_ERROR_NULL_POINTER    @p writer is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p writer was never initialized.
 * @retval ARNM_ERROR_OUT_OF_MEMORY   The allocator had nothing left; also recorded.
 * @whisper The page turned, and the last one let go
 */
arnm_result arnm_json_writer_begin_object(arnm_json_writer *writer);

/**
 * @brief As @ref arnm_json_writer_begin_object(), with an array as the root.
 *
 * Call it before the first field; anything already written is thrown away. Fields go in with a
 * NULL key from then on, because an array has no names.
 *
 * @param[in,out] writer Writer to start over; not NULL and initialized.
 * @return As @ref arnm_json_writer_begin_object().
 */
arnm_result arnm_json_writer_begin_array(arnm_json_writer *writer);

// ********** what the writer carries *******************

/**
 * @brief The first error since the document was begun, or @ref ARNM_SUCCESS while all is well.
 *
 * The one call a mapper has to make, and it may be skipped entirely where
 * @ref arnm_json_writer_write() is checked instead -- a writer carrying an error refuses to
 * write and answers the same code.
 *
 * @param[in] writer Writer to ask; may be NULL.
 * @return The recorded error, @ref ARNM_SUCCESS when there is none, or
 *         @ref ARNM_ERROR_NOT_INITIALIZED for NULL and for a writer that was never initialized.
 * @whisper The first slip, kept while the writing goes on
 */
arnm_result arnm_json_writer_status(const arnm_json_writer *writer);

/**
 * @brief The field name the recorded error happened at.
 *
 * The key of the field, `"[]"` for an element added to an array, and the empty string for
 * anything that belongs to no field. Truncated to @ref ARNM_JSON_WRITER_FIELD_NAME_SIZE bytes
 * including the terminator.
 *
 * @param[in] writer Writer to ask; may be NULL.
 * @return The name, never NULL, valid until the next `begin` or
 *         @ref arnm_json_writer_clear_error(). The empty string for NULL and for an
 *         uninitialized writer.
 */
const char *arnm_json_writer_error_field(const arnm_json_writer *writer);

/**
 * @brief Forget the recorded error and let the writing count again.
 *
 * The document, the open levels and the size already accounted for are all untouched; only the
 * verdict is cleared. What a refused field would have written is not there and does not come
 * back -- clearing says "go on from here", not "try that again".
 *
 * @param[in,out] writer Writer to clear; not NULL and initialized.
 * @retval ARNM_SUCCESS               Cleared, whether or not there was anything to clear.
 * @retval ARNM_ERROR_NULL_POINTER    @p writer is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p writer was never initialized.
 */
arnm_result arnm_json_writer_clear_error(arnm_json_writer *writer);

/**
 * @brief Containers standing open, the root counted.
 *
 * 0 before the first field, 1 inside the root, one more per @ref arnm_json_writer_open_object().
 *
 * @param[in] writer Writer to ask; may be NULL.
 * @return The depth, or 0.
 */
uint32_t arnm_json_writer_depth(const arnm_json_writer *writer);

// ********** adding a field, one line per struct member *******************

/*
 * Every call below adds one field to the container currently open, under @p key when that is an
 * object and with @p key NULL when it is an array. None of them returns anything: what could
 * have gone wrong is recorded under @p key and asked about once, and the first of these is what
 * stays.
 *
 *   ARNM_ERROR_INVALID_PARAM        a key inside an array, or none inside an object
 *   ARNM_ERROR_OUT_OF_MEMORY        the allocator had nothing left
 *   ARNM_ERROR_ARITHMETIC_OVERFLOW  the text would grow past what a uint32_t can measure
 *   ARNM_ERROR_RESOURCE_EXHAUSTED   more levels open than ARNM_JSON_WRITER_MAX_DEPTH allows
 *   ARNM_ERROR_INVALID_STATE        every level is closed and the document is finished
 *
 * Once anything is recorded the writer stops building, so a run of calls after a failure costs
 * nothing and changes nothing.
 */

/** @brief Add the literal `null`. @param writer Writer. @param key Field name, NULL in an array. */
void arnm_json_writer_add_null(arnm_json_writer *writer, const char *key, size_t key_length);

/** @brief Add `true` or `false`.
 *  @param[in,out] writer Writer to add to; may be NULL.
 *  @param[in]     key    Field name, or NULL for an element of the current array.
 *  @param[in]     value  What to write. */
void arnm_json_writer_add_bool(
    arnm_json_writer *writer, const char *key, size_t key_length, bool value
);

/** @brief Add a signed integer, written whole and without an exponent.
 *  @param[in,out] writer Writer to add to; may be NULL.
 *  @param[in]     key    Field name, or NULL for an element of the current array.
 *  @param[in]     value  What to write. */
void arnm_json_writer_add_int64(
    arnm_json_writer *writer, const char *key, size_t key_length, int64_t value
);

/** @brief Add an unsigned integer, written whole and without an exponent.
 *  @param[in,out] writer Writer to add to; may be NULL.
 *  @param[in]     key    Field name, or NULL for an element of the current array.
 *  @param[in]     value  What to write. */
void arnm_json_writer_add_uint64(
    arnm_json_writer *writer, const char *key, size_t key_length, uint64_t value
);

/**
 * @brief Add a real number, in the shortest form that reads back as the same `double`.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     value  What to write.
 * @note A non-finite value needs @ref ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL, which writes it as
 *       `null`; without it the write refuses the whole document rather than this one field.
 *       This build has no way to spell `Infinity` or `NaN` in the output at all, so `null` or a
 *       refusal are the only two answers -- see the flags section.
 * @note This is the one value @ref arnm_json_writer_size() cannot measure exactly beforehand.
 */
void arnm_json_writer_add_double(
    arnm_json_writer *writer, const char *key, size_t key_length, double value
);

/**
 * @brief Add a string, borrowing the bytes where they lie.
 *
 * Nothing is copied and nothing is measured beyond its length: @p value and @p key both have to
 * stay unchanged and in place until @ref arnm_json_writer_write() has run.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     value  NUL terminated bytes to write, or NULL for the literal `null` -- which
 *                       is what an absent optional member usually means. Where a NULL pointer
 *                       would be a mistake, catch it before it gets here.
 * @whisper Lent to the page, never lifted from its place
 */
void arnm_json_writer_add_string(
    arnm_json_writer *writer, const char *key, size_t key_length, const char *value
);

/**
 * @brief @ref arnm_json_writer_add_string() for bytes that are not NUL terminated.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     value  Bytes to write; NULL writes the literal `null`. An embedded NUL is
 *                       carried through as `\\u0000`, which is what @p length is for.
 * @param[in]     length Bytes in @p value.
 */
void arnm_json_writer_add_string_length(
    arnm_json_writer *writer, const char *key, size_t key_length, const char *value, uint32_t length
);

/*
 * write string directly into json, without escaping or utf8 check
 * 
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     value  Bytes to write; NULL writes the literal `null`.
 * @param[in]     length Bytes in @p value.
*/
void arnm_json_writer_add_string_raw(
  arnm_json_writer* writer, const char* key, size_t key_length, const char* value, uint32_t length
);

/**
 * @brief Add a string, taking a copy into the writer's own allocator.
 *
 * For a value that will not stand still until the write: a number just formatted into a local
 * buffer, a name from a builder about to be reused. The copy lives exactly as long as the
 * document and goes back with it. The key is still borrowed -- a key is almost always a
 * literal, and the one that is not can be copied by the caller.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     value  NUL terminated bytes to copy, or NULL for the literal `null`.
 * @whisper Taken along, because the road ahead is longer than the ground it stood on
 */
void arnm_json_writer_add_string_copy(
    arnm_json_writer *writer, const char *key, size_t key_length, const char *value
);

/**
 * @brief @ref arnm_json_writer_add_string_copy() for bytes that are not NUL terminated.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     value  Bytes to copy; NULL writes the literal `null`.
 * @param[in]     length Bytes in @p value.
 */
void arnm_json_writer_add_string_copy_length(
    arnm_json_writer *writer, const char *key, size_t key_length, const char *value, uint32_t length
);

/**
 * @brief Add a block of bytes as a lowercase hex string, formatted where it will be written.
 *
 * The pair to @c arnm_binary_to_hex(), with the buffer question taken off the caller: the
 * characters are formatted directly into the document's own storage, so there is no scratch to
 * size, none to hand back, and no copy from the one into the other. Two characters per byte, in
 * order, no separators. An empty block is the empty string, not `null`.
 *
 * ### Why this is not add_string_copy() with a hex buffer in front of it
 *
 * The serializer reserves room for a string by assuming the worst: every byte of it might come
 * out as `\uXXXX`, so it asks for six bytes per character before it writes one. Hex escapes to
 * nothing -- the sixteen digits are the tamest characters there are -- and this call is the one
 * place that can say so, because it is what put them there. The text goes into the document
 * already quoted and is written out verbatim, which costs one byte per character plus the
 * separator instead of six.
 *
 * On a document whose longest field is a hex blob, that reservation is what decides the peak:
 * a 1 KiB block written through @ref arnm_json_writer_add_string_copy() asks the serializer for
 * about 12 KiB of working buffer and through this call for about 2 KiB. Reach for it wherever
 * the bytes are a key, a hash, a signature or a payload -- which is nearly everywhere a binary
 * field meets JSON.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     data   Bytes to render; NULL writes the empty string.
 * @param[in]     size   How many, 0 writing the empty string. Past
 *                       `(ARNM_MAX_ALLOC_SIZE - 3) / 2` the field is refused with
 *                       @ref ARNM_ERROR_RESOURCE_SIZE_EXCEED, which
 *                       @ref arnm_json_writer_status() answers with.
 * @note @p data is read here and never again: unlike the value
 *       @ref arnm_json_writer_add_string() takes, it does not have to stay standing until the
 *       write. @p key still does -- every adder borrows the key, as the group note above says.
 * @whisper Every byte says its name twice, and the page already knows it will not shout
 */
void arnm_json_writer_add_hex(
    arnm_json_writer *writer, const char *key, size_t key_length, const uint8_t *data, uint32_t size
);

/**
 * @brief Add a block of bytes as a base64 string, encoded where it will be written.
 *
 * @ref arnm_json_writer_add_hex() for a payload rather than for a value a person will read:
 * four characters per three bytes instead of two per one, so the field costs a third less than
 * its hex. The standard alphabet with padding, which is what `atob()` reads.
 *
 * Everything else is as add_hex(): the characters are formatted straight into the document, the
 * text goes in already quoted and is written out verbatim, and the serializer is asked for one
 * byte a character rather than the six it reserves against escaping. An empty block is the
 * empty string, not `null`.
 *
 * Which of the two to reach for is a question about the reader, not about the bytes. Hex where
 * the value will be compared against another tool's output by eye -- a key, a hash, a
 * transaction id. This where it is a payload nobody reads directly and its length is what
 * matters.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     data   Bytes to encode; NULL writes the empty string.
 * @param[in]     size   How many, 0 writing the empty string. Past
 *                       `((ARNM_MAX_ALLOC_SIZE - 3) / 4) * 3` the field is refused with
 *                       @ref ARNM_ERROR_RESOURCE_SIZE_EXCEED, which
 *                       @ref arnm_json_writer_status() answers with.
 * @note @p data is read here and never again and does not have to outlive the call. @p key
 *       still does, as with every adder.
 * @whisper Three bytes fold into four letters, on a page that will not ask them to shout
 */
void arnm_json_writer_add_base64(
    arnm_json_writer *writer, const char *key, size_t key_length, const uint8_t *data, uint32_t size
);

/**
 * @brief Add 16 bytes as a uuid in the canonical 8-4-4-4-12 form.
 *
 * The pair to @c arnm_uuid_to_string(), and @ref arnm_json_writer_add_hex() for the one binary
 * field that is not written as hex: the characters are formatted directly into the document and
 * written out verbatim, so there is no buffer to hold one uuid in on the way past and the
 * serializer is not asked for six bytes a character it will never need.
 *
 * Nothing is validated -- any 16 bytes are a uuid here, as they are in arnm_uuid_to_string();
 * version and variant are the caller's business.
 *
 * @param[in,out] writer Writer to add to; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @param[in]     uuid   The 16 bytes, or NULL for the literal `null`. There is no size here
 *                       that could make an absent uuid an empty one, which is why NULL is the
 *                       member that is not there rather than the empty string
 *                       @ref arnm_json_writer_add_hex() writes for a block of no bytes.
 * @note @p uuid is read here and never again and does not have to outlive the call. @p key
 *       still does, as with every adder.
 * @whisper Sixteen bytes take the shape the world reads them by, on the page they were written for
 */
void arnm_json_writer_add_uuid(
    arnm_json_writer *writer, const char *key, size_t key_length, const uint8_t *uuid
);

// ********** nesting *******************

/**
 * @brief Open an object and add every field that follows to it.
 *
 * @param[in,out] writer Writer to descend in; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 * @note Every open needs a @ref arnm_json_writer_close(). One that is missing is not an error
 *       and not a refusal: the container simply stays open, and the write closes it.
 * @whisper One level down, and the way back kept here
 */
void arnm_json_writer_open_object(arnm_json_writer *writer, const char *key, size_t key_length);

/**
 * @brief Open an array and add every field that follows to it, keys left out.
 *
 * @param[in,out] writer Writer to descend in; may be NULL.
 * @param[in]     key    Field name, or NULL for an element of the current array.
 */
void arnm_json_writer_open_array(arnm_json_writer *writer, const char *key, size_t key_length);

/**
 * @brief Close the container the last open began.
 *
 * @param[in,out] writer Writer to come back up in; may be NULL.
 * @note Closing when nothing is open records @ref ARNM_ERROR_INVALID_STATE -- one close too
 *       many is a bug in the mapper, and a silent one would move the next field somewhere
 *       nobody expects.
 */
void arnm_json_writer_close(arnm_json_writer *writer);

// ********** measuring and writing *******************

/**
* try to estimate buffer size needed by writer, don't work for extra long value
*/
uint32_t arnm_json_writer_buffer_size_min(const arnm_json_writer *writer);

/**
 * @brief Render the document into memory drawn from @p allocator.
 *
 * Every open container is closed, the text is rendered under the flags the writer was
 * initialized with, and what comes back is a plain arnm allocation the caller owns: NUL
 * terminated, and shrunk to exactly what the text needed before it is handed over.
 *
 * A writer carrying an error writes nothing and answers that error, so this one result stands
 * in for a check after every field.
 *
 * @param[in,out] writer     Writer to render; not NULL and initialized.
 * @param[in,out] allocator  Where the text comes from, or NULL for the host. May be the one the
 *                           document is built in.
 * @param[out]    out        Receives pointer and size; not NULL. Untouched unless the call
 *                           succeeds. Give it back with `arnm_memory_block_free()`.
 * @param[out]    out_length Receives the text length, terminator excluded; may be NULL. JSON
 *                           never holds a NUL byte, so `strlen` answers the same thing.
 * @retval ARNM_SUCCESS                   Written.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Written, and an arena kept the working room
 *                                        the serializer needed until it resets. @p out is
 *                                        complete and correct either way.
 * @retval ARNM_ERROR_NULL_POINTER        @p writer or @p out is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED     @p writer was never initialized.
 * @retval ARNM_ERROR_INVALID_STATE       No document was ever begun.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       @p allocator had nothing left.
 * @retval ARNM_ERROR_ENCODE_FAILED       A non-finite number without
 *                                        @ref ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL. Malformed
 *                                        UTF-8 does not reach this: it is written through
 *                                        unchecked, see the flags section.
 * @return Otherwise the error the writer was already carrying, unwritten.
 * @note The document is not consumed. Ask again for the same text, or begin the next payload.
 * @whisper Everything set down at once, in the order it was gathered
 */
arnm_result arnm_json_writer_write(
    arnm_json_writer *writer, arnm *allocator, arnm_memory_block *out, uint32_t *out_length
);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_JSON_WRITER_H

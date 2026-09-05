#ifndef ARNM_JSON_READER_H
#define ARNM_JSON_READER_H

#include "arnm/converter.h"
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
 * @defgroup arnm_json_reader arnm_json_reader
 * @brief Reading JSON into a caller's own structs, a whole shape at a time.
 *
 * ### The shape of a session
 *
 * Four steps, and there is no fifth:
 *
 * 1. @ref arnm_json_reader_init() over storage the caller already has, or
 *    @ref arnm_json_reader_create() to have the storage carved out too.
 * 2. @ref arnm_json_reader_parse() or @ref arnm_json_reader_parse_insitu(), which hands back the
 *    document's root value.
 * 3. @ref arnm_json_read_object() and @ref arnm_json_read_array() from that root downwards.
 * 4. @ref arnm_json_reader_release(), or @ref arnm_json_reader_destroy() for a created one.
 *
 * There is no cursor here and no getter per value. A reader holds a document and nothing else --
 * no place in it, no path, no memory of the last thing asked for. Everything about where you are
 * lives in the caller's own variables, which is where it is easiest to read and hardest to get
 * quietly wrong.
 *
 * ### Two calls, because JSON has two shapes that branch
 *
 * An object is read by handing over a table: what a key is called, what it should become, and
 * where to put it. One walk of the member chain answers all of them.
 *
 * An array is read by handing over a buffer of value handles, and it comes back filled. The
 * elements are not converted, because an array's elements have no names to hang a type on; the
 * caller looks at each in turn and knows from its own shape what it is.
 *
 * Everything else -- a number, a string, a nested object -- is a member of one of those two, and
 * therefore something a table already names. Values are never handed out one at a time.
 *
 * ### What is borrowed, and for how long
 *
 * Nothing a read hands back is owned by the caller. An @ref arnm_json_value pointer is a view
 * into the document, and a string read into an @ref arnm_memory_block points at the document's
 * own bytes. All of it stops being valid at the next parse on that reader, and at the release
 * that ends the session. Nothing here is ever freed by the caller, and a borrowed block must
 * never be handed to arnm_memory_block_free().
 *
 * The decoding field types are the exception, and they are not one really: HEX_FIXED and UUID
 * write into a buffer the caller already had, so what they leave behind is the caller's for as
 * long as that buffer is.
 *
 * ### Strictly RFC 8259, with one thing it cannot promise
 *
 * yyjson is compiled underneath with YYJSON_DISABLE_NON_STANDARD, which takes the code behind
 * its extensions out rather than defaulting it off. Comments, trailing commas, `Infinity`, `NaN`
 * and a byte order mark are refused, and there is no switch to ask otherwise -- so no such
 * switch is offered here.
 *
 * The same build sets YYJSON_DISABLE_UTF8_VALIDATION, so a string is never checked and
 * malformed bytes are carried through unexamined. A caller that needs well formed text has to
 * check its own. That is the one place this reader is more permissive than the standard, and it
 * cannot be turned off from a call site either.
 *
 * @note Nothing here is thread safe. One reader belongs to one thread at a time, and a document
 *       belongs to the reader that parsed it.
 * @{
 */

/**
 * @brief Bytes the opaque reader state occupies.
 *
 * Named rather than hidden behind a pointer, so a reader can live on the stack, in a struct, or
 * in memory another allocator handed out. The implementation pins this against its real layout
 * with a `static_assert`, so the number cannot drift away from what it describes.
 */
#define ARNM_JSON_READER_SIZE 72

/**
 * @brief Bytes an insitu buffer must hold beyond its JSON, and which the parse overwrites.
 *
 * @ref arnm_json_reader_parse_insitu() reads four bytes past the end of the document and writes
 * zeroes there, which is what lets the scanner run without a bounds test in its inner loop.
 * `capacity` therefore has to be at least `length` plus this much.
 */
#define ARNM_JSON_READER_INSITU_PADDING 4u

/**
 * @brief Longest input either parse accepts -- one padding short of @ref ARNM_MAX_ALLOC_SIZE.
 *
 * The copying parse takes the input plus @ref ARNM_JSON_READER_INSITU_PADDING bytes through
 * @ref arnm_alloc(), and that sum is what has to stay inside a `uint32_t`.
 */
#define ARNM_JSON_READER_MAX_INPUT_SIZE (ARNM_MAX_ALLOC_SIZE - ARNM_JSON_READER_INSITU_PADDING)

/**
 * @brief A reader: one allocator, one document, and the first refusal it saw.
 *
 * Opaque by construction -- the bytes carry a layout that lives entirely in `json_reader.c`.
 *
 * A reader may not be moved once it has been initialized. The document keeps its own copy of the
 * allocator hooks, and those hooks point back into this storage; copying the struct to another
 * address leaves the document calling into where it used to be.
 *
 * Not usable until @ref arnm_json_reader_init() or @ref arnm_json_reader_create(). A zeroed
 * reader reads as one that was never initialized and is refused with
 * @ref ARNM_ERROR_NOT_INITIALIZED.
 */
typedef struct arnm_json_reader {
  uint8_t opaqu[ARNM_JSON_READER_SIZE]; /**< Opaque; never read these directly. */
} arnm_json_reader;

/**
 * @brief A value inside a parsed document -- incomplete on purpose.
 *
 * Only ever handled through a pointer, and that pointer is owned by the document. Nothing here
 * is allocated, copied or released by a caller; the whole tree comes and goes with the reader
 * that parsed it.
 */
typedef struct arnm_json_value arnm_json_value;

// ********** starting and ending a session *******************

/**
 * @brief Prepare a reader over storage the caller already has.
 *
 * Every field is written and none is read, so uninitialized storage is a valid input and a
 * reader may be initialized again over one that is finished with. Re-initializing one that still
 * holds a document abandons that document instead of releasing it -- call
 * @ref arnm_json_reader_release() first.
 *
 * @param[out]    reader    Storage of @ref ARNM_JSON_READER_SIZE bytes; not NULL.
 * @param[in,out] allocator Where documents come from, or NULL for the host allocator. Must
 *                          outlive the reader.
 * @retval ARNM_SUCCESS            Ready to parse.
 * @retval ARNM_ERROR_NULL_POINTER @p reader is NULL.
 * @whisper A bed is dug before any water is let into it
 */
arnm_result arnm_json_reader_init(arnm_json_reader *reader, arnm *allocator);

/**
 * @brief Carve a reader out of @p allocator and prepare it against that same allocator.
 *
 * For a caller that would otherwise keep an @ref arnm_json_reader somewhere of its own. Ended
 * with @ref arnm_json_reader_destroy(), which gives the storage back as well as the document.
 *
 * @param[in,out] allocator Where the reader's own bytes and its documents come from, or NULL for
 *                          the host allocator.
 * @return The reader, or NULL where @p allocator had no room. Nothing is left allocated on that
 *         path.
 * @whisper The vessel and the water it will hold, drawn from one source
 */
arnm_json_reader *arnm_json_reader_create(arnm *allocator);

/**
 * @brief Let the document go, and leave the reader ready for another parse.
 *
 * Every value handed out by a read on this reader stops being valid here. The reader itself
 * stays initialized: parsing again is allowed and does not need another
 * @ref arnm_json_reader_init().
 *
 * @param[in,out] reader Reader to empty; NULL is accepted and does nothing.
 * @retval ARNM_SUCCESS            The document is gone and its bytes are back.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED The document is gone, but an arena could not
 *                                 take its bytes back -- they were not its tail, and they stay
 *                                 reserved until arnm_reset(). Neither success nor failure;
 *                                 decide at the call site what it should mean.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p reader was never initialized.
 * @whisper The water drains, the bed stays open
 */
arnm_result arnm_json_reader_release(arnm_json_reader *reader);

/**
 * @brief Release the document and give the reader's own bytes back.
 *
 * For a reader from @ref arnm_json_reader_create(), and only for one: storage the caller
 * provided is the caller's to keep, and @ref arnm_json_reader_release() is the end of that
 * session. @p allocator has to be the one the reader was created from.
 *
 * A second destroy at the same address finds an uninitialized reader rather than walking a
 * document that is already gone.
 *
 * @param[in,out] reader    Reader to destroy; NULL is accepted and does nothing.
 * @param[in,out] allocator The allocator @ref arnm_json_reader_create() was given.
 * @retval ARNM_SUCCESS            Document and storage are both back.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED An arena kept bytes of the document or of the
 *                                 reader; see @ref arnm_json_reader_release().
 * @retval ARNM_ERROR_NOT_INITIALIZED @p reader was never initialized.
 * @whisper Both the water and the bed return to the ground they came from
 */
arnm_result arnm_json_reader_destroy(arnm_json_reader *reader, arnm *allocator);

// ********** parsing *******************

/**
 * @brief Parse @p json into a fresh document, leaving the caller's bytes untouched.
 *
 * The input is copied first and the copy is what the parser writes through, so @p json is read
 * only and need not survive the call. Strings in the document point into that copy, which the
 * reader owns until it is released.
 *
 * Whatever the reader held before is let go first, whether this parse succeeds or not. A refused
 * parse therefore leaves the reader empty rather than still answering for the document before
 * it, and @ref arnm_json_reader_error_message() says what was wrong.
 *
 * @param[in,out] reader         Reader to fill; not NULL and initialized.
 * @param[in]     json           Bytes to read; not NULL. Need not be NUL terminated.
 * @param[in]     length         Bytes in @p json; more than 0 and at most
 *                               @ref ARNM_JSON_READER_MAX_INPUT_SIZE.
 * @param[in]     stop_when_done End the document at its last byte instead of refusing whatever
 *                               follows it -- for a stream that carries one document and then
 *                               something else. @ref arnm_json_reader_bytes_read() says where it
 *                               stopped.
 * @param[out]    doc_root_out   Receives the root value; may be NULL, though a caller with no
 *                               root has nothing to read. Written only on success.
 * @retval ARNM_SUCCESS                   Parsed; @p doc_root_out is the way in.
 * @retval ARNM_ERROR_NULL_POINTER        @p reader or @p json is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED     @p reader was never initialized.
 * @retval ARNM_ERROR_INVALID_PARAM       @p length is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p length is past
 *                                        @ref ARNM_JSON_READER_MAX_INPUT_SIZE.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The allocator had nothing left.
 * @retval ARNM_ERROR_DECODE_FAILED       The bytes are not JSON this reader accepts.
 * @note Behind an arena the string pool is taken first and the value buffer above it, so
 *       releasing gives the value buffer back and leaves the pool buried until arnm_reset().
 *       @ref arnm_json_reader_parse_insitu() has no pool and does not pay this.
 * @whisper Bytes settle into branches, and the branches hold
 */
arnm_result arnm_json_reader_parse(
    arnm_json_reader *reader,
    const char *json,
    uint32_t length,
    bool stop_when_done,
    arnm_json_value **doc_root_out
);

/**
 * @brief Parse in place, spending @p buffer instead of copying it.
 *
 * The parser unescapes strings into the caller's own bytes and terminates them there, so no
 * string pool is allocated and a document costs exactly one allocation. Behind an arena that
 * allocation sits at the tail, so @ref arnm_json_reader_release() moves the index all the way
 * home rather than leaving anything buried.
 *
 * What that buys has a price in three parts, all of them the caller's to keep:
 *
 * - @p buffer is modified. What it held is gone by the time this returns.
 * - @p buffer must outlive the document. Every string in the tree points into it.
 * - @p buffer must have @ref ARNM_JSON_READER_INSITU_PADDING bytes beyond @p length, which the
 *   parse overwrites with zeroes.
 *
 * In place parsing does not remove the copy so much as move it to the caller, and then let a
 * caller who never needed one skip it. Where the bytes arrived in a buffer nothing else will
 * look at again -- read off a socket, off a file, out of a scratch arena -- there is no copy
 * left to make.
 *
 * @param[in,out] reader         Reader to fill; not NULL and initialized.
 * @param[in,out] buffer         Bytes to read and to write through; not NULL.
 * @param[in]     length         JSON bytes in @p buffer, padding excluded; more than 0 and at
 *                               most @ref ARNM_JSON_READER_MAX_INPUT_SIZE.
 * @param[in]     capacity       Bytes @p buffer really holds; at least @p length plus
 *                               @ref ARNM_JSON_READER_INSITU_PADDING.
 * @param[in]     stop_when_done As @ref arnm_json_reader_parse().
 * @param[out]    doc_root_out   Receives the root value; may be NULL. Written only on success.
 * @retval ARNM_SUCCESS                   Parsed; @p doc_root_out is the way in.
 * @retval ARNM_ERROR_NULL_POINTER        @p reader or @p buffer is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED     @p reader was never initialized.
 * @retval ARNM_ERROR_INVALID_PARAM       @p length is 0, or @p capacity leaves no room for the
 *                                        padding.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p length is past
 *                                        @ref ARNM_JSON_READER_MAX_INPUT_SIZE.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The allocator had nothing left.
 * @retval ARNM_ERROR_DECODE_FAILED       The bytes are not JSON this reader accepts.
 * @warning Freeing @p buffer, reusing it, or letting it fall out of scope while the document is
 *          still being read leaves every string in that document pointing at ground that has
 *          been handed out again.
 * @whisper The riverbed is reshaped by the water it carries
 */
arnm_result arnm_json_reader_parse_insitu(
    arnm_json_reader *reader,
    char *buffer,
    uint32_t length,
    uint32_t capacity,
    bool stop_when_done,
    arnm_json_value **doc_root_out
);

// ********** what the reader carries *******************

/**
 * @brief Whether the last parse on this reader held.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @retval ARNM_SUCCESS               The last parse held, or none has run yet.
 * @retval ARNM_ERROR_DECODE_FAILED   The last parse was refused; the two calls below say where
 *                                    and what.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p reader is NULL or was never initialized.
 */
arnm_result arnm_json_reader_status(const arnm_json_reader *reader);

/**
 * @brief What the last refusal was, in the parser's own words.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return Static text owned by the library, or `"no error"` where nothing was refused. Never
 *         NULL, and never something the caller has to free.
 */
const char *arnm_json_reader_error_message(const arnm_json_reader *reader);

/**
 * @brief Where the last refusal happened, as a byte offset into the input.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The offset, or 0 where nothing was refused. 0 is also a real offset, so read
 *         @ref arnm_json_reader_status() first to know which of the two this is.
 */
uint32_t arnm_json_reader_error_position(const arnm_json_reader *reader);

/**
 * @brief Whether a document is standing right now.
 *
 * False before the first parse, after a refused one, and after a release.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return true while a document is held.
 */
bool arnm_json_reader_has_document(const arnm_json_reader *reader);

/**
 * @brief Nodes in the document, containers and member names counted along with the rest.
 *
 * A key is a node here as much as the value it names, which is what makes `{"a":1}` three and
 * `{"a":[1,2]}` five. A measure of how much tree there is, and what the document's memory is
 * proportional to -- not a count of anything a caller would call a value.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The count, or 0 where no document is held.
 */
uint32_t arnm_json_reader_value_count(const arnm_json_reader *reader);

/**
 * @brief Bytes of the input the parse actually consumed.
 *
 * The whole length, unless @p stop_when_done ended the document early -- there this is where the
 * first document stopped, and therefore where the next one starts.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The count, or 0 where no document is held.
 */
uint32_t arnm_json_reader_bytes_read(const arnm_json_reader *reader);

// ********** reading an object in one walk *******************

/*
 * A mapping that wants most of an object's members has a choice of two shapes, and only one of
 * them is linear. Asking for members by name walks the member chain once per question; walking
 * the object once and answering each key where it is met costs the chain a single time.
 *
 * That walk is easy to write by hand and easy to write differently every time: the length check
 * before the comparison, the type check before the conversion, the record of what was actually
 * there. arnm_json_read_object() is that walk written once, driven by a table the caller states
 * as a list of fields.
 *
 * What the table does not do is nest. A member that is itself an object or an array is asked for
 * as ARNM_JSON_FIELD_TYPE_VALUE, which hands the value over untouched; the caller walks that one
 * with a table of its own, or with arnm_json_read_array(). A shape described all the way down
 * would have to be built before it could be used, and the few lines it saves are not worth what
 * it costs to read.
 *
 * Nor does it decide what is required. Every field the walk filled is named in the mask it hands
 * back, and the caller reads that mask once for the whole object -- which is where the answer
 * belongs, because it is the same answer whichever way the object was read.
 */

/**
 * @brief What a field of a walk should become, and therefore what its target points at.
 *
 * The order of these is load bearing and not a matter of taste. @ref arnm_json_read_object()
 * sorts a tag into its family with two range tests rather than a table, so the integer types are
 * contiguous and end at @ref ARNM_JSON_FIELD_TYPE_INT32, and the types that read a JSON string
 * are contiguous and end at @ref ARNM_JSON_FIELD_TYPE_UUID. Inserting a member into either run,
 * or between them, moves the boundary the prefilter is written against.
 *
 * The three string types all point at an @ref arnm_memory_block, but they do not use it the same
 * way. STRING writes the descriptor and reads nothing from it; HEX_FIXED and UUID read it -- the
 * caller hands over the buffer to decode into, and its size is what the string is measured
 * against. Nothing here allocates.
 */
typedef enum arnm_json_field_type {
  /** Refused wherever it is met, so a table assembled field by field cannot reach the walk with
   *  an entry nobody set. There is no placeholder tag; a member that is not wanted is left out
   *  of the table. */
  ARNM_JSON_FIELD_TYPE_NONE = 0,
  ARNM_JSON_FIELD_TYPE_UINT64, /**< `uint64_t *`, refusing a negative number. */
  ARNM_JSON_FIELD_TYPE_INT64,  /**< `int64_t *`, refusing what will not fit. */
  ARNM_JSON_FIELD_TYPE_UINT32, /**< `uint32_t *`, refusing a negative number and what will not
                                    fit. */
  /** `int32_t *`, refusing what will not fit. Last integer type -- the prefilter reads this
   *  bound, so nothing may be inserted after it that is not an integer. */
  ARNM_JSON_FIELD_TYPE_INT32,
  /** `arnm_memory_block *`, filled with the document's own bytes and their length. Borrowed, not
   *  owned: it lives exactly as long as the document and is never freed. @c size carries the
   *  string's length here, not an allocation, so it must never be handed to
   *  arnm_memory_block_free(). */
  ARNM_JSON_FIELD_TYPE_STRING,
  /** `arnm_memory_block *` the caller filled with a buffer and the size of it; nothing is
   *  allocated and the descriptor is not changed. A string of other than twice that many
   *  characters is refused, so the buffer is never partly written. */
  ARNM_JSON_FIELD_TYPE_HEX_FIXED,
  /** `arnm_memory_block *` the caller filled with a buffer of exactly
   *  @ref ARNM_UUID_BINARY_SIZE bytes; nothing is allocated. Last string type -- the prefilter
   *  reads this bound. */
  ARNM_JSON_FIELD_TYPE_UUID,
  ARNM_JSON_FIELD_TYPE_DOUBLE, /**< `double *`; every JSON number converts. */
  ARNM_JSON_FIELD_TYPE_BOOL,   /**< `bool *` */
  /** `arnm_json_value **`, handed over untouched -- for a member that is itself an object or an
   *  array, which the caller then reads with a table or a buffer of its own. */
  ARNM_JSON_FIELD_TYPE_VALUE
} arnm_json_field_type;

/**
 * @brief One member of an object: what it is called, what it becomes, and where it goes.
 *
 * Written by the macros below rather than by hand -- they take the length from the key literal
 * and check the target against the type, neither of which a hand written entry can be made to
 * do.
 */
typedef struct arnm_json_field {
  const char *key;     /**< The member name, compared over @ref key_length bytes; it needs no
                            terminator and may carry any byte, NUL included. Not NULL. */
  uint32_t key_length; /**< Characters in @ref key. */
  uint32_t type;       /**< An @ref arnm_json_field_type; not @c NONE. */
  void *target;        /**< Where the value goes; not NULL. */
} arnm_json_field;

/** @brief Length of a key literal, terminator excluded, worked out by the compiler. */
#define ARNM_JSON_KEY_LENGTH(key) ((uint32_t)(sizeof(key) - 1u))

/**
 * @brief The target of a field entry, checked against the type the entry declares.
 *
 * A `void *` and a type tag beside it is a contract the compiler cannot see: a field that says
 * INT64 over a pointer to an `int32_t` writes four bytes into whatever follows it, and says
 * nothing. `_Generic` puts the check back -- a target of any other type matches no association
 * and the translation unit does not compile.
 *
 * Where the compiler has no `_Generic` -- C99, C++, or a C11 that does not carry it -- the check
 * falls away and the cast is all that is left. The contract is the same either way; only the
 * moment it is caught moves from the build to the run, which is worth knowing when a table is
 * written in a C++ translation unit and only compiled as C elsewhere.
 */
#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define ARNM_JSON_TARGET(want, ptr) _Generic((ptr), want: (void *)(ptr))
#else
#define ARNM_JSON_TARGET(want, ptr) ((void *)(ptr))
#endif

/** @cond */
#define ARNM_JSON_FIELD_MAKE(key, type_tag, want, ptr)                                             \
  {(key), ARNM_JSON_KEY_LENGTH(key), (type_tag), ARNM_JSON_TARGET(want, (ptr))}
/** @endcond */

/** @brief A `true`/`false` member into a `bool`. */
#define ARNM_JSON_FIELD_BOOL(key, ptr)                                                             \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_BOOL, bool *, ptr)

/** @brief A number into an `int64_t`, refusing what will not fit. */
#define ARNM_JSON_FIELD_INT64(key, ptr)                                                            \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_INT64, int64_t *, ptr)

/** @brief A number into a `uint64_t`, refusing a negative one. */
#define ARNM_JSON_FIELD_UINT64(key, ptr)                                                           \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_UINT64, uint64_t *, ptr)

/** @brief A number into an `int32_t`, refusing what will not fit. */
#define ARNM_JSON_FIELD_INT32(key, ptr)                                                            \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_INT32, int32_t *, ptr)

/** @brief A number into a `uint32_t`, refusing a negative one and what will not fit. */
#define ARNM_JSON_FIELD_UINT32(key, ptr)                                                           \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_UINT32, uint32_t *, ptr)

/** @brief A number into a `double`. */
#define ARNM_JSON_FIELD_DOUBLE(key, ptr)                                                           \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_DOUBLE, double *, ptr)

/** @brief A string borrowed from the document, into a block that must not be freed. */
#define ARNM_JSON_FIELD_STRING(key, ptr)                                                           \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_STRING, arnm_memory_block *, ptr)

/** @brief A hex string into the buffer the block already carries; nothing is allocated. */
#define ARNM_JSON_FIELD_HEX_FIXED(key, ptr)                                                        \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_HEX_FIXED, arnm_memory_block *, ptr)

/** @brief A uuid into a block of exactly @ref ARNM_UUID_BINARY_SIZE bytes. */
#define ARNM_JSON_FIELD_UUID(key, ptr)                                                             \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_UUID, arnm_memory_block *, ptr)

/** @brief The member itself, for an object or an array the caller reads in turn. */
#define ARNM_JSON_FIELD_VALUE(key, ptr)                                                            \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_VALUE, arnm_json_value **, ptr)

/**
 * @brief An @ref arnm_memory_block over an array whose size the compiler already knows.
 *
 * What @ref ARNM_JSON_FIELD_HEX_FIXED and @ref ARNM_JSON_FIELD_UUID want: the buffer to decode
 * into, and its size, in one initializer that cannot disagree with itself.
 */
#define ARNM_JSON_BLOCK_OF(array) {(uint8_t *)(array), (uint32_t)sizeof(array)}

/** @brief Fields one walk can carry, which is what a bit per field in a mask leaves room for. */
#define ARNM_JSON_FIELDS_MAX 64u

/**
 * @brief Walk @p object once and read every member @p fields names into where it points.
 *
 * The member chain is walked a single time, and each key is compared only against the entries
 * the walk has not filled yet -- it starts at the lowest of them and stops at the first match. A
 * table that lists its fields in the document's own order is therefore met one comparison per
 * member; a table in any other order costs the entries above the lowest open one, and a key the
 * table does not name costs those same entries and is then passed over. Once every entry is
 * filled the remaining members are not looked at, so a document that carries far more than this
 * reader wants is left where it stands.
 *
 * That an entry is skipped once filled is what decides duplicates: **a member named twice is
 * read the first time and ignored after**, which is the opposite of what most JSON readers do
 * and is worth knowing where a document is not written by the same mapping that reads it. It is
 * a consequence of the shape rather than a preference, and the mask cannot tell the two cases
 * apart -- a member read once and a member read once out of three look the same in it.
 *
 * A member the table does not name is not an error: a document is allowed to carry more than
 * this reader wants.
 *
 * Nothing here allocates, and there is no allocator to hand over. A string member is borrowed
 * from the document, and the two decoding types write into buffers the caller already owns, so
 * every target is either the caller's storage or a view into a document that outlives the call.
 *
 * @param[in]     object    Object to walk; not NULL.
 * @param[in,out] fields    The table; not NULL. Targets are written; the descriptor of a
 *                          @ref ARNM_JSON_FIELD_TYPE_HEX_FIXED or @ref ARNM_JSON_FIELD_TYPE_UUID
 *                          entry is also read, for the buffer it names and the size of it.
 * @param[in]     count     Entries in @p fields, at least 1 and at most
 *                          @ref ARNM_JSON_FIELDS_MAX.
 * @param[out]    out_found Bit @c i is set where entry @c i was read; may be NULL. Cleared
 *                          before anything else is asked, and written again when the walk stops
 *                          early, so a caller can see how far it came.
 * @retval ARNM_SUCCESS                   Walked; @p out_found says what was there.
 * @retval ARNM_ERROR_NULL_POINTER        @p object or @p fields is NULL, or a matched entry has
 *                                        no target.
 * @retval ARNM_ERROR_INVALID_PARAM       @p count is 0 or past @ref ARNM_JSON_FIELDS_MAX, or a
 *                                        matched entry carries @ref ARNM_JSON_FIELD_TYPE_NONE or
 *                                        a type this reader does not know.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE   @p object is no object, or a member is of another JSON
 *                                        type than its entry names.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW A number does not fit the target its entry names.
 * @retval ARNM_ERROR_DECODE_FAILED       A hex or uuid string is not the shape its entry names.
 * @note The walk stops at the first member it cannot read; the targets filled before it keep
 *       what they were given, and the mask names exactly those.
 * @note Which members are required is not asked here. The mask says what was there and the
 *       caller decides once, for the whole object, what that means.
 * @whisper Every name asked once what it is, in the order it expects to meet them
 */
arnm_result arnm_json_read_object(
    arnm_json_value *object, arnm_json_field *fields, uint32_t count, uint64_t *out_found
);

/**
 * @brief Fill @p out_values with every element of @p array, in order.
 *
 * The counterpart to @ref arnm_json_read_object(), and deliberately the plainer of the two: an
 * array's elements have no names, so there is nothing to hang a type on and nothing is
 * converted. What comes back is handles, and the caller reads each one the way its own shape
 * says to -- another table, another buffer, or the element it was expecting all along.
 *
 * All or nothing. An array with more elements than @p capacity is refused rather than truncated,
 * and nothing is written into @p out_values on that path. Where the size is not known ahead of
 * time, ask with a buffer, widen on
 * @ref ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL, and ask again.
 *
 * Because of that, @p out_array_size on success is always exactly the array's length; it is
 * there so the caller does not have to ask twice, not because a partial fill can happen.
 *
 * @param[in]  array          Array to read; not NULL.
 * @param[out] out_values     Receives one handle per element; not NULL. Untouched unless the
 *                            call succeeds.
 * @param[in]  capacity       Handles @p out_values holds; more than 0.
 * @param[out] out_array_size Receives the number of handles written; may be NULL. Cleared before
 *                            anything else is asked.
 * @retval ARNM_SUCCESS                            Filled; @p out_array_size says how far.
 * @retval ARNM_ERROR_NULL_POINTER                 @p array or @p out_values is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM                @p capacity is 0.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE            @p array is no array.
 * @retval ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL  The array is longer than @p capacity.
 * @note An empty array is not a refusal: nothing is written and @p out_array_size is 0.
 * @whisper Every element laid out in the order the water carried it
 */
arnm_result arnm_json_read_array(
    arnm_json_value *array,
    arnm_json_value **out_values,
    uint32_t capacity,
    uint32_t *out_array_size
);

bool arnm_json_read_is_null(arnm_json_value *json_value);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_JSON_READER_H

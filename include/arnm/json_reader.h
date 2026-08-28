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
 * @brief A JSON document parsed into an arena and read field by field, errors collected as it
 *        goes.
 *
 * One call turns a stretch of bytes into a tree; from there a struct is filled one line per
 * member, with nothing between the lines. The parser underneath is yyjson; nothing of it
 * reaches this header, so a consumer includes `arnm/json_reader.h` and links against arnm alone
 * -- no third party type, no third party constant, no include path to add.
 *
 * ### The shape a mapper takes
 *
 * Three steps, and only the last one is checked:
 *
 * @code
 * arnm_json_reader reader;
 * arnm_json_reader_init(&reader, arena, ARNM_JSON_READ_DEFAULT);
 * arnm_json_reader_parse(&reader, text, length);
 *
 * user.name   = arnm_json_reader_get_string(&reader, "name");
 * user.age    = arnm_json_reader_get_uint32(&reader, "age");
 * user.active = arnm_json_reader_get_bool(&reader, "active");
 *
 * if (ARNM_SUCCESS != arnm_json_reader_status(&reader)) {
 *   log("%s at field '%s'", arnm_result_to_string(arnm_json_reader_status(&reader)),
 *       arnm_json_reader_error_field(&reader));
 * }
 * @endcode
 *
 * The reader keeps the **first** error and the field name it happened at, and every later
 * access answers its empty value -- NULL for a string, false for a bool, 0 for a number --
 * without overwriting what was recorded. So a refusal in the middle of a struct does not have
 * to stop the reading; it only has to be asked about once, at the end. A parse that fails is
 * that same first error, which is why the parse above needs no test of its own.
 *
 * ### Where the reading happens
 *
 * Every `arnm_json_reader_get_*()` reads a member of the **current value**, which a successful
 * parse sets to the root. @ref arnm_json_reader_enter() moves it into a member and hands back
 * the one it left, @ref arnm_json_reader_leave() puts that one back -- so nesting costs two
 * lines and no bookkeeping:
 *
 * @code
 * arnm_json_value *outer = arnm_json_reader_enter(&reader, "address");
 * user.city = arnm_json_reader_get_string(&reader, "city");
 * arnm_json_reader_leave(&reader, outer);
 * @endcode
 *
 * An array is walked the same way, by index, with @ref arnm_json_reader_count() saying how far.
 * A key of NULL names the current value itself, which is how the elements of an array of
 * scalars are read:
 *
 * @code
 * arnm_json_value *array = arnm_json_reader_enter(&reader, "tags");
 * for (uint32_t i = 0, n = arnm_json_reader_count(&reader); i < n; ++i) {
 *   arnm_json_value *element = arnm_json_reader_enter_at(&reader, i);
 *   user.tags[i] = arnm_json_reader_get_string(&reader, NULL);
 *   arnm_json_reader_leave(&reader, element);
 * }
 * arnm_json_reader_leave(&reader, array);
 * @endcode
 *
 * ### Borrowed strings, or copied ones
 *
 * A string a getter hands back points into the document by default and dies with it. Naming an
 * output allocator through @ref arnm_json_reader_set_output_allocator() changes that for every
 * later getter: each string is copied there, NUL terminated, and outlives the document and the
 * reader both. Nothing else is copied -- numbers and bools travel by value already.
 *
 * ### Where the memory comes from
 *
 * The allocator is named once, at @ref arnm_json_reader_init() or
 * @ref arnm_json_reader_create(), and every byte a document occupies is drawn from it. NULL is
 * the host, exactly as everywhere else in arnm -- see @ref arnm_memory. Behind an arena a parse
 * is two allocations at most, and behind @ref arnm_json_reader_parse_insitu() it is one.
 *
 * ### Two ways in
 *
 * @ref arnm_json_reader_parse() leaves the caller's bytes untouched and pays for that with a
 * copy of the whole input, which becomes the string pool the parsed strings point into.
 * @ref arnm_json_reader_parse_insitu() spends the caller's buffer instead: strings are
 * unescaped in place, no copy is made, and the buffer has to stay alive and unread until the
 * document is released. The insitu path is the cheaper one and the stricter one.
 *
 * ### What a value is
 *
 * @ref arnm_json_value is a handle into the document, never something a caller allocates or
 * frees. Every handle a reader ever handed out becomes dangling at
 * @ref arnm_json_reader_release(), at @ref arnm_json_reader_destroy(), and at the next parse
 * on the same reader -- a document does not outlive the one that follows it.
 *
 * ### NULL and mismatch
 *
 * The value level calls below the reader level are uniform, so a binding on top needs no table.
 * Every one of them returning an @ref arnm_result answers @ref ARNM_ERROR_NULL_POINTER for a
 * NULL handle or a NULL output, and @ref ARNM_ERROR_INVALID_ENUM_TYPE when the value is of a
 * different JSON type than the call reads. The predicates and the size queries answer their
 * empty value instead and never dereference a pointer they were not given.
 *
 * @note Nothing here is thread safe. One reader belongs to one thread at a time.
 * @{
 */

/**
 * @brief Bytes the opaque reader state occupies.
 *
 * Named rather than hidden behind a pointer, so a reader can live on the stack, in a struct,
 * or in memory another allocator handed out. The implementation pins this against its real
 * layout with a `static_assert`, so the number cannot drift away from what it describes.
 */
#define ARNM_JSON_READER_SIZE 256

/**
 * @brief Bytes of the failing field name the reader keeps, terminator included.
 *
 * A key longer than this is recorded truncated rather than not at all -- the name is there to
 * point a reader of the log at the right member, and the first bytes do that.
 */
#define ARNM_JSON_READER_FIELD_NAME_SIZE 64

/**
 * @brief Bytes an insitu buffer must hold beyond its JSON, and which the parse overwrites.
 *
 * @ref arnm_json_reader_parse_insitu() reads four bytes past the end of the document and
 * writes zeroes there, which is what lets the scanner run without a bounds test in its inner
 * loop. `capacity` therefore has to be at least `length` plus this much.
 */
#define ARNM_JSON_READER_INSITU_PADDING 4u

/**
 * @brief Longest input either parse accepts -- one padding short of @ref ARNM_MAX_ALLOC_SIZE.
 *
 * The non-insitu path copies the input plus @ref ARNM_JSON_READER_INSITU_PADDING bytes
 * through @ref arnm_alloc(), and that sum is what has to stay inside a `uint32_t`.
 */
#define ARNM_JSON_READER_MAX_INPUT_SIZE (ARNM_MAX_ALLOC_SIZE - ARNM_JSON_READER_INSITU_PADDING)

// ********** flags *******************

/*
 * There is one flag, and it is not an oversight.
 *
 * yyjson is compiled here with YYJSON_DISABLE_NON_STANDARD, which removes the code behind every
 * one of its YYJSON_READ_ALLOW_* switches rather than merely defaulting them off. A flag for
 * trailing commas, comments, `Infinity`, `NaN` or a byte order mark would therefore be a bit
 * this reader could accept, translate and hand over to a parser that no longer has anything to
 * do with it -- accepted at init, and then quietly without effect at every parse. Those flags
 * were removed for that reason: a switch that cannot be honoured is worse than an absent one,
 * because the caller has no way to find out.
 *
 * What that leaves is a parser that is strict about the grammar and cannot be asked not to be.
 * A document carrying any of those extensions is refused, and it is refused the same way
 * whatever this reader was initialised with.
 *
 * One thing moved the other way. The same build also sets YYJSON_DISABLE_UTF8_VALIDATION, so a
 * string is never checked against UTF-8 and malformed bytes are always carried through. That is
 * what the removed ALLOW_INVALID_UNICODE used to ask for, and it is now simply how this reader
 * behaves; a caller that needs well formed text has to validate it itself.
 *
 * Turning any of it back on is a build change and not a call site one -- drop the macro in
 * build.zig and CMakeLists.txt, and the flag that belongs to it can come back here.
 */

/** @brief Bit set of `ARNM_JSON_READ_*`, handed to @ref arnm_json_reader_init(). */
typedef uint32_t arnm_json_read_flags;

/**
 * @brief Strict RFC 8259, with the one exception this build cannot help: no comments, no
 *        trailing commas, no inf, no nan, no byte order mark -- but no UTF-8 validation either.
 */
#define ARNM_JSON_READ_DEFAULT ((arnm_json_read_flags)0u)
/** @brief Stop at the end of the first document instead of refusing what follows it. */
#define ARNM_JSON_READ_STOP_WHEN_DONE ((arnm_json_read_flags)(1u << 0))

// ********** types *******************

/** @brief The six shapes a JSON value can have, plus the answer for no value at all. */
typedef enum arnm_json_type {
  /** No value: a NULL handle, or a document that was never parsed. */
  ARNM_JSON_TYPE_NONE = 0,
  /** The literal `null`. Distinct from @ref ARNM_JSON_TYPE_NONE, which is the absence of a
   *  value rather than a value that is empty. */
  ARNM_JSON_TYPE_NULL,
  /** `true` or `false`; read it with @ref arnm_json_read_bool(). */
  ARNM_JSON_TYPE_BOOL,
  /** A number. JSON has only one; @ref arnm_json_value_number_type() says which C type holds
   *  it without loss. */
  ARNM_JSON_TYPE_NUMBER,
  /** A string, unescaped and NUL terminated by the parse. */
  ARNM_JSON_TYPE_STRING,
  /** An array; walk it with @ref arnm_json_reader_enter_at() or @ref arnm_json_array_get(). */
  ARNM_JSON_TYPE_ARRAY,
  /** An object; read its members with `arnm_json_reader_get_*()` or walk it with
   *  @ref arnm_json_object_iter_next(). */
  ARNM_JSON_TYPE_OBJECT
} arnm_json_type;

/**
 * @brief Which C type carries a number without loss.
 *
 * JSON draws no line between an integer and a fraction; the parser does, by looking at how
 * the number was written and at whether it still fits. This is that verdict, and it decides
 * which of @ref arnm_json_read_uint64(), @ref arnm_json_read_int64() and
 * @ref arnm_json_read_double() answers exactly rather than approximately.
 */
typedef enum arnm_json_number_type {
  /** The value is not a number. */
  ARNM_JSON_NUMBER_TYPE_NONE = 0,
  /** A non-negative integer, exact in a `uint64_t`. */
  ARNM_JSON_NUMBER_TYPE_UINT,
  /** A negative integer, exact in an `int64_t`. */
  ARNM_JSON_NUMBER_TYPE_SINT,
  /** Written with a fraction or an exponent, or too large for an integer: a `double`. */
  ARNM_JSON_NUMBER_TYPE_REAL
} arnm_json_number_type;

/**
 * @brief A reader: one allocator, one document, one place in it, and the first error it saw.
 *
 * Opaque by construction -- the bytes carry a layout that lives entirely in `json_reader.c`.
 * The union is not a choice between members but an alignment floor: a bare `uint8_t` array
 * would be one byte aligned, and the layout inside holds pointers.
 *
 * Not usable until @ref arnm_json_reader_init() or @ref arnm_json_reader_create(). A zeroed
 * reader reads as one that was never initialized and refuses every call with
 * @ref ARNM_ERROR_NOT_INITIALIZED.
 */
typedef struct arnm_json_reader {
  union {
    uint8_t bytes[ARNM_JSON_READER_SIZE]; /**< Opaque; never read these directly. */
    void *alignment_pointer;              /**< Never read. Present for its alignment alone. */
    uint64_t alignment_integer;           /**< Never read. Present for its alignment alone. */
  } opaque;                               /**< The storage itself. Never named by a caller. */
} arnm_json_reader;

/**
 * @brief A value inside a parsed document -- incomplete on purpose.
 *
 * Only ever handled through a pointer, and that pointer is owned by the document. Nothing
 * here is allocated, copied or released by a caller; the whole tree comes and goes with the
 * reader that parsed it.
 */
typedef struct arnm_json_value arnm_json_value;

/**
 * @brief A walk through an array, its position kept outside the document.
 *
 * Opaque and sized, like @ref arnm_json_reader, so it can live on the stack. Valid only for
 * as long as the document it was opened on.
 */
typedef struct arnm_json_array_iter {
  union {
    uint8_t bytes[32];          /**< Opaque; never read these directly. */
    void *alignment_pointer;    /**< Never read. Present for its alignment alone. */
    uint64_t alignment_integer; /**< Never read. Present for its alignment alone. */
  } opaque;                     /**< The storage itself. Never named by a caller. */
} arnm_json_array_iter;

/**
 * @brief A walk through an object, key and value arriving together.
 *
 * Opaque and sized, like @ref arnm_json_reader, so it can live on the stack. Valid only for
 * as long as the document it was opened on.
 */
typedef struct arnm_json_object_iter {
  union {
    uint8_t bytes[40];          /**< Opaque; never read these directly. */
    void *alignment_pointer;    /**< Never read. Present for its alignment alone. */
    uint64_t alignment_integer; /**< Never read. Present for its alignment alone. */
  } opaque;                     /**< The storage itself. Never named by a caller. */
} arnm_json_object_iter;

// ********** manage the reader itself *******************

/**
 * @brief Prepare a reader in storage the caller owns. Allocates nothing.
 *
 * The allocator and the flags are only remembered here; the first parse is what draws from the
 * one and reads under the other. Strings are borrowed until
 * @ref arnm_json_reader_set_output_allocator() says otherwise, and the error the reader carries
 * starts at @ref ARNM_SUCCESS.
 *
 * @param[out]    reader    Storage to initialize; not NULL. Every field is written and none is
 *                          read, so uninitialized storage is a valid input.
 * @param[in,out] allocator Where documents will come from, or NULL for the host. Kept for the
 *                          reader's whole life.
 * @param[in]     flags     Bit set of `ARNM_JSON_READ_*` every later parse reads under, or
 *                          @ref ARNM_JSON_READ_DEFAULT.
 * @retval ARNM_SUCCESS             Ready, holding no document.
 * @retval ARNM_ERROR_NULL_POINTER  @p reader is NULL.
 * @retval ARNM_ERROR_INVALID_PARAM @p flags holds a bit this header does not define; @p reader
 *                                  is left untouched and stays uninitialized.
 * @warning Calling this on a reader that still holds a document strands it. Use
 *          @ref arnm_json_reader_release() first.
 * @warning An initialized reader may not be moved or copied to another address. A document
 *          reaches its allocator back through the reader it was parsed by, and that route is
 *          the reader's address. Where a reader has to change places, release it, move the
 *          storage, and initialize it again.
 * @whisper A still surface, waiting for the first stone
 */
arnm_result arnm_json_reader_init(
    arnm_json_reader *reader, arnm *allocator, arnm_json_read_flags flags
);

/**
 * @brief Carve a reader out of @p allocator and initialize it against that same allocator.
 *
 * For code that wants the reader itself to come from ground it controls rather than from the
 * host. The state and every document it later holds are drawn from the one allocator, so
 * @ref arnm_json_reader_destroy() needs no second one.
 *
 * @param[in,out] allocator Where the state's @ref ARNM_JSON_READER_SIZE bytes and every later
 *                          document come from, or NULL for the host.
 * @param[in]     flags     As @ref arnm_json_reader_init().
 * @return The new reader, or NULL if @p allocator had no room or @p flags holds an unknown bit.
 * @note Give it back with @ref arnm_json_reader_destroy().
 * @whisper A vessel shaped from the ground it will stand on
 */
arnm_json_reader *arnm_json_reader_create(arnm *allocator, arnm_json_read_flags flags);

/**
 * @brief Say where the strings a getter hands back should live.
 *
 * Without this, and with @p allocator NULL, a string points into the document and is gone the
 * moment it is released -- nothing is copied and nothing has to be given back. With an
 * allocator named, every later `arnm_json_reader_get_string*()` copies its bytes there and NUL
 * terminates them, so the result outlives the document, the reader, and the buffer an insitu
 * parse read through. The copies are given back the way an arena gives anything back: all at
 * once, at @ref arnm_reset() or @ref arnm_release(), never one string at a time.
 *
 * Only the reader level getters copy. @ref arnm_json_read_string() borrows either way, which is
 * what it is for.
 *
 * @param[in,out] reader    Reader to change; not NULL and initialized.
 * @param[in,out] allocator Where copies go, or NULL to borrow again. This is the one place in
 *                          arnm where NULL does not mean the host: a copy nobody can free one
 *                          by one belongs in an arena, and borrowing is the cheaper answer
 *                          anyway.
 * @retval ARNM_SUCCESS               Set; every later getter follows it.
 * @retval ARNM_ERROR_NULL_POINTER    @p reader is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p reader was never initialized.
 * @note Strings already handed out keep whatever they were. This decides the next one, not the
 *       last one.
 * @whisper Water carried away in a vessel, or drunk where it runs
 */
arnm_result arnm_json_reader_set_output_allocator(arnm_json_reader *reader, arnm *allocator);

/**
 * @brief Let go of the document, keep the reader.
 *
 * Everything the parse allocated goes back to the allocator it came from; the reader itself
 * survives, still bound to that allocator and to its flags, and can parse again. Behind an
 * arena the document buffer is given back from the tail where it can be, and waits for
 * @ref arnm_reset() where it cannot -- see the note on @ref arnm_json_reader_parse().
 *
 * The recorded error survives, so a status may still be asked for afterwards; the next parse is
 * what clears it. Strings that were copied into the output allocator survive too -- they were
 * never the document's to hold.
 *
 * @param[in,out] reader Reader to empty; NULL is a no-op, and so is a reader holding nothing.
 * @retval ARNM_SUCCESS Released, or there was nothing to release.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED The document was released and an arena kept
 *                     part of its bytes until that arena resets.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p reader was never initialized.
 * @warning Every @ref arnm_json_value the reader handed out is dangling afterwards, and so is
 *          every borrowed string.
 * @whisper The tree is let go, the ground stays
 */
arnm_result arnm_json_reader_release(arnm_json_reader *reader);

/**
 * @brief @ref arnm_json_reader_release(), then the reader's own bytes.
 *
 * @param[in,out] reader    From @ref arnm_json_reader_create(), never stack or static storage;
 *                          may be NULL.
 * @param[in,out] allocator The allocator @p reader was carved from -- the same one
 *                          @ref arnm_json_reader_create() was handed, NULL for the host.
 * @retval ARNM_SUCCESS Released and given back, or @p reader was NULL.
 * @retval ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED Everything was released; an arena kept some
 *                     of it, the reader's own bytes included, until that arena resets.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p reader was never initialized.
 * @warning @p allocator is not remembered from @ref arnm_json_reader_create() and has to
 *          match. Naming NULL for memory an arena gave reaches the host unchecked. See
 *          @ref arnm_free().
 * @whisper Last of all, the vessel itself
 */
arnm_result arnm_json_reader_destroy(arnm_json_reader *reader, arnm *allocator);

// ********** parsing *******************

/**
 * @brief Parse @p json into a fresh document, leaving the caller's bytes untouched.
 *
 * The input is copied first, and that copy becomes the pool the parsed strings point into --
 * so @p json may be `const`, may be freed the moment this returns, and the document stands on
 * its own. Two allocations behind an arena: the copy, then the value buffer above it. Any
 * document the reader already held is released before the new one is built, whether or not
 * the parse then succeeds.
 *
 * A parse is where the reader starts over: the recorded error goes back to @ref ARNM_SUCCESS,
 * the field name is emptied, and the current value becomes the root of what was just read. A
 * refusal here is the first error the reader carries, which is why the result may be ignored
 * and asked for later through @ref arnm_json_reader_status().
 *
 * @param[in,out] reader Reader to fill; not NULL and initialized.
 * @param[in]     json   Bytes to read; not NULL. Need not be NUL terminated.
 * @param[in]     length Bytes in @p json; must be > 0 and at most
 *                       @ref ARNM_JSON_READER_MAX_INPUT_SIZE.
 * @retval ARNM_SUCCESS                   Parsed; @ref arnm_json_reader_root() answers the root.
 * @retval ARNM_ERROR_NULL_POINTER        @p reader or @p json is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED     @p reader was never initialized.
 * @retval ARNM_ERROR_INVALID_PARAM       @p length is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p length exceeds
 *                                        @ref ARNM_JSON_READER_MAX_INPUT_SIZE.
 * @retval ARNM_ERROR_OUT_OF_MEMORY       The allocator had nothing left.
 * @retval ARNM_ERROR_DECODE_FAILED       The bytes are not the JSON the flags allow.
 *                                        @ref arnm_json_reader_error_message() and
 *                                        @ref arnm_json_reader_error_position() say where and
 *                                        what.
 * @note Behind an arena the string pool is allocated first and the value buffer above it, so
 *       releasing gives the value buffer back and leaves the pool buried until
 *       @ref arnm_reset(). @ref arnm_json_reader_parse_insitu() has no pool and does not pay
 *       this.
 * @whisper Bytes settle into branches, and the branches hold
 */
arnm_result arnm_json_reader_parse(arnm_json_reader *reader, const char *json, uint32_t length);

/**
 * @brief Parse in place, spending @p buffer instead of copying it.
 *
 * The parser unescapes strings into the caller's own bytes and terminates them there, so no
 * string pool is allocated and a document costs exactly one allocation. What that buys has a
 * price in three parts, all of them the caller's to keep:
 *
 * - @p buffer is modified. What it held is gone by the time this returns.
 * - @p buffer must outlive the document. Every string in the tree points into it, unless an
 *   output allocator was named and the getters copy.
 * - @p buffer must have @ref ARNM_JSON_READER_INSITU_PADDING bytes beyond @p length, which
 *   the parse overwrites with zeroes.
 *
 * Behind an arena this is the shape that gives everything back: the single allocation sits at
 * the tail, so @ref arnm_json_reader_release() moves the index all the way home.
 *
 * @param[in,out] reader   Reader to fill; not NULL and initialized.
 * @param[in,out] buffer   Bytes to read and to write through; not NULL.
 * @param[in]     length   JSON bytes in @p buffer, padding excluded; must be > 0 and at most
 *                         @ref ARNM_JSON_READER_MAX_INPUT_SIZE.
 * @param[in]     capacity Bytes @p buffer really holds; must be at least
 *                         `length + ARNM_JSON_READER_INSITU_PADDING`.
 * @retval ARNM_SUCCESS             Parsed; @ref arnm_json_reader_root() answers the root.
 * @retval ARNM_ERROR_INVALID_PARAM @p length is 0, or @p capacity leaves no room for the
 *                                  padding.
 * @return Otherwise what @ref arnm_json_reader_parse() answers, for the same reasons.
 * @warning @p buffer is read through by every borrowed string in the document. Freeing it,
 *          reusing it or letting it fall out of scope before
 *          @ref arnm_json_reader_release() leaves the tree pointing at memory that is no
 *          longer yours.
 * @whisper The riverbed is reshaped by the water it carries
 */
arnm_result arnm_json_reader_parse_insitu(
    arnm_json_reader *reader, char *buffer, uint32_t length, uint32_t capacity
);

// ********** what the reader carries *******************

/**
 * @brief The first error since the last parse, or @ref ARNM_SUCCESS while nothing went wrong.
 *
 * The one call a mapper has to make. Every refusal a getter, an
 * @ref arnm_json_reader_enter() or a parse ran into is recorded here once and then left alone,
 * so this is the earliest thing that went wrong and not the latest -- the later ones are mostly
 * consequences of it.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The recorded error, @ref ARNM_SUCCESS when there is none, or
 *         @ref ARNM_ERROR_NOT_INITIALIZED for NULL and for a reader that was never initialized.
 * @whisper The first stumble, kept while the walking goes on
 */
arnm_result arnm_json_reader_status(const arnm_json_reader *reader);

/**
 * @brief The field name the recorded error happened at.
 *
 * A key for a member, `"[3]"` for an array element, and the empty string for anything that
 * belongs to no field -- a parse that failed, or a reader with nothing to report. Truncated to
 * @ref ARNM_JSON_READER_FIELD_NAME_SIZE bytes including the terminator.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The name, never NULL, valid until the next parse or
 *         @ref arnm_json_reader_clear_error(). The empty string for NULL and for an
 *         uninitialized reader.
 * @whisper The name of the branch that would not bear weight
 */
const char *arnm_json_reader_error_field(const arnm_json_reader *reader);

/**
 * @brief Forget the recorded error and let the reading count again.
 *
 * For a mapper that wants to go on deliberately -- reading an optional section, reporting what
 * it found, and continuing with the next one. The document, the current value and the output
 * allocator are all untouched; only the verdict is cleared.
 *
 * @param[in,out] reader Reader to clear; not NULL and initialized.
 * @retval ARNM_SUCCESS               Cleared, whether or not there was anything to clear.
 * @retval ARNM_ERROR_NULL_POINTER    @p reader is NULL.
 * @retval ARNM_ERROR_NOT_INITIALIZED @p reader was never initialized.
 * @whisper The slate wiped, the walk resumed
 */
arnm_result arnm_json_reader_clear_error(arnm_json_reader *reader);

/**
 * @brief What the last parse said when it refused.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return A static, never owned string describing the last @ref ARNM_ERROR_DECODE_FAILED, or
 *         `"no error"` when the last parse succeeded and when there was none. Never NULL.
 * @note This describes the parse alone. A refusal at a field is described by
 *       @ref arnm_json_reader_status() and @ref arnm_json_reader_error_field().
 * @whisper Where the reading stopped, in words
 */
const char *arnm_json_reader_error_message(const arnm_json_reader *reader);

/**
 * @brief Byte offset the last parse stopped at.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The offset into the input where the refusal happened, or 0 when the last parse
 *         succeeded and when there was none. An offset of 0 is also a real position, so read
 *         it together with the @ref arnm_result the parse answered.
 */
uint32_t arnm_json_reader_error_position(const arnm_json_reader *reader);

/**
 * @brief Whether a parse succeeded and its document is still held.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return true only while a document stands. NULL, uninitialized and released all answer
 *         false.
 */
bool arnm_json_reader_has_document(const arnm_json_reader *reader);

/**
 * @brief Values the document holds, root included.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The count, or 0 without a document. Every array element, object key and object
 *         value counts as one.
 */
uint32_t arnm_json_reader_value_count(const arnm_json_reader *reader);

/**
 * @brief Input bytes the last parse consumed.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return Bytes read, or 0 without a document. Equal to the input length except under
 *         @ref ARNM_JSON_READ_STOP_WHEN_DONE, where it marks the end of the first document
 *         and the start of whatever follows it.
 */
uint32_t arnm_json_reader_bytes_read(const arnm_json_reader *reader);

// ********** where the reader stands *******************

/**
 * @brief The root of the document the reader holds.
 *
 * @param[in] reader Reader to look into; may be NULL.
 * @return The root value, or NULL when @p reader is NULL, uninitialized, or holds no
 *         document. The root is whatever the text was -- an object and an array are the
 *         common ones, but a bare number or string is a document too.
 */
arnm_json_value *arnm_json_reader_root(const arnm_json_reader *reader);

/**
 * @brief The value the getters are reading members of.
 *
 * The root after a successful parse, whatever @ref arnm_json_reader_enter() last moved to
 * otherwise. NULL once an access has failed, which is what makes the getters after it quiet.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The current value, or NULL.
 */
arnm_json_value *arnm_json_reader_current(const arnm_json_reader *reader);

/**
 * @brief Members of the current value: elements of an array, pairs of an object.
 *
 * The bound of a loop over an array, and it answers 0 exactly where such a loop should not run
 * -- no document, a scalar, or an error already recorded. Asking costs nothing and records
 * nothing.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @return The count, or 0.
 */
uint32_t arnm_json_reader_count(const arnm_json_reader *reader);

/**
 * @brief Whether the current value holds @p key with something to read.
 *
 * The test an optional member is written with: `arnm_json_reader_has(r, "note") ?
 * arnm_json_reader_get_string(r, "note") : NULL`. A member present as the literal `null`
 * answers false, because for a mapper a null and an absent member mean the same thing -- and a
 * getter would refuse both.
 *
 * Nothing is recorded either way. A key that is missing is an answer here, not an error.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @param[in] key    Member to look for; may be NULL, which asks about the current value itself.
 * @return true when the member is there and is not `null`.
 * @whisper Asked before it is needed, and never held against anyone
 */
bool arnm_json_reader_has(const arnm_json_reader *reader, const char *key);

/**
 * @brief The JSON type of a member of the current value.
 *
 * For a mapper that has to look before it reads -- a field that is a number in one document and
 * a string in the next. Nothing is recorded; a missing member is @ref ARNM_JSON_TYPE_NONE.
 *
 * @param[in] reader Reader to ask; may be NULL.
 * @param[in] key    Member to look at; may be NULL, which asks about the current value itself.
 * @return The type, or @ref ARNM_JSON_TYPE_NONE when there is no such member.
 */
arnm_json_type arnm_json_reader_type_of(const arnm_json_reader *reader, const char *key);

/**
 * @brief Step into a member and hand back the value that was left.
 *
 * The current value becomes the member @p key names, and the return is what the current value
 * was -- pass it to @ref arnm_json_reader_leave() to come back up. Nothing is allocated and
 * nothing is pushed anywhere: the stack is the caller's own, one local per level.
 *
 * Where the step cannot be taken -- no document, the current value is not an object, the key is
 * missing -- the error is recorded, the current value becomes NULL, and everything read below
 * this level answers empty until the matching leave. The return is still the value that was
 * left, so the pairing holds on every path.
 *
 * @param[in,out] reader Reader to move; may be NULL.
 * @param[in]     key    Member to step into; not NULL.
 * @return The value that was current, to be handed to @ref arnm_json_reader_leave().
 * @whisper Down one branch, with the way back in hand
 */
arnm_json_value *arnm_json_reader_enter(arnm_json_reader *reader, const char *key);

/**
 * @brief Step into the element at @p index of the current array.
 *
 * As @ref arnm_json_reader_enter(), by position rather than by name. Elements sit in a chain
 * rather than in a table, and the reader remembers where it stood: walking an array in order
 * costs one step per element, while jumping backwards starts the walk again from the front.
 * A loop from 0 upwards is therefore linear in the array, not quadratic.
 *
 * @param[in,out] reader Reader to move; may be NULL.
 * @param[in]     index  Position in the current array, counted from 0.
 * @return The value that was current, to be handed to @ref arnm_json_reader_leave().
 * @note Out of bounds records @ref ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS under the field name
 *       `"[index]"`, and a current value that is no array records
 *       @ref ARNM_ERROR_INVALID_ENUM_TYPE.
 * @whisper Along the chain, one link at a time, remembering the last
 */
arnm_json_value *arnm_json_reader_enter_at(arnm_json_reader *reader, uint32_t index);

/**
 * @brief Come back to the value an enter handed over.
 *
 * Restores the current value and nothing else -- a recorded error stays recorded, which is what
 * lets a whole nested read run to the end before anyone asks how it went.
 *
 * @param[in,out] reader Reader to move; may be NULL.
 * @param[in]     value  What the matching @ref arnm_json_reader_enter() returned. NULL is
 *                       allowed and means the reading below this point answers empty from here
 *                       on.
 * @whisper Back up the branch, to where the light came from
 */
void arnm_json_reader_leave(arnm_json_reader *reader, arnm_json_value *value);

// ********** reserving the output arena *******************

/**
 * @brief Bytes an output arena needs for every string in the document.
 *
 * Counted from the current value downwards, exactly as
 * @ref arnm_json_reader_set_output_allocator() would spend them: each string costs its own
 * bytes, one terminator, and the rounding up to eight that an arena charges. Nothing is walked
 * twice and nothing is measured -- the parser already knows every length, so this is a scan over
 * the values and an addition per string.
 *
 * The number is exact for what it covers, and what it covers is *every* string: a document that
 * carries members a mapper never reads pays for them here. Where that matters,
 * @ref arnm_json_reader_output_size_for_keys() counts only the members that will be asked for.
 *
 * Object keys are not counted. A key is never copied -- it is the caller's own string that goes
 * into a getter, and only the value comes back.
 *
 * @param[in] reader Reader to measure; may be NULL.
 * @return Bytes to hand to @ref arnm_init_arena(), or 0 without a document and for a document
 *         holding no string at all. Saturates at @ref ARNM_MAX_ALLOC_SIZE, which no arena can
 *         be built with anyway.
 * @note Measured from the current value, so entering a member first measures that member alone.
 * @whisper The whole tree weighed before a single leaf is carried
 */
uint32_t arnm_json_reader_output_size(const arnm_json_reader *reader);

/**
 * @brief Bytes an output arena needs for the members a mapper actually reads.
 *
 * As @ref arnm_json_reader_output_size(), counting only the string values whose member name is
 * one of @p keys. A name is looked for at every depth below the current value, so an array of
 * objects is covered by naming its member once -- `"name"` counts the name of every element.
 *
 * What has no member name is never counted: a string at the root, and a string that is an array
 * element read through a NULL key. Where those are copied too, measure with
 * @ref arnm_json_reader_output_size() instead.
 *
 * @param[in] reader Reader to measure; may be NULL.
 * @param[in] keys   Member names to count, NUL terminated each; may be NULL, which counts
 *                   nothing. A NULL entry inside the array is skipped.
 * @param[in] count  Entries in @p keys. A mapper naming more than 255 members has outgrown one
 *                   call, and two calls add up.
 * @return Bytes to hand to @ref arnm_init_arena(), or 0 when nothing matched. Saturates at
 *         @ref ARNM_MAX_ALLOC_SIZE.
 * @note The same key named twice counts its matches twice. The list is read as given, not as a
 *       set.
 * @whisper Only the branches that will be carried are weighed
 */
uint32_t arnm_json_reader_output_size_for_keys(
    const arnm_json_reader *reader, const char *const *keys, uint8_t count
);

// ********** reading a field, one line per struct member *******************

/*
 * Every getter below reads the member @p key names inside the current value, or the current
 * value itself when @p key is NULL. Every one of them answers its empty value -- NULL, false,
 * 0 -- and records the first of these under @p key, instead of returning it:
 *
 *   ARNM_ERROR_INVALID_STATE       no document is held
 *   ARNM_ERROR_INVALID_ENUM_TYPE   the current value is no object, or the member has another
 *                                  JSON type than the getter reads
 *   ARNM_ERROR_INVALID_PARAM       the current value holds no such member
 *   ARNM_ERROR_ARITHMETIC_OVERFLOW the number does not fit the type asked for
 *   ARNM_ERROR_OUT_OF_MEMORY       a string had to be copied and the output allocator was full
 *
 * Once anything is recorded, the getters stop looking and simply answer empty, so a run of them
 * after a failure costs nothing and reports nothing new. Ask @ref arnm_json_reader_status()
 * when the struct is filled.
 *
 * A reader that was never initialized has nowhere to record anything; it answers empty all the
 * same, and @ref arnm_json_reader_status() says @ref ARNM_ERROR_NOT_INITIALIZED for it.
 */

/**
 * @brief Read a string member, borrowed or copied.
 *
 * Where no output allocator was named, the bytes belong to the document and die with it. Where
 * one was, they are copied there and NUL terminated and outlive everything the reader holds --
 * see @ref arnm_json_reader_set_output_allocator().
 *
 * @param[in,out] reader Reader to read from; may be NULL.
 * @param[in]     key    Member to read; NULL reads the current value itself.
 * @return The string, or NULL when it could not be read.
 * @note A JSON string may hold an embedded NUL. Where that matters, take the length from
 *       @ref arnm_json_reader_get_string_length() rather than from `strlen`.
 * @whisper A name lifted out whole, or read where it lies
 */
const char *arnm_json_reader_get_string(arnm_json_reader *reader, const char *key);

/**
 * @brief @ref arnm_json_reader_get_string() with the byte length beside it.
 *
 * @param[in,out] reader     Reader to read from; may be NULL.
 * @param[in]     key        Member to read; NULL reads the current value itself.
 * @param[out]    out_length Receives the length, terminator excluded; may be NULL. Written only
 *                           when a string is returned.
 * @return The string, or NULL when it could not be read.
 */
const char *arnm_json_reader_get_string_length(
    arnm_json_reader *reader, const char *key, uint32_t *out_length
);

/**
 * @brief Read a boolean member.
 *
 * @param[in,out] reader Reader to read from; may be NULL.
 * @param[in]     key    Member to read; NULL reads the current value itself.
 * @return The value, or false when it could not be read. false is also a valid reading, so a
 *         document where the difference matters is checked with
 *         @ref arnm_json_reader_status().
 */
bool arnm_json_reader_get_bool(arnm_json_reader *reader, const char *key);

/**
 * @brief Read a number member as a signed 64 bit integer.
 *
 * A number written without fraction or exponent arrives exactly. One written as a real arrives
 * only if it has no fractional part and fits; anything else records
 * @ref ARNM_ERROR_ARITHMETIC_OVERFLOW rather than rounding, because a truncation that nobody
 * was told about is the expensive kind.
 *
 * @param[in,out] reader Reader to read from; may be NULL.
 * @param[in]     key    Member to read; NULL reads the current value itself.
 * @return The value, or 0 when it could not be read.
 * @whisper What cannot be carried whole is not carried at all
 */
int64_t arnm_json_reader_get_int64(arnm_json_reader *reader, const char *key);

/**
 * @brief Read a number member as an unsigned 64 bit integer.
 *
 * @param[in,out] reader Reader to read from; may be NULL.
 * @param[in]     key    Member to read; NULL reads the current value itself.
 * @return The value, or 0 when it could not be read. A negative number is out of range and
 *         records @ref ARNM_ERROR_ARITHMETIC_OVERFLOW.
 */
uint64_t arnm_json_reader_get_uint64(arnm_json_reader *reader, const char *key);

/**
 * @brief @ref arnm_json_reader_get_int64() narrowed to 32 bits.
 *
 * @param[in,out] reader Reader to read from; may be NULL.
 * @param[in]     key    Member to read; NULL reads the current value itself.
 * @return The value, or 0 when it could not be read or does not fit an `int32_t`.
 */
int32_t arnm_json_reader_get_int32(arnm_json_reader *reader, const char *key);

/**
 * @brief @ref arnm_json_reader_get_uint64() narrowed to 32 bits.
 *
 * @param[in,out] reader Reader to read from; may be NULL.
 * @param[in]     key    Member to read; NULL reads the current value itself.
 * @return The value, or 0 when it could not be read or does not fit a `uint32_t`.
 */
uint32_t arnm_json_reader_get_uint32(arnm_json_reader *reader, const char *key);

/**
 * @brief Read a number member as a `double`.
 *
 * Every JSON number converts, which is what separates this from the integer getters: an integer
 * beyond 2^53 arrives rounded rather than refused.
 *
 * @param[in,out] reader Reader to read from; may be NULL.
 * @param[in]     key    Member to read; NULL reads the current value itself.
 * @return The value, or 0.0 when it could not be read.
 */
double arnm_json_reader_get_double(arnm_json_reader *reader, const char *key);

// ********** reader functions for the shapes a string carries *******************

/*
 * JSON has no type for bytes, so a document that carries them spells them: hex where a person
 * will compare the value against another tool's output, base64 where a payload only has to
 * survive the trip, and the canonical 8-4-4-4-12 for a uuid. The writer puts all three into the
 * document with a call of its own -- @ref arnm_json_writer_add_hex(),
 * @ref arnm_json_writer_add_base64(), @ref arnm_json_writer_add_uuid() -- and these are what
 * reads them back.
 *
 * Each is the string read and the conversion in one call, which is what saves the caller the
 * length: the document knows how long the string is, and every one of these has to check that
 * length before it converts anything. Spelled out at the call site instead, that check is three
 * lines that every field repeats and any field can forget.
 *
 * A string that does not spell what it should is answered with @ref ARNM_ERROR_DECODE_FAILED
 * throughout, including where the converter these sit on calls the same condition
 * @ref ARNM_ERROR_INVALID_PARAM: what arrives here is a document, and a document that is wrong
 * is a decode that failed, not a parameter a caller passed by mistake.
 */

/**
 * @brief Read a hex string into bytes, however many it spells.
 *
 * @param[in]  value    Value to read; not NULL.
 * @param[out] out      Receives the bytes; not NULL. Untouched unless the call succeeds.
 * @param[in]  capacity Bytes @p out holds. A string that spells more is refused rather than
 *                      truncated.
 * @param[out] out_size Receives the bytes written; may be NULL to skip it.
 * @retval ARNM_SUCCESS                 The bytes are in @p out.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not a string.
 * @retval ARNM_ERROR_DECODE_FAILED     The string has an odd number of characters, is empty,
 *                                      spells more bytes than @p capacity holds, holds a
 *                                      character that is no hex digit, or carries a NUL of its
 *                                      own and so ends before the document says it does. An
 *                                      empty string spells no bytes, and no bytes is not a
 *                                      length this reads -- the same answer
 *                                      @ref arnm_binary_from_hex() gives it.
 * @whisper However long it runs, it says the same thing twice over
 */
arnm_result arnm_json_read_hex(
    const arnm_json_value *value, uint8_t *out, uint32_t capacity, uint32_t *out_size
);

/**
 * @brief Read a hex string into a field whose length is fixed.
 *
 * The length is the check: a public key, a hash, a signature is as long as its type says, and a
 * string of any other length is refused before a byte of it is converted. @ref
 * arnm_json_read_hex() is the one to reach for where the field's length is the document's to
 * decide.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   @p size bytes; not NULL. Untouched unless the call succeeds.
 * @param[in]  size  Bytes the field holds; the string has to be exactly twice as long.
 * @retval ARNM_SUCCESS                 The bytes are in @p out.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not a string.
 * @retval ARNM_ERROR_DECODE_FAILED     The string is not exactly @p size * 2 hex digits, or
 *                                      carries a NUL of its own and so ends before the document
 *                                      says it does.
 * @whisper A field that knows its own length asks the string to prove it
 */
arnm_result arnm_json_read_hex_fixed(const arnm_json_value *value, uint8_t *out, uint32_t size);

/**
 * @brief Read a uuid in the canonical 8-4-4-4-12 form into @ref ARNM_UUID_BINARY_SIZE bytes.
 *
 * The version and variant fields are not looked at, exactly as @ref arnm_uuid_from_string()
 * does not look at them: what is checked is the shape, because that is what the document could
 * get wrong.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   @ref ARNM_UUID_BINARY_SIZE bytes; not NULL. Untouched unless the call
 *                   succeeds.
 * @retval ARNM_SUCCESS                 The 16 bytes are in @p out.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not a string.
 * @retval ARNM_ERROR_DECODE_FAILED     The string is not @ref ARNM_UUID_STRING_LENGTH
 *                                      characters, or not that shape.
 */
arnm_result arnm_json_read_uuid(const arnm_json_value *value, uint8_t *out);

/**
 * @brief Read a base64 string of any length into a block drawn from @p memory.
 *
 * The one of these that does not know its size in advance, so the string is measured first --
 * by @ref arnm_base64_binary_size(), which reads the padding rather than assuming it -- and the
 * block is taken at exactly what the decode then writes. An arena charged two bytes it never
 * gives back is an arena that ends short of the read that follows it.
 *
 * @p out is cleared first, so a string of no characters leaves the empty block it came from and
 * costs no allocation at all.
 *
 * @param[out]    out    Block to fill; not NULL. Written in full, read not at all.
 * @param[in]     value  Value to read; not NULL.
 * @param[in,out] memory Where the bytes come from; NULL for the host allocator.
 * @retval ARNM_SUCCESS                 The bytes are in @p out, or there were none to take.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not a string.
 * @retval ARNM_ERROR_DECODE_FAILED     The string is not a whole number of four character
 *                                      groups, or holds a character outside the standard
 *                                      alphabet.
 * @return Otherwise what @ref arnm_memory_block_alloc() answers.
 * @warning The block outlives the document and belongs to @p memory, not to the reader:
 *          releasing the reader does not release it.
 * @whisper The characters are counted, and only then is the ground asked for
 */
arnm_result arnm_json_read_base64_block(
    arnm_memory_block *out, const arnm_json_value *value, arnm *memory
);

// ********** reading a whole object in one walk *******************

/*
 * A mapping that wants most of an object's members has a choice of two shapes, and only one of
 * them is linear. Asking for members by name walks the member chain once per question; walking
 * the object once and answering each key where it is met costs the chain a single time.
 *
 * The walk is easy to write by hand and easy to write differently every time: the length check
 * before the comparison, the type check before the conversion, the record of what was actually
 * there. @ref arnm_json_read_object() is that walk once, driven by a table the caller writes as
 * a list of fields -- what a key is called, what it should become, and where it goes.
 *
 * What the table does not do is nest. A member that is itself an object or an array is asked for
 * as @ref ARNM_JSON_FIELD_TYPE_VALUE, which hands the value over untouched; the caller walks
 * that one with a table of its own. A shape described all the way down would have to be built
 * before it could be used, and the two lines it saves are not worth what it costs to read.
 *
 * Nor does it decide what is required. Every field the walk filled is named in the mask it hands
 * back, and the caller reads that mask once for the whole object -- which is where the answer
 * belongs, because it is the same answer whichever way the object was read.
 */

/**
 * @brief What a field of a walk should become, and therefore what its target points at.
 *
 * The order of these is load bearing and not a matter of taste. @ref arnm_json_read_object()
 * sorts a tag into its family with two range tests rather than a table, so the integer types
 * are contiguous and end at @ref ARNM_JSON_FIELD_TYPE_INT32, and the types that read a JSON
 * string are contiguous and end at @ref ARNM_JSON_FIELD_TYPE_UUID. Inserting a member into
 * either run, or between them, moves the boundary the prefilter is written against.
 *
 * The three string types all point at an @ref arnm_memory_block, but they do not use it the
 * same way. STRING writes the descriptor and reads nothing from it; HEX_FIXED and UUID read it
 * -- the caller hands over the buffer to decode into, and its size is what the string is
 * measured against. Nothing here allocates, which is why the walk needs no allocator.
 */
typedef enum arnm_json_field_type {
  /** Refused wherever it is met, so a table assembled field by field cannot reach the walk with
   *  an entry nobody set. There is no placeholder tag; a member that is not wanted is left out
   *  of the table. */
  ARNM_JSON_FIELD_TYPE_NONE = 0,
  ARNM_JSON_FIELD_TYPE_UINT64, /**< `uint64_t *` */
  ARNM_JSON_FIELD_TYPE_INT64,  /**< `int64_t *` */
  ARNM_JSON_FIELD_TYPE_UINT32, /**< `uint32_t *`, refusing what will not fit */
  /** `int32_t *`, refusing what will not fit. Last integer type -- the prefilter reads this
   *  bound, so nothing may be inserted after it that is not an integer. */
  ARNM_JSON_FIELD_TYPE_INT32,
  /** `arnm_memory_block *`, filled with the document's own bytes and their length. Borrowed,
   *  not owned: it lives exactly as long as the document and is never freed. @c size carries
   *  the string's length here, not an allocation, so it must never be handed to
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
  ARNM_JSON_FIELD_TYPE_DOUBLE, /**< `double *` */
  ARNM_JSON_FIELD_TYPE_BOOL,   /**< `bool *` */
  ARNM_JSON_FIELD_TYPE_VALUE   /**< `arnm_json_value **`, handed over untouched. */
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
 * Where the compiler has no `_Generic` -- C99, C++, or a C11 that does not carry it -- the
 * check falls away and the cast is all that is left. The contract is the same either way; only
 * the moment it is caught moves from the build to the run, which is worth knowing when a table
 * is written in a C++ translation unit and only compiled as C elsewhere.
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

/** @brief The member itself, for an object or an array the caller walks in turn. */
#define ARNM_JSON_FIELD_VALUE(key, ptr)                                                            \
  ARNM_JSON_FIELD_MAKE(key, ARNM_JSON_FIELD_TYPE_VALUE, arnm_json_value **, ptr)

/** @brief An @ref arnm_memory_block over an array whose size the compiler already knows. */
#define ARNM_JSON_BLOCK_OF(array) {(uint8_t *)(array), (uint32_t)sizeof(array)}

/** @brief Fields one walk can carry, which is what a bit per field in a mask leaves room for. */
#define ARNM_JSON_FIELDS_MAX 64u

/**
 * @brief Walk @p object once and read every member @p fields names into where it points.
 *
 * The member chain is walked a single time, and each key is compared only against the entries
 * the walk has not filled yet -- it starts at the lowest of them and stops at the first match.
 * A table that lists its fields in the document's own order is therefore met one comparison per
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
 * from the document and the two decoded types write into buffers the caller already owns, so
 * every target is either the caller's storage or a view into a document that outlives the call.
 *
 * @param[in]     object    Object to walk; not NULL.
 * @param[in,out] fields    The table; not NULL. Targets are written; the descriptor of a
 *                          @ref ARNM_JSON_FIELD_TYPE_HEX_FIXED or
 *                          @ref ARNM_JSON_FIELD_TYPE_UUID entry is also read, for the buffer it
 *                          names and the size of it.
 * @param[in]     count     Entries in @p fields, at least 1 and at most
 *                          @ref ARNM_JSON_FIELDS_MAX.
 * @param[out]    out_found Bit @c i is set where entry @c i was read; may be NULL. Cleared
 *                          before anything else is asked, and written again when the walk stops
 *                          early, so a caller can see how far it came.
 * @retval ARNM_SUCCESS                   Walked; @p out_found says what was there.
 * @retval ARNM_ERROR_NULL_POINTER        @p object or @p fields is NULL, or a matched entry has
 *                                        no target.
 * @retval ARNM_ERROR_INVALID_PARAM       @p count is 0 or past @ref ARNM_JSON_FIELDS_MAX, or a
 *                                        matched entry carries @ref ARNM_JSON_FIELD_TYPE_NONE
 *                                        or a type this reader does not know.
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
arnm_result arnm_json_read_array(
    arnm_json_value *object, arnm_json_value **out_values, uint32_t capacity, uint32_t *out_found
);

// ********** what a value is *******************

/**
 * @brief The JSON type of a value.
 *
 * @param[in] value Value to ask; may be NULL.
 * @return One of @ref arnm_json_type, or @ref ARNM_JSON_TYPE_NONE for NULL.
 */
arnm_json_type arnm_json_value_type(const arnm_json_value *value);

/**
 * @brief Which C type holds this number exactly.
 *
 * @param[in] value Value to ask; may be NULL.
 * @return One of @ref arnm_json_number_type, or @ref ARNM_JSON_NUMBER_TYPE_NONE for NULL and
 *         for anything that is not a number.
 * @whisper Every number arrives already knowing its own shape
 */
arnm_json_number_type arnm_json_value_number_type(const arnm_json_value *value);

/**
 * @brief The enumerator's own spelling, for logs and assertion messages.
 *
 * @param[in] type Value to name.
 * @return A static string equal to the identifier -- `arnm_json_type_to_string(
 *         ARNM_JSON_TYPE_NULL)` is `"ARNM_JSON_TYPE_NULL"` -- or `"ARNM_JSON_TYPE_UNKNOWN"`
 *         for anything this header does not define. Never NULL, never owned by the caller.
 */
const char *arnm_json_type_to_string(arnm_json_type type);

// ********** read functions for the value level *******************

/*
 * A value handed over by a walk -- @ref arnm_json_object_iter_next(), @ref
 * arnm_json_array_iter_next(), @ref arnm_json_reader_current() -- is read by one of these, and
 * the walk is what found it: nothing here searches a document for a key, which is what makes
 * these the calls a mapping over a whole object reaches for. Where a field is looked up by name
 * instead, the reader level getters above say the same thing in one line.
 *
 * `null` has no reading of its own. It never carried anything a reading could hand back -- the
 * whole answer was in the result code -- so it was a predicate wearing a read function's shape,
 * and @ref arnm_json_value_type() was already the predicate.
 */

/**
 * @brief Read `true` or `false`.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   Receives the value; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_SUCCESS                  Read.
 * @retval ARNM_ERROR_NULL_POINTER       @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE  The value is of another JSON type.
 */
arnm_result arnm_json_read_bool(const arnm_json_value *value, bool *out);

/**
 * @brief Read a number as a signed 64 bit integer.
 *
 * A number written without fraction or exponent arrives exactly. One written as a real
 * arrives only if it has no fractional part and fits; anything else is refused rather than
 * rounded, because a truncation that nobody was told about is the expensive kind.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   Receives the value; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_SUCCESS                   Read exactly.
 * @retval ARNM_ERROR_NULL_POINTER        @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE   The value is not a number.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The number is outside `int64_t`, or has a fractional
 *                                        part.
 * @whisper What cannot be carried whole is not carried at all
 */
arnm_result arnm_json_read_int64(const arnm_json_value *value, int64_t *out);

/**
 * @brief Read a number as an unsigned 64 bit integer.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   Receives the value; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_SUCCESS                   Read exactly.
 * @retval ARNM_ERROR_NULL_POINTER        @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE   The value is not a number.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The number is negative, outside `uint64_t`, or has a
 *                                        fractional part.
 */
arnm_result arnm_json_read_uint64(const arnm_json_value *value, uint64_t *out);

/**
 * @brief @ref arnm_json_read_int64() narrowed to 32 bits, refusing what will not fit.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   Receives the value; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The number is outside `int32_t`.
 * @return Otherwise what @ref arnm_json_read_int64() answers.
 */
arnm_result arnm_json_read_int32(const arnm_json_value *value, int32_t *out);

/**
 * @brief @ref arnm_json_read_uint64() narrowed to 32 bits, refusing what will not fit.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   Receives the value; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The number is outside `uint32_t`.
 * @return Otherwise what @ref arnm_json_read_uint64() answers.
 */
arnm_result arnm_json_read_uint32(const arnm_json_value *value, uint32_t *out);

/**
 * @brief Read a number as a `double`.
 *
 * Every JSON number converts, which is what separates this from the integer reads: an integer
 * beyond 2^53 arrives rounded rather than refused. Where that matters, ask
 * @ref arnm_json_value_number_type() first and take the exact path it names.
 *
 * @param[in]  value Value to read; not NULL.
 * @param[out] out   Receives the value; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_SUCCESS                 Converted.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not a number.
 * @note No document can put a non-finite value here: this build has no spelling for one, so
 *       every number that reaches this converted from digits and is finite.
 */
arnm_result arnm_json_read_double(const arnm_json_value *value, double *out);

/**
 * @brief Borrow a string, already unescaped and NUL terminated.
 *
 * Nothing is copied, whatever the reader's output allocator says -- this is the value level,
 * and the value level always borrows. The bytes live in the document -- in the parse's own
 * pool, or in the caller's buffer after @ref arnm_json_reader_parse_insitu() -- and stay
 * readable until the document goes. A JSON string may hold an embedded NUL, so @p out_length is
 * the length that counts and `strlen` is the one that lies.
 *
 * @param[in]  value      Value to read; not NULL.
 * @param[out] out        Receives the first byte; not NULL. Untouched unless the call
 *                        succeeds.
 * @param[out] out_length Receives the byte length, terminator excluded; may be NULL to skip
 *                        it.
 * @retval ARNM_SUCCESS                 Borrowed.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not a string.
 * @warning Borrowed, not owned. Copy it before the reader releases if it has to outlive the
 *          document.
 * @whisper Read from where it lies, never lifted away
 */
arnm_result arnm_json_read_string(
    const arnm_json_value *value, const char **out, uint32_t *out_length
);

// ********** arrays *******************

/**
 * @brief Elements in an array.
 *
 * @param[in] value Value to ask; may be NULL.
 * @return The element count, or 0 for NULL and for anything that is not an array.
 */
uint32_t arnm_json_array_size(const arnm_json_value *value);

/**
 * @brief The element at @p index.
 *
 * Elements sit in a chain rather than in a table, so reaching index n walks n links. Indexing
 * a whole array costs the square of its length; @ref arnm_json_array_iter_next() walks it
 * once, and @ref arnm_json_reader_enter_at() remembers where it stood. Use this to reach into
 * an array, not to go through one.
 *
 * @param[in]  value Array to read; not NULL.
 * @param[in]  index Position, counted from 0.
 * @param[out] out   Receives the element; not NULL. Untouched unless the call succeeds.
 * @retval ARNM_SUCCESS                          Found.
 * @retval ARNM_ERROR_NULL_POINTER               @p value or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE          The value is not an array.
 * @retval ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS  @p index is at or past the element count.
 */
arnm_result arnm_json_array_get(
    const arnm_json_value *value, uint32_t index, arnm_json_value **out
);

/**
 * @brief Open a walk through an array.
 *
 * @param[in]  value Array to walk; not NULL.
 * @param[out] iter  Iterator to initialize; not NULL. Every field is written and none is read.
 * @retval ARNM_SUCCESS                 Ready; an empty array simply ends at the first step.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p iter is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not an array.
 * @warning The iterator points into the document. Releasing or reparsing invalidates it.
 * @whisper A path opened along the branch
 */
arnm_result arnm_json_array_iter_init(const arnm_json_value *value, arnm_json_array_iter *iter);

/**
 * @brief Take the next element, or learn there is none.
 *
 * @param[in,out] iter From @ref arnm_json_array_iter_init(); not NULL.
 * @param[out]    out  Receives the element; not NULL. Untouched at the end of the walk.
 * @return true while an element was handed over, false once the array is spent and for a NULL
 *         argument. The iterator stays spent -- calling again keeps answering false.
 */
bool arnm_json_array_iter_next(arnm_json_array_iter *iter, arnm_json_value **out);

// ********** objects *******************

/**
 * @brief Key and value pairs in an object.
 *
 * @param[in] value Value to ask; may be NULL.
 * @return The pair count, or 0 for NULL and for anything that is not an object.
 */
uint32_t arnm_json_object_size(const arnm_json_value *value);

/**
 * @brief Look a key up.
 *
 * Keys sit in the order they were written and are compared one by one, so a lookup walks the
 * object. Where every key of an object is wanted, @ref arnm_json_object_iter_next() gets them
 * all in one pass.
 *
 * @param[in]  value Object to search; not NULL.
 * @param[in]  key   NUL terminated key to find; not NULL. A key holding an embedded NUL
 *                   cannot be named this way.
 * @param[out] out   Receives the value belonging to @p key; not NULL. Untouched unless the
 *                   call succeeds.
 * @retval ARNM_SUCCESS                 Found.
 * @retval ARNM_ERROR_NULL_POINTER      @p value, @p key or @p out is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not an object.
 * @retval ARNM_ERROR_INVALID_PARAM     The object holds no such key.
 * @whisper A name spoken, and the branch that answers to it
 */
arnm_result arnm_json_object_get(
    const arnm_json_value *value, const char *key, arnm_json_value **out
);

/**
 * @brief Open a walk through an object.
 *
 * @param[in]  value Object to walk; not NULL.
 * @param[out] iter  Iterator to initialize; not NULL. Every field is written and none is read.
 * @retval ARNM_SUCCESS                 Ready; an empty object simply ends at the first step.
 * @retval ARNM_ERROR_NULL_POINTER      @p value or @p iter is NULL.
 * @retval ARNM_ERROR_INVALID_ENUM_TYPE The value is not an object.
 * @warning The iterator points into the document. Releasing or reparsing invalidates it.
 */
arnm_result arnm_json_object_iter_init(const arnm_json_value *value, arnm_json_object_iter *iter);

/**
 * @brief Take the next pair, or learn there is none.
 *
 * Key and value arrive together, because that is how they lie in the document -- reaching one
 * without the other would cost the same walk twice.
 *
 * @param[in,out] iter           From @ref arnm_json_object_iter_init(); not NULL.
 * @param[out]    out_key        Receives the key bytes, NUL terminated; may be NULL to skip.
 * @param[out]    out_key_length Receives the key length, terminator excluded; may be NULL.
 * @param[out]    out_value      Receives the value; may be NULL to skip.
 * @return true while a pair was handed over, false once the object is spent and for a NULL
 *         @p iter. The outputs stay untouched at the end of the walk.
 * @warning The key is borrowed from the document, on the same terms as
 *          @ref arnm_json_read_string().
 * @whisper Name and branch, arriving as one
 */
bool arnm_json_object_iter_next(
    arnm_json_object_iter *iter,
    const char **out_key,
    uint32_t *out_key_length,
    arnm_json_value **out_value
);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_JSON_READER_H

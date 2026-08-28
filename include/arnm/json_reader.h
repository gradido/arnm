#ifndef ARNM_JSON_READER_H
#define ARNM_JSON_READER_H

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
 * @brief Bytes the opaque reader state occupies.
 *
 * Named rather than hidden behind a pointer, so a reader can live on the stack, in a struct,
 * or in memory another allocator handed out. The implementation pins this against its real
 * layout with a `static_assert`, so the number cannot drift away from what it describes.
 */
#define ARNM_JSON_READER_SIZE 72

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
  uint8_t opaqu[ARNM_JSON_READER_SIZE]; /**< Opaque; never read these directly. */
} arnm_json_reader;

/**
 * @brief A value inside a parsed document -- incomplete on purpose.
 *
 * Only ever handled through a pointer, and that pointer is owned by the document. Nothing
 * here is allocated, copied or released by a caller; the whole tree comes and goes with the
 * reader that parsed it.
 */
typedef struct arnm_json_value arnm_json_value;

arnm_result arnm_json_reader_init(arnm_json_reader *reader, arnm *allocator);

arnm_json_reader *arnm_json_reader_create(arnm *allocator);

arnm_result arnm_json_reader_release(arnm_json_reader *reader);

arnm_result arnm_json_reader_destroy(arnm_json_reader *reader, arnm *allocator);

// ********** parsing *******************

arnm_result arnm_json_reader_parse(
    arnm_json_reader *reader,
    const char *json,
    uint32_t length,
    bool stop_when_done,
    arnm_json_value **doc_root_out
);

arnm_result arnm_json_reader_parse_insitu(
    arnm_json_reader *reader,
    char *buffer,
    uint32_t length,
    uint32_t capacity,
    bool stop_when_done,
    arnm_json_value **doc_root_out
);

arnm_result arnm_json_reader_status(const arnm_json_reader *reader);

arnm_result arnm_json_reader_clear_error(arnm_json_reader *reader);

const char *arnm_json_reader_error_message(const arnm_json_reader *reader);

uint32_t arnm_json_reader_error_position(const arnm_json_reader *reader);

bool arnm_json_reader_has_document(const arnm_json_reader *reader);

uint32_t arnm_json_reader_value_count(const arnm_json_reader *reader);

uint32_t arnm_json_reader_bytes_read(const arnm_json_reader *reader);

arnm_json_value *arnm_json_reader_root(const arnm_json_reader *reader);

uint32_t arnm_json_reader_count(const arnm_json_reader *reader);

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
    arnm_json_value *array,
    arnm_json_value **out_values,
    uint32_t capacity,
    uint32_t *out_array_size
);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // ARNM_JSON_READER_H

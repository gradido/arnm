#include "arnm/json_reader.h"

#include "arnm/converter.h"
#include "arnm/memory_block.h"

#include "json_memory.h"

#include <stdint.h>
#include <string.h>

/*
 * The whole of yyjson lives behind this file. Nothing above -- not a type, not a constant, not
 * an include path -- reaches arnm/json_reader.h, so a consumer of arnm never learns which
 * parser is underneath and never has to link one itself.
 *
 * Two things have to be bridged for that to hold.
 *
 * First, the allocator: yyjson frees without a size and arnm needs one. That seam is shared
 * with the writer and lives in json_memory.h.
 *
 * Second, the flags. yyjson's read flags are its own numbering and free to move; the
 * ARNM_JSON_READ_* bits are ours and pinned by the public header. They are translated one by
 * one rather than passed through, which is what lets an unknown bit be refused instead of
 * arriving somewhere as a feature nobody asked for. The translation happens once, at init, and
 * the reader carries the result.
 *
 * Above that seam sits the field level: a cursor, a first error and the name it happened at.
 * Nothing of it reaches into yyjson beyond a lookup -- it exists so that a mapper reads one
 * line per struct member and asks once, at the end, whether any of them held.
 */

/* C11 static assert fallback; in C++ the keyword is already there */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/** @brief Marks a reader as initialized. A zeroed reader cannot hold it by accident. */
#define JSON_READER_MAGIC 0x6172736Eu /* "arsn" */

/**
 * @brief The layout behind the opaque @ref arnm_json_reader.
 *
 * @c alc carries a pointer to @c alc_context as its context, which is why a reader may not be
 * moved once it has been initialized: the document keeps its own copy of @c alc and calls back
 * through it when it is released.
 *
 * @c walk is the memory of the last array walked, and the reason a loop over an array costs one
 * step per element instead of one walk per element. It is a hint and nothing more: it is
 * checked against @c walk_array before it is trusted, and dropped whenever the document goes.
 */
typedef struct json_reader_state {
  json_alc_context alc_context; /**< Where documents come from, and what an arena kept back. */
  arnm *output_allocator;       /**< Where copied strings go; NULL borrows instead. */
  yyjson_alc alc;               /**< yyjson's hooks, bound to alc_context. */
  yyjson_doc *doc;              /**< The document held, or NULL. */
  yyjson_val *current;          /**< What the field getters read members of, or NULL. */
  yyjson_val *walk_array;       /**< The array @c walk stands in, or NULL. */
  yyjson_arr_iter walk;         /**< Where the last walk of @c walk_array stopped. */
  const char *error_message;    /**< Static text of the last parse refusal, or NULL. */
  yyjson_read_flag read_flags;  /**< The public flags, translated once at init. */
  uint32_t error_position;      /**< Byte offset of the last parse refusal, or 0. */
  arnm_result status;           /**< First error since the last parse, or ARNM_SUCCESS. */
  uint32_t magic;               /**< JSON_READER_MAGIC once initialized. */
  char error_field[ARNM_JSON_READER_FIELD_NAME_SIZE]; /**< Field name of @c status, or empty. */
} json_reader_state;

static_assert(
    sizeof(json_reader_state) <= ARNM_JSON_READER_SIZE,
    "ARNM_JSON_READER_SIZE in arnm/json_reader.h no longer covers json_reader_state"
);
static_assert(
    sizeof(yyjson_arr_iter) <= 32, "arnm_json_array_iter no longer covers a yyjson array iterator"
);
static_assert(
    sizeof(yyjson_obj_iter) <= 40, "arnm_json_object_iter no longer covers a yyjson object iterator"
);
static_assert(
    ARNM_JSON_READER_INSITU_PADDING == YYJSON_PADDING_SIZE,
    "ARNM_JSON_READER_INSITU_PADDING has drifted away from what yyjson writes past the input"
);

// ********** casting across the seam *******************

/** @brief The state behind a reader, or NULL when there is none to speak of. */
static json_reader_state *state_of(const arnm_json_reader *reader) {
  if (!reader) { return NULL; }
  json_reader_state *state = (json_reader_state *)(void *)(uintptr_t)(const void *)reader;
  return (JSON_READER_MAGIC == state->magic) ? state : NULL;
}

/** @brief A public value handle read as what it really is. */
static yyjson_val *to_yyjson(const arnm_json_value *value) {
  return (yyjson_val *)(void *)(uintptr_t)(const void *)value;
}

/** @brief A yyjson value dressed as a public handle. */
static arnm_json_value *to_public(yyjson_val *value) {
  return (arnm_json_value *)(void *)value;
}

/**
 * @brief A size_t count narrowed to the uint32_t every count in arnm is carried in.
 *
 * Saturates rather than wraps. Nothing here can reach the ceiling in practice: an input is
 * capped at ARNM_JSON_READER_MAX_INPUT_SIZE and no value, key or element is written in fewer
 * than one byte, so every count this narrows is already bounded by the input length.
 */
static uint32_t narrow_count(size_t count) {
#if SIZE_MAX > UINT32_MAX
  if (count > (size_t)UINT32_MAX) { return UINT32_MAX; }
#endif
  return (uint32_t)count;
}

// ********** flags *******************

/** @brief Every bit arnm/json_reader.h defines; anything else is refused. */
#define JSON_READ_KNOWN_FLAGS                                                                      \
  (ARNM_JSON_READ_STOP_WHEN_DONE | ARNM_JSON_READ_ALLOW_TRAILING_COMMAS |                          \
   ARNM_JSON_READ_ALLOW_COMMENTS | ARNM_JSON_READ_ALLOW_INF_AND_NAN |                              \
   ARNM_JSON_READ_ALLOW_INVALID_UNICODE | ARNM_JSON_READ_ALLOW_BOM)

/**
 * @brief Translate our flags into yyjson's, refusing a bit we do not know.
 *
 * @param[in]  flags Bit set from the public header.
 * @param[out] out   Receives yyjson's spelling of the same set.
 * @retval ARNM_SUCCESS             Translated.
 * @retval ARNM_ERROR_INVALID_PARAM @p flags holds a bit outside JSON_READ_KNOWN_FLAGS.
 */
static arnm_result translate_read_flags(arnm_json_read_flags flags, yyjson_read_flag *out) {
  if (0 != (flags & ~(arnm_json_read_flags)JSON_READ_KNOWN_FLAGS)) {
    return ARNM_ERROR_INVALID_PARAM;
  }

  yyjson_read_flag translated = YYJSON_READ_NOFLAG;
  if (0 != (flags & ARNM_JSON_READ_STOP_WHEN_DONE)) { translated |= YYJSON_READ_STOP_WHEN_DONE; }
  if (0 != (flags & ARNM_JSON_READ_ALLOW_TRAILING_COMMAS)) {
    translated |= YYJSON_READ_ALLOW_TRAILING_COMMAS;
  }
  if (0 != (flags & ARNM_JSON_READ_ALLOW_COMMENTS)) { translated |= YYJSON_READ_ALLOW_COMMENTS; }
  if (0 != (flags & ARNM_JSON_READ_ALLOW_INF_AND_NAN)) {
    translated |= YYJSON_READ_ALLOW_INF_AND_NAN;
  }
  if (0 != (flags & ARNM_JSON_READ_ALLOW_INVALID_UNICODE)) {
    translated |= YYJSON_READ_ALLOW_INVALID_UNICODE;
  }
  if (0 != (flags & ARNM_JSON_READ_ALLOW_BOM)) { translated |= YYJSON_READ_ALLOW_BOM; }

  *out = translated;
  return ARNM_SUCCESS;
}

/** @brief What a refusal from yyjson means in arnm's vocabulary. */
static arnm_result translate_read_error(uint32_t code) {
  if (YYJSON_READ_ERROR_MEMORY_ALLOCATION == code) { return ARNM_ERROR_OUT_OF_MEMORY; }
  if (YYJSON_READ_ERROR_INVALID_PARAMETER == code) { return ARNM_ERROR_INVALID_PARAM; }
  return ARNM_ERROR_DECODE_FAILED;
}

// ********** the first error, and the name it wears *******************

/**
 * @brief Keep @p result under @p key, unless something was kept already.
 *
 * The first refusal is the one that explains the others, so it is the one that stays. Every
 * later one passes through here and changes nothing -- which is what lets a whole struct be
 * read without a test between the lines.
 *
 * @param[in,out] state  Reader state; not NULL.
 * @param[in]     result The refusal to keep; never ARNM_SUCCESS at this call site.
 * @param[in]     key    Field name to keep with it, truncated to fit; NULL records no name.
 */
static void record_error(json_reader_state *state, arnm_result result, const char *key) {
  if (ARNM_SUCCESS != state->status) { return; }
  state->status = result;

  state->error_field[0] = '\0';
  if (!key) { return; }
  size_t length = strlen(key);
  if (length > sizeof(state->error_field) - 1u) { length = sizeof(state->error_field) - 1u; }
  memcpy(state->error_field, key, length);
  state->error_field[length] = '\0';
}

/** @brief record_error() for a position rather than a name, written as `[index]`. */
static void record_index_error(json_reader_state *state, arnm_result result, uint32_t index) {
  if (ARNM_SUCCESS != state->status) { return; }

  // Ten digits at most, plus the two brackets and the terminator: the buffer cannot be reached.
  char name[16];
  name[0] = '[';
  const uint8_t digits = arnm_uint64_to_string(name + 1, (uint8_t)(sizeof(name) - 2u), index);
  name[1u + digits] = ']';
  name[2u + digits] = '\0';
  record_error(state, result, name);
}

// ********** manage the reader itself *******************

/** @brief Let the document go, and say whether an arena kept any of it. */
static arnm_result dispose_document(json_reader_state *state) {
  state->current = NULL;
  state->walk_array = NULL;
  if (!state->doc) { return ARNM_SUCCESS; }

  state->alc_context.arena_kept_bytes = false;
  yyjson_doc_free(state->doc);
  state->doc = NULL;
  return state->alc_context.arena_kept_bytes ? ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
                                             : ARNM_SUCCESS;
}

arnm_result arnm_json_reader_init(
    arnm_json_reader *reader, arnm *allocator, arnm_json_read_flags flags
) {
  if (!reader) { return ARNM_ERROR_NULL_POINTER; }

  // Translated before a byte is written, so a flag nobody defined leaves the storage as it was
  // found rather than half prepared.
  yyjson_read_flag read_flags = YYJSON_READ_NOFLAG;
  const arnm_result translated = translate_read_flags(flags, &read_flags);
  if (ARNM_SUCCESS != translated) { return translated; }

  json_reader_state *state = (json_reader_state *)(void *)reader;
  state->alc_context.allocator = allocator;
  state->alc_context.arena_kept_bytes = false;
  json_alc_bind(&state->alc, &state->alc_context);
  state->output_allocator = NULL;
  state->doc = NULL;
  state->current = NULL;
  state->walk_array = NULL;
  memset(&state->walk, 0, sizeof(state->walk));
  state->error_message = NULL;
  state->read_flags = read_flags;
  state->error_position = 0;
  state->status = ARNM_SUCCESS;
  state->error_field[0] = '\0';
  state->magic = JSON_READER_MAGIC;
  return ARNM_SUCCESS;
}

arnm_json_reader *arnm_json_reader_create(arnm *allocator, arnm_json_read_flags flags) {
  uint8_t *storage = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&storage, (uint32_t)sizeof(arnm_json_reader), allocator)) {
    return NULL;
  }

  arnm_json_reader *reader = (arnm_json_reader *)(void *)storage;
  if (ARNM_SUCCESS != arnm_json_reader_init(reader, allocator, flags)) {
    // A handle that could not be prepared is not one to hand back, and the bytes go home the way
    // they came.
    (void)arnm_free(storage, (uint32_t)sizeof(arnm_json_reader), allocator);
    return NULL;
  }
  return reader;
}

arnm_result arnm_json_reader_set_output_allocator(arnm_json_reader *reader, arnm *allocator) {
  if (!reader) { return ARNM_ERROR_NULL_POINTER; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  state->output_allocator = allocator;
  return ARNM_SUCCESS;
}

arnm_result arnm_json_reader_release(arnm_json_reader *reader) {
  if (!reader) { return ARNM_SUCCESS; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }
  return dispose_document(state);
}

arnm_result arnm_json_reader_destroy(arnm_json_reader *reader, arnm *allocator) {
  if (!reader) { return ARNM_SUCCESS; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  const arnm_result released = dispose_document(state);
  // The magic goes before the bytes do, so a second destroy on the same address finds an
  // uninitialized reader rather than walking a document that is already gone.
  state->magic = 0;

  const arnm_result given_back =
      arnm_free((uint8_t *)(void *)reader, (uint32_t)sizeof(arnm_json_reader), allocator);

  if (ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == released) { return released; }
  return given_back;
}

// ********** parsing *******************

/**
 * @brief The shared tail of both parse calls: hand the bytes to yyjson and record what came
 *        back.
 *
 * @param[in,out] state        Reader state; already checked.
 * @param[in,out] bytes        Input, writable when @p extra carries YYJSON_READ_INSITU.
 * @param[in]     length       Bytes of JSON in @p bytes.
 * @param[in]     extra        Flags on top of the ones the reader was initialized with.
 * @return As documented on arnm_json_reader_parse().
 */
static arnm_result run_parse(
    json_reader_state *state, char *bytes, uint32_t length, yyjson_read_flag extra
) {
  yyjson_read_err error;
  yyjson_doc *doc =
      yyjson_read_opts(bytes, (size_t)length, state->read_flags | extra, &state->alc, &error);
  if (!doc) {
    state->error_message = error.msg;
    state->error_position = narrow_count(error.pos);
    const arnm_result result = translate_read_error((uint32_t)error.code);
    record_error(state, result, NULL);
    return result;
  }

  state->doc = doc;
  state->current = yyjson_doc_get_root(doc);
  return ARNM_SUCCESS;
}

/**
 * @brief Clear everything the last reading left behind, and let go of its document.
 *
 * Whatever was held goes first, whether or not what follows succeeds: a reader answering the
 * previous document after a failed parse is the kind of state nobody checks for. The verdict
 * starts over with it -- a parse is the beginning of a reading, not a step inside one.
 *
 * @param[in,out] state  Reader state; not NULL.
 * @param[in]     length Input length to check, in bytes.
 * @retval ARNM_SUCCESS                   The length is one a parse can work with.
 * @retval ARNM_ERROR_INVALID_PARAM       @p length is 0; recorded as the first error.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p length is past ARNM_JSON_READER_MAX_INPUT_SIZE;
 *                                        recorded as the first error.
 */
static arnm_result begin_parse(json_reader_state *state, uint32_t length) {
  (void)dispose_document(state);
  state->error_message = NULL;
  state->error_position = 0;
  state->status = ARNM_SUCCESS;
  state->error_field[0] = '\0';

  if (0 == length) {
    record_error(state, ARNM_ERROR_INVALID_PARAM, NULL);
    return ARNM_ERROR_INVALID_PARAM;
  }
  if (length > ARNM_JSON_READER_MAX_INPUT_SIZE) {
    record_error(state, ARNM_ERROR_ARITHMETIC_OVERFLOW, NULL);
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }
  return ARNM_SUCCESS;
}

arnm_result arnm_json_reader_parse(arnm_json_reader *reader, const char *json, uint32_t length) {
  if (!reader || !json) { return ARNM_ERROR_NULL_POINTER; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  const arnm_result checked = begin_parse(state, length);
  if (ARNM_SUCCESS != checked) { return checked; }

  // Const is cast off and never spent: without YYJSON_READ_INSITU the parser copies the input
  // before it reads a byte of it, and the copy is what it writes through. yyjson makes the same
  // cast in its own const entry point, for the same reason.
  char *writable = (char *)(void *)(uintptr_t)(const void *)json;
  return run_parse(state, writable, length, YYJSON_READ_NOFLAG);
}

arnm_result arnm_json_reader_parse_insitu(
    arnm_json_reader *reader, char *buffer, uint32_t length, uint32_t capacity
) {
  if (!reader || !buffer) { return ARNM_ERROR_NULL_POINTER; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  const arnm_result checked = begin_parse(state, length);
  if (ARNM_SUCCESS != checked) { return checked; }
  // length is bounded above, so the sum cannot wrap before it is compared
  if (capacity < length + ARNM_JSON_READER_INSITU_PADDING) {
    record_error(state, ARNM_ERROR_INVALID_PARAM, NULL);
    return ARNM_ERROR_INVALID_PARAM;
  }

  return run_parse(state, buffer, length, YYJSON_READ_INSITU);
}

// ********** what the reader carries *******************

arnm_result arnm_json_reader_status(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  return state ? state->status : ARNM_ERROR_NOT_INITIALIZED;
}

const char *arnm_json_reader_error_field(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  return state ? state->error_field : "";
}

arnm_result arnm_json_reader_clear_error(arnm_json_reader *reader) {
  if (!reader) { return ARNM_ERROR_NULL_POINTER; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  state->status = ARNM_SUCCESS;
  state->error_field[0] = '\0';
  return ARNM_SUCCESS;
}

const char *arnm_json_reader_error_message(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  if (!state || !state->error_message) { return "no error"; }
  return state->error_message;
}

uint32_t arnm_json_reader_error_position(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  return state ? state->error_position : 0u;
}

bool arnm_json_reader_has_document(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  return state && state->doc;
}

uint32_t arnm_json_reader_value_count(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  if (!state || !state->doc) { return 0u; }
  return narrow_count(yyjson_doc_get_val_count(state->doc));
}

uint32_t arnm_json_reader_bytes_read(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  if (!state || !state->doc) { return 0u; }
  return narrow_count(yyjson_doc_get_read_size(state->doc));
}

// ********** where the reader stands *******************

arnm_json_value *arnm_json_reader_root(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  if (!state || !state->doc) { return NULL; }
  return to_public(yyjson_doc_get_root(state->doc));
}

arnm_json_value *arnm_json_reader_current(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  return state ? to_public(state->current) : NULL;
}

/**
 * @brief The member @p key names inside the current value, or NULL with the reason recorded.
 *
 * The one place a field is looked up, so every getter refuses the same things for the same
 * reasons. A key of NULL is the current value itself, which is how an element of an array of
 * scalars is read.
 *
 * @param[in,out] state Reader state; not NULL.
 * @param[in]     key   Member to find, or NULL for the current value.
 * @return The value, or NULL once the reading has stopped.
 */
static yyjson_val *field_of(json_reader_state *state, const char *key) {
  // Quiet after the first refusal: what follows it would mostly report the same thing twice.
  if (ARNM_SUCCESS != state->status) { return NULL; }
  if (!state->doc || !state->current) {
    record_error(state, ARNM_ERROR_INVALID_STATE, key);
    return NULL;
  }
  if (!key) { return state->current; }

  if (!yyjson_is_obj(state->current)) {
    record_error(state, ARNM_ERROR_INVALID_ENUM_TYPE, key);
    return NULL;
  }
  yyjson_val *found = yyjson_obj_get(state->current, key);
  if (!found) {
    record_error(state, ARNM_ERROR_INVALID_PARAM, key);
    return NULL;
  }
  return found;
}

/** @brief field_of() without the recording, for the queries that answer instead of refusing. */
static yyjson_val *peek_field(const json_reader_state *state, const char *key) {
  if (!state || !state->doc || !state->current) { return NULL; }
  if (!key) { return state->current; }
  if (!yyjson_is_obj(state->current)) { return NULL; }
  return yyjson_obj_get(state->current, key);
}

uint32_t arnm_json_reader_count(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  if (!state || ARNM_SUCCESS != state->status || !state->current) { return 0u; }
  if (yyjson_is_arr(state->current)) { return narrow_count(yyjson_arr_size(state->current)); }
  if (yyjson_is_obj(state->current)) { return narrow_count(yyjson_obj_size(state->current)); }
  return 0u;
}

bool arnm_json_reader_has(const arnm_json_reader *reader, const char *key) {
  yyjson_val *field = peek_field(state_of(reader), key);
  return field && !yyjson_is_null(field);
}

arnm_json_type arnm_json_reader_type_of(const arnm_json_reader *reader, const char *key) {
  return arnm_json_value_type(to_public(peek_field(state_of(reader), key)));
}

arnm_json_value *arnm_json_reader_enter(arnm_json_reader *reader, const char *key) {
  json_reader_state *state = state_of(reader);
  if (!state) { return NULL; }

  // The way back is handed over before the step is taken, on every path -- a leave that only
  // pairs with a successful enter would need a test at each level, which is the one thing this
  // interface exists to avoid.
  arnm_json_value *left = to_public(state->current);
  state->current = field_of(state, key);
  return left;
}

/**
 * @brief The element at @p index of the current array, the last walk continued where it fits.
 *
 * Elements sit in a chain, so a fresh lookup at index n walks n links and a loop over the array
 * walks it n times over. The iterator kept in the state turns that back into one pass: it is
 * trusted only while it stands in the same array and has not gone past @p index, and starts
 * again from the front whenever it has.
 *
 * @param[in,out] state Reader state; not NULL, current value already known to be an array.
 * @param[in]     array The current array.
 * @param[in]     index Position, counted from 0.
 * @return The element, or NULL when @p index is past the end.
 */
static yyjson_val *element_at(json_reader_state *state, yyjson_val *array, uint32_t index) {
  if (state->walk_array != array || state->walk.idx > (size_t)index) {
    yyjson_arr_iter_init(array, &state->walk);
    state->walk_array = array;
  }

  yyjson_val *element = NULL;
  while (state->walk.idx <= (size_t)index) {
    element = yyjson_arr_iter_next(&state->walk);
    if (!element) { return NULL; }
  }
  return element;
}

arnm_json_value *arnm_json_reader_enter_at(arnm_json_reader *reader, uint32_t index) {
  json_reader_state *state = state_of(reader);
  if (!state) { return NULL; }

  arnm_json_value *left = to_public(state->current);
  if (ARNM_SUCCESS != state->status) {
    state->current = NULL;
    return left;
  }
  if (!state->doc || !state->current) {
    record_index_error(state, ARNM_ERROR_INVALID_STATE, index);
    state->current = NULL;
    return left;
  }
  if (!yyjson_is_arr(state->current)) {
    record_index_error(state, ARNM_ERROR_INVALID_ENUM_TYPE, index);
    state->current = NULL;
    return left;
  }

  yyjson_val *element = element_at(state, state->current, index);
  if (!element) { record_index_error(state, ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS, index); }
  state->current = element;
  return left;
}

void arnm_json_reader_leave(arnm_json_reader *reader, arnm_json_value *value) {
  json_reader_state *state = state_of(reader);
  if (!state) { return; }
  state->current = to_yyjson(value);
}

// ********** reserving the output arena *******************

/*
 * Both measurements walk the document's own value array rather than its tree.
 *
 * An immutable yyjson document keeps every value it read in one block, in the order they were
 * written, and a container records the byte offset to whatever follows its last descendant --
 * which is what the public foreach macros step along. Two things follow. A subtree is a
 * contiguous run, so its extent is one subtraction rather than a walk. And visiting every value
 * in it is a straight loop over that run, with no recursion and no stack of its own, so a
 * document nested a hundred thousand deep costs the same per value as a flat one.
 *
 * Object keys are strings too and sit in the same run, so counting every string counts them as
 * well. They are taken off again per object, where the key chain is right there to walk -- each
 * key belongs to exactly one object and each object is met exactly once, so nothing is
 * subtracted twice.
 */

/** @brief Values the subtree rooted at @p value occupies in the document's value array. */
static size_t subtree_span(yyjson_val *value) {
  if (!unsafe_yyjson_is_ctn(value)) { return 1u; }
  return (size_t)(unsafe_yyjson_get_next(value) - value);
}

/** @brief What one copied string costs in an arena: its bytes, the terminator, the rounding. */
static uint64_t copy_cost(yyjson_val *string) {
  return (uint64_t)ARNM_ALIGN8((uint64_t)unsafe_yyjson_get_len(string) + 1u);
}

/** @brief A sum of copy costs narrowed to what an allocation can ever be. */
static uint32_t narrow_reserve(uint64_t total) {
  return (total > (uint64_t)ARNM_MAX_ALLOC_SIZE) ? ARNM_MAX_ALLOC_SIZE : (uint32_t)total;
}

uint32_t arnm_json_reader_output_size(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  if (!state || !state->doc || !state->current) { return 0u; }

  yyjson_val *first = state->current;
  const size_t span = subtree_span(first);
  uint64_t total = 0;

  for (size_t index = 0; index < span; ++index) {
    yyjson_val *value = first + index;
    if (yyjson_is_str(value)) {
      total += copy_cost(value);
      continue;
    }
    if (!yyjson_is_obj(value)) { continue; }

    // the keys of this object were counted as strings a moment ago, or will be further along the
    // run; either way they are not copies and come off again here
    yyjson_obj_iter keys;
    yyjson_obj_iter_init(value, &keys);
    yyjson_val *key = NULL;
    while ((key = yyjson_obj_iter_next(&keys)) != NULL) { total -= copy_cost(key); }
  }
  return narrow_reserve(total);
}

/** @brief Whether @p key is one of the names the caller asked to be counted. */
static bool key_is_named(
    yyjson_val *key, const char *const *keys, const uint32_t *lengths, uint8_t count
) {
  const size_t length = unsafe_yyjson_get_len(key);
  const char *text = unsafe_yyjson_get_str(key);
  for (uint8_t index = 0; index < count; ++index) {
    // length and first byte before the walk: names of equal length are common in one document,
    // and their first letters rarely are, so this is where most candidates fall away
    if (keys[index] && (size_t)lengths[index] == length && keys[index][0] == text[0] &&
        0 == memcmp(keys[index], text, length)) {
      return true;
    }
  }
  return false;
}

uint32_t arnm_json_reader_output_size_for_keys(
    const arnm_json_reader *reader, const char *const *keys, uint8_t count
) {
  const json_reader_state *state = state_of(reader);
  if (!state || !state->doc || !state->current || !keys || 0 == count) { return 0u; }

  // measured once, here, rather than once per member: the same name is compared against every
  // key of every object below, and its length does not change in between
  uint32_t lengths[UINT8_MAX];
  for (uint8_t index = 0; index < count; ++index) {
    lengths[index] = keys[index] ? narrow_count(strlen(keys[index])) : 0u;
  }

  yyjson_val *first = state->current;
  const size_t span = subtree_span(first);
  uint64_t total = 0;

  for (size_t index = 0; index < span; ++index) {
    yyjson_val *value = first + index;
    if (!yyjson_is_obj(value)) { continue; }

    yyjson_obj_iter members;
    yyjson_obj_iter_init(value, &members);
    yyjson_val *key = NULL;
    while ((key = yyjson_obj_iter_next(&members)) != NULL) {
      yyjson_val *member = yyjson_obj_iter_get_val(key);
      if (yyjson_is_str(member) && key_is_named(key, keys, lengths, count)) {
        total += copy_cost(member);
      }
    }
  }
  return narrow_reserve(total);
}

// ********** read functions, one per JSON standard type *******************

/*
 * The two bounds below are exact powers of two and therefore exact as doubles, which is what
 * makes them safe to compare against. INT64_MAX is not: 2^63 - 1 rounds up to 2^63 on the way
 * into a double, so a test written with it would let 2^63 itself through and overflow on the
 * cast. The upper bounds are exclusive for that reason.
 */
static const double json_int64_lower_bound = -9223372036854775808.0;  /* -2^63, inclusive */
static const double json_int64_upper_bound = 9223372036854775808.0;   /*  2^63, exclusive */
static const double json_uint64_upper_bound = 18446744073709551616.0; /* 2^64, exclusive */

arnm_result arnm_json_read_bool(const arnm_json_value *value, bool *out) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *val = to_yyjson(value);
  if (!yyjson_is_bool(val)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }
  *out = yyjson_get_bool(val);
  return ARNM_SUCCESS;
}

arnm_result arnm_json_read_int64(const arnm_json_value *value, int64_t *out) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *val = to_yyjson(value);

  if (yyjson_is_sint(val)) {
    *out = yyjson_get_sint(val);
    return ARNM_SUCCESS;
  }
  if (yyjson_is_uint(val)) {
    const uint64_t raw = yyjson_get_uint(val);
    if (raw > (uint64_t)INT64_MAX) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
    *out = (int64_t)raw;
    return ARNM_SUCCESS;
  }
  if (yyjson_is_real(val)) {
    const double raw = yyjson_get_real(val);
    // A NaN fails both comparisons and leaves through the same door as a value out of range,
    // which is the right answer: neither can be carried whole.
    if (!(raw >= json_int64_lower_bound && raw < json_int64_upper_bound)) {
      return ARNM_ERROR_ARITHMETIC_OVERFLOW;
    }
    const int64_t truncated = (int64_t)raw;
    if ((double)truncated != raw) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
    *out = truncated;
    return ARNM_SUCCESS;
  }
  return ARNM_ERROR_INVALID_ENUM_TYPE;
}

arnm_result arnm_json_read_uint64(const arnm_json_value *value, uint64_t *out) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *val = to_yyjson(value);

  if (yyjson_is_uint(val)) {
    *out = yyjson_get_uint(val);
    return ARNM_SUCCESS;
  }
  if (yyjson_is_sint(val)) {
    const int64_t raw = yyjson_get_sint(val);
    if (raw < 0) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
    *out = (uint64_t)raw;
    return ARNM_SUCCESS;
  }
  if (yyjson_is_real(val)) {
    const double raw = yyjson_get_real(val);
    if (!(raw >= 0.0 && raw < json_uint64_upper_bound)) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
    const uint64_t truncated = (uint64_t)raw;
    if ((double)truncated != raw) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
    *out = truncated;
    return ARNM_SUCCESS;
  }
  return ARNM_ERROR_INVALID_ENUM_TYPE;
}

arnm_result arnm_json_read_int32(const arnm_json_value *value, int32_t *out) {
  if (!out) { return ARNM_ERROR_NULL_POINTER; }
  int64_t wide = 0;
  const arnm_result result = arnm_json_read_int64(value, &wide);
  if (ARNM_SUCCESS != result) { return result; }
  if (wide < (int64_t)INT32_MIN || wide > (int64_t)INT32_MAX) {
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }
  *out = (int32_t)wide;
  return ARNM_SUCCESS;
}

arnm_result arnm_json_read_uint32(const arnm_json_value *value, uint32_t *out) {
  if (!out) { return ARNM_ERROR_NULL_POINTER; }
  uint64_t wide = 0;
  const arnm_result result = arnm_json_read_uint64(value, &wide);
  if (ARNM_SUCCESS != result) { return result; }
  if (wide > (uint64_t)UINT32_MAX) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
  *out = (uint32_t)wide;
  return ARNM_SUCCESS;
}

arnm_result arnm_json_read_double(const arnm_json_value *value, double *out) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *val = to_yyjson(value);
  if (!yyjson_is_num(val)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }
  *out = yyjson_get_num(val);
  return ARNM_SUCCESS;
}

arnm_result arnm_json_read_string(
    const arnm_json_value *value, const char **out, uint32_t *out_length
) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *val = to_yyjson(value);
  if (!yyjson_is_str(val)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }
  *out = yyjson_get_str(val);
  if (out_length) { *out_length = narrow_count(yyjson_get_len(val)); }
  return ARNM_SUCCESS;
}

// ********** reading a field, one line per struct member *******************

/**
 * @brief Lift a string out of the document into the output allocator.
 *
 * NUL terminated on top of its own length, so the copy serves both a caller that counts and one
 * that scans. Given back the way an arena gives anything back -- all at once, never one string
 * at a time.
 *
 * @param[in,out] state  Reader state with a non-NULL output allocator.
 * @param[in]     text   Bytes to copy; not NULL.
 * @param[in]     length Bytes to copy, terminator excluded.
 * @param[in]     key    Field name to record a refusal under.
 * @return The copy, or NULL with the refusal recorded.
 */
static const char *copy_string(
    json_reader_state *state, const char *text, uint32_t length, const char *key
) {
  if (length > ARNM_MAX_ALLOC_SIZE - 1u) {
    record_error(state, ARNM_ERROR_ARITHMETIC_OVERFLOW, key);
    return NULL;
  }

  uint8_t *copy = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&copy, length + 1u, state->output_allocator)) {
    record_error(state, ARNM_ERROR_OUT_OF_MEMORY, key);
    return NULL;
  }
  if (length > 0u) { memcpy(copy, text, (size_t)length); }
  copy[length] = '\0';
  return (const char *)copy;
}

const char *arnm_json_reader_get_string_length(
    arnm_json_reader *reader, const char *key, uint32_t *out_length
) {
  json_reader_state *state = state_of(reader);
  if (!state) { return NULL; }
  yyjson_val *field = field_of(state, key);
  if (!field) { return NULL; }

  const char *text = NULL;
  uint32_t length = 0;
  const arnm_result result = arnm_json_read_string(to_public(field), &text, &length);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key);
    return NULL;
  }

  if (state->output_allocator) {
    text = copy_string(state, text, length, key);
    if (!text) { return NULL; }
  }

  if (out_length) { *out_length = length; }
  return text;
}

const char *arnm_json_reader_get_string(arnm_json_reader *reader, const char *key) {
  return arnm_json_reader_get_string_length(reader, key, NULL);
}

bool arnm_json_reader_get_bool(arnm_json_reader *reader, const char *key) {
  json_reader_state *state = state_of(reader);
  if (!state) { return false; }
  yyjson_val *field = field_of(state, key);
  if (!field) { return false; }

  bool value = false;
  const arnm_result result = arnm_json_read_bool(to_public(field), &value);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key);
    return false;
  }
  return value;
}

int64_t arnm_json_reader_get_int64(arnm_json_reader *reader, const char *key) {
  json_reader_state *state = state_of(reader);
  if (!state) { return 0; }
  yyjson_val *field = field_of(state, key);
  if (!field) { return 0; }

  int64_t value = 0;
  const arnm_result result = arnm_json_read_int64(to_public(field), &value);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key);
    return 0;
  }
  return value;
}

uint64_t arnm_json_reader_get_uint64(arnm_json_reader *reader, const char *key) {
  json_reader_state *state = state_of(reader);
  if (!state) { return 0u; }
  yyjson_val *field = field_of(state, key);
  if (!field) { return 0u; }

  uint64_t value = 0;
  const arnm_result result = arnm_json_read_uint64(to_public(field), &value);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key);
    return 0u;
  }
  return value;
}

int32_t arnm_json_reader_get_int32(arnm_json_reader *reader, const char *key) {
  json_reader_state *state = state_of(reader);
  if (!state) { return 0; }
  yyjson_val *field = field_of(state, key);
  if (!field) { return 0; }

  int32_t value = 0;
  const arnm_result result = arnm_json_read_int32(to_public(field), &value);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key);
    return 0;
  }
  return value;
}

uint32_t arnm_json_reader_get_uint32(arnm_json_reader *reader, const char *key) {
  json_reader_state *state = state_of(reader);
  if (!state) { return 0u; }
  yyjson_val *field = field_of(state, key);
  if (!field) { return 0u; }

  uint32_t value = 0;
  const arnm_result result = arnm_json_read_uint32(to_public(field), &value);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key);
    return 0u;
  }
  return value;
}

double arnm_json_reader_get_double(arnm_json_reader *reader, const char *key) {
  json_reader_state *state = state_of(reader);
  if (!state) { return 0.0; }
  yyjson_val *field = field_of(state, key);
  if (!field) { return 0.0; }

  double value = 0.0;
  const arnm_result result = arnm_json_read_double(to_public(field), &value);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key);
    return 0.0;
  }
  return value;
}

// ********** what a value is *******************

arnm_json_type arnm_json_value_type(const arnm_json_value *value) {
  if (!value) { return ARNM_JSON_TYPE_NONE; }
  switch (yyjson_get_type(to_yyjson(value))) {
  case YYJSON_TYPE_NULL:
    return ARNM_JSON_TYPE_NULL;
  case YYJSON_TYPE_BOOL:
    return ARNM_JSON_TYPE_BOOL;
  case YYJSON_TYPE_NUM:
    return ARNM_JSON_TYPE_NUMBER;
  case YYJSON_TYPE_STR:
    return ARNM_JSON_TYPE_STRING;
  case YYJSON_TYPE_ARR:
    return ARNM_JSON_TYPE_ARRAY;
  case YYJSON_TYPE_OBJ:
    return ARNM_JSON_TYPE_OBJECT;
  default:
    return ARNM_JSON_TYPE_NONE;
  }
}

arnm_json_number_type arnm_json_value_number_type(const arnm_json_value *value) {
  if (!value) { return ARNM_JSON_NUMBER_TYPE_NONE; }
  yyjson_val *val = to_yyjson(value);
  if (yyjson_is_uint(val)) { return ARNM_JSON_NUMBER_TYPE_UINT; }
  if (yyjson_is_sint(val)) { return ARNM_JSON_NUMBER_TYPE_SINT; }
  if (yyjson_is_real(val)) { return ARNM_JSON_NUMBER_TYPE_REAL; }
  return ARNM_JSON_NUMBER_TYPE_NONE;
}

const char *arnm_json_type_to_string(arnm_json_type type) {
  switch (type) {
  case ARNM_JSON_TYPE_NONE:
    return "ARNM_JSON_TYPE_NONE";
  case ARNM_JSON_TYPE_NULL:
    return "ARNM_JSON_TYPE_NULL";
  case ARNM_JSON_TYPE_BOOL:
    return "ARNM_JSON_TYPE_BOOL";
  case ARNM_JSON_TYPE_NUMBER:
    return "ARNM_JSON_TYPE_NUMBER";
  case ARNM_JSON_TYPE_STRING:
    return "ARNM_JSON_TYPE_STRING";
  case ARNM_JSON_TYPE_ARRAY:
    return "ARNM_JSON_TYPE_ARRAY";
  case ARNM_JSON_TYPE_OBJECT:
    return "ARNM_JSON_TYPE_OBJECT";
  default:
    return "ARNM_JSON_TYPE_UNKNOWN";
  }
}

arnm_result arnm_json_read_hex(
    const arnm_json_value *value, uint8_t *out, uint32_t capacity, uint32_t *out_size
) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  const char *hex = NULL;
  uint32_t length = 0;
  const arnm_result result = arnm_json_read_string(value, &hex, &length);
  if (ARNM_SUCCESS != result) { return result; }
  // two characters make one byte, so anything else was never hex -- and the buffer is asked
  // before the converter writes into it, not after. The converter reads to the terminator, so
  // an embedded NUL would stop it early and leave the rest of the field as the caller had it:
  // a string that ends before the document says it does is refused instead.
  if (length % 2u || length / 2u > capacity || strlen(hex) != length) {
    return ARNM_ERROR_DECODE_FAILED;
  }
  if (ARNM_SUCCESS != arnm_binary_from_hex(out, hex)) { return ARNM_ERROR_DECODE_FAILED; }
  if (out_size) { *out_size = length / 2u; }
  return ARNM_SUCCESS;
}

arnm_result arnm_json_read_hex_fixed(const arnm_json_value *value, uint8_t *out, uint32_t size) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  const char *hex = NULL;
  uint32_t length = 0;
  const arnm_result result = arnm_json_read_string(value, &hex, &length);
  if (ARNM_SUCCESS != result) { return result; }
  // as arnm_json_read_hex(): the converter reads to the terminator, so a string carrying one
  // of its own is not the length the document claims and is no hex string either
  if (length != size * 2u || strlen(hex) != length) { return ARNM_ERROR_DECODE_FAILED; }
  return (ARNM_SUCCESS == arnm_binary_from_hex(out, hex)) ? ARNM_SUCCESS : ARNM_ERROR_DECODE_FAILED;
}

arnm_result arnm_json_read_uuid(const arnm_json_value *value, uint8_t *out) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  const char *text = NULL;
  uint32_t length = 0;
  const arnm_result result = arnm_json_read_string(value, &text, &length);
  if (ARNM_SUCCESS != result) { return result; }
  if (ARNM_UUID_STRING_LENGTH != length) { return ARNM_ERROR_DECODE_FAILED; }
  return (ARNM_SUCCESS == arnm_uuid_from_string(out, text)) ? ARNM_SUCCESS
                                                            : ARNM_ERROR_DECODE_FAILED;
}

arnm_result arnm_json_read_base64_block(
    arnm_memory_block *out, const arnm_json_value *value, arnm *memory
) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  out->data = NULL;
  out->size = 0;

  const char *text = NULL;
  uint32_t length = 0;
  arnm_result result = arnm_json_read_string(value, &text, &length);
  if (ARNM_SUCCESS != result) { return result; }

  uint32_t size = 0;
  result = arnm_base64_binary_size(text, length, &size);
  if (ARNM_SUCCESS != result) { return result; }
  if (0 == size) { return ARNM_SUCCESS; }

  result = arnm_memory_block_alloc(out, size, memory);
  if (ARNM_SUCCESS != result) { return result; }

  uint32_t written = 0;
  if (ARNM_SUCCESS != arnm_binary_from_base64(out->data, &written, text)) {
    return ARNM_ERROR_DECODE_FAILED;
  }
  // the block was reserved from the same answer, so a disagreement is this file contradicting
  // itself rather than a document being wrong
  return (written == size) ? ARNM_SUCCESS : ARNM_ERROR_DECODE_FAILED;
}

// ********** arrays *******************

uint32_t arnm_json_array_size(const arnm_json_value *value) {
  if (!value) { return 0u; }
  return narrow_count(yyjson_arr_size(to_yyjson(value)));
}

arnm_result arnm_json_array_get(
    const arnm_json_value *value, uint32_t index, arnm_json_value **out
) {
  if (!value || !out) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *array = to_yyjson(value);
  if (!yyjson_is_arr(array)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }

  yyjson_val *element = yyjson_arr_get(array, (size_t)index);
  if (!element) { return ARNM_ERROR_ARRAY_INDEX_OUT_OF_BOUNDS; }
  *out = to_public(element);
  return ARNM_SUCCESS;
}

arnm_result arnm_json_array_iter_init(const arnm_json_value *value, arnm_json_array_iter *iter) {
  if (!value || !iter) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *array = to_yyjson(value);
  if (!yyjson_is_arr(array)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }

  // Every byte of the opaque storage is written, so an iterator that was never touched before
  // is a valid input and no stale tail survives underneath a shorter yyjson iterator.
  memset(iter, 0, sizeof(*iter));
  yyjson_arr_iter_init(array, (yyjson_arr_iter *)(void *)iter);
  return ARNM_SUCCESS;
}

bool arnm_json_array_iter_next(arnm_json_array_iter *iter, arnm_json_value **out) {
  if (!iter || !out) { return false; }
  yyjson_val *element = yyjson_arr_iter_next((yyjson_arr_iter *)(void *)iter);
  if (!element) { return false; }
  *out = to_public(element);
  return true;
}

// ********** objects *******************

uint32_t arnm_json_object_size(const arnm_json_value *value) {
  if (!value) { return 0u; }
  return narrow_count(yyjson_obj_size(to_yyjson(value)));
}

arnm_result arnm_json_object_get(
    const arnm_json_value *value, const char *key, arnm_json_value **out
) {
  if (!value || !key || !out) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *object = to_yyjson(value);
  if (!yyjson_is_obj(object)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }

  yyjson_val *found = yyjson_obj_get(object, key);
  if (!found) { return ARNM_ERROR_INVALID_PARAM; }
  *out = to_public(found);
  return ARNM_SUCCESS;
}

arnm_result arnm_json_object_iter_init(const arnm_json_value *value, arnm_json_object_iter *iter) {
  if (!value || !iter) { return ARNM_ERROR_NULL_POINTER; }
  yyjson_val *object = to_yyjson(value);
  if (!yyjson_is_obj(object)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }

  memset(iter, 0, sizeof(*iter));
  yyjson_obj_iter_init(object, (yyjson_obj_iter *)(void *)iter);
  return ARNM_SUCCESS;
}

bool arnm_json_object_iter_next(
    arnm_json_object_iter *iter,
    const char **out_key,
    uint32_t *out_key_length,
    arnm_json_value **out_value
) {
  if (!iter) { return false; }
  yyjson_val *key = yyjson_obj_iter_next((yyjson_obj_iter *)(void *)iter);
  if (!key) { return false; }

  if (out_key) { *out_key = yyjson_get_str(key); }
  if (out_key_length) { *out_key_length = narrow_count(yyjson_get_len(key)); }
  if (out_value) { *out_value = to_public(yyjson_obj_iter_get_val(key)); }
  return true;
}

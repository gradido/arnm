#include "arnm/json_reader.h"

#include "arnm/bitmap.h"
#include "arnm/converter.h"
#include "arnm/memory_block.h"

#include "arnm/result.h"
// includes also yyjson
#include "json_memory.h"
#include "yyjson.h"

#include <stdint.h>
#include <string.h>

/* C11 static assert fallback; in C++ the keyword is already there */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/** @brief Marks a reader as initialized. A zeroed reader cannot hold it by accident. */
#define JSON_READER_MAGIC 0x6172736Eu /* "arsn" */

/**
 * @brief The layout behind the opaque @ref arnm_json_reader.
 *
 * Six fields, and every one of them is about the document rather than about a place inside it.
 * There is no cursor here because the public shape has none: a caller holds its own
 * @ref arnm_json_value pointers and the reader never has to know which one it is looking at.
 *
 * @c alc carries a pointer to @c alc_context as its context, which is why a reader may not be
 * moved once it has been initialized: the document keeps its own copy of @c alc and calls back
 * through it when it is released.
 *
 * @c error_message is the parser's own static text and is never copied. It doubles as the flag
 * @ref arnm_json_reader_status() reads, which is why every parse clears it before it starts.
 */
typedef struct json_reader_state {
  json_alc_context alc_context; /**< Where documents come from, and what an arena kept back. */
  yyjson_alc alc;               /**< yyjson's hooks, bound to alc_context. */
  yyjson_doc *doc;              /**< The document held, or NULL. */
  const char *error_message;    /**< Static text of the last parse refusal, or NULL. */
  uint32_t error_position;      /**< Byte offset of the last parse refusal, or 0. */
  uint32_t magic;               /**< JSON_READER_MAGIC once initialized. */
} json_reader_state;

static_assert(
    sizeof(json_reader_state) <= ARNM_JSON_READER_SIZE,
    "ARNM_JSON_READER_SIZE in arnm/json_reader.h no longer covers json_reader_state"
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

/**
 * @brief What a refusal from yyjson means in arnm's vocabulary.
 *
 * Translated rather than passed through, so yyjson's numbering stays its own to change. The
 * two it distinguishes are the ones a caller can act on -- an allocator that ran out, and a
 * call it made wrongly; everything else is the document being wrong, which is one answer.
 */
static arnm_result translate_read_error(uint32_t code) {
  if (YYJSON_READ_ERROR_MEMORY_ALLOCATION == code) { return ARNM_ERROR_OUT_OF_MEMORY; }
  if (YYJSON_READ_ERROR_INVALID_PARAMETER == code) { return ARNM_ERROR_INVALID_PARAM; }
  return ARNM_ERROR_DECODE_FAILED;
}

// ********** manage the reader itself *******************

/** @brief Let the document go, and say whether an arena kept any of it. */
static arnm_result dispose_document(json_reader_state *state) {
  if (!state->doc) { return ARNM_SUCCESS; }

  state->alc_context.arena_kept_bytes = false;
  yyjson_doc_free(state->doc);
  state->doc = NULL;
  return state->alc_context.arena_kept_bytes ? ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED
                                             : ARNM_SUCCESS;
}

arnm_result arnm_json_reader_init(arnm_json_reader *reader, arnm *allocator) {
  if (!reader) { return ARNM_ERROR_NULL_POINTER; }

  // Every field is written and none is read, so uninitialized storage is a valid input. The
  // magic goes last, so a reader that was interrupted mid preparation still reads as one that
  // was never prepared at all.
  json_reader_state *state = (json_reader_state *)(void *)reader;
  state->alc_context.allocator = allocator;
  state->alc_context.arena_kept_bytes = false;
  json_alc_bind(&state->alc, &state->alc_context);
  state->doc = NULL;
  state->error_message = NULL;
  state->error_position = 0;
  state->magic = JSON_READER_MAGIC;
  return ARNM_SUCCESS;
}

arnm_json_reader *arnm_json_reader_create(arnm *allocator) {
  uint8_t *storage = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&storage, (uint32_t)sizeof(arnm_json_reader), allocator)) {
    return NULL;
  }

  arnm_json_reader *reader = (arnm_json_reader *)(void *)storage;
  if (ARNM_SUCCESS != arnm_json_reader_init(reader, allocator)) {
    // A handle that could not be prepared is not one to hand back, and the bytes go home the way
    // they came.
    (void)arnm_free(storage, (uint32_t)sizeof(arnm_json_reader), allocator);
    return NULL;
  }
  return reader;
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

/**
 * @brief Hand the bytes to yyjson and keep whatever comes back.
 *
 * The one place a document is born. Both public parses meet here having already checked what
 * they can check on their own, so what is left is the parser's verdict and where to put it.
 *
 * @param[in,out] state        Reader state with no document standing.
 * @param[in,out] bytes        What to read; written through only under YYJSON_READ_INSITU.
 * @param[in]     length       Bytes in @p bytes.
 * @param[in]     flags        yyjson's own spelling, assembled by the caller.
 * @param[out]    doc_root_out Receives the root; may be NULL. Written only on success.
 * @return ARNM_SUCCESS, or what translate_read_error() makes of the refusal. On a refusal the
 *         message and position are kept and no document is held.
 */
static arnm_result run_parse(
    json_reader_state *state,
    char *bytes,
    uint32_t length,
    yyjson_read_flag flags,
    arnm_json_value **doc_root_out
) {
  yyjson_read_err error;
  yyjson_doc *doc = yyjson_read_opts(bytes, (size_t)length, flags, &state->alc, &error);
  if (!doc) {
    state->error_message = error.msg;
    state->error_position = narrow_count(error.pos);
    return translate_read_error((uint32_t)error.code);
  }

  state->doc = doc;
  if (doc_root_out) { *doc_root_out = to_public(yyjson_doc_get_root(doc)); }
  return ARNM_SUCCESS;
}

/**
 * @brief Clear the way for a parse, and refuse a length no parse could carry.
 *
 * The previous document goes first, whether the parse that follows holds or not. That is what
 * keeps a refused parse from leaving the reader answering for the document before it -- a state
 * nobody checks for and everybody would misread.
 *
 * @param[in,out] state  Reader state; the document it held is released here.
 * @param[in]     length Bytes the caller means to hand over.
 * @retval ARNM_SUCCESS                   Clear; the parse may run.
 * @retval ARNM_ERROR_INVALID_PARAM       @p length is 0.
 * @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p length is past ARNM_JSON_READER_MAX_INPUT_SIZE.
 */
static arnm_result begin_parse(json_reader_state *state, uint32_t length) {
  (void)dispose_document(state);
  state->error_message = NULL;
  state->error_position = 0;

  if (0 == length) return ARNM_ERROR_INVALID_PARAM;
  if (length > ARNM_JSON_READER_MAX_INPUT_SIZE) { return ARNM_ERROR_ARITHMETIC_OVERFLOW; }
  return ARNM_SUCCESS;
}

arnm_result arnm_json_reader_parse(
    arnm_json_reader *reader,
    const char *json,
    uint32_t length,
    bool stop_when_done,
    arnm_json_value **doc_root_out
) {
  if (!reader || !json) { return ARNM_ERROR_NULL_POINTER; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  const arnm_result checked = begin_parse(state, length);
  if (ARNM_SUCCESS != checked) { return checked; }

  // Const is cast off and never spent: without YYJSON_READ_INSITU the parser copies the input
  // before it reads a byte of it, and the copy is what it writes through. yyjson makes the same
  // cast in its own const entry point, for the same reason.
  char *writable = (char *)(void *)(uintptr_t)(const void *)json;
  yyjson_read_flag read_flags = YYJSON_READ_NOFLAG;
  if (stop_when_done) { read_flags |= YYJSON_READ_STOP_WHEN_DONE; }
  return run_parse(state, writable, length, read_flags, doc_root_out);
}

arnm_result arnm_json_reader_parse_insitu(
    arnm_json_reader *reader,
    char *buffer,
    uint32_t length,
    uint32_t capacity,
    bool stop_when_done,
    arnm_json_value **doc_root_out
) {
  if (!reader || !buffer) { return ARNM_ERROR_NULL_POINTER; }
  json_reader_state *state = state_of(reader);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  const arnm_result checked = begin_parse(state, length);
  if (ARNM_SUCCESS != checked) { return checked; }
  // length is bounded above, so the sum cannot wrap before it is compared
  if (capacity < length + ARNM_JSON_READER_INSITU_PADDING) { return ARNM_ERROR_INVALID_PARAM; }

  yyjson_read_flag read_flags = YYJSON_READ_INSITU;
  if (stop_when_done == true) { read_flags |= YYJSON_READ_STOP_WHEN_DONE; }
  return run_parse(state, buffer, length, read_flags, doc_root_out);
}

// ********** what the reader carries *******************

arnm_result arnm_json_reader_status(const arnm_json_reader *reader) {
  const json_reader_state *state = state_of(reader);
  if (!state) return ARNM_ERROR_NOT_INITIALIZED;
  if (state->error_message) return ARNM_ERROR_DECODE_FAILED;
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

// ********** reading a shape in one walk *******************

/**
 * @brief Read one member into the target its entry names, once the key has matched.
 *
 * Sorted into a family by two range tests before anything is converted, which is what the
 * enum's order exists for: the four integer types are one contiguous run and the three string
 * types another, so a tag lands in its branch without a table lookup. Everything the two runs
 * do not cover falls through to the switch at the end.
 *
 * The integer branch reads the number once, in both signednesses where both fit, and lets each
 * target say what it cannot carry -- a negative into an unsigned, a magnitude past a narrow
 * width. Nothing is written where the answer is a refusal.
 *
 * @param[in]  field Entry whose key matched; its target is not NULL.
 * @param[in]  value The member; not NULL.
 * @return ARNM_SUCCESS, or the refusal named at arnm_json_read_object().
 */
// hand written by human, for highly optimized hot-path
static arnm_result read_field(const arnm_json_field *field, yyjson_val *value) {
  if (!field || !field->target || !value) return ARNM_ERROR_NULL_POINTER;
  arnm_json_field_type type = (arnm_json_field_type)field->type;
  if (ARNM_JSON_FIELD_TYPE_NONE == type) return ARNM_ERROR_INVALID_PARAM;

  // for all four integer types
  if ((type <= ARNM_JSON_FIELD_TYPE_INT32)) {
    uint64_t integer_value = 0;
    int64_t s_integer_value = 0;
    bool negative = false;
    bool signed_overflow = false;
    if (unsafe_yyjson_is_uint((void *)value)) {
      integer_value = unsafe_yyjson_get_uint((void *)value);
      if (integer_value <= INT64_MAX) {
        s_integer_value = (int64_t)integer_value;
      } else {
        signed_overflow = true;
      }
    } else if (yyjson_is_sint((void *)value)) {
      s_integer_value = unsafe_yyjson_get_sint((void *)value);
      if (s_integer_value >= 0) {
        integer_value = (uint64_t)s_integer_value;
      } else {
        negative = true;
      }
    } else {
      return ARNM_ERROR_INVALID_ENUM_TYPE;
    }
    switch (type) {
    case ARNM_JSON_FIELD_TYPE_UINT64:
      if (negative) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
      *(uint64_t *)field->target = integer_value;
      return ARNM_SUCCESS;
    case ARNM_JSON_FIELD_TYPE_UINT32:
      if (negative) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
      if (integer_value > (uint64_t)UINT32_MAX) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
      *(uint32_t *)field->target = (uint32_t)integer_value;
      return ARNM_SUCCESS;
    case ARNM_JSON_FIELD_TYPE_INT64:
      if (signed_overflow) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
      *(int64_t *)field->target = s_integer_value;
      return ARNM_SUCCESS;
    case ARNM_JSON_FIELD_TYPE_INT32:
      if (signed_overflow) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
      if (s_integer_value < (int64_t)INT32_MIN || s_integer_value > (int64_t)INT32_MAX)
        return ARNM_ERROR_ARITHMETIC_OVERFLOW;
      *(int32_t *)field->target = (int32_t)s_integer_value;
      return ARNM_SUCCESS;
    default:
      return ARNM_ERROR_INVALID_STATE;
    }
    // for all string types (string, hex_fixed and uuid)
  } else if ((type <= ARNM_JSON_FIELD_TYPE_UUID)) {
    if (!unsafe_yyjson_is_str((void *)value)) return ARNM_ERROR_INVALID_ENUM_TYPE;
    const char *str = unsafe_yyjson_get_str((void *)value);
    uint32_t str_size = narrow_count(unsafe_yyjson_get_len((void *)value));
    arnm_memory_block *out = (arnm_memory_block *)field->target;
    if (ARNM_JSON_FIELD_TYPE_STRING == type) {
      out->data = (uint8_t *)str;
      out->size = str_size;
      return ARNM_SUCCESS;
    } else if (ARNM_JSON_FIELD_TYPE_HEX_FIXED == type) {
      if ((uint64_t)out->size * 2u != (uint64_t)str_size) return ARNM_ERROR_DECODE_FAILED;
      return (ARNM_SUCCESS == arnm_binary_from_hex_with_known_hex_size(out->data, str, str_size))
                 ? ARNM_SUCCESS
                 : ARNM_ERROR_DECODE_FAILED;
    } else if (ARNM_JSON_FIELD_TYPE_UUID == type) {
      if (ARNM_UUID_STRING_LENGTH != str_size || out->size != ARNM_UUID_BINARY_SIZE)
        return ARNM_ERROR_DECODE_FAILED;
      return (ARNM_SUCCESS == arnm_uuid_from_string(out->data, str)) ? ARNM_SUCCESS
                                                                     : ARNM_ERROR_DECODE_FAILED;
    }
  }
  switch (type) {
  case ARNM_JSON_FIELD_TYPE_VALUE:
    *(arnm_json_value **)field->target = to_public(value);
    return ARNM_SUCCESS;
  case ARNM_JSON_FIELD_TYPE_BOOL:
    if (!unsafe_yyjson_is_bool((void *)value)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }
    *(bool *)field->target = unsafe_yyjson_get_bool((void *)value);
    return ARNM_SUCCESS;
  case ARNM_JSON_FIELD_TYPE_DOUBLE:
    if (!unsafe_yyjson_is_num((void *)value)) { return ARNM_ERROR_INVALID_ENUM_TYPE; }
    *(double *)field->target = unsafe_yyjson_get_num((void *)value);
    return ARNM_SUCCESS;
  case ARNM_JSON_FIELD_TYPE_NONE:
  default:
    return ARNM_ERROR_INVALID_PARAM;
  }
}
// hand written by human, for highly optimized hot-path
arnm_result arnm_json_read_object(
    arnm_json_value *object, arnm_json_field *fields, uint32_t count, uint64_t *out_found
) {
  if (out_found) *out_found = 0;
  if (!object || !fields) return ARNM_ERROR_NULL_POINTER;
  if (0 == count || count > ARNM_JSON_FIELDS_MAX) return ARNM_ERROR_INVALID_PARAM;

  if (!unsafe_yyjson_is_obj((void *)object)) return ARNM_ERROR_INVALID_ENUM_TYPE;
  yyjson_obj_iter iter = yyjson_obj_iter_with(to_yyjson(object));

  const uint64_t valid_mask = (count == 64) ? UINT64_MAX : (UINT64_C(1) << count) - 1u;
  uint64_t found = 0;
  yyjson_val *key = NULL;
  while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
    // use bitmask magic to start search by first not found key in fields
    uint64_t remaining = ~found & valid_mask;
    if (!remaining) {
      if (out_found) { *out_found = found; }
      return ARNM_SUCCESS;
    }
    uint32_t key_size = narrow_count(unsafe_yyjson_get_len((void *)key));
    const char *key_string = unsafe_yyjson_get_str((void *)key);
    for (uint32_t i = (uint32_t)arnm_ctzll(remaining); i < count; ++i) {
      const uint64_t bit = (uint64_t)1u << i;
      if ((found & bit) == bit) continue;

      const arnm_json_field *field = &fields[i];
      if (key_size == field->key_length && 0 == memcmp(key_string, field->key, key_size)) {
        const arnm_result result = read_field(field, yyjson_obj_iter_get_val(key));
        if (ARNM_SUCCESS != result) {
          if (out_found) { *out_found = found; }
          return result;
        }
        found |= bit;
        break;
      }
    }
  }

  if (out_found) { *out_found = found; }
  return ARNM_SUCCESS;
}

arnm_result arnm_json_read_array(
    arnm_json_value *array,
    arnm_json_value **out_values,
    uint32_t capacity,
    uint32_t *out_array_size
) {
  if (out_array_size) *out_array_size = 0;
  if (!array || !out_values) return ARNM_ERROR_NULL_POINTER;
  if (0 == capacity) return ARNM_ERROR_INVALID_PARAM;
  if (!unsafe_yyjson_is_arr((void *)array)) return ARNM_ERROR_INVALID_ENUM_TYPE;
  if (unsafe_yyjson_get_len((void *)array) > capacity)
    return ARNM_ERROR_DESTINATION_BUFFER_TO_SMALL;

  yyjson_val *val = NULL;
  yyjson_arr_iter iter = yyjson_arr_iter_with(to_yyjson(array));
  uint32_t count = 0;
  while ((val = yyjson_arr_iter_next(&iter)) != NULL) { out_values[count++] = to_public(val); }
  if (out_array_size) { *out_array_size = count; }
  return ARNM_SUCCESS;
}

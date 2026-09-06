
#include "arnm/json_writer.h"
#include "arnm/arena.h"

#include "arnm/converter.h"

// includes also yyjson
#include "json_memory.h"

#include <stdint.h>
#include <string.h>

/*
 * The whole of yyjson lives behind this file, exactly as it does behind json_reader.c. Nothing
 * above -- not a type, not a constant, not an include path -- reaches arnm/json_writer.h.
 *
 * Three things are bridged here.
 *
 * The allocator, which yyjson frees without a size and arnm needs one for: that seam is shared
 * with the reader and lives in json_memory.h. The document draws through the headered hooks;
 * the written text draws through the headerless ones, because it leaves in the caller's hands
 * and has to be a plain arnm allocation they can free themselves.
 *
 * The flags, which are ours and pinned by the public header, translated one by one so that a
 * bit nobody defined is refused rather than arriving somewhere as a feature nobody asked for.
 *
 * And the length of the text, which is the one thing yyjson cannot be asked for in advance. It
 * is kept here instead, a field at a time: every value knows its own rendered length, its
 * separator and its indentation the moment it is added, so the total is always current and
 * arnm_json_writer_size() reads rather than works. What that costs is one pass over each string
 * to count what escaping will do to it -- the same pass the serializer will make later, done
 * once more so the answer can exist before the text does.
 */

/* C11 static assert fallback; in C++ the keyword is already there */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/** @brief What a field without a name is called in a record: an element of an array. */
#define JSON_ELEMENT_FIELD_NAME "[]"
#define ERROR_MESSAGE_MISSING_KEY_OBJECT "Missing key for object container"
#define ERROR_MESSAGE_KEY_ON_ARRAY "Not null key for array container"
#define ERROR_INVALID_DEPTH_MESSAGE "arnm_json_writer_close was called to often"

/**
 * @brief Largest pool hints that can still be served, in the units the hint is written in.
 *
 * yyjson turns a hint into a chunk -- `(values + 1) * sizeof(yyjson_mut_val)` for the one,
 * `string_bytes + sizeof(yyjson_str_chunk)` for the other -- and that chunk is asked of the
 * allocator through the headered hooks, so what fits is JSON_BLOCK_MAX_PAYLOAD minus the
 * chunk's own head.
 */
#define JSON_HINT_MAX_VALUES ((uint32_t)(JSON_BLOCK_MAX_PAYLOAD / sizeof(yyjson_mut_val)) - 1u)
#define JSON_HINT_MAX_STRING_BYTES (JSON_BLOCK_MAX_PAYLOAD - (uint32_t)sizeof(yyjson_str_chunk))

/** @brief Marks a writer as initialized. A zeroed writer cannot hold it by accident. */
#define JSON_WRITER_MAGIC 0x61727377u /* "arsw" */

/**
 * @brief The layout behind the opaque @ref arnm_json_writer.
 *
 * @c size is the running length of the text, terminator and trailing newline excluded. It is
 * kept rather than computed because every part of it is known where it arrives and nowhere
 * else: the indent depends on the level the field went in at, the comma on whether anything was
 * there before it, and both are gone by the time a walk would come looking.
 *
 * @c stack holds the containers standing open, the root at index 0. A fixed depth and not an
 * allocation: a writer that reaches for memory in the middle of a field can fail in the middle
 * of a field, and there is no good answer to half a field.
 */
typedef struct json_writer_state {
  json_alc_context alc_context;    /**< Where the document comes from, and what an arena kept. */
  yyjson_alc alc;                  /**< yyjson's hooks, bound to alc_context. */
  yyjson_mut_doc *doc;             /**< The document being built, or NULL. */
  yyjson_write_flag write_flags;   /**< The public flags, translated once at init. */
  uint32_t depth;                  /**< Containers open, the root counted. */
  uint32_t elements;               /**< Element counts, used for size estimation. */
  uint32_t /*arnm_result*/ status; /**< First error since the document began, or ARNM_SUCCESS. */
  uint32_t magic;                  /**< JSON_WRITER_MAGIC once initialized. */
  uint32_t hint_values;            /**< Values a document is expected to hold, or 0 for none. */
  uint32_t hint_string_bytes;      /**< Copied string bytes expected, or 0 for none. */
  char error_field[ARNM_JSON_WRITER_FIELD_NAME_SIZE]; /**< Field name of @c status, or empty. */
  yyjson_mut_val *stack[ARNM_JSON_WRITER_MAX_DEPTH];  /**< Open containers, root first. */
} json_writer_state;

static_assert(
    sizeof(json_writer_state) <= ARNM_JSON_WRITER_SIZE,
    "ARNM_JSON_WRITER_SIZE in arnm/json_writer.h no longer covers json_writer_state"
);

// ********** casting across the seam *******************

/** @brief The name a refusal at @p key is recorded under. */
static const char *field_name(const char *key) {
  return key ? key : JSON_ELEMENT_FIELD_NAME;
}

/**
 * @brief The length that goes with @ref field_name(), the sentinel's own included.
 *
 * An element of an array has no key and therefore no length, but it is still recorded under a
 * name -- and a name recorded with a length of zero is no name at all. The sentinel measures
 * itself here, on the error path only, so nothing on the way through pays for it.
 */
static size_t field_name_length(const char *key, size_t key_length) {
  return key ? key_length : sizeof(JSON_ELEMENT_FIELD_NAME) - 1u;
}

/** @brief The state behind a writer, or NULL when there is none to speak of. */
static json_writer_state *state_of(const arnm_json_writer *writer) {
  if (!writer) { return NULL; }
  json_writer_state *state = (json_writer_state *)(void *)(uintptr_t)(const void *)writer;
  return (JSON_WRITER_MAGIC == state->magic) ? state : NULL;
}

/** @brief A size_t length narrowed to the uint32_t every size in arnm is carried in. */
static uint32_t narrow_length(size_t length) {
#if SIZE_MAX > UINT32_MAX
  if (length > (size_t)UINT32_MAX) { return UINT32_MAX; }
#endif
  return (uint32_t)length;
}

// ********** flags *******************

/** @brief Every bit arnm/json_writer.h defines; anything else is refused. */
#define JSON_WRITE_KNOWN_FLAGS                                                                     \
  (ARNM_JSON_WRITE_PRETTY | ARNM_JSON_WRITE_PRETTY_TWO_SPACES | ARNM_JSON_WRITE_ESCAPE_UNICODE |   \
   ARNM_JSON_WRITE_ESCAPE_SLASHES | ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL |                          \
   ARNM_JSON_WRITE_NEWLINE_AT_END)

/**
 * @brief Translate our flags into yyjson's, refusing a bit we do not know.
 *
 * @param[in]  flags Bit set from the public header.
 * @param[out] out   Receives yyjson's spelling of the same set.
 * @retval ARNM_SUCCESS             Translated.
 * @retval ARNM_ERROR_INVALID_PARAM @p flags holds a bit outside JSON_WRITE_KNOWN_FLAGS.
 */
static arnm_result translate_write_flags(arnm_json_write_flags flags, yyjson_write_flag *out) {
  if (0 != (flags & ~(arnm_json_write_flags)JSON_WRITE_KNOWN_FLAGS)) {
    return ARNM_ERROR_INVALID_PARAM;
  }

  // Two switches do not survive the build's YYJSON_DISABLE_NON_STANDARD, which takes the code
  // behind every YYJSON_WRITE_ALLOW_* out rather than defaulting it off. Translating a bit into
  // a serializer that no longer reads it would be a flag accepted here and ignored there, so the
  // public header carries no such bit to translate, and bits 4 and 6 are left empty so an old
  // value is refused above instead of landing on a neighbour. See arnm/json_writer.h.
  yyjson_write_flag translated = YYJSON_WRITE_NOFLAG;
  if (0 != (flags & ARNM_JSON_WRITE_PRETTY)) { translated |= YYJSON_WRITE_PRETTY; }
  if (0 != (flags & ARNM_JSON_WRITE_PRETTY_TWO_SPACES)) {
    translated |= YYJSON_WRITE_PRETTY_TWO_SPACES;
  }
  if (0 != (flags & ARNM_JSON_WRITE_ESCAPE_UNICODE)) { translated |= YYJSON_WRITE_ESCAPE_UNICODE; }
  if (0 != (flags & ARNM_JSON_WRITE_ESCAPE_SLASHES)) { translated |= YYJSON_WRITE_ESCAPE_SLASHES; }
  if (0 != (flags & ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL)) {
    translated |= YYJSON_WRITE_INF_AND_NAN_AS_NULL;
  }
  if (0 != (flags & ARNM_JSON_WRITE_NEWLINE_AT_END)) { translated |= YYJSON_WRITE_NEWLINE_AT_END; }

  *out = translated;
  return ARNM_SUCCESS;
}

/** @brief What a refusal from yyjson's serializer means in arnm's vocabulary. */
static arnm_result translate_write_error(uint32_t code) {
  if (YYJSON_WRITE_ERROR_MEMORY_ALLOCATION == code) { return ARNM_ERROR_OUT_OF_MEMORY; }
  if (YYJSON_WRITE_ERROR_INVALID_PARAMETER == code) { return ARNM_ERROR_INVALID_PARAM; }
  return ARNM_ERROR_ENCODE_FAILED;
}

// ********** the first error, and the name it wears *******************

/**
 * @brief Keep @p result under @p key, unless something was kept already.
 *
 * The first refusal is the one that explains the others, so it is the one that stays -- the
 * same rule the reader follows, for the same reason.
 *
 * @param[in,out] state  Writer state; not NULL.
 * @param[in]     result The refusal to keep; never ARNM_SUCCESS at this call site.
 * @param[in]     key    Field name to keep with it, truncated to fit; NULL records no name,
 *                       which is what a refusal belonging to no field wears.
 */
static void record_error(
    json_writer_state *state, arnm_result result, const char *key, size_t key_length
) {
  if (ARNM_SUCCESS != state->status) { return; }
  state->status = result;

  if (!key) {
    state->error_field[0] = '\0';
    return;
  }
  if (key_length > sizeof(state->error_field) - 1u) {
    key_length = sizeof(state->error_field) - 1u;
  }
  memcpy(state->error_field, key, key_length);
  state->error_field[key_length] = '\0';
}

// ********** building *******************

/** @brief Let the document go, and say whether an arena kept any of it. */
static void dispose_document(json_writer_state *state) {
  state->depth = 0;
  if (state->doc) {
    state->alc_context.arena_kept_bytes = false;
    if (arnm_is_arena(state->alc_context.allocator)) {
      arnm_reset(state->alc_context.allocator);
    } else {
      yyjson_mut_doc_free(state->doc);
    }
    state->doc = NULL;
  }
}

/**
 * @brief Start a document whose root is an object or an array.
 *
 * @param[in,out] state    Writer state; not NULL.
 * @param[in]     is_array Whether the root should be an array rather than an object.
 * @retval ARNM_SUCCESS             Ready, holding an empty root.
 * @retval ARNM_ERROR_OUT_OF_MEMORY The allocator had nothing left; recorded as well.
 */
static arnm_result begin_document(json_writer_state *state, bool is_array) {
  (void)dispose_document(state);
  state->status = ARNM_SUCCESS;
  state->error_field[0] = '\0';

  state->doc = yyjson_mut_doc_new(&state->alc);
  if (!state->doc) {
    record_error(state, ARNM_ERROR_OUT_OF_MEMORY, NULL, 0);
    return ARNM_ERROR_OUT_OF_MEMORY;
  }

  // Between the document and its root, which is the first value there is and so the first thing
  // to open a chunk.
  //
  // Both figures are bounded against what the allocator behind them could ever hand out, and a
  // hint past that is dropped rather than passed on. yyjson takes a chunk size in a size_t and
  // checks it against SIZE_MAX, so on a 64 bit host it accepts a figure no arnm allocator can
  // serve and then fails on the first allocation -- which would turn a hint that is only meant
  // to be a hint into a write that does not happen. What is dropped here leaves the default
  // growth, which is exactly the state a caller who said nothing is in.
  if (state->hint_values && state->hint_values <= JSON_HINT_MAX_VALUES) {
    (void)yyjson_mut_doc_set_val_pool_size(state->doc, state->hint_values);
  }
  if (state->hint_string_bytes && state->hint_string_bytes <= JSON_HINT_MAX_STRING_BYTES) {
    (void)yyjson_mut_doc_set_str_pool_size(state->doc, state->hint_string_bytes);
  }

  yyjson_mut_val *root = is_array ? yyjson_mut_arr(state->doc) : yyjson_mut_obj(state->doc);
  if (!root) {
    (void)dispose_document(state);
    record_error(state, ARNM_ERROR_OUT_OF_MEMORY, NULL, 0);
    return ARNM_ERROR_OUT_OF_MEMORY;
  }

  yyjson_mut_doc_set_root(state->doc, root);
  state->stack[0] = root;
  state->depth = 1;
  state->elements = 1;
  return ARNM_SUCCESS;
}

static yyjson_mut_val *field_obj(
    yyjson_mut_val *container,
    json_writer_state *state,
    const char *key,
    size_t key_length,
    bool escape_key
) {
  yyjson_mut_val *node = unsafe_yyjson_mut_val(state->doc, 2u);
  if (!node) {
    record_error(
        state, ARNM_ERROR_OUT_OF_MEMORY, field_name(key), field_name_length(key, key_length)
    );
    return NULL;
  }

  unsafe_yyjson_set_tag(
      node, YYJSON_TYPE_STR, escape_key ? YYJSON_SUBTYPE_NONE : YYJSON_SUBTYPE_NOESC, key_length
  );
  ((yyjson_val *)node)->uni.str = key;

  unsafe_yyjson_mut_obj_add(container, node, node + 1, unsafe_yyjson_get_len(container));
  state->elements += 2u;
  return node + 1;
}

static yyjson_mut_val *field_array(yyjson_mut_val *container, json_writer_state *state) {
  yyjson_mut_val *node = unsafe_yyjson_mut_val(state->doc, 1u);
  if (!node) {
    record_error(
        state, ARNM_ERROR_OUT_OF_MEMORY, JSON_ELEMENT_FIELD_NAME,
        sizeof(JSON_ELEMENT_FIELD_NAME) - 1
    );
    return NULL;
  }
  (void)yyjson_mut_arr_append(container, node);
  state->elements += 1u;
  return node;
}

static yyjson_mut_val *field(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key
) {
  json_writer_state *state = (json_writer_state *)writer;
  if (!state || ARNM_SUCCESS != state->status) { return NULL; }
  if (!state->doc && ARNM_SUCCESS != begin_document(state, false)) { return NULL; }
  if (0 == state->depth) {
    record_error(
        state, ARNM_ERROR_INVALID_STATE, ERROR_INVALID_DEPTH_MESSAGE,
        sizeof(ERROR_INVALID_DEPTH_MESSAGE)
    );
    return NULL;
  }

  yyjson_mut_val *container = state->stack[state->depth - 1u];
  const bool is_object = unsafe_yyjson_is_obj(container);
  if (is_object != (NULL != key)) {
    // if the container is an object but the key is missing... error
    if (is_object) {
      record_error(
          state, ARNM_ERROR_INVALID_PARAM, ERROR_MESSAGE_MISSING_KEY_OBJECT,
          sizeof(ERROR_MESSAGE_MISSING_KEY_OBJECT) - 1u
      );
    }
    // if the container is an array but there is also a key... error
    else {
      record_error(
          state, ARNM_ERROR_INVALID_PARAM, ERROR_MESSAGE_KEY_ON_ARRAY,
          sizeof(ERROR_MESSAGE_KEY_ON_ARRAY) - 1u
      );
    }
    return NULL;
  }
  if (is_object) {
    return field_obj(container, state, key, key_length, escape_key);
  } else {
    return field_array(container, state);
  }
}

// ********** manage the writer itself *******************

arnm_result arnm_json_writer_init(
    arnm_json_writer *writer,
    arnm *allocator,
    arnm_json_write_flags flags,
    const arnm_json_writer_hint *hint
) {
  if (!writer) { return ARNM_ERROR_NULL_POINTER; }

  // translated before a byte is written, so a flag nobody defined leaves the storage as it was
  // found rather than half prepared
  yyjson_write_flag write_flags = YYJSON_WRITE_NOFLAG;
  const arnm_result translated = translate_write_flags(flags, &write_flags);
  if (ARNM_SUCCESS != translated) { return translated; }

  json_writer_state *state = (json_writer_state *)(void *)writer;
  state->alc_context.allocator = allocator;
  state->alc_context.arena_kept_bytes = false;
  json_alc_bind(&state->alc, &state->alc_context);
  state->doc = NULL;
  state->write_flags = write_flags;
  state->depth = 0;
  state->elements = 0;
  state->status = ARNM_SUCCESS;
  state->hint_values = hint ? hint->values : 0u;
  state->hint_string_bytes = hint ? hint->string_bytes : 0u;
  state->error_field[0] = '\0';
  memset(state->stack, 0, sizeof(state->stack));
  state->magic = JSON_WRITER_MAGIC;
  return ARNM_SUCCESS;
}

arnm_json_writer *arnm_json_writer_create(
    arnm *allocator, arnm_json_write_flags flags, const arnm_json_writer_hint *hint
) {
  uint8_t *storage = NULL;
  if (ARNM_SUCCESS != arnm_alloc(&storage, (uint32_t)sizeof(arnm_json_writer), allocator)) {
    return NULL;
  }

  arnm_json_writer *writer = (arnm_json_writer *)(void *)storage;
  if (ARNM_SUCCESS != arnm_json_writer_init(writer, allocator, flags, hint)) {
    (void)arnm_free(storage, (uint32_t)sizeof(arnm_json_writer), allocator);
    return NULL;
  }
  return writer;
}

arnm_result arnm_json_writer_release(arnm_json_writer *writer) {
  if (!writer) { return ARNM_SUCCESS; }
  json_writer_state *state = state_of(writer);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }
  dispose_document(state);
  return ARNM_SUCCESS;
}

arnm_result arnm_json_writer_destroy(arnm_json_writer *writer, arnm *allocator) {
  if (!writer) { return ARNM_SUCCESS; }
  json_writer_state *state = state_of(writer);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }

  dispose_document(state);
  // the magic goes before the bytes do, so a second destroy finds an uninitialized writer
  state->magic = 0;

  const arnm_result given_back =
      arnm_free((uint8_t *)(void *)writer, (uint32_t)sizeof(arnm_json_writer), allocator);

  return given_back;
}

arnm_result arnm_json_writer_begin_object(arnm_json_writer *writer) {
  if (!writer) { return ARNM_ERROR_NULL_POINTER; }
  json_writer_state *state = state_of(writer);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }
  return begin_document(state, false);
}

arnm_result arnm_json_writer_begin_array(arnm_json_writer *writer) {
  if (!writer) { return ARNM_ERROR_NULL_POINTER; }
  json_writer_state *state = state_of(writer);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }
  return begin_document(state, true);
}

// ********** what the writer carries *******************

arnm_result arnm_json_writer_status(const arnm_json_writer *writer) {
  if (!writer) return ARNM_ERROR_NOT_INITIALIZED;
  const json_writer_state *state = (const json_writer_state *)writer;
  if (state->magic != JSON_WRITER_MAGIC) return ARNM_ERROR_NOT_INITIALIZED;
  return state->status;
}

const char *arnm_json_writer_error_field(const arnm_json_writer *writer) {
  const json_writer_state *state = (const json_writer_state *)writer;
  return state ? state->error_field : "";
}

arnm_result arnm_json_writer_clear_error(arnm_json_writer *writer) {
  if (!writer) { return ARNM_ERROR_NULL_POINTER; }
  json_writer_state *state = (json_writer_state *)writer;
  state->status = ARNM_SUCCESS;
  state->error_field[0] = '\0';
  return ARNM_SUCCESS;
}

uint32_t arnm_json_writer_depth(const arnm_json_writer *writer) {
  const json_writer_state *state = (const json_writer_state *)writer;
  return state ? state->depth : 0u;
}

// ********** adding a field, one line per struct member *******************

void arnm_json_writer_add_null(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key
) {
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (node) { unsafe_yyjson_set_null(node); }
}

void arnm_json_writer_add_bool(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key, bool value
) {
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (node) { unsafe_yyjson_set_bool(node, value); }
}

void arnm_json_writer_add_int64(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key, int64_t value
) {
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (node) { unsafe_yyjson_set_sint(node, value); }
}

void arnm_json_writer_add_uint64(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key, uint64_t value
) {
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (node) { unsafe_yyjson_set_uint(node, value); }
}

void arnm_json_writer_add_double(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key, double value
) {
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (node) { unsafe_yyjson_set_real(node, value); }
}

static inline bool is_string_copy(arnm_json_writer_string_flags flags) {
  return ARNM_JSON_WRITER_STRING_COPY == (flags & ARNM_JSON_WRITER_STRING_COPY);
}
static inline bool is_string_raw(arnm_json_writer_string_flags flags) {
  return ARNM_JSON_WRITER_STRING_RAW == (flags & ARNM_JSON_WRITER_STRING_RAW);
}
static inline bool is_string_escape(arnm_json_writer_string_flags flags) {
  return ARNM_JSON_WRITER_STRING_ESCAPE == (flags & ARNM_JSON_WRITER_STRING_ESCAPE);
}

void arnm_json_writer_add_string_flags(
    arnm_json_writer *writer,
    const char *key,
    size_t key_length,
    bool escape_key,
    const char *value,
    size_t value_length,
    arnm_json_writer_string_flags flags
) {
  json_writer_state *state = (json_writer_state *)writer;
  if (!state->doc && ARNM_SUCCESS != begin_document(state, false)) { return; }

  // is copy
  if (is_string_copy(flags)) {
    char *copied = unsafe_yyjson_mut_strncpy(state->doc, value, value_length);
    value = copied;
  }
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (!node) { return; }

  // a NULL string is the literal null: an optional member that is not there, which is what a
  // mapper means by it far more often than it means a mistake
  if (!value) {
    unsafe_yyjson_set_null(node);
    return;
  }

  unsafe_yyjson_set_tag(
      node, is_string_raw(flags) ? YYJSON_TYPE_RAW : YYJSON_TYPE_STR,
      is_string_escape(flags) ? YYJSON_SUBTYPE_NONE : YYJSON_SUBTYPE_NOESC, value_length
  );
  ((yyjson_val *)node)->uni.str = value;
}

void arnm_json_writer_add_string(
    arnm_json_writer *writer,
    const char *key,
    size_t key_length,
    bool escape_key,
    const char *value,
    size_t value_length
) {
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (!node) { return; }

  // a NULL string is the literal null: an optional member that is not there, which is what a
  // mapper means by it far more often than it means a mistake
  if (!value) {
    unsafe_yyjson_set_null(node);
    return;
  }
  unsafe_yyjson_set_tag(node, YYJSON_TYPE_STR, YYJSON_SUBTYPE_NOESC, value_length);
  ((yyjson_val *)node)->uni.str = value;
}

/**
 * @brief Largest block whose hex, its two quotes and a terminator still fit a uint32_t.
 *
 * Two characters per byte, plus the quotes this writes around them, plus the NUL the string
 * pool wants behind every entry.
 */
void arnm_json_writer_add_hex(
    arnm_json_writer *writer,
    const char *key,
    size_t key_length,
    bool escape_key,
    const uint8_t *data,
    uint32_t size
) {
  json_writer_state *state = (json_writer_state *)writer;
  if (!state->doc && ARNM_SUCCESS != begin_document(state, false)) { return; }

  // an empty block is the empty string and not `null`: it says the field was there and held
  // nothing, which is what the block itself said
  if (!data || 0 == size) {
    yyjson_mut_set_raw(field(writer, key, key_length, escape_key), "\"\"", 2u);
    return;
  }

  // The text is written straight into the document's string pool -- no buffer of the caller's
  // to format in and no copy out of it afterwards. The quotes are part of what is written,
  // because this goes in as a raw value: see the note above the declaration for why.
  const size_t text_length = (size_t)size * 2u + 2u;
  char *text = unsafe_yyjson_mut_str_alc(state->doc, text_length);
  if (!text) {
    record_error(state, ARNM_ERROR_OUT_OF_MEMORY, key, key_length);
    return;
  }

  text[0] = '"';
  // arnm_binary_to_hex() closes its run with a terminator, which lands exactly where the
  // closing quote goes and is overwritten by it a line later
  const arnm_result result = arnm_binary_to_hex(text + 1, data, size);
  if (ARNM_SUCCESS != result) {
    record_error(state, result, key, key_length);
    return;
  }
  text[text_length - 1u] = '"';
  text[text_length] = '\0';

  // The length is exact rather than a bound: hex holds no character any flag escapes, so what
  // is measured here is what the serializer will lay down.
  yyjson_mut_set_raw(field(writer, key, key_length, escape_key), text, text_length);
}

/**
 * @brief Largest block whose base64, its two quotes and a terminator still fit a uint32_t.
 *
 * Four characters per three bytes, rounded up to a whole group, plus the quotes and the NUL.
 */
#define JSON_BASE64_MAX_BYTES (((ARNM_MAX_ALLOC_SIZE - 3u) / 4u) * 3u)

void arnm_json_writer_add_base64(
    arnm_json_writer *writer,
    const char *key,
    size_t key_length,
    bool escape_key,
    const uint8_t *data,
    uint32_t size
) {
  json_writer_state *state = (json_writer_state *)writer;
  if (!state->doc && ARNM_SUCCESS != begin_document(state, false)) { return; }

  // an empty block is the empty string and not `null`: it says the field was there and held
  // nothing, which is what the block itself said
  if (!data || 0 == size) {
    yyjson_mut_set_raw(field(writer, key, key_length, escape_key), "\"\"", 2u);
    return;
  }

  const uint32_t text_length = ARNM_BASE64_STRING_LENGTH(size) + 2u;
  char *text = unsafe_yyjson_mut_str_alc(state->doc, text_length);
  if (!text) {
    record_error(state, ARNM_ERROR_OUT_OF_MEMORY, key, key_length);
    return;
  }

  text[0] = '"';
  // as in arnm_json_writer_add_hex(): the terminator the converter leaves behind lands exactly
  // where the closing quote goes
  const arnm_result encoded = arnm_binary_to_base64(text + 1, data, size);
  if (ARNM_SUCCESS != encoded) {
    record_error(state, encoded, key, key_length);
    return;
  }
  text[text_length - 1u] = '"';
  text[text_length] = '\0';

  // exact, not a bound: no character of the standard alphabet, padding included, is one any
  // write flag escapes
  yyjson_mut_set_raw(field(writer, key, key_length, escape_key), text, text_length);
}

/** @brief What a uuid renders as: the canonical form and the two quotes around it. */
#define JSON_UUID_TEXT_LENGTH (ARNM_UUID_STRING_LENGTH + 2u)

void arnm_json_writer_add_uuid(
    arnm_json_writer *writer,
    const char *key,
    size_t key_length,
    bool escape_key,
    const uint8_t *uuid
) {
  json_writer_state *state = (json_writer_state *)writer;
  if (!state->doc && ARNM_SUCCESS != begin_document(state, false)) { return; }

  // sixteen bytes or nothing at all -- there is no size here that could make an absent uuid an
  // empty one, so NULL is the member that is not there
  if (!uuid) {
    yyjson_mut_set_null(field(writer, key, key_length, escape_key));
    return;
  }

  char *text = unsafe_yyjson_mut_str_alc(state->doc, JSON_UUID_TEXT_LENGTH);
  if (!text) {
    record_error(state, ARNM_ERROR_OUT_OF_MEMORY, key, key_length);
    return;
  }

  text[0] = '"';
  // as in arnm_json_writer_add_hex(): the terminator arnm_uuid_to_string() leaves behind lands
  // where the closing quote goes
  arnm_uuid_to_string(text + 1, uuid);
  text[JSON_UUID_TEXT_LENGTH - 1u] = '"';
  text[JSON_UUID_TEXT_LENGTH] = '\0';

  unsafe_yyjson_set_raw(field(writer, key, key_length, escape_key), text, JSON_UUID_TEXT_LENGTH);
}

// ********** nesting *******************

/** @brief The shared body of the two opens. */
static void open_container(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key, bool is_array
) {
  json_writer_state *state = (json_writer_state *)writer;
  if (state->depth >= ARNM_JSON_WRITER_MAX_DEPTH) {
    record_error(state, ARNM_ERROR_RESOURCE_EXHAUSTED, key, key_length);
    return;
  }
  yyjson_mut_val *node = field(writer, key, key_length, escape_key);
  if (!node) { return; }

  // an empty container is its tag and nothing else, exactly as yyjson_mut_obj() leaves one
  node->tag =
      is_array ? (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE) : (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE);
  // the empty form is charged now; the first field that lands inside pays the difference
  state->stack[state->depth] = node;
  state->depth += 1u;
  state->elements++;
}

void arnm_json_writer_open_object(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key
) {
  open_container(writer, key, key_length, escape_key, false);
}

void arnm_json_writer_open_array(
    arnm_json_writer *writer, const char *key, size_t key_length, bool escape_key
) {
  open_container(writer, key, key_length, escape_key, true);
}

void arnm_json_writer_close(arnm_json_writer *writer) {
  json_writer_state *state = state_of(writer);
  if (!state || ARNM_SUCCESS != state->status) { return; }

  if (state->depth <= 1u) {
    // the root closes itself at the write; one close too many would move the next field
    // somewhere nobody expects, and silence about that is worse than the record
    record_error(state, ARNM_ERROR_INVALID_STATE, NULL, 0);
    return;
  }
  state->depth -= 1u;
}

// ********** measuring and writing *******************

// copied from yyjson.c
/** Returns whether the size is power of 2 (size should not be 0). */
static inline bool size_is_pow2(size_t size) {
  return (size & (size - 1)) == 0;
}

/** Align size upwards (may overflow). */
static inline size_t size_align_up(size_t size, size_t align) {
  if (size_is_pow2(align)) {
    return (size + (align - 1)) & ~(align - 1);
  } else {
    return size + align - (size + align - 1) % align - 1;
  }
}

/** @brief Bytes charged per element, minified and under either pretty layout. */
#define JSON_ELEMENT_TEXT_ESTIMATE 18u
#define JSON_ELEMENT_TEXT_ESTIMATE_PRETTY 32u
/** @brief Added once, so the smallest documents are not estimated down to nothing. */
#define JSON_ESTIMATE_FLOOR 64u

uint32_t arnm_json_writer_buffer_size_min(const arnm_json_writer *writer) {
  const json_writer_state *state = state_of(writer);
  if (!state || !state->doc) { return 0u; }

  // write_flags is yyjson's bit set and not the public one, so the two pretty layouts are asked
  // for by yyjson's names -- ARNM_JSON_WRITE_PRETTY_TWO_SPACES does not sit on the same bit
  const bool is_pretty =
      0 != (state->write_flags & (YYJSON_WRITE_PRETTY | YYJSON_WRITE_PRETTY_TWO_SPACES));
  const uint32_t per_element =
      is_pretty ? JSON_ELEMENT_TEXT_ESTIMATE_PRETTY : JSON_ELEMENT_TEXT_ESTIMATE;

  // widened before the multiply: a document of millions of elements overflows a uint32_t here
  // long before it reaches the ceiling, and an estimate that wraps is worse than one that saturates
  uint64_t total = (uint64_t)state->elements * per_element + JSON_ESTIMATE_FLOOR;
  if (total > (uint64_t)ARNM_MAX_ALLOC_SIZE) { return ARNM_MAX_ALLOC_SIZE; }
  total = (uint64_t)size_align_up((size_t)total, 16u);
  return (total > (uint64_t)ARNM_MAX_ALLOC_SIZE) ? ARNM_MAX_ALLOC_SIZE : (uint32_t)total;
}

arnm_result arnm_json_writer_write(
    arnm_json_writer *writer, arnm *allocator, arnm_memory_block *out, uint32_t *out_length
) {
  if (!writer || !out) { return ARNM_ERROR_NULL_POINTER; }
  json_writer_state *state = state_of(writer);
  if (!state) { return ARNM_ERROR_NOT_INITIALIZED; }
  if (ARNM_SUCCESS != state->status) { return state->status; }
  if (!state->doc) { return ARNM_ERROR_INVALID_STATE; }

  // The serializer keeps its own state at the far end of the buffer it writes into and grows
  // that buffer by its own reckoning, which is generous where a string might need escaping. That
  // room is scratch, so it comes from the writer's own allocator -- the one already carrying the
  // document -- and goes back before this returns.
  json_buffer_context write_buffer;
  write_buffer.allocator = allocator;
  write_buffer.size = 0;
  yyjson_alc alc;
  json_buffer_bind(&alc, &write_buffer);

  size_t length = 0;
  yyjson_write_err error;
  char *written = yyjson_mut_write_opts(state->doc, state->write_flags, &alc, &length, &error);
  if (!written) { return translate_write_error((uint32_t)error.code); }
  uint32_t narrowed_length = narrow_length(length);
  if ((size_t)narrowed_length != length) {
    // the bytes exist and cannot be handed over, so they go back rather than being stranded
    json_buffer_dispose(&write_buffer, written);
    return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  }
  const uint32_t needed = narrowed_length + 1u; /* the terminator yyjson wrote */

  arnm_memory_block block;
  block.data = (uint8_t *)written;
  block.size = write_buffer.size;
  // measured exactly, this changes nothing; measured as a bound -- a real number, an escaped
  // character -- the slack goes home here, while the block is still at the tail
  const arnm_result shrunk = arnm_memory_block_realloc(&block, needed, allocator);

  *out = block;
  if (out_length) { *out_length = narrowed_length; }
  return (ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED == shrunk) ? shrunk : ARNM_SUCCESS;
}

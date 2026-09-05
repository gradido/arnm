#include "arnm/arena.h"
#include "arnm/converter.h"
#include "arnm/json_reader.h"
#include "arnm/json_writer.h"
#include "arnm/memory.h"
#include "arnm/memory_block.h"
#include "arnm/mono_timer.h"
#include "arnm/result.h"
#include "bench_report.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What this benchmark measures
 *
 * Both directions, over one set of documents.
 *
 * The writer has two moving parts: the building, where a field at a time goes into a document
 * held in the arena, and the rendering, where that document becomes text. They are timed apart
 * because a caller pays them apart -- the size is asked for between the two, and that is what an
 * output arena is sized by. Both are timed a second time with the writer told up front how big
 * the document will be, because that hint is the one knob its header offers and a row beside the
 * one without it is the only honest way to say what turning it is worth.
 *
 * The reader has three moving parts: a parse, a walk over an object, and a read of an array.
 * Everything a consumer does is some arrangement of those, so each is measured on its own and
 * then all three together on a document shaped like something real.
 *
 * Every payload below is built by the writer at prepare time and rendered once; that text is
 * what the reader then parses, so both halves are about the same six documents. Nothing is
 * described twice, and no row compares a document with a different one wearing its name.
 *
 * One comparison that used to live here is gone with the API it measured. The walk used to be
 * timed against asking for each member by name, which is what a caller wrote before there was a
 * table; there is no longer a call that fetches a single member, so there is nothing left to
 * compare it against. What can still be shown is how the walk's own cost moves with where the
 * members sit, which is section three.
 *
 * Numbers from a debug build answer a different question -- there is no vector body and no
 * inlining there. Build with -Doptimize=ReleaseFast before reporting any of them.
 */

#define PARSE_STEPS 20000
#define WALK_STEPS 200000
/* a build and the render after it cost more than a parse of the same document, and there are
   four such rows per payload, so the writing runs at half the parse count */
#define WRITE_STEPS 10000
/* a field read, so the count has to be large enough that the loop around it is not the figure */
#define ASK_STEPS 2000000

#define SCRATCH_CAPACITY (8u * 1024u * 1024u)
#define KEPT_CAPACITY (8u * 1024u * 1024u)
#define OUTPUT_CAPACITY (1u * 1024u * 1024u)
#define TEXT_CAPACITY 65536

#define RECORD_DIGEST_SIZE 32
#define RECORD_FIELD_COUNT 9
#define SPARE_COUNT 24
#define ARRAY_ELEMENTS 64
#define LONG_VALUE_LENGTH 256

static arnm scratch;
static arnm kept;
static arnm output;

/** Receives every result, so the compiler cannot drop the call. */
static uint64_t g_sink;

static void require_ok(arnm_result result, const char *what) {
  if (ARNM_SUCCESS != result && ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED != result) {
    fprintf(stderr, "benchmark setup failed: %s (%s)\n", what, arnm_result_to_string(result));
    exit(EXIT_FAILURE);
  }
}

/* --- the one description both directions rest on --------------------------------------------- */

/**
 * @brief What a payload is made of.
 *
 * @c elements 0 is a flat record; anything else wraps that many of them in an `items` array. The
 * two flags move where the members sit without changing which members they are, which is what
 * lets section three attribute a difference to the layout and nothing else.
 */
typedef struct shape {
  uint32_t elements;    /**< Records inside `items`, or 0 for a flat one. */
  uint32_t spares;      /**< Members per record beyond the nine that are wanted. */
  bool spares_in_front; /**< Whether those spares come before the nine or after them. */
  bool reversed;        /**< Whether the nine are written last first. */
  uint32_t length;      /**< Bytes in the record's one string member. */
} shape;

typedef struct payload {
  const char *name;
  shape form;
  char text[TEXT_CAPACITY]; /**< The rendered JSON, written once at prepare time. */
  uint32_t length;
  arnm_json_reader reader;    /**< Parsed once and held, for the rows that are not about parsing. */
  arnm_json_value *root;      /**< That document's way in. */
  arnm_json_writer_hint hint; /**< This document's own size, for the rows that are told it. */
  uint32_t promised;          /**< What the writer said the text would take, before it wrote it. */
  arnm_json_writer writer;    /**< Built once and held, for the row that only asks it a question. */
} payload;

#define SHORT_VALUE "a string value of an ordinary length"

static char spare_keys[SPARE_COUNT][16];
static char long_value[LONG_VALUE_LENGTH + 1];
static uint8_t record_digest[RECORD_DIGEST_SIZE];
static uint8_t record_uuid[ARNM_UUID_BINARY_SIZE];

/** One member of the record, by its index in the table's order. */
static void write_record_field(arnm_json_writer *writer, unsigned index, const shape *form) {
  const char *value = (LONG_VALUE_LENGTH == form->length) ? long_value : SHORT_VALUE;
  switch (index) {
  case 0:
    arnm_json_writer_add_string_length(writer, "name", 4, value, form->length);
    break;
  case 1:
    arnm_json_writer_add_uint64(writer, "id", 2, UINT64_C(0x0123456789abcdef));
    break;
  case 2:
    arnm_json_writer_add_int64(writer, "balance", 7, INT64_C(-4200000000));
    break;
  case 3:
    arnm_json_writer_add_uint64(writer, "port", 4, 8443);
    break;
  case 4:
    arnm_json_writer_add_int64(writer, "offset", 6, -12345);
    break;
  case 5:
    arnm_json_writer_add_double(writer, "ratio", 5, 0.6180339887498949);
    break;
  case 6:
    arnm_json_writer_add_bool(writer, "active", 6, true);
    break;
  case 7:
    arnm_json_writer_add_hex(writer, "digest", 6, record_digest, RECORD_DIGEST_SIZE);
    break;
  default:
    arnm_json_writer_add_uuid(writer, "uuid", 4, record_uuid);
    break;
  }
}

static void write_spares(arnm_json_writer *writer, uint32_t count) {
  for (uint32_t index = 0; index < count; ++index) {
    arnm_json_writer_add_string_length(
        writer, spare_keys[index], 8, SHORT_VALUE, (uint32_t)(sizeof(SHORT_VALUE) - 1u)
    );
  }
}

static void build_record(arnm_json_writer *writer, const shape *form) {
  if (form->spares_in_front) { write_spares(writer, form->spares); }
  for (unsigned index = 0; index < RECORD_FIELD_COUNT; ++index) {
    write_record_field(writer, form->reversed ? RECORD_FIELD_COUNT - 1u - index : index, form);
  }
  if (!form->spares_in_front) { write_spares(writer, form->spares); }
}

static void build_payload(arnm_json_writer *writer, const shape *form) {
  if (0 == form->elements) {
    build_record(writer, form);
    return;
  }
  arnm_json_writer_open_array(writer, "items", 5);
  for (uint32_t index = 0; index < form->elements; ++index) {
    arnm_json_writer_open_object(writer, NULL, 0);
    build_record(writer, form);
    arnm_json_writer_close(writer);
  }
  arnm_json_writer_close(writer);
}

/* clang-format off */
static payload in_order = {"in order",  {0, 0, false, false, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{0}}, NULL, {0, 0}, 0, {{0}}};
static payload behind   = {"24 behind", {0, SPARE_COUNT, false, false, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{0}}, NULL, {0, 0}, 0, {{0}}};
static payload in_front = {"24 in front",{0, SPARE_COUNT, true, false, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{0}}, NULL, {0, 0}, 0, {{0}}};
static payload reversed = {"reversed",  {0, 0, false, true, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{0}}, NULL, {0, 0}, 0, {{0}}};
static payload nested   = {"nested",    {ARRAY_ELEMENTS, 0, false, false, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{0}}, NULL, {0, 0}, 0, {{0}}};
static payload text     = {"text",      {0, 0, false, false, LONG_VALUE_LENGTH}, {0}, 0, {{0}}, NULL, {0, 0}, 0, {{0}}};
/* clang-format on */

static payload *const payloads[] = {&in_order, &behind, &in_front, &reversed, &nested, &text};
#define PAYLOAD_COUNT (sizeof(payloads) / sizeof(payloads[0]))

/** The four layouts section three is about, in the order it prints them. */
static payload *const layouts[] = {&in_order, &behind, &in_front, &reversed};
#define LAYOUT_COUNT (sizeof(layouts) / sizeof(layouts[0]))

/* --- writing: the document built, and the text rendered from it ------------------------------ */

/**
 * @brief The hint this payload's document answers to, worked out rather than guessed.
 *
 * Both figures follow the header's own accounting. The values are what the reader counted in the
 * text -- it counts a key as a node and a container as one of its own, which is the convention
 * the hint is written in, so the number the reader arrived at from the finished text is the same
 * one the writer wants before there is any. The bytes are only what the document copies: the
 * digest as hex and the uuid, both formatted into its storage, terminator each. Every other
 * member borrows its string or is a number, and borrowed costs nothing however long it is.
 *
 * Being exact is the point of the row it feeds. A hint that was merely close would still be a
 * fair thing for a caller to pass, but then the row would be about how close it was.
 */
static arnm_json_writer_hint hint_of(const payload *one) {
  const uint32_t records = one->form.elements ? one->form.elements : 1u;
  const uint32_t copied_per_record =
      (RECORD_DIGEST_SIZE * 2u + 1u) + (ARNM_UUID_STRING_LENGTH + 1u);
  arnm_json_writer_hint hint;
  hint.values = arnm_json_reader_value_count(&one->reader);
  hint.string_bytes = records * copied_per_record;
  return hint;
}

/** Build one document field by field and stop before the text: the writer's own first half. */
static void build_document(payload *one, const arnm_json_writer_hint *hint, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    arnm_json_writer writer;
    require_ok(
        arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, hint), "writer init"
    );
    build_payload(&writer, &one->form);
    require_ok(arnm_json_writer_status(&writer), "build");
    sink += arnm_json_writer_buffer_size_min(&writer);
    (void)arnm_json_writer_release(&writer);
    arnm_reset(&scratch);
  }
  g_sink += sink;
}

/**
 * @brief The same building, and then the text, and then both given back.
 *
 * The text is drawn from its own arena rather than from the one the document sits in, which is
 * how a caller who keeps the text past the writer has to do it: the document goes back at the
 * end of the round and the text does not.
 */
static void render_document(payload *one, const arnm_json_writer_hint *hint, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    arnm_json_writer writer;
    require_ok(
        arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, hint), "writer init"
    );
    build_payload(&writer, &one->form);

    arnm_memory_block rendered;
    require_ok(arnm_json_writer_write(&writer, &output, &rendered, NULL), "write");
    sink += rendered.size;
    require_ok(arnm_memory_block_free(&rendered, &output), "give the text back");

    (void)arnm_json_writer_release(&writer);
    arnm_reset(&scratch);
    arnm_reset(&output);
  }
  g_sink += sink;
}

/**
 * @brief The one question a writer answers without doing anything: how long the text will be.
 *
 * A field read, not a walk -- the number is kept as the document is built. The row exists to say
 * that in a figure, because it is what lets an output arena be sized before there is any text to
 * size it against, and a caller only does that if asking is free. Timed on the writer that
 * stands for the whole run rather than on one built inside the loop, which would time the
 * building instead.
 */
static void measure_size(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) { sink += arnm_json_writer_buffer_size_min(&one->writer); }
  g_sink += sink;
}

/* --- reading one record ---------------------------------------------------------------------- */

typedef struct record_target {
  arnm_memory_block name;
  uint64_t id;
  int64_t balance;
  uint32_t port;
  int32_t offset;
  double ratio;
  bool active;
  uint8_t digest[RECORD_DIGEST_SIZE];
  uint8_t uuid[ARNM_UUID_BINARY_SIZE];
} record_target;

/**
 * @brief The whole record in one walk, every field type the table knows represented once.
 *
 * The two blocks are locals rather than members of @p into because HEX_FIXED and UUID read the
 * descriptor they are handed -- it names the buffer to decode into and the size to measure the
 * string against. Filling them is part of what a caller pays and therefore part of what is
 * timed.
 */
static arnm_result read_record(arnm_json_value *object, record_target *into, uint64_t *out_found) {
  arnm_memory_block digest = ARNM_JSON_BLOCK_OF(into->digest);
  arnm_memory_block uuid = ARNM_JSON_BLOCK_OF(into->uuid);
  arnm_json_field fields[] = {
      ARNM_JSON_FIELD_STRING("name", &into->name),
      ARNM_JSON_FIELD_UINT64("id", &into->id),
      ARNM_JSON_FIELD_INT64("balance", &into->balance),
      ARNM_JSON_FIELD_UINT32("port", &into->port),
      ARNM_JSON_FIELD_INT32("offset", &into->offset),
      ARNM_JSON_FIELD_DOUBLE("ratio", &into->ratio),
      ARNM_JSON_FIELD_BOOL("active", &into->active),
      ARNM_JSON_FIELD_HEX_FIXED("digest", &digest),
      ARNM_JSON_FIELD_UUID("uuid", &uuid)
  };
  return arnm_json_read_object(object, fields, RECORD_FIELD_COUNT, out_found);
}

/** Something from every field, so no part of a walk can be optimized away. */
static uint64_t digest_of(const record_target *one) {
  uint64_t sum = one->id ^ (uint64_t)one->balance;
  sum += one->port + (uint64_t)(uint32_t)one->offset + (uint64_t)(one->ratio * 1000.0);
  sum += (one->active ? 1u : 0u) + one->name.size;
  for (unsigned index = 0; index < RECORD_DIGEST_SIZE; ++index) { sum += one->digest[index]; }
  for (unsigned index = 0; index < ARNM_UUID_BINARY_SIZE; ++index) { sum += one->uuid[index]; }
  return sum;
}

/* --- the three moving parts ------------------------------------------------------------------ */

static char insitu_buffer[TEXT_CAPACITY];

static void refill_insitu(payload *one) {
  memcpy(insitu_buffer, one->text, (size_t)one->length);
  memset(insitu_buffer + one->length, 0, ARNM_JSON_READER_INSITU_PADDING);
}

static void parse_copying(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    require_ok(arnm_json_reader_init(&reader, &scratch), "reader init");
    require_ok(arnm_json_reader_parse(&reader, one->text, one->length, false, &root), "parse");
    sink += arnm_json_reader_value_count(&reader);
    (void)arnm_json_reader_release(&reader);
    arnm_reset(&scratch);
  }
  g_sink += sink;
}

static void parse_insitu(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    refill_insitu(one);
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    require_ok(arnm_json_reader_init(&reader, &scratch), "reader init");
    require_ok(
        arnm_json_reader_parse_insitu(
            &reader, insitu_buffer, one->length, (uint32_t)sizeof(insitu_buffer), false, &root
        ),
        "parse insitu"
    );
    sink += arnm_json_reader_value_count(&reader);
    (void)arnm_json_reader_release(&reader);
    arnm_reset(&scratch);
  }
  g_sink += sink;
}

/** The refill on its own: what the row above carries that the copying parse does not. */
static void refill_only(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    refill_insitu(one);
    sink += (unsigned char)insitu_buffer[one->length - 1u];
  }
  g_sink += sink;
}

static void walk_record(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    record_target into;
    uint64_t found = 0;
    require_ok(read_record(one->root, &into, &found), "walk");
    sink += digest_of(&into) + found;
  }
  g_sink += sink;
}

/** The array read on its own: handles out, nothing converted. */
static void read_items(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    arnm_json_value *items = NULL;
    arnm_json_field outer[] = {ARNM_JSON_FIELD_VALUE("items", &items)};
    require_ok(arnm_json_read_object(one->root, outer, 1, NULL), "items");

    arnm_json_value *elements[ARRAY_ELEMENTS];
    uint32_t count = 0;
    require_ok(arnm_json_read_array(items, elements, ARRAY_ELEMENTS, &count), "array");
    sink += count + (uintptr_t)elements[count - 1u];
  }
  g_sink += sink;
}

/** All three together, over a document nothing has touched yet: what a consumer actually pays. */
static void traverse_whole(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    refill_insitu(one);
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    require_ok(arnm_json_reader_init(&reader, &scratch), "reader init");
    require_ok(
        arnm_json_reader_parse_insitu(
            &reader, insitu_buffer, one->length, (uint32_t)sizeof(insitu_buffer), false, &root
        ),
        "parse insitu"
    );

    arnm_json_value *items = NULL;
    arnm_json_field outer[] = {ARNM_JSON_FIELD_VALUE("items", &items)};
    require_ok(arnm_json_read_object(root, outer, 1, NULL), "items");

    arnm_json_value *elements[ARRAY_ELEMENTS];
    uint32_t count = 0;
    require_ok(arnm_json_read_array(items, elements, ARRAY_ELEMENTS, &count), "array");
    for (uint32_t index = 0; index < count; ++index) {
      record_target into;
      require_ok(read_record(elements[index], &into, NULL), "walk");
      sink += digest_of(&into);
    }

    (void)arnm_json_reader_release(&reader);
    arnm_reset(&scratch);
  }
  g_sink += sink;
}

/* --- checks that keep the rows honest -------------------------------------------------------- */

/**
 * @brief Read every layout once and check it came back whole, before any of them is timed.
 *
 * Four layouts carrying the same nine members must produce the same nine values; a row whose
 * document quietly lost one would otherwise show up as the fastest of them.
 */
static void verify_layouts_agree(void) {
  record_target first;
  uint64_t found = 0;
  require_ok(read_record(in_order.root, &first, &found), "walk");
  if (found != (UINT64_C(1) << RECORD_FIELD_COUNT) - 1u) {
    fprintf(stderr, "benchmark setup failed: the reference record is incomplete\n");
    exit(EXIT_FAILURE);
  }
  const uint64_t expected = digest_of(&first);

  for (size_t index = 1; index < LAYOUT_COUNT; ++index) {
    record_target other;
    require_ok(read_record(layouts[index]->root, &other, &found), "walk");
    if (found != (UINT64_C(1) << RECORD_FIELD_COUNT) - 1u || digest_of(&other) != expected) {
      fprintf(
          stderr, "benchmark setup failed: layout '%s' did not read back the same record\n",
          layouts[index]->name
      );
      exit(EXIT_FAILURE);
    }
  }
}

/** @brief Where the scratch arena's index stands, as an address. */
static uintptr_t arena_mark(void) {
  uint8_t *probe = NULL;
  require_ok(arnm_alloc(&probe, 1, &scratch), "arena probe");
  require_ok(arnm_free(probe, 1, &scratch), "arena probe");
  return (uintptr_t)probe;
}

/* --- test data -------------------------------------------------------------------------------- */

static void prepare_test_data(void) {
  require_ok(arnm_init_arena(&scratch, SCRATCH_CAPACITY), "scratch arena");
  require_ok(arnm_init_arena(&kept, KEPT_CAPACITY), "kept arena");
  require_ok(arnm_init_arena(&output, OUTPUT_CAPACITY), "output arena");

  memset(long_value, 'x', LONG_VALUE_LENGTH);
  long_value[LONG_VALUE_LENGTH] = '\0';
  for (uint32_t index = 0; index < SPARE_COUNT; ++index) {
    snprintf(spare_keys[index], sizeof(spare_keys[index]), "spare_%02u", (unsigned)index);
  }
  for (uint32_t index = 0; index < RECORD_DIGEST_SIZE; ++index) {
    record_digest[index] = (uint8_t)(index * 7u + 1u);
  }
  for (uint32_t index = 0; index < ARNM_UUID_BINARY_SIZE; ++index) {
    record_uuid[index] = (uint8_t)(0xf0u - index);
  }

  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    payload *one = payloads[index];

    arnm_json_writer writer;
    require_ok(
        arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, NULL), "writer init"
    );
    build_payload(&writer, &one->form);
    require_ok(arnm_json_writer_status(&writer), one->name);
    // asked before the text exists, which is the only moment the answer is of any use
    one->promised = arnm_json_writer_buffer_size_min(&writer);

    arnm_memory_block rendered;
    require_ok(arnm_json_writer_write(&writer, &output, &rendered, &one->length), "write payload");
    
    // the insitu rows render this text into a buffer of the same size and write padding past its
    // end, so the room for that padding is what has to fit
    if (one->length + ARNM_JSON_READER_INSITU_PADDING > TEXT_CAPACITY) {
      fprintf(stderr, "benchmark setup failed: payload '%s' outgrew its buffer\n", one->name);
      exit(EXIT_FAILURE);
    }
    memcpy(one->text, rendered.data, (size_t)one->length + 1u);
    require_ok(arnm_memory_block_free(&rendered, &output), "give the text back");
    (void)arnm_json_writer_release(&writer);
    arnm_reset(&scratch);
    arnm_reset(&output);

    // the reader that stays, on that same text: the walk rows are not about parsing
    require_ok(arnm_json_reader_init(&one->reader, &kept), "reader init");
    require_ok(
        arnm_json_reader_parse(&one->reader, one->text, one->length, false, &one->root), one->name
    );

    // and the hint, which the count that parse just arrived at is half of
    one->hint = hint_of(one);

    // the writer that stays, holding this same document: the row that only asks it a question
    // needs a document to ask about, and building one inside that loop would be the measurement
    require_ok(
        arnm_json_writer_init(&one->writer, &kept, ARNM_JSON_WRITE_DEFAULT, &one->hint),
        "writer init"
    );
    build_payload(&one->writer, &one->form);
    require_ok(arnm_json_writer_status(&one->writer), one->name);
  }

  verify_layouts_agree();

  // A short run of everything before anything is timed. Without it the first row of the first
  // section carries the cost of pulling the code and the arenas into cache and reads high by
  // about a third -- a figure about this machine's cold start rather than about the payload.
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    build_document(payloads[index], NULL, 64);
    build_document(payloads[index], &payloads[index]->hint, 64);
    render_document(payloads[index], NULL, 64);
    render_document(payloads[index], &payloads[index]->hint, 64);
    measure_size(payloads[index], 64);
    parse_copying(payloads[index], 64);
    parse_insitu(payloads[index], 64);
    refill_only(payloads[index], 64);
    walk_record(payloads[index], 64);
  }
  read_items(&nested, 64);
  traverse_whole(&nested, 16);
}

static void release_test_data(void) {
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    (void)arnm_json_reader_release(&payloads[index]->reader);
    (void)arnm_json_writer_release(&payloads[index]->writer);
  }
  arnm_release(&output);
  arnm_release(&kept);
  arnm_release(&scratch);
}

/* --- driver ----------------------------------------------------------------------------------- */

/**
 * @brief Time the writing of one payload four ways and print the row.
 *
 * The building and the rendering are separate calls to a caller and separate columns here, but
 * the second cannot be timed without the first in front of it -- there is no document to render
 * otherwise. So what the rendering costs is the difference between two columns that are both
 * printed, and neither of them is a derived figure the way the parse table's last one is.
 *
 * The two hinted columns differ from the two beside them in one respect: the writer was told
 * what it was about to be handed, so its pools open once at that size instead of doubling their
 * way there. The last column is what that is worth against the clock -- the rendered figure
 * without the hint over the one with it -- and the answer these rows give is nothing: it sits at
 * 1.00x, and what moves it further than a percent or two is the noise of the machine rather than
 * the hint. The handful of chunk allocations it saves does not register beside the field work.
 * What the hint does change is underneath, in the section below this one.
 */
static void bench_write_modes(payload *one, int steps) {
  char building_text[BENCH_STRING_BUFFER_SIZE];
  char rendered_text[BENCH_STRING_BUFFER_SIZE];
  char hinted_text[BENCH_STRING_BUFFER_SIZE];
  char hinted_rendered_text[BENCH_STRING_BUFFER_SIZE];
  arnm_mono_timer timer;

  arnm_mono_timer_reset(&timer);
  build_document(one, NULL, steps);
  const double building = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  render_document(one, NULL, steps);
  const double rendered = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  build_document(one, &one->hint, steps);
  const double hinted = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  render_document(one, &one->hint, steps);
  const double hinted_rendered = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  bench_per_step_string(building_text, BENCH_STRING_BUFFER_SIZE, building);
  bench_per_step_string(rendered_text, BENCH_STRING_BUFFER_SIZE, rendered);
  bench_per_step_string(hinted_text, BENCH_STRING_BUFFER_SIZE, hinted);
  bench_per_step_string(hinted_rendered_text, BENCH_STRING_BUFFER_SIZE, hinted_rendered);

  printf(
      "  %-12s %11s %14s %13s %16s %10.2fx\n", one->name, building_text, rendered_text, hinted_text,
      hinted_rendered_text, (hinted_rendered > 0.0) ? rendered / hinted_rendered : 0.0
  );
}

/**
 * @brief What building one document costs the arena, told its size and not told it.
 *
 * The clock above found nothing to say about the hint; this is where it answers. A document
 * whose pools grew into place keeps every chunk they outgrew until the arena resets, so the
 * first column is that whole series and the third is the one chunk that replaced it.
 *
 * Exact figures rather than timings, as the reader's footprint section is -- an arena bump is
 * deterministic, so these come out the same on every run and on every machine.
 *
 * The last column is the one to read carefully. An arena releases only from its tail, so what it
 * takes back on a release is whatever the document left at the top of it; which of its chunks
 * that is depends on the order they were asked for, and that is why two payloads carrying the
 * same members in a different order can disagree there. The hinted column has nothing to
 * disagree about -- one chunk, at the tail, and the index comes all the way home.
 */
static void report_write_footprint(payload *one) {
  arnm_json_writer writer;

  arnm_reset(&scratch);
  const uintptr_t base = arena_mark();
  require_ok(
      arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, NULL), "writer init"
  );
  build_payload(&writer, &one->form);
  require_ok(arnm_json_writer_status(&writer), "build");
  const uintptr_t growing_held = arena_mark();
  (void)arnm_json_writer_release(&writer);
  const uintptr_t growing_after = arena_mark();

  arnm_reset(&scratch);
  require_ok(
      arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, &one->hint), "writer init"
  );
  build_payload(&writer, &one->form);
  require_ok(arnm_json_writer_status(&writer), "build");
  const uintptr_t hinted_held = arena_mark();
  (void)arnm_json_writer_release(&writer);
  const uintptr_t hinted_after = arena_mark();
  arnm_reset(&scratch);

  printf(
      "  %-12s %11u %14u %14u %14u\n", one->name, (unsigned)(growing_held - base),
      (unsigned)(growing_after - base), (unsigned)(hinted_held - base),
      (unsigned)(hinted_after - base)
  );
}

/**
 * @brief Time all three parses of one payload and print the row, the derived column included.
 *
 * bench_step() is not used here because the figures only mean something beside each other. An in
 * place parse cannot run twice over the same buffer, so the loop has to refill it; that refill is
 * work the measurement adds and a real caller might not pay, so it is measured on its own and
 * subtracted rather than folded in. The last column is therefore a difference of two
 * measurements and the noisiest figure on the page -- the two it comes from are printed so it
 * can be checked rather than believed.
 */
static void bench_parse_modes(payload *one, int steps) {
  char copying_text[BENCH_STRING_BUFFER_SIZE];
  char refilled_text[BENCH_STRING_BUFFER_SIZE];
  char refill_text[BENCH_STRING_BUFFER_SIZE];
  char alone_text[BENCH_STRING_BUFFER_SIZE];
  arnm_mono_timer timer;

  arnm_mono_timer_reset(&timer);
  parse_copying(one, steps);
  const double copying = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  parse_insitu(one, steps);
  const double refilled = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  refill_only(one, steps);
  const double refill = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  const double alone = refilled - refill;
  bench_per_step_string(copying_text, BENCH_STRING_BUFFER_SIZE, copying);
  bench_per_step_string(refilled_text, BENCH_STRING_BUFFER_SIZE, refilled);
  bench_per_step_string(refill_text, BENCH_STRING_BUFFER_SIZE, refill);
  bench_per_step_string(alone_text, BENCH_STRING_BUFFER_SIZE, alone > 0.0 ? alone : 0.0);

  printf(
      "  %-12s %11s %14s %14s %13s %10.2fx\n", one->name, copying_text, refilled_text, refill_text,
      alone_text, (alone > 0.0) ? copying / alone : 0.0
  );
}

/**
 * @brief What each parse costs the arena, which the clock cannot show.
 *
 * The copying parse takes the string pool first and the value buffer above it, so releasing gives
 * the value buffer back and leaves the pool buried until arnm_reset(). The in place parse has no
 * pool: its one allocation sits at the tail and release moves the index all the way home.
 *
 * Exact figures, not timings -- an arena bump is deterministic, so these are the same on every
 * run and on every machine. Subtracting the two "held" columns lands on the same number as the
 * copying "still held" column, which is the pool itself.
 */
static void report_footprint(payload *one) {
  arnm_json_reader reader;
  arnm_json_value *root = NULL;

  arnm_reset(&scratch);
  const uintptr_t base = arena_mark();
  require_ok(arnm_json_reader_init(&reader, &scratch), "reader init");
  require_ok(arnm_json_reader_parse(&reader, one->text, one->length, false, &root), "parse");
  const uintptr_t copying_held = arena_mark();
  (void)arnm_json_reader_release(&reader);
  const uintptr_t copying_after = arena_mark();

  arnm_reset(&scratch);
  refill_insitu(one);
  require_ok(arnm_json_reader_init(&reader, &scratch), "reader init");
  require_ok(
      arnm_json_reader_parse_insitu(
          &reader, insitu_buffer, one->length, (uint32_t)sizeof(insitu_buffer), false, &root
      ),
      "parse insitu"
  );
  const uintptr_t insitu_held = arena_mark();
  (void)arnm_json_reader_release(&reader);
  const uintptr_t insitu_after = arena_mark();
  arnm_reset(&scratch);

  printf(
      "  %-12s %11u %14u %14u %14u\n", one->name, (unsigned)(copying_held - base),
      (unsigned)(copying_after - base), (unsigned)(insitu_held - base),
      (unsigned)(insitu_after - base)
  );
}

/* one entry point per payload, because bench_step takes no context */
#define BENCH_WALK(id, one)                                                                        \
  static void walk_##id(int steps) {                                                               \
    walk_record(one, steps);                                                                       \
  }
/* clang-format off */
BENCH_WALK(in_order, &in_order)
BENCH_WALK(behind, &behind)
BENCH_WALK(in_front, &in_front)
BENCH_WALK(reversed, &reversed)
/* clang-format on */

static void (*const walk_rows[LAYOUT_COUNT])(int) = {
    walk_in_order, walk_behind, walk_in_front, walk_reversed
};

#define BENCH_ASK(id, one)                                                                         \
  static void ask_##id(int steps) {                                                                \
    measure_size(one, steps);                                                                      \
  }
/* clang-format off */
BENCH_ASK(in_order, &in_order)
BENCH_ASK(behind, &behind)
BENCH_ASK(in_front, &in_front)
BENCH_ASK(reversed, &reversed)
BENCH_ASK(nested, &nested)
BENCH_ASK(text, &text)
/* clang-format on */

static void (*const ask_rows[PAYLOAD_COUNT])(int) = {ask_in_order, ask_behind, ask_in_front,
                                                     ask_reversed, ask_nested, ask_text};

static void read_items_nested(int steps) {
  read_items(&nested, steps);
}
static void traverse_nested(int steps) {
  traverse_whole(&nested, steps);
}

int main(void) {
  arnm_mono_timer time_used;

  if (!bench_timer_start(&time_used)) { return EXIT_FAILURE; }
  prepare_test_data();
  bench_prepared(time_used);

  // "promised" is what the writer answered before the text existed, terminator counted, and
  // "over" is how much of that the text did not need. Every record here carries a double, and a
  // double is charged its longest possible rendering because its real length is not known until
  // it has been rendered -- so the promise is a bound on all six, and the last column is one
  // record's worth of that overcharge times the number of records.
  printf("\nthe payloads\n");
  printf(
      "  %-12s %8s %8s %10s %10s %8s\n", "payload", "bytes", "nodes", "records", "promised", "over"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    payload *one = payloads[index];
    printf(
        "  %-12s %8u %8u %10u %10u %8u\n", one->name, (unsigned)one->length,
        (unsigned)arnm_json_reader_value_count(&one->reader),
        (unsigned)(one->form.elements ? one->form.elements : 1u), (unsigned)one->promised,
        (unsigned)(one->promised - (one->length + 1u))
    );
  }

  bench_section("one document built field by field, and rendered from there, per document");
  printf(
      "  %-12s %11s %14s %13s %16s %11s\n", "payload", "building", "and rendered", "hinted",
      "hinted, rendered", "the hint is"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    bench_write_modes(payloads[index], WRITE_STEPS);
  }

  bench_section("what building costs the arena, which the clock above cannot show");
  printf(
      "  %-12s %11s %14s %14s %14s\n", "payload", "growing", "still held", "hinted", "still held"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    report_write_footprint(payloads[index]);
  }

  bench_section("asking the writer for the size it has been keeping, on a document already built");
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    char name[BENCH_NAME_WIDTH];
    snprintf(name, sizeof(name), "  %s", payloads[index]->name);
    bench_step(ask_rows[index], ASK_STEPS, name, "answer");
  }

  bench_section("one document parsed, copying against in place, per document");
  printf(
      "  %-12s %11s %14s %14s %13s %11s\n", "payload", "copying", "insitu, refilled", "the refill",
      "insitu alone", "insitu is"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    bench_parse_modes(payloads[index], PARSE_STEPS);
  }

  bench_section("what each parse costs the arena, which the clock above cannot show");
  printf(
      "  %-12s %11s %14s %14s %14s\n", "payload", "copying", "still held", "insitu", "still held"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) { report_footprint(payloads[index]); }

  bench_section(
      "one record of nine mixed members into a struct, the parse not counted -- by where the "
      "members sit"
  );
  for (size_t index = 0; index < LAYOUT_COUNT; ++index) {
    char name[BENCH_NAME_WIDTH];
    snprintf(name, sizeof(name), "  %s", layouts[index]->name);
    bench_step(walk_rows[index], WALK_STEPS, name, "record");
  }

  bench_section("an array of 64 objects, handles out and nothing converted");
  bench_step(read_items_nested, WALK_STEPS, "  nested", "array");

  bench_section("all three together: parse in place, find the array, read every record in it");
  bench_step(traverse_nested, PARSE_STEPS, "  nested", "document");

  bench_total_time(time_used);
  release_test_data();
  return 0;
}

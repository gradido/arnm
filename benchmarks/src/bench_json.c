#include "arnm/arena.h"
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
 * Both directions of the JSON module over one and the same payload, so the two can be read
 * against each other rather than guessed at. Every document below is built once by the writer
 * and written out at prepare time; that text is what the reader then parses. Nothing is
 * described twice, and no row compares a document with a different one wearing its name.
 *
 * Four payloads, chosen because the answers differ by shape and not by size:
 *
 *   lean    five members, all of them read -- a small struct going out and coming back
 *   fat     the same five plus twenty-five nobody asks for, where naming the keys pays
 *   nested  an array of sixty-four objects, where the walk is the cost and not the fields
 *   text    ten members whose values are two hundred and fifty-six bytes each, where the
 *           per byte work of escaping and unescaping is the cost and nothing else is
 *
 * Three questions are asked of each of them.
 *
 * How much does the work itself cost, in each direction: a parse against a build and a write.
 *
 * How much does knowing the size beforehand cost. The writer keeps its total as fields arrive,
 * so the answer is a field read; the reader has to walk a document it did not build, either
 * over every string or over the members it was told to expect.
 *
 * And how much does that knowledge save: what each measurement reserves, against what is
 * actually needed.
 *
 * Numbers from a debug build answer a different question -- there is no vector body and no
 * inlining there. Build with -Doptimize=ReleaseFast before reporting any of them.
 */

#define WORK_STEPS 20000
#define MEASURE_STEPS 200000

/** Arena for the documents a timed round builds; reset between rounds rather than grown. */
#define SCRATCH_CAPACITY (8u * 1024u * 1024u)
/**
 * Arena for the reader and writer that stay standing for the whole run.
 *
 * Separate from the scratch one on purpose: a timed round ends by resetting its arena, and a
 * document that outlives such a reset would be read through a pointer into ground that has been
 * handed out again.
 */
#define KEPT_CAPACITY (8u * 1024u * 1024u)
/** Arena for the written text and for copied strings, large enough for the longest payload. */
#define OUTPUT_CAPACITY (1u * 1024u * 1024u)
/** Longest text any payload renders to, with room to spare. */
#define TEXT_CAPACITY 65536

/** A short value, the length a name or an identifier tends to have. */
#define SHORT_VALUE "a string value of an ordinary length"
/** A long value, where the per byte work is what the row is about. */
#define LONG_VALUE_LENGTH 256

/**
 * The names a mapper asks for, the same five in every payload.
 *
 * They sit at the front of each object and the spares behind them, which is what a real
 * document does: neither measurement may assume where a name will turn up.
 */
static const char *const wanted_keys[] = {"alpha", "bravo", "charlie", "delta", "echo"};
#define WANTED_KEY_COUNT ((uint8_t)(sizeof(wanted_keys) / sizeof(wanted_keys[0])))

/** Most spares any payload below carries. */
#define SPARE_KEY_COUNT 32

/**
 * The names nobody asks for, written out once and kept.
 *
 * Not formatted where they are used: a key is borrowed and never copied, so one built into a
 * local buffer would be gone by the time the text is rendered. A mapper meets the same rule --
 * its keys are literals, or a table like this one.
 */
static char spare_keys[SPARE_KEY_COUNT][16];

/**
 * @brief What one payload is made of, in the one place both directions read it from.
 *
 * @c elements 0 is a flat object; anything else wraps that many of them in an `items` array.
 */
typedef struct shape {
  uint32_t elements; /**< Objects inside `items`, or 0 for a flat object. */
  uint32_t spares;   /**< Members per object beyond the wanted five. */
  uint32_t length;   /**< Bytes in each value. */
} shape;

typedef struct payload {
  const char *name;         /**< Column label. */
  shape form;               /**< What the writer builds and the reader then reads. */
  char text[TEXT_CAPACITY]; /**< The rendered JSON, written once at prepare time. */
  uint32_t length;          /**< Bytes of JSON in text. */
  arnm_json_reader reader;  /**< Parsed once and held, for the measuring section. */
  arnm_json_writer writer;  /**< Built once and held, for the measuring section. */
} payload;

static payload lean = {"lean", {0, 0, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{{0}}}, {{{0}}}};
static payload fat = {"fat", {0, 25, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{{0}}}, {{{0}}}};
static payload nested = {"nested", {64, 1, sizeof(SHORT_VALUE) - 1}, {0}, 0, {{{0}}}, {{{0}}}};
static payload text = {"text", {0, 5, LONG_VALUE_LENGTH}, {0}, 0, {{{0}}}, {{{0}}}};
static payload *const payloads[] = {&lean, &fat, &nested, &text};
#define PAYLOAD_COUNT (sizeof(payloads) / sizeof(payloads[0]))

static arnm scratch;
static arnm kept;
static arnm output;
static char long_value[LONG_VALUE_LENGTH + 1];

/** Receives every result, so the compiler cannot drop the call. */
static uint64_t g_sink;

static void require_ok(arnm_result result, const char *what) {
  if (ARNM_SUCCESS != result && ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED != result) {
    fprintf(stderr, "benchmark setup failed: %s (%s)\n", what, arnm_result_to_string(result));
    exit(EXIT_FAILURE);
  }
}

/* --- the one description both directions use ------------------------------------------------ */

/** The value every member carries, short or long by the shape. */
static const char *value_of(const shape *form) {
  return (LONG_VALUE_LENGTH == form->length) ? long_value : SHORT_VALUE;
}

/** One object: the five wanted members, then the spares nobody asks for. */
static void build_object(arnm_json_writer *writer, const shape *form) {
  const char *value = value_of(form);
  for (uint8_t index = 0; index < WANTED_KEY_COUNT; ++index) {
    arnm_json_writer_add_string_length(writer, wanted_keys[index], value, form->length);
  }
  for (uint32_t index = 0; index < form->spares; ++index) {
    arnm_json_writer_add_string_length(writer, spare_keys[index], value, form->length);
  }
}

/**
 * @brief Build one payload into @p writer, from the shape and nothing else.
 *
 * The single description both directions rest on: what this writes is what the reader reads,
 * because the reader reads what this wrote.
 */
static void build_payload(arnm_json_writer *writer, const shape *form) {
  if (0 == form->elements) {
    build_object(writer, form);
    return;
  }
  arnm_json_writer_open_array(writer, "items");
  for (uint32_t index = 0; index < form->elements; ++index) {
    arnm_json_writer_open_object(writer, NULL);
    build_object(writer, form);
    arnm_json_writer_close(writer);
  }
  arnm_json_writer_close(writer);
}

/* --- the work, in both directions ------------------------------------------------------------ */

/** Parse one payload and let it go, the arena reset behind it so every round is the first. */
static void parse_payload(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    arnm_json_reader reader;
    require_ok(arnm_json_reader_init(&reader, &scratch, ARNM_JSON_READ_DEFAULT), "reader init");
    require_ok(arnm_json_reader_parse(&reader, one->text, one->length), "parse");
    sink += arnm_json_reader_value_count(&reader);
    (void)arnm_json_reader_release(&reader);
    arnm_reset(&scratch);
  }
  g_sink += sink;
}

/** Build one payload from scratch and stop before the text: the writer's own half of the work. */
static void build_payload_only(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    arnm_json_writer writer;
    require_ok(
        arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, NULL), "writer init"
    );
    build_payload(&writer, &one->form);
    require_ok(arnm_json_writer_status(&writer), "build");
    sink += arnm_json_writer_size(&writer);
    (void)arnm_json_writer_release(&writer);
    arnm_reset(&scratch);
  }
  g_sink += sink;
}

/** Build one payload and render it, text and all, then give both back. */
static void write_payload(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    arnm_json_writer writer;
    require_ok(
        arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, NULL), "writer init"
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

/* --- measuring, before either direction runs ------------------------------------------------- */

/** What the writer says the text will be: a field it has been keeping all along. */
static void measure_writer(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) { sink += arnm_json_writer_size(&one->writer); }
  g_sink += sink;
}

/** What the reader says every string in the document would cost to copy out. */
static void measure_reader_all(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) { sink += arnm_json_reader_output_size(&one->reader); }
  g_sink += sink;
}

/** The same, counting only the members a mapper said it would read. */
static void measure_reader_keys(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    sink += arnm_json_reader_output_size_for_keys(&one->reader, wanted_keys, WANTED_KEY_COUNT);
  }
  g_sink += sink;
}

/* --- one entry point per payload, because bench_step takes no context ------------------------ */

#define BENCH_PAYLOAD(name, one)                                                                   \
  static void parse_##name(int steps) {                                                            \
    parse_payload(one, steps);                                                                     \
  }                                                                                                \
  static void build_##name(int steps) {                                                            \
    build_payload_only(one, steps);                                                                \
  }                                                                                                \
  static void write_##name(int steps) {                                                            \
    write_payload(one, steps);                                                                     \
  }                                                                                                \
  static void size_writer_##name(int steps) {                                                      \
    measure_writer(one, steps);                                                                    \
  }                                                                                                \
  static void size_reader_all_##name(int steps) {                                                  \
    measure_reader_all(one, steps);                                                                \
  }                                                                                                \
  static void size_reader_keys_##name(int steps) {                                                 \
    measure_reader_keys(one, steps);                                                               \
  }

/* clang-format off */
BENCH_PAYLOAD(lean, &lean)
BENCH_PAYLOAD(fat, &fat)
BENCH_PAYLOAD(nested, &nested)
BENCH_PAYLOAD(text, &text)
/* clang-format on */

typedef struct row {
  void (*parse)(int);
  void (*build)(int);
  void (*write)(int);
  void (*size_writer)(int);
  void (*size_reader_all)(int);
  void (*size_reader_keys)(int);
} row;

#define BENCH_ROW(name)                                                                            \
  {parse_##name,       build_##name,           write_##name,                                       \
   size_writer_##name, size_reader_all_##name, size_reader_keys_##name}

static const row rows[PAYLOAD_COUNT] = {
    BENCH_ROW(lean), BENCH_ROW(fat), BENCH_ROW(nested), BENCH_ROW(text)
};

/* --- test data -------------------------------------------------------------------------------- */

/**
 * @brief Render every payload once, and keep a reader and a writer standing on each.
 *
 * The text a payload carries from here on is the writer's own output, so the parse rows read
 * exactly what the write rows produce.
 */
static void prepare_test_data(void) {
  require_ok(arnm_init_arena(&scratch, SCRATCH_CAPACITY), "scratch arena");
  require_ok(arnm_init_arena(&kept, KEPT_CAPACITY), "kept arena");
  require_ok(arnm_init_arena(&output, OUTPUT_CAPACITY), "output arena");

  memset(long_value, 'x', LONG_VALUE_LENGTH);
  long_value[LONG_VALUE_LENGTH] = '\0';
  for (uint32_t index = 0; index < SPARE_KEY_COUNT; ++index) {
    snprintf(spare_keys[index], sizeof(spare_keys[index]), "spare_%02u", (unsigned)index);
  }

  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    payload *one = payloads[index];

    // the writer that stays: its document is what the measuring section asks about
    require_ok(
        arnm_json_writer_init(&one->writer, &kept, ARNM_JSON_WRITE_DEFAULT, NULL), "writer init"
    );
    build_payload(&one->writer, &one->form);
    require_ok(arnm_json_writer_status(&one->writer), one->name);

    arnm_memory_block rendered;
    require_ok(
        arnm_json_writer_write(&one->writer, &output, &rendered, &one->length), "write payload"
    );
    if (one->length + 1u > TEXT_CAPACITY) {
      fprintf(stderr, "benchmark setup failed: payload '%s' outgrew its buffer\n", one->name);
      exit(EXIT_FAILURE);
    }
    memcpy(one->text, rendered.data, (size_t)one->length + 1u);
    require_ok(arnm_memory_block_free(&rendered, &output), "give the text back");

    // and the reader that stays, on that same text
    require_ok(arnm_json_reader_init(&one->reader, &kept, ARNM_JSON_READ_DEFAULT), "reader init");
    require_ok(arnm_json_reader_parse(&one->reader, one->text, one->length), one->name);
  }
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

static void report_payload(payload *one) {
  const uint32_t promised = arnm_json_writer_size(&one->writer);
  const uint32_t all = arnm_json_reader_output_size(&one->reader);
  const uint32_t keyed =
      arnm_json_reader_output_size_for_keys(&one->reader, wanted_keys, WANTED_KEY_COUNT);
  const double saved = (all > 0) ? (100.0 * (double)(all - keyed) / (double)all) : 0.0;

  printf(
      "  %-8s %8u %8u %10u %8s %10u %10u %8.0f %%\n", one->name, (unsigned)one->length,
      (unsigned)arnm_json_reader_value_count(&one->reader), (unsigned)promised,
      (promised == one->length + 1u) ? "exact" : "bound", (unsigned)all, (unsigned)keyed, saved
  );
}

static void bench_every_payload(const char *title, size_t offset, int steps, const char *unit) {
  bench_section(title);
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    char name[BENCH_NAME_WIDTH];
    void (*const *slot)(int) =
        (void (*const *)(int))(const void *)((const char *)&rows[index] + offset);
    snprintf(name, sizeof(name), "  %s", payloads[index]->name);
    bench_step(*slot, steps, name, unit);
  }
}

int main(void) {
  arnm_mono_timer time_used;

  if (!bench_timer_start(&time_used)) { return EXIT_FAILURE; }
  prepare_test_data();
  bench_prepared(time_used);

  printf("\nthe payloads, and what each measurement says about them\n");
  printf(
      "  %-8s %8s %8s %10s %8s %10s %10s %10s\n", "payload", "bytes", "values", "write size", "",
      "read all", "read keys", "saved"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) { report_payload(payloads[index]); }

  bench_every_payload(
      "one payload, read: parse and release", offsetof(row, parse), WORK_STEPS, "document"
  );
  bench_every_payload(
      "the same payload, written: build, render, give back", offsetof(row, write), WORK_STEPS,
      "document"
  );
  bench_every_payload(
      "of which the building alone, no text rendered", offsetof(row, build), WORK_STEPS, "document"
  );

  bench_every_payload(
      "asking the writer for the size it has been keeping", offsetof(row, size_writer),
      MEASURE_STEPS, "answer"
  );
  bench_every_payload(
      "asking the reader, every string in the document", offsetof(row, size_reader_all),
      MEASURE_STEPS, "answer"
  );
  bench_every_payload(
      "asking the reader, five named keys only", offsetof(row, size_reader_keys), MEASURE_STEPS,
      "answer"
  );

  /* the sections do not share a step count, so the closing line names none */
  bench_total_time(time_used);

  release_test_data();
  return 0;
}

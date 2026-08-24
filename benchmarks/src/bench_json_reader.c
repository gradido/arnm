#include "arnm/json_reader.h"
#include "arnm/memory.h"
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
 * Two ways of learning how large an output arena has to be before a single string is copied
 * into it, and what each of them costs against what it saves.
 *
 *   arnm_json_reader_output_size()           every string in the document, whatever it is called
 *   arnm_json_reader_output_size_for_keys()  only the strings whose member name was named
 *
 * The first is the shorter call and the larger answer: a document carrying members the mapper
 * never reads pays for them in the arena. The second walks the same values and compares every
 * member name against the list, so it costs more per document and reserves less.
 *
 * Three documents, because the difference lives in their shape rather than in their size:
 *
 *   lean    every member is one a mapper reads -- both approaches must agree on the bytes
 *   fat     the same members plus five times as many nobody asks for -- the widest gap
 *   nested  an array of objects, so the keyed walk compares names at every element
 *
 * A parse of the same document runs beside them, so the measurement can be read against the
 * work it prepares rather than in isolation. That is the number that decides whether measuring
 * is worth doing at all.
 *
 * Numbers from a debug build answer a different question. Build with -Doptimize=ReleaseFast
 * before reporting any of them.
 */

#define MEASURE_STEPS 200000
#define PARSE_STEPS 20000

/** Longest document below, with room to spare. */
#define DOCUMENT_CAPACITY 65536

/** Members of the nested document's array -- enough that its shape, not its head, dominates. */
#define NESTED_ELEMENTS 64

/** Every string in the documents is written at this length, so the arithmetic stays readable. */
#define VALUE_TEXT "a string value of a very ordinary length"

typedef struct document {
  const char *name;             /**< Column label. */
  char text[DOCUMENT_CAPACITY]; /**< The JSON itself. */
  uint32_t length;              /**< Bytes of JSON in text. */
  arnm_json_reader reader;      /**< Parsed once at prepare time and held for the whole run. */
} document;

static document lean;
static document fat;
static document nested;

/**
 * The names a mapper would ask for, the same five in every document.
 *
 * They sit at the front of the objects in the lean document and are scattered through the fat
 * one, which is what a real document does: the keyed walk cannot assume where a name will turn
 * up, and the comparison count is what it pays for that.
 */
static const char *const wanted_keys[] = {"alpha", "bravo", "charlie", "delta", "echo"};
#define WANTED_KEY_COUNT ((uint8_t)(sizeof(wanted_keys) / sizeof(wanted_keys[0])))

/** Receives every measurement, so the compiler cannot drop the call. */
static uint64_t g_sink;

/* --- test data ------------------------------------------------------------------------------ */

static void require_ok(arnm_result result, const char *what) {
  if (ARNM_SUCCESS != result) {
    fprintf(stderr, "benchmark setup failed: %s (%s)\n", what, arnm_result_to_string(result));
    exit(EXIT_FAILURE);
  }
}

/** Append to a document's text, refusing to run past its buffer. */
static void append(document *doc, const char *text) {
  const size_t length = strlen(text);
  if (doc->length + length + 1u > DOCUMENT_CAPACITY) {
    fprintf(stderr, "benchmark setup failed: document '%s' outgrew its buffer\n", doc->name);
    exit(EXIT_FAILURE);
  }
  memcpy(doc->text + doc->length, text, length + 1u);
  doc->length += (uint32_t)length;
}

/** One object of five wanted members, optionally padded with members nobody asks for. */
static void append_object(document *doc, uint32_t unwanted) {
  append(doc, "{");
  for (uint8_t index = 0; index < WANTED_KEY_COUNT; ++index) {
    if (index > 0) { append(doc, ","); }
    append(doc, "\"");
    append(doc, wanted_keys[index]);
    append(doc, "\":\"" VALUE_TEXT "\"");
  }
  for (uint32_t index = 0; index < unwanted; ++index) {
    char key[32];
    snprintf(key, sizeof(key), ",\"spare_%02u\":\"", (unsigned)index);
    append(doc, key);
    append(doc, VALUE_TEXT);
    append(doc, "\"");
  }
  append(doc, "}");
}

static void build_documents(void) {
  lean.name = "lean";
  append_object(&lean, 0);

  fat.name = "fat";
  append_object(&fat, 25);

  nested.name = "nested";
  append(&nested, "{\"items\":[");
  for (uint32_t index = 0; index < NESTED_ELEMENTS; ++index) {
    if (index > 0) { append(&nested, ","); }
    append_object(&nested, 1);
  }
  append(&nested, "]}");
}

static void parse_document(document *doc) {
  require_ok(arnm_json_reader_init(&doc->reader, NULL, ARNM_JSON_READ_DEFAULT), "reader init");
  require_ok(arnm_json_reader_parse(&doc->reader, doc->text, doc->length), doc->name);
}

static void prepare_test_data(void) {
  build_documents();
  parse_document(&lean);
  parse_document(&fat);
  parse_document(&nested);
}

static void release_test_data(void) {
  (void)arnm_json_reader_release(&lean.reader);
  (void)arnm_json_reader_release(&fat.reader);
  (void)arnm_json_reader_release(&nested.reader);
}

/* --- the two measurements ------------------------------------------------------------------- */

static void measure_all(document *doc, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) { sink += arnm_json_reader_output_size(&doc->reader); }
  g_sink += sink;
}

static void measure_keys(document *doc, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    sink += arnm_json_reader_output_size_for_keys(&doc->reader, wanted_keys, WANTED_KEY_COUNT);
  }
  g_sink += sink;
}

static void all_lean(int steps) {
  measure_all(&lean, steps);
}
static void keys_lean(int steps) {
  measure_keys(&lean, steps);
}
static void all_fat(int steps) {
  measure_all(&fat, steps);
}
static void keys_fat(int steps) {
  measure_keys(&fat, steps);
}
static void all_nested(int steps) {
  measure_all(&nested, steps);
}
static void keys_nested(int steps) {
  measure_keys(&nested, steps);
}

/** One name instead of five, to separate the walk from the comparing. */
static void keys_fat_single(int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    sink += arnm_json_reader_output_size_for_keys(&fat.reader, wanted_keys, 1);
  }
  g_sink += sink;
}

/* --- what the measurement prepares ---------------------------------------------------------- */

/**
 * A parse of the same bytes, for scale.
 *
 * Same reader, so each parse releases the document before it builds the next one -- which is
 * what a mapper does between two payloads, and what the measurement would sit beside.
 */
static void parse_fat(int steps) {
  for (int step = 0; step < steps; ++step) {
    require_ok(arnm_json_reader_parse(&fat.reader, fat.text, fat.length), "reparse");
  }
}

static void parse_nested(int steps) {
  for (int step = 0; step < steps; ++step) {
    require_ok(arnm_json_reader_parse(&nested.reader, nested.text, nested.length), "reparse");
  }
}

/* --- driver --------------------------------------------------------------------------------- */

static void report_bytes(const document *doc) {
  const uint32_t all = arnm_json_reader_output_size(&doc->reader);
  const uint32_t keyed =
      arnm_json_reader_output_size_for_keys(&doc->reader, wanted_keys, WANTED_KEY_COUNT);
  const double saved = (all > 0) ? (100.0 * (double)(all - keyed) / (double)all) : 0.0;

  printf(
      "  %-8s %8u %8u %12u %12u %9.0f %%\n", doc->name, (unsigned)doc->length,
      (unsigned)arnm_json_reader_value_count(&doc->reader), (unsigned)all, (unsigned)keyed, saved
  );
}

int main(void) {
  arnm_mono_timer time_used;

  if (!bench_timer_start(&time_used)) { return EXIT_FAILURE; }
  prepare_test_data();
  bench_prepared(time_used);

  printf("\nwhat each approach reserves\n");
  printf(
      "  %-8s %8s %8s %12s %12s %11s\n", "document", "bytes", "values", "all strings", "named keys",
      "saved"
  );
  report_bytes(&lean);
  report_bytes(&fat);
  report_bytes(&nested);

  bench_section("measuring one document, every string -- vs -- five named keys");
  bench_step(all_lean, MEASURE_STEPS, "  lean, all strings", "document");
  bench_step(keys_lean, MEASURE_STEPS, "  lean, named keys", "document");
  bench_step(all_fat, MEASURE_STEPS, "  fat, all strings", "document");
  bench_step(keys_fat, MEASURE_STEPS, "  fat, named keys", "document");
  bench_step(keys_fat_single, MEASURE_STEPS, "  fat, one named key", "document");
  bench_step(all_nested, MEASURE_STEPS, "  nested, all strings", "document");
  bench_step(keys_nested, MEASURE_STEPS, "  nested, named keys", "document");

  bench_section("the parse the measurement prepares, for scale");
  bench_step(parse_fat, PARSE_STEPS, "  fat, parse and release", "document");
  bench_step(parse_nested, PARSE_STEPS, "  nested, parse and release", "document");

  /* the sections do not share a step count, so the closing line names none */
  bench_total_time(time_used);

  release_test_data();
  return 0;
}

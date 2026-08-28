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
 * The reading half is asked twice over, copying and in place, in time and in arena bytes --
 * that pair carries its own commentary further down, at the in place parse.
 *
 * How much does knowing the size beforehand cost. The writer keeps its total as fields arrive,
 * so the answer is a field read; the reader has to walk a document it did not build, either
 * over every string or over the members it was told to expect.
 *
 * And how much does that knowledge save: what each measurement reserves, against what is
 * actually needed.
 *
 * A fifth payload stands apart from those four and answers a different question: given a
 * document already parsed, what does it cost to get its members into a struct? A record of nine
 * members of nine different types is mapped twice over -- once by asking for each member by
 * name, once by handing the whole table to arnm_json_read_object() -- in four member layouts.
 * That section carries its own commentary further down, at the record itself.
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

/* --- the same parse, in place ---------------------------------------------------------------- */

/*
 * arnm_json_reader_parse() copies the input and leaves the caller's bytes alone.
 * arnm_json_reader_parse_insitu() unescapes into the caller's own buffer and spends it: the
 * string pool is never allocated, and what the buffer held is gone when the call returns.
 *
 * Timing the second one honestly is awkward, because a loop cannot parse the same buffer twice
 * -- the first round destroys it. Refilling it between rounds is work the measurement adds and
 * a real caller might not pay, so all three parts are measured separately and printed side by
 * side rather than folded into one number:
 *
 *   copying         arnm_json_reader_parse(), the copy inside it included
 *   insitu, refilled  the refill and the in place parse together
 *   the refill alone  the memcpy on its own, which is the part the loop added
 *
 * The last column subtracts the third from the second, which is what an in place parse costs a
 * caller whose buffer was already its to spend -- text just read off a socket or a file into a
 * scratch buffer that nothing else will look at again. It is a difference of two measurements
 * and therefore the noisiest figure on the page; the two it is derived from are printed so it
 * can be checked rather than believed.
 *
 * A caller that has to keep its text reads the middle column instead, and will find it close to
 * the first: the copy happens either way, once inside the parse or once in the refill. That is
 * the whole trade -- in place parsing does not remove the copy, it moves it to the caller and
 * then lets a caller who never needed one skip it.
 */

/** Scratch buffer for the in place parses; one is enough, since one payload is timed at a time. */
static char insitu_buffer[TEXT_CAPACITY];

/**
 * @brief Put the payload's text back into the scratch buffer, padding included.
 *
 * The padding is zeroed rather than left as the last round found it: the parse writes through
 * it, so a round that inherited the previous one's bytes there would not be starting where the
 * one before it did.
 */
static void refill_insitu(payload *one) {
  memcpy(insitu_buffer, one->text, (size_t)one->length);
  memset(insitu_buffer + one->length, 0, ARNM_JSON_READER_INSITU_PADDING);
}

/** Parse in place and let it go, the buffer refilled first because the last round spent it. */
static void parse_payload_insitu(payload *one, int steps) {
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    refill_insitu(one);
    arnm_json_reader reader;
    require_ok(arnm_json_reader_init(&reader, &scratch, ARNM_JSON_READ_DEFAULT), "reader init");
    require_ok(
        arnm_json_reader_parse_insitu(
            &reader, insitu_buffer, one->length, (uint32_t)sizeof(insitu_buffer)
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
    // read the copy back, so the memcpy cannot be lifted out of the loop as dead
    sink += (unsigned char)insitu_buffer[one->length - 1u];
  }
  g_sink += sink;
}

/**
 * @brief Check that both parses see the same document, before either is timed.
 *
 * The in place parse rewrites the bytes it reads. A payload whose text did not survive being
 * rendered into the scratch buffer would still parse, and would still produce a row -- of a
 * document that is not the one the copying parse read.
 */
static void verify_parses_agree(payload *one) {
  arnm_json_reader copied;
  arnm_json_reader in_place;

  require_ok(arnm_json_reader_init(&copied, &scratch, ARNM_JSON_READ_DEFAULT), "reader init");
  require_ok(arnm_json_reader_parse(&copied, one->text, one->length), "parse");

  refill_insitu(one);
  require_ok(arnm_json_reader_init(&in_place, &scratch, ARNM_JSON_READ_DEFAULT), "reader init");
  require_ok(
      arnm_json_reader_parse_insitu(
          &in_place, insitu_buffer, one->length, (uint32_t)sizeof(insitu_buffer)
      ),
      "parse insitu"
  );

  const uint32_t copied_values = arnm_json_reader_value_count(&copied);
  const uint32_t in_place_values = arnm_json_reader_value_count(&in_place);
  const uint32_t copied_bytes = arnm_json_reader_bytes_read(&copied);
  const uint32_t in_place_bytes = arnm_json_reader_bytes_read(&in_place);

  (void)arnm_json_reader_release(&in_place);
  (void)arnm_json_reader_release(&copied);
  arnm_reset(&scratch);

  if (copied_values != in_place_values || copied_bytes != in_place_bytes) {
    fprintf(
        stderr, "benchmark setup failed: the two parses of '%s' disagree (%u/%u values)\n",
        one->name, (unsigned)copied_values, (unsigned)in_place_values
    );
    exit(EXIT_FAILURE);
  }
}

/**
 * @brief Where the arena's index stands, as an address.
 *
 * Measured the way a consumer can measure it: one byte handed out is the index itself, and
 * giving it straight back leaves the arena as it was found. Only differences between two of
 * these mean anything.
 */
static uintptr_t arena_mark(void) {
  uint8_t *probe = NULL;
  require_ok(arnm_alloc(&probe, 1, &scratch), "arena probe");
  require_ok(arnm_free(probe, 1, &scratch), "arena probe");
  return (uintptr_t)probe;
}

/**
 * @brief What each parse costs the arena, which is the part the clock cannot show.
 *
 * The copying parse takes the string pool first and the value buffer above it, so releasing
 * gives the value buffer back and leaves the pool buried until arnm_reset(). The in place parse
 * has no pool: its one allocation sits at the tail and release moves the index all the way home.
 *
 * The second column of each pair is what a release did not give back. Subtracting the two
 * "held" columns from each other lands on the same figure, which is the pool itself: what the
 * copying parse still holds afterwards is exactly what it took beyond the in place parse.
 *
 * Exact figures, not timings -- an arena bump is deterministic, so these are the same on every
 * run and on every machine.
 */
static void report_parse_footprint(payload *one) {
  arnm_json_reader reader;

  arnm_reset(&scratch);
  const uintptr_t base = arena_mark();
  require_ok(arnm_json_reader_init(&reader, &scratch, ARNM_JSON_READ_DEFAULT), "reader init");
  require_ok(arnm_json_reader_parse(&reader, one->text, one->length), "parse");
  const uintptr_t copying_held = arena_mark();
  (void)arnm_json_reader_release(&reader);
  const uintptr_t copying_after = arena_mark();

  arnm_reset(&scratch);
  refill_insitu(one);
  require_ok(arnm_json_reader_init(&reader, &scratch, ARNM_JSON_READ_DEFAULT), "reader init");
  require_ok(
      arnm_json_reader_parse_insitu(
          &reader, insitu_buffer, one->length, (uint32_t)sizeof(insitu_buffer)
      ),
      "parse insitu"
  );
  const uintptr_t insitu_held = arena_mark();
  (void)arnm_json_reader_release(&reader);
  const uintptr_t insitu_after = arena_mark();
  arnm_reset(&scratch);

  printf(
      "  %-8s %11u %14u %14u %14u\n", one->name, (unsigned)(copying_held - base),
      (unsigned)(copying_after - base), (unsigned)(insitu_held - base),
      (unsigned)(insitu_after - base)
  );
}

/**
 * @brief Time all three parses of one payload and print the row.
 *
 * bench_step() is not used here for the same reason as in the mapping section: the figures only
 * mean something beside each other, and the derived column is the answer the section exists for.
 */
static void bench_parse_modes(payload *one, int steps) {
  char copying_text[BENCH_STRING_BUFFER_SIZE];
  char refilled_text[BENCH_STRING_BUFFER_SIZE];
  char refill_text[BENCH_STRING_BUFFER_SIZE];
  char alone_text[BENCH_STRING_BUFFER_SIZE];
  arnm_mono_timer timer;

  arnm_mono_timer_reset(&timer);
  parse_payload(one, steps);
  const double copying = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  parse_payload_insitu(one, steps);
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
      "  %-8s %11s %14s %14s %13s %10.2fx\n", one->name, copying_text, refilled_text, refill_text,
      alone_text, (alone > 0.0) ? copying / alone : 0.0
  );
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

/* --- a record of mixed members, mapped two ways ---------------------------------------------- */

/*
 * The question this section asks is narrower than the ones above: given a document that is
 * already parsed, what does it cost to get its members into a struct?
 *
 * Two answers are measured over the same document. Asking for each member by name walks the
 * member chain once per question, so a struct of nine costs nine walks. Handing the whole table
 * to arnm_json_read_object() walks it once and answers each key where it is met.
 *
 * The record is deliberately not nine strings. Every field type the walk knows appears in it --
 * the four integer widths, a double, a bool, a borrowed string, a fixed hex field and a uuid --
 * because a table of one repeated type would measure the chain walk and nothing of the
 * conversion that hangs off it.
 *
 * Three layouts, because the walk keeps a guess about where the next key sits:
 *
 *   in table order     what a document written by the same mapping looks like, and the case
 *                      the guess is right for every time
 *   24 spares behind   the same nine, followed by members nobody asks for. Both layouts with
 *   24 spares in front spares carry the same members and differ only in where they sit, which
 *                      is the point: a lookup stops at the key it wanted, so spares behind it
 *                      cost it nothing while the walk still steps over every one of them.
 *                      In front, every lookup pays for all of them and the walk still does not.
 *   reversed           the nine last first, which is the guess wrong at every step
 *
 * Both mappings are run once against each other at prepare time and their results compared
 * field by field. A row that measured two different amounts of work would not be a comparison.
 */

#define RECORD_DIGEST_SIZE 32
#define RECORD_FIELD_COUNT 9
#define RECORD_SPARE_COUNT 24
#define MAP_STEPS 200000

/** What both mappings fill, so the two can be compared field by field. */
typedef struct record_target {
  arnm_memory_block name; /**< Borrowed from the document, never freed. */
  uint64_t id;
  int64_t balance;
  uint32_t port;
  int32_t offset;
  double ratio;
  bool active;
  uint8_t digest[RECORD_DIGEST_SIZE];
  uint8_t uuid[ARNM_UUID_BINARY_SIZE];
} record_target;

#define RECORD_NAME "a record with several kinds of member"
#define RECORD_ID UINT64_C(0x0123456789abcdef)
#define RECORD_BALANCE INT64_C(-4200000000)
#define RECORD_PORT 8443
#define RECORD_OFFSET (-12345)
#define RECORD_RATIO 0.6180339887498949
#define RECORD_ACTIVE true

static uint8_t record_digest[RECORD_DIGEST_SIZE];
static uint8_t record_uuid[ARNM_UUID_BINARY_SIZE];

/** Where the nine members sit in the document, relative to the table's order. */
typedef enum record_layout {
  RECORD_IN_ORDER,     /**< The nine, in the order the table names them. */
  RECORD_SPARES_AFTER, /**< The nine in order, then members nobody asks for. */
  RECORD_SPARES_FIRST, /**< The spares first, so every lookup has to walk past them. */
  RECORD_REVERSED      /**< The nine, last first. */
} record_layout;

typedef struct record_payload {
  const char *name;         /**< Row label. */
  record_layout layout;     /**< How the members are arranged. */
  char text[TEXT_CAPACITY]; /**< The rendered JSON, written once at prepare time. */
  uint32_t length;          /**< Bytes of JSON in text. */
  arnm_json_reader
      reader; /**< Parsed once and held: the mapping is what is timed, not the parse. */
} record_payload;

static record_payload records[] = {
    {"in table order", RECORD_IN_ORDER, {0}, 0, {{{0}}}},
    {"24 spares behind", RECORD_SPARES_AFTER, {0}, 0, {{{0}}}},
    {"24 spares in front", RECORD_SPARES_FIRST, {0}, 0, {{{0}}}},
    {"reversed", RECORD_REVERSED, {0}, 0, {{{0}}}}
};
#define RECORD_COUNT (sizeof(records) / sizeof(records[0]))

/** One member of the record, written by its index in the table's order. */
static void write_record_field(arnm_json_writer *writer, unsigned index) {
  switch (index) {
  case 0:
    arnm_json_writer_add_string_length(
        writer, "name", RECORD_NAME, (uint32_t)(sizeof(RECORD_NAME) - 1u)
    );
    break;
  case 1:
    arnm_json_writer_add_uint64(writer, "id", RECORD_ID);
    break;
  case 2:
    arnm_json_writer_add_int64(writer, "balance", RECORD_BALANCE);
    break;
  case 3:
    arnm_json_writer_add_uint64(writer, "port", RECORD_PORT);
    break;
  case 4:
    arnm_json_writer_add_int64(writer, "offset", RECORD_OFFSET);
    break;
  case 5:
    arnm_json_writer_add_double(writer, "ratio", RECORD_RATIO);
    break;
  case 6:
    arnm_json_writer_add_bool(writer, "active", RECORD_ACTIVE);
    break;
  case 7:
    arnm_json_writer_add_hex(writer, "digest", record_digest, RECORD_DIGEST_SIZE);
    break;
  default:
    arnm_json_writer_add_uuid(writer, "uuid", record_uuid);
    break;
  }
}

/** The members nobody asks for, however many the layout carries. */
static void write_record_spares(arnm_json_writer *writer) {
  for (unsigned index = 0; index < RECORD_SPARE_COUNT; ++index) {
    arnm_json_writer_add_string_length(
        writer, spare_keys[index], SHORT_VALUE, (uint32_t)(sizeof(SHORT_VALUE) - 1u)
    );
  }
}

static void build_record(arnm_json_writer *writer, record_layout layout) {
  if (RECORD_SPARES_FIRST == layout) { write_record_spares(writer); }
  for (unsigned index = 0; index < RECORD_FIELD_COUNT; ++index) {
    write_record_field(
        writer, (RECORD_REVERSED == layout) ? RECORD_FIELD_COUNT - 1u - index : index
    );
  }
  if (RECORD_SPARES_AFTER == layout) { write_record_spares(writer); }
}

/**
 * @brief The whole record in one walk: the table says what it wants and the chain is read once.
 *
 * The two blocks are locals rather than members of @p into because HEX_FIXED and UUID read the
 * descriptor they are handed -- it names the buffer to decode into and the size to measure the
 * string against. Pointing them at the arrays costs two stores per call, which is part of what
 * this row measures.
 */
static arnm_result map_walk(arnm_json_value *root, record_target *into, uint64_t *out_found) {
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
  return arnm_json_read_object(root, fields, RECORD_FIELD_COUNT, out_found);
}

/**
 * @brief The same record, asked for a member at a time -- what a mapper writes without a table.
 *
 * Nine lookups, each one a walk of the member chain, and the conversion spelled out beside every
 * one of them. This is the shape the walk replaces, written the way it is actually written.
 */
static arnm_result map_fields(arnm_json_value *root, record_target *into) {
  arnm_json_value *member = NULL;
  arnm_result result;
  const char *text = NULL;
  uint32_t length = 0;

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "name", &member))) { return result; }
  if (ARNM_SUCCESS != (result = arnm_json_read_string(member, &text, &length))) { return result; }
  into->name.data = (uint8_t *)(uintptr_t)(const void *)text;
  into->name.size = length;

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "id", &member))) { return result; }
  if (ARNM_SUCCESS != (result = arnm_json_read_uint64(member, &into->id))) { return result; }

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "balance", &member))) { return result; }
  if (ARNM_SUCCESS != (result = arnm_json_read_int64(member, &into->balance))) { return result; }

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "port", &member))) { return result; }
  if (ARNM_SUCCESS != (result = arnm_json_read_uint32(member, &into->port))) { return result; }

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "offset", &member))) { return result; }
  if (ARNM_SUCCESS != (result = arnm_json_read_int32(member, &into->offset))) { return result; }

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "ratio", &member))) { return result; }
  if (ARNM_SUCCESS != (result = arnm_json_read_double(member, &into->ratio))) { return result; }

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "active", &member))) { return result; }
  if (ARNM_SUCCESS != (result = arnm_json_read_bool(member, &into->active))) { return result; }

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "digest", &member))) { return result; }
  result = arnm_json_read_hex_fixed(member, into->digest, RECORD_DIGEST_SIZE);
  if (ARNM_SUCCESS != result) { return result; }

  if (ARNM_SUCCESS != (result = arnm_json_object_get(root, "uuid", &member))) { return result; }
  return arnm_json_read_uuid(member, into->uuid);
}

/** Something from every field, so no part of a mapping can be optimized away. */
static uint64_t record_digest_of(const record_target *one) {
  uint64_t sum = one->id ^ (uint64_t)one->balance;
  sum += one->port + (uint64_t)(uint32_t)one->offset + (uint64_t)(one->ratio * 1000.0);
  sum += one->active ? 1u : 0u;
  sum += one->name.size;
  for (unsigned index = 0; index < RECORD_DIGEST_SIZE; ++index) { sum += one->digest[index]; }
  for (unsigned index = 0; index < ARNM_UUID_BINARY_SIZE; ++index) { sum += one->uuid[index]; }
  return sum;
}

static void map_walk_steps(record_payload *one, int steps) {
  arnm_json_value *root = arnm_json_reader_root(&one->reader);
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    record_target into;
    uint64_t found = 0;
    require_ok(map_walk(root, &into, &found), "map by walk");
    sink += record_digest_of(&into) + found;
  }
  g_sink += sink;
}

static void map_fields_steps(record_payload *one, int steps) {
  arnm_json_value *root = arnm_json_reader_root(&one->reader);
  uint64_t sink = 0;
  for (int step = 0; step < steps; ++step) {
    record_target into;
    require_ok(map_fields(root, &into), "map by field");
    sink += record_digest_of(&into);
  }
  g_sink += sink;
}

/**
 * @brief Check that the two mappings agree, before either of them is timed.
 *
 * A benchmark comparing two amounts of work is only a comparison while both amounts are the
 * same. Every field is looked at: a mapping that quietly skipped one would otherwise show up as
 * the faster of the two.
 */
static void verify_mappings_agree(record_payload *one) {
  arnm_json_value *root = arnm_json_reader_root(&one->reader);
  record_target walked;
  record_target asked;
  uint64_t found = 0;

  require_ok(map_walk(root, &walked, &found), "map by walk");
  require_ok(map_fields(root, &asked), "map by field");

  const bool agree = found == (UINT64_C(1) << RECORD_FIELD_COUNT) - 1u &&
                     walked.name.data == asked.name.data && walked.name.size == asked.name.size &&
                     walked.id == asked.id && walked.balance == asked.balance &&
                     walked.port == asked.port && walked.offset == asked.offset &&
                     walked.ratio == asked.ratio && walked.active == asked.active &&
                     0 == memcmp(walked.digest, asked.digest, RECORD_DIGEST_SIZE) &&
                     0 == memcmp(walked.uuid, asked.uuid, ARNM_UUID_BINARY_SIZE);
  if (!agree) {
    fprintf(stderr, "benchmark setup failed: the two mappings of '%s' disagree\n", one->name);
    exit(EXIT_FAILURE);
  }
  // and against what the writer put there, so neither is agreeing on the wrong thing
  if (walked.id != RECORD_ID || walked.balance != RECORD_BALANCE || walked.port != RECORD_PORT ||
      walked.offset != RECORD_OFFSET || walked.name.size != sizeof(RECORD_NAME) - 1u ||
      0 != memcmp(walked.digest, record_digest, RECORD_DIGEST_SIZE) ||
      0 != memcmp(walked.uuid, record_uuid, ARNM_UUID_BINARY_SIZE)) {
    fprintf(stderr, "benchmark setup failed: '%s' did not read back what was written\n", one->name);
    exit(EXIT_FAILURE);
  }
}

/**
 * @brief Time both mappings of one record and print the row, the factor included.
 *
 * bench_step() is not used here because the two figures only mean something beside each other:
 * printed in separate sections a reader would have to divide two columns from different parts
 * of the output, and the factor is the whole answer this section exists for.
 */
static void bench_mapping(record_payload *one, int steps) {
  char walk_per_step[BENCH_STRING_BUFFER_SIZE];
  char fields_per_step[BENCH_STRING_BUFFER_SIZE];
  arnm_mono_timer timer;

  arnm_mono_timer_reset(&timer);
  map_walk_steps(one, steps);
  const double walk_nanos = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  map_fields_steps(one, steps);
  const double fields_nanos = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  bench_per_step_string(walk_per_step, BENCH_STRING_BUFFER_SIZE, walk_nanos);
  bench_per_step_string(fields_per_step, BENCH_STRING_BUFFER_SIZE, fields_nanos);

  printf(
      "  %-20s %12s %14s %11.2fx\n", one->name, walk_per_step, fields_per_step,
      walk_nanos > 0.0 ? fields_nanos / walk_nanos : 0.0
  );
}

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
  for (uint32_t index = 0; index < RECORD_DIGEST_SIZE; ++index) {
    record_digest[index] = (uint8_t)(index * 7u + 1u);
  }
  for (uint32_t index = 0; index < ARNM_UUID_BINARY_SIZE; ++index) {
    record_uuid[index] = (uint8_t)(0xf0u - index);
  }
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
    // the insitu rows render this text into a buffer of the same size and write padding past
    // its end, so the room for that padding is what has to fit, not just the terminator
    if (one->length + ARNM_JSON_READER_INSITU_PADDING > TEXT_CAPACITY) {
      fprintf(stderr, "benchmark setup failed: payload '%s' outgrew its buffer\n", one->name);
      exit(EXIT_FAILURE);
    }
    memcpy(one->text, rendered.data, (size_t)one->length + 1u);
    require_ok(arnm_memory_block_free(&rendered, &output), "give the text back");

    // and the reader that stays, on that same text
    require_ok(arnm_json_reader_init(&one->reader, &kept, ARNM_JSON_READ_DEFAULT), "reader init");
    require_ok(arnm_json_reader_parse(&one->reader, one->text, one->length), one->name);

    verify_parses_agree(one);
  }

  // the three record layouts, rendered by the writer for the same reason: what the mapping rows
  // read is what the writer put there, so verify_mappings_agree() can check both against it
  for (size_t index = 0; index < RECORD_COUNT; ++index) {
    record_payload *one = &records[index];
    arnm_json_writer writer;
    require_ok(
        arnm_json_writer_init(&writer, &scratch, ARNM_JSON_WRITE_DEFAULT, NULL), "writer init"
    );
    build_record(&writer, one->layout);
    require_ok(arnm_json_writer_status(&writer), one->name);

    arnm_memory_block rendered;
    require_ok(arnm_json_writer_write(&writer, &output, &rendered, &one->length), "write record");
    if (one->length + 1u > TEXT_CAPACITY) {
      fprintf(stderr, "benchmark setup failed: record '%s' outgrew its buffer\n", one->name);
      exit(EXIT_FAILURE);
    }
    memcpy(one->text, rendered.data, (size_t)one->length + 1u);
    require_ok(arnm_memory_block_free(&rendered, &output), "give the text back");
    (void)arnm_json_writer_release(&writer);
    arnm_reset(&scratch);
    arnm_reset(&output);

    require_ok(arnm_json_reader_init(&one->reader, &kept, ARNM_JSON_READ_DEFAULT), "reader init");
    require_ok(arnm_json_reader_parse(&one->reader, one->text, one->length), one->name);
    verify_mappings_agree(one);
  }
}

static void release_test_data(void) {
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    (void)arnm_json_reader_release(&payloads[index]->reader);
    (void)arnm_json_writer_release(&payloads[index]->writer);
  }
  for (size_t index = 0; index < RECORD_COUNT; ++index) {
    (void)arnm_json_reader_release(&records[index].reader);
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
  bench_section("the same parse, copying against in place, per document");
  printf(
      "  %-8s %11s %14s %14s %13s %11s\n", "payload", "copying", "insitu, refilled", "the refill",
      "insitu alone", "insitu is"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    bench_parse_modes(payloads[index], WORK_STEPS);
  }

  bench_section("what each parse costs the arena, which the clock above cannot show");
  printf(
      "  %-8s %11s %14s %14s %14s\n", "payload", "copying", "still held", "insitu", "still held"
  );
  for (size_t index = 0; index < PAYLOAD_COUNT; ++index) {
    report_parse_footprint(payloads[index]);
  }

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

  bench_section("one parsed object into a struct of nine mixed members, the parse not counted");
  printf("  %-20s %12s %14s %12s\n", "member layout", "one walk", "per field", "walk is");
  for (size_t index = 0; index < RECORD_COUNT; ++index) {
    bench_mapping(&records[index], MAP_STEPS);
  }

  /* the sections do not share a step count, so the closing line names none */
  bench_total_time(time_used);

  release_test_data();
  return 0;
}

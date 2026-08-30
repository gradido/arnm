#include "arnm/arena.h"
#include "arnm/converter.h"
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
 * Both directions of the byte-to-text conversions -- hex and base64 over a whole block, and a
 * uuid in its canonical 8-4-4-4-12 form -- on their own, no baseline beside them. arnm links no
 * crypto library, so the constant time conversions such a library ships are not here to compare
 * against, and a printf loop would answer a question nobody asks in a hot path.
 *
 * hex and base64 sit in the same section on purpose: same bytes, same length, two alphabets, so
 * the rows read straight against each other. What they say is not what the character counts
 * suggest -- base64 writes a third fewer characters and takes several times as long for
 * them. The reason is in the shapes rather than in the work: hex maps one byte to two
 * characters with no carry between them, which the compiler turns into a tight vector body,
 * while base64 has to shuffle bits across a three byte group, and that regrouping is what an
 * auto vectoriser handles badly. Computing the characters instead of reading the alphabet
 * table was tried against exactly these rows and lost -- see the note at BASE64_ALPHABET in
 * converter.c for the figures. Both directions are still a fraction of what the write around
 * them costs, which is why the mapping in gradido-blockchain-core picks its alphabet by who
 * reads the field and not by these rows.
 *
 * A comparison against libsodium's constant time pair -- a different question, and one that
 * needs a crypto library -- lives in bench_base64 of gradido-blockchain-core.
 *
 * The last section is about one question and not about the alphabets: whether decoding base64
 * over the string it came from beats decoding it into a buffer of its own. Three bytes take the
 * room four characters had, so the bytes can be written behind the read that produced them and
 * no second buffer is touched at all. That is a cache argument, so it is asked at lengths that
 * reach past a cache -- 1 KiB sits in L1, 8 MiB is past any L3 this is likely to run on, and
 * the rows in between are where the answer would change if it changed anywhere.
 *
 * It barely changes. On a 5700G the in place decode comes out 1.01x to 1.04x ahead from 1 KiB
 * to 1 MiB and 1.04x to 1.10x at 8 MiB, and the 1.1x or so on the 16 byte row is not the cache
 * at all -- it is the strlen the copying call makes on its own input, which the row beside it
 * prices. The reason the cache argument does not pay is in the throughput rather than in the
 * argument: 8 MiB decodes in about 5.5 ms, which is 1.5 GB/s, and this machine's memory answers
 * an order of magnitude faster than that. The second buffer costs write traffic the decoder was
 * never waiting on, because what it waits on is the scalar regrouping that the section above
 * already explains no vectoriser will take. So the in place call is worth reaching for to be rid
 * of the buffer -- the allocation, the sizing, the lifetime -- and not to be quicker; a caller
 * who wants both bytes and characters afterwards loses nothing by using the copying one.
 *
 * What the columns do say is how the cost grows. Each section fixes an input length and reports
 * the nanoseconds one whole conversion takes, so the per byte cost falls out of dividing by the
 * length: a short input pays mostly for the call and the length check, while a long one settles
 * into the loop the compiler vectorised, and the two numbers per byte are far apart.
 *
 * The lengths are the ones that actually turn up: 16 bytes for a uuid, 32 for a hash or a public
 * key, 64 for a signature, 1024 for a serialised record. The uuid section converts the same 16
 * bytes through the table driven path, so the two rows at 16 bytes say what the separators and
 * the scattered positions cost.
 *
 * Numbers from a debug build answer a different question -- there is no vector body there at
 * all. Build with -Doptimize=ReleaseFast before reporting any of them.
 */

#define MAX_PAYLOAD_SIZE 1024
#define PAYLOAD_VARIANTS 64

/*
 * Conversions per step, the same in every section.
 *
 * One count throughout, so the rows can be read straight against each other and the closing
 * line has a single figure to name. It is set by the slowest section rather than by the
 * fastest: 1024 bytes costs orders of magnitude more per conversion than 16, so a count that
 * suits the short inputs would drag the whole run out for nothing. The per conversion figures
 * are what the sections are about, and those do not depend on how often the step was repeated.
 */
#define BENCH_CONVERSIONS 400000

/*
 * Several payloads rather than one, cycled through: a single buffer converted over and over
 * sits in L1 and reports a cache the real caller will not have. The hex strings are prepared
 * once from the same payloads, so the decoding steps read valid input and never take the
 * failure path.
 */
static uint8_t payloads[PAYLOAD_VARIANTS][MAX_PAYLOAD_SIZE];
static char hexStrings[PAYLOAD_VARIANTS][MAX_PAYLOAD_SIZE * 2 + 1];
static char base64Strings[PAYLOAD_VARIANTS][ARNM_BASE64_STRING_LENGTH(MAX_PAYLOAD_SIZE) + 1];

/** The same payloads rendered as uuids, prepared once so the decoding step reads valid input. */
static char uuidStrings[PAYLOAD_VARIANTS][ARNM_UUID_STRING_LENGTH + 1];

/** Receives every conversion, so the compiler cannot drop the call. */
static char benchHexBuffer[MAX_PAYLOAD_SIZE * 2 + 1];
static char benchBase64Buffer[ARNM_BASE64_STRING_LENGTH(MAX_PAYLOAD_SIZE) + 1];
static uint8_t benchBinaryBuffer[MAX_PAYLOAD_SIZE];
static char benchUuidBuffer[ARNM_UUID_STRING_LENGTH + 1];

static int cursor = 0;
/** Set by the driver before each step; the step functions take their length from here. */
static uint32_t currentLength = 0;
/** Folds every result code in, so a silent failure cannot hide behind a fast number. */
static unsigned resultSink = 0;
/** The uuid encoding returns nothing, so a byte of what it wrote stands in for a result code. */
static unsigned writtenSink = 0;

static int nextVariant(void) {
  int result = cursor++;
  if (cursor >= PAYLOAD_VARIANTS) { cursor = 0; }
  return result;
}

static void test_binary_to_hex(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    arnm_memory_block block = {payloads[nextVariant()], currentLength};
    resultSink |= (unsigned)arnm_binary_to_hex(benchHexBuffer, &block);
  }
}

static void test_binary_from_hex(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    // the string is terminated at twice the current length, so only that much is read
    resultSink |= (unsigned)arnm_binary_from_hex(benchBinaryBuffer, hexStrings[nextVariant()]);
  }
}

static void test_binary_to_base64(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    arnm_memory_block block = {payloads[nextVariant()], currentLength};
    resultSink |= (unsigned)arnm_binary_to_base64(benchBase64Buffer, &block);
  }
}

static void test_binary_from_base64(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    uint32_t written = 0;
    // the string was prepared at the current length, so only that much is read
    resultSink |= (unsigned)arnm_binary_from_base64(
        benchBinaryBuffer, &written, base64Strings[nextVariant()]
    );
    writtenSink |= written;
  }
}

static void test_uuid_to_string(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    arnm_uuid_to_string(benchUuidBuffer, payloads[nextVariant()]);
  }
  writtenSink |= (unsigned char)benchUuidBuffer[0];
}

static void test_uuid_from_string(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    resultSink |= (unsigned)arnm_uuid_from_string(benchBinaryBuffer, uuidStrings[nextVariant()]);
  }
}

/* --- base64 decoded in place, against decoded into a buffer of its own --------------------- */

/*
 * Room for the pristine strings. As many variants of the length under test as fit, so a short
 * one is still cycled the way the sections above cycle theirs, and a long one -- where a single
 * copy already outgrows every cache -- is measured on the one that fits.
 */
#define DECODE_POOL_CAPACITY (16u * 1024u * 1024u)
#define DECODE_MAX_BYTES (8u * 1024u * 1024u)
#define DECODE_MAX_VARIANTS 64

/*
 * Bytes a row decodes in total, which is what sets its step count.
 *
 * The rows here span from 16 bytes to 8 MiB, so one count for all of them is not possible: it
 * would be either meaningless at the top or an hour long at the bottom. Each row runs as often
 * as it takes to move about this much through the decoder, within the two bounds below -- long
 * enough that the clock is not the thing being measured, short enough that the largest row does
 * not own the run. The step count is printed beside every row, because a figure that came from
 * twenty repetitions deserves to be read differently from one that came from two hundred
 * thousand.
 */
#define DECODE_BUDGET (128u * 1024u * 1024u)
#define DECODE_MIN_STEPS 40
#define DECODE_MAX_STEPS 200000

static arnm decodeArena;
/** The strings nothing writes to, several of them back to back. */
static char *decodePool = NULL;
/** The one buffer both decoders are handed, refilled from the pool before every decode. */
static char *decodeWork = NULL;
/** Where the copying decode puts its bytes; the in place decode has no use for it. */
static uint8_t *decodeSink = NULL;

static uint32_t decodeStringLength = 0;
static uint32_t decodeVariants = 1;
static uint32_t decodeCursor = 0;

static const char *next_decode_source(void) {
  const char *result = decodePool + (size_t)decodeCursor * decodeStringLength;
  if (++decodeCursor >= decodeVariants) { decodeCursor = 0; }
  return result;
}

/**
 * The refill both decoding rows carry.
 *
 * An in place decode eats its input, so the loop around it has to put the string back before
 * every round. That copy is work a caller does not do -- the string it decodes is already
 * wherever it arrived -- so it is timed on its own and subtracted. The copying row pays the
 * same refill although it would not need one: what is being compared is where the bytes go, and
 * that comparison only holds if both rows start their decode from the same buffer in the same
 * state.
 */
static void decode_refill(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    memcpy(decodeWork, next_decode_source(), decodeStringLength);
    writtenSink |= (unsigned char)decodeWork[0];
  }
}

/**
 * The refill and the strlen the copying call makes on top of it.
 *
 * arnm_binary_from_base64() takes a terminated string and measures it; the in place call is
 * handed the length, because a caller who decodes over a borrowed field has it and that field is
 * not always terminated. That difference belongs to the calls rather than to the buffers, so it
 * is measured here and printed -- it is not subtracted from anything. Reading it against the
 * refill column beside it says how much of the copying figure is the strlen and how much is the
 * decode.
 */
static void decode_refill_and_measure(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    memcpy(decodeWork, next_decode_source(), decodeStringLength);
    writtenSink |= (unsigned)strlen(decodeWork);
  }
}

static void decode_copying(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    uint32_t written = 0;
    memcpy(decodeWork, next_decode_source(), decodeStringLength);
    resultSink |= (unsigned)arnm_binary_from_base64(decodeSink, &written, decodeWork);
    writtenSink |= written;
  }
}

static void decode_in_place(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    uint32_t written = 0;
    memcpy(decodeWork, next_decode_source(), decodeStringLength);
    resultSink |=
        (unsigned)arnm_binary_from_base64_insitu(decodeWork, decodeStringLength, &written);
    writtenSink |= written;
  }
}

/* --- driver ------------------------------------------------------------------------------ */

static void prepare_test_data(void) {
  srand(4711);
  for (int v = 0; v < PAYLOAD_VARIANTS; ++v) {
    for (int i = 0; i < MAX_PAYLOAD_SIZE; ++i) { payloads[v][i] = (uint8_t)(rand() & 0xFF); }
    arnm_uuid_to_string(uuidStrings[v], payloads[v]);
  }
}

/*
 * Renders every prepared string at the length about to be measured, so each decoding step reads
 * exactly as many characters as its encoding step wrote. Called once per section.
 */
static void set_length(uint32_t length) {
  currentLength = length;
  for (int v = 0; v < PAYLOAD_VARIANTS; ++v) {
    arnm_memory_block block = {payloads[v], length};
    if (ARNM_SUCCESS != arnm_binary_to_hex(hexStrings[v], &block)) {
      printf("could not prepare the hex strings\n");
      exit(1);
    }
    if (ARNM_SUCCESS != arnm_binary_to_base64(base64Strings[v], &block)) {
      printf("could not prepare the base64 strings\n");
      exit(1);
    }
  }
  cursor = 0;
}

/**
 * Fills the pool with strings of the length about to be measured, as many as fit.
 *
 * The bytes come from a counter rather than from rand(): the largest row needs 8 MiB of them
 * and none of the figures depend on what they are, only on how many.
 */
static void set_decode_length(uint32_t length) {
  decodeStringLength = ARNM_BASE64_STRING_LENGTH(length);
  decodeVariants = DECODE_POOL_CAPACITY / decodeStringLength;
  if (decodeVariants > DECODE_MAX_VARIANTS) { decodeVariants = DECODE_MAX_VARIANTS; }
  if (0 == decodeVariants) {
    printf("the pool is too small for %u bytes\n", (unsigned)length);
    exit(1);
  }
  decodeCursor = 0;

  uint32_t seed = 2463534242u;
  for (uint32_t v = 0; v < decodeVariants; ++v) {
    for (uint32_t i = 0; i < length; ++i) {
      seed = seed * 1664525u + 1013904223u;
      decodeSink[i] = (uint8_t)(seed >> 24);
    }
    const arnm_memory_block block = {decodeSink, length};
    if (ARNM_SUCCESS !=
        arnm_binary_to_base64(decodePool + (size_t)v * decodeStringLength, &block)) {
      printf("could not prepare the base64 pool\n");
      exit(1);
    }
  }
  // the copying call measures its input, so the buffer it reads has to end somewhere; the in
  // place decode writes only over the front and never disturbs this
  decodeWork[decodeStringLength] = '\0';
}

static int decode_steps(void) {
  uint32_t steps = DECODE_BUDGET / decodeStringLength;
  if (steps > DECODE_MAX_STEPS) { steps = DECODE_MAX_STEPS; }
  if (steps < DECODE_MIN_STEPS) { steps = DECODE_MIN_STEPS; }
  return (int)steps;
}

/**
 * Times one length four ways and prints its row.
 *
 * Every figure is per conversion, and both decoding columns carry the refill. The last column is
 * `(copying - refill) / (in place - refill)`: one baseline, subtracted from both, so what is
 * left is each call as its caller pays it -- the copying one including the strlen it makes for
 * itself, which the strlen column prices separately.
 *
 * Subtracting a different baseline from each side was tried first and is the reason this note
 * exists. Taking the strlen off the copying row and only the refill off the in place row turned
 * the 16 byte row into 0.73x, which read as the in place decode being a third slower and was an
 * artifact: at 16 bytes the refill, the strlen and the decode overlap in the machine rather than
 * adding up, so subtracting a larger baseline from one side alone moves the quotient more than
 * the thing being measured does. One baseline for both, and the difference between the columns
 * is a difference between the calls.
 *
 * The measurements the last column is made of are printed beside it rather than only their
 * quotient, because a ratio of two differences is the noisiest thing on the page and this way it
 * can be checked instead of believed.
 */
static void bench_decode_modes(uint32_t bytes) {
  char refill_text[BENCH_STRING_BUFFER_SIZE];
  char measure_text[BENCH_STRING_BUFFER_SIZE];
  char copying_text[BENCH_STRING_BUFFER_SIZE];
  char in_place_text[BENCH_STRING_BUFFER_SIZE];
  arnm_mono_timer timer;

  set_decode_length(bytes);
  const int steps = decode_steps();

  // a round of each before the clock starts: the pool was just written, and the first row would
  // otherwise carry the page faults of a buffer nothing has read yet
  decode_refill(8);
  decode_refill_and_measure(8);
  decode_copying(8);
  decode_in_place(8);

  arnm_mono_timer_reset(&timer);
  decode_refill(steps);
  const double refill = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  decode_refill_and_measure(steps);
  const double measured = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  decode_copying(steps);
  const double copying = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  arnm_mono_timer_reset(&timer);
  decode_in_place(steps);
  const double in_place = (double)arnm_mono_timer_nanos(timer) / (double)steps;

  const double copying_alone = copying - refill;
  const double in_place_alone = in_place - refill;

  bench_per_step_string(refill_text, BENCH_STRING_BUFFER_SIZE, refill);
  bench_per_step_string(measure_text, BENCH_STRING_BUFFER_SIZE, measured);
  bench_per_step_string(copying_text, BENCH_STRING_BUFFER_SIZE, copying);
  bench_per_step_string(in_place_text, BENCH_STRING_BUFFER_SIZE, in_place);

  printf(
      "  %9u %8d %11s %11s %11s %11s %10.2fx\n", (unsigned)bytes, steps, refill_text, measure_text,
      copying_text, in_place_text, (in_place_alone > 0.0) ? copying_alone / in_place_alone : 0.0
  );
}

static void prepare_decode_buffers(void) {
  const uint32_t work_size = ARNM_BASE64_STRING_LENGTH(DECODE_MAX_BYTES) + 1u;
  const uint32_t capacity = DECODE_POOL_CAPACITY + work_size + DECODE_MAX_BYTES + 4096u;

  if (ARNM_SUCCESS != arnm_init_arena(&decodeArena, capacity) ||
      ARNM_SUCCESS != arnm_alloc((uint8_t **)&decodePool, DECODE_POOL_CAPACITY, &decodeArena) ||
      ARNM_SUCCESS != arnm_alloc((uint8_t **)&decodeWork, work_size, &decodeArena) ||
      ARNM_SUCCESS != arnm_alloc(&decodeSink, DECODE_MAX_BYTES, &decodeArena)) {
    printf("could not reserve the buffers for the in place section\n");
    exit(1);
  }
}

int main(void) {
  arnm_mono_timer timeUsed;
  static const uint32_t lengths[] = {16, 32, 64, 1024};

  if (!bench_timer_start(&timeUsed)) { return EXIT_FAILURE; }
  prepare_test_data();
  bench_prepared(timeUsed);

  for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l) {
    char heading[64];

    set_length(lengths[l]);
    // the count is the same everywhere and named once at the end, so the heading carries only
    // what actually varies between sections
    snprintf(
        heading, sizeof(heading), "%u bytes, %d conversions per row", lengths[l], BENCH_CONVERSIONS
    );
    bench_section(heading);
    bench_step(test_binary_to_hex, BENCH_CONVERSIONS, "  binary to hex", "conversion");
    bench_step(test_binary_from_hex, BENCH_CONVERSIONS, "  binary from hex", "conversion");
    bench_step(test_binary_to_base64, BENCH_CONVERSIONS, "  binary to base64", "conversion");
    bench_step(test_binary_from_base64, BENCH_CONVERSIONS, "  binary from base64", "conversion");
  }

  {
    cursor = 0;
    bench_section("uuid (16 bytes, canonical form), the same count per row");
    bench_step(test_uuid_to_string, BENCH_CONVERSIONS, "  uuid to string", "conversion");
    bench_step(test_uuid_from_string, BENCH_CONVERSIONS, "  uuid from string", "conversion");
  }

  {
    static const uint32_t decodeLengths[] = {16, 1024, 65536, 1048576, DECODE_MAX_BYTES};

    prepare_decode_buffers();
    bench_section("base64 decoded in place, against decoded into a buffer of its own");
    printf(
        "  %9s %8s %11s %11s %11s %11s %11s\n", "bytes", "steps", "the refill", "and strlen",
        "copying", "in place", "in place is"
    );
    for (size_t l = 0; l < sizeof(decodeLengths) / sizeof(decodeLengths[0]); ++l) {
      bench_decode_modes(decodeLengths[l]);
    }
    arnm_release(&decodeArena);
  }

  // the sections above this one share a count and the last one does not, so it is named where
  // it belongs rather than in the closing line -- see bench_total_time()
  bench_total_time(timeUsed);
  // read once at the end: without this the compiler may drop every call whose result nobody
  // wanted, and the benchmark would time an empty loop
  if (resultSink != (unsigned)ARNM_SUCCESS) { printf("a conversion failed: %u\n", resultSink); }
  if (!writtenSink) { printf("the uuid encoding wrote nothing\n"); }
  return 0;
}

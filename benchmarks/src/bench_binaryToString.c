#include "arnm/converter.h"
#include "arnm/memory_block.h"
#include "arnm/mono_timer.h"
#include "arnm/result.h"
#include "bench_report.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
    snprintf(heading, sizeof(heading), "%u bytes", lengths[l]);
    bench_section(heading);
    bench_step(test_binary_to_hex, BENCH_CONVERSIONS, "  binary to hex", "conversion");
    bench_step(test_binary_from_hex, BENCH_CONVERSIONS, "  binary from hex", "conversion");
    bench_step(test_binary_to_base64, BENCH_CONVERSIONS, "  binary to base64", "conversion");
    bench_step(test_binary_from_base64, BENCH_CONVERSIONS, "  binary from base64", "conversion");
  }

  {
    cursor = 0;
    bench_section("uuid (16 bytes, canonical form)");
    bench_step(test_uuid_to_string, BENCH_CONVERSIONS, "  uuid to string", "conversion");
    bench_step(test_uuid_from_string, BENCH_CONVERSIONS, "  uuid from string", "conversion");
  }

  // every step ran the same count, so the closing line can name it -- see bench_total_time()
  bench_total(timeUsed, BENCH_CONVERSIONS, "conversion");
  // read once at the end: without this the compiler may drop every call whose result nobody
  // wanted, and the benchmark would time an empty loop
  if (resultSink != (unsigned)ARNM_SUCCESS) { printf("a conversion failed: %u\n", resultSink); }
  if (!writtenSink) { printf("the uuid encoding wrote nothing\n"); }
  return 0;
}

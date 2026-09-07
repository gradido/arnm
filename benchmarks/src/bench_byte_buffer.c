#include "arnm/byte_buffer.h"
#include "arnm/duration.h"
#include "arnm/memory.h"
#include "arnm/mono_timer.h"
#include "bench_report.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * What this benchmark measures
 *
 * arnm_byte_buffer_copy() and arnm_byte_buffer_push() answer a result code and check their
 * arguments before they write; unsafe_arnm_byte_buffer_copy() and its push do neither, and
 * expect the caller to have asked arnm_byte_buffer_available() first. The steps below put the
 * two side by side, each written the way it would actually be written:
 *
 *   safe    -- call it, and clear the buffer when it says there is no room
 *   unsafe  -- ask for room, clear when there is none, then write without asking again
 *
 * Both therefore take one bounds decision per step; what separates them is the argument checks,
 * the result code and, in the last section, how many times the room is asked for.
 *
 * The last section is the one the pair exists for: a record and the separator behind it are one
 * question, not two, and a run of fields is one question rather than one per field.
 */

#define BUFFER_BYTES (4u * 1024u * 1024u)
#define RECORD_SMALL 8u
#define RECORD_MEDIUM 64u
#define RECORD_LARGE 512u
#define GROUP_FIELDS 4u

static arnm_byte_buffer buffer;
static uint8_t record[RECORD_LARGE];
/** Read once at the end so the writes cannot be reasoned away. */
static uint64_t sink;

static void take_sink(void) {
  const uint8_t *data = NULL;
  uint32_t size = 0;
  if (ARNM_SUCCESS == arnm_byte_buffer_access(&buffer, &data, &size) && size) {
    sink += data[size - 1u];
  }
}

/* --- one byte at a time ------------------------------------------------------------------- */

static void test_push_safe(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    if (ARNM_SUCCESS != arnm_byte_buffer_push(&buffer, (uint8_t)i)) {
      take_sink();
      arnm_byte_buffer_clear(&buffer);
      (void)arnm_byte_buffer_push(&buffer, (uint8_t)i);
    }
  }
}

static void test_push_unsafe(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    if (0 == arnm_byte_buffer_available(&buffer)) {
      take_sink();
      arnm_byte_buffer_clear(&buffer);
    }
    unsafe_arnm_byte_buffer_push(&buffer, (uint8_t)i);
  }
}

/* --- a record at a time -------------------------------------------------------------------- */

static void copy_safe(int stepCount, uint32_t size) {
  for (int i = 0; i < stepCount; ++i) {
    if (ARNM_SUCCESS != arnm_byte_buffer_copy(&buffer, record, size)) {
      take_sink();
      arnm_byte_buffer_clear(&buffer);
      (void)arnm_byte_buffer_copy(&buffer, record, size);
    }
  }
}

static void copy_unsafe(int stepCount, uint32_t size) {
  for (int i = 0; i < stepCount; ++i) {
    if (arnm_byte_buffer_available(&buffer) < size) {
      take_sink();
      arnm_byte_buffer_clear(&buffer);
    }
    unsafe_arnm_byte_buffer_copy(&buffer, record, size);
  }
}

static void test_copy_small_safe(int n) {
  copy_safe(n, RECORD_SMALL);
}
static void test_copy_small_unsafe(int n) {
  copy_unsafe(n, RECORD_SMALL);
}
static void test_copy_medium_safe(int n) {
  copy_safe(n, RECORD_MEDIUM);
}
static void test_copy_medium_unsafe(int n) {
  copy_unsafe(n, RECORD_MEDIUM);
}
static void test_copy_large_safe(int n) {
  copy_safe(n, RECORD_LARGE);
}
static void test_copy_large_unsafe(int n) {
  copy_unsafe(n, RECORD_LARGE);
}

/* --- a group of writes behind one question ------------------------------------------------- */

/** A record, a separator, four of those: what a log line actually costs. */
#define GROUP_BYTES ((RECORD_SMALL + 1u) * GROUP_FIELDS)

static void test_group_safe(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    if (arnm_byte_buffer_available(&buffer) < GROUP_BYTES) {
      take_sink();
      arnm_byte_buffer_clear(&buffer);
    }
    for (uint32_t field = 0; field < GROUP_FIELDS; ++field) {
      (void)arnm_byte_buffer_copy(&buffer, record, RECORD_SMALL);
      (void)arnm_byte_buffer_push(&buffer, ',');
    }
  }
}

static void test_group_unsafe(int stepCount) {
  for (int i = 0; i < stepCount; ++i) {
    // one question for the whole group, which is the shape the unsafe pair is written for
    if (arnm_byte_buffer_available(&buffer) < GROUP_BYTES) {
      take_sink();
      arnm_byte_buffer_clear(&buffer);
    }
    for (uint32_t field = 0; field < GROUP_FIELDS; ++field) {
      unsafe_arnm_byte_buffer_copy(&buffer, record, RECORD_SMALL);
      unsafe_arnm_byte_buffer_push(&buffer, ',');
    }
  }
}

/* --- driver -------------------------------------------------------------------------------- */

int main(void) {
  arnm_mono_timer timeUsed;

  if (!bench_timer_start(&timeUsed)) { return EXIT_FAILURE; }
  for (uint32_t i = 0; i < RECORD_LARGE; ++i) { record[i] = (uint8_t)(i * 7u + 1u); }
  if (ARNM_SUCCESS != arnm_byte_buffer_init(&buffer, BUFFER_BYTES, NULL)) {
    fprintf(stderr, "could not reserve the buffer\n");
    return EXIT_FAILURE;
  }
  bench_prepared(timeUsed);

  const int stepCount = 4000000;

  bench_section("one byte at a time");
  bench_step(test_push_safe, stepCount, "  push", "byte");
  bench_step(test_push_unsafe, stepCount, "  unsafe push", "byte");

  bench_section("one record at a time, 8 bytes");
  bench_step(test_copy_small_safe, stepCount, "  copy", "record");
  bench_step(test_copy_small_unsafe, stepCount, "  unsafe copy", "record");

  bench_section("one record at a time, 64 bytes");
  bench_step(test_copy_medium_safe, stepCount, "  copy", "record");
  bench_step(test_copy_medium_unsafe, stepCount, "  unsafe copy", "record");

  bench_section("one record at a time, 512 bytes");
  bench_step(test_copy_large_safe, stepCount / 4, "  copy", "record");
  bench_step(test_copy_large_unsafe, stepCount / 4, "  unsafe copy", "record");

  bench_section("four records and their separators, room asked once");
  bench_step(test_group_safe, stepCount / 8, "  copy and push", "group");
  bench_step(test_group_unsafe, stepCount / 8, "  unsafe copy and push", "group");

  printf("\nchecksum %llu\n", (unsigned long long)sink);
  bench_total_time(timeUsed);
  (void)arnm_byte_buffer_free(&buffer, NULL);
  return 0;
}

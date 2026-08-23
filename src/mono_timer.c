#include "arnm/mono_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "arnm/duration.h"

#ifdef _WIN32
#include <windows.h>

// counts per second
static LARGE_INTEGER freq = {.QuadPart = 0};

// for support more platforms, look into this as example:
// https://github.com/siu/minunit/blob/master/minunit.h
static int64_t get_time_ns() {
  if (freq.QuadPart == 0) { arnm_mono_timer_init(); }

  LARGE_INTEGER counter;
  if (!QueryPerformanceCounter(&counter)) {
    fprintf(stderr, "Error: QueryPerformanceCounter failed\n");
    exit(1);
  }
  // Scaling to nanoseconds means counter * 1e9 / freq, and the product overflows int64 after
  // a few seconds of uptime. Splitting the counter into whole seconds and the remainder keeps
  // every intermediate inside 64 bit and yields the identical value: with c = q*f + r,
  // (c * 1e9) / f == q * 1e9 + (r * 1e9) / f, exactly, for integer division.
  //
  // Done this way rather than in __int128 because MSVC has no such type and the CMake build
  // exists for exactly that compiler. One path for every toolchain also means the Windows
  // arithmetic is the arithmetic every Windows build exercises, not an untested fallback.
  //
  // Neither half can overflow in practice: r < freq, and QueryPerformanceFrequency reports
  // 10 MHz on current Windows, so r * 1e9 stays sixteen digits short of INT64_MAX; q counts
  // seconds since boot, which reaches INT64_MAX / 1e9 after some 292 years.
  const int64_t ticks = (int64_t)counter.QuadPart;
  const int64_t ticks_per_second = (int64_t)freq.QuadPart;
  const int64_t whole_seconds = ticks / ticks_per_second;
  const int64_t leftover_ticks = ticks % ticks_per_second;
  return whole_seconds * 1000000000LL + (leftover_ticks * 1000000000LL) / ticks_per_second;
}

#else

static int64_t get_time_ns() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

#endif

bool arnm_mono_timer_init() {
#ifdef _WIN32
  if (!QueryPerformanceFrequency(&freq)) {
    fprintf(stderr, "Error: QueryPerformanceFrequency failed\n");
    return false;
  }
#endif
  return true;
}

void arnm_mono_timer_reset(arnm_mono_timer *start) {
  *start = get_time_ns();
}

int64_t arnm_mono_timer_nanos(arnm_mono_timer start) {
  return get_time_ns() - start;
}

double arnm_mono_timer_micros(arnm_mono_timer start) {
  return (double)arnm_mono_timer_nanos(start) / 1e3;
}

double arnm_mono_timer_millis(arnm_mono_timer start) {
  return (double)arnm_mono_timer_nanos(start) / 1e6;
}

double arnm_mono_timer_seconds(arnm_mono_timer start) {
  return (double)arnm_mono_timer_nanos(start) / 1e9;
}
int arnm_mono_timer_string(char *buffer, size_t buffer_size, arnm_mono_timer start) {
  return arnm_duration_string(buffer, buffer_size, arnm_mono_timer_nanos(start), 4);
}

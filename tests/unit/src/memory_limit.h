#ifndef ARNM_TESTS_MEMORY_LIMIT_H
#define ARNM_TESTS_MEMORY_LIMIT_H

/*
 * Caps the address space of a test binary, so that a test asking for an absurd allocation
 * fails inside the process instead of pulling the whole machine into swap. A bucket vector
 * reserve with a bad bound once claimed 64 GB before anyone could stop it; that is a lost
 * afternoon, while an allocation failure is a red test.
 *
 * The cap sits in the binary rather than in a ctest wrapper on purpose: test binaries get run
 * straight from zig-out/bin at least as often as through ctest, and that is exactly when
 * nothing else is watching.
 *
 * Every unit test .cpp includes this. The whole suite peaks well under 1 GB, so the default
 * leaves generous headroom and still stops a runaway an order of magnitude short of hurting.
 *
 * Override with ARNM_TEST_MEMORY_LIMIT_MB, e.g. for a deliberately large test run:
 *   ARNM_TEST_MEMORY_LIMIT_MB=8192 ./zig-out/bin/test_bucket_vector
 *   ARNM_TEST_MEMORY_LIMIT_MB=0    ./zig-out/bin/test_bucket_vector   # off
 *
 * Linux only. Sanitizer builds are skipped: ASan and TSan reserve terabytes of address space
 * up front, so any RLIMIT_AS would stop them from starting at all.
 */

/*
 * The cap is also the only thing that can make an allocation fail on demand, which a test
 * verifying a failure path needs. ArnmTestAllocationMustFail() below is that seam: it is
 * true only where the cap is really in force, so such a test skips instead of leaning on how
 * much memory the machine happens to have. Without a cap a 4 GiB malloc usually *succeeds* --
 * Linux overcommit hands back an address and maps nothing -- and the test would fail while
 * leaking the block it did not expect to get.
 */

#if defined(__linux__)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ARNM_TEST_SKIP_MEMORY_LIMIT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define ARNM_TEST_SKIP_MEMORY_LIMIT 1
#endif
#endif

#if !defined(ARNM_TEST_SKIP_MEMORY_LIMIT)

#include <cstdlib>
#include <sys/resource.h>

namespace {

constexpr rlim_t kArnmTestMemoryLimitDefaultMb = 2048;

struct ArnmTestMemoryLimit {
  ArnmTestMemoryLimit() {
    rlim_t megabytes = kArnmTestMemoryLimitDefaultMb;
    if (const char *env = std::getenv("ARNM_TEST_MEMORY_LIMIT_MB")) {
      char *end = nullptr;
      const unsigned long long parsed = std::strtoull(env, &end, 10);
      if (end == env || *end != '\0') { return; } // unparsable: leave the process alone
      if (parsed == 0) { return; }                // explicitly disabled
      megabytes = static_cast<rlim_t>(parsed);
    }

    rlimit limit{};
    if (getrlimit(RLIMIT_AS, &limit) != 0) { return; }

    const rlim_t wanted = megabytes * 1024 * 1024;
    // never loosen what the environment already decided, only tighten
    if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur <= wanted) { return; }
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < wanted) { return; }

    limit.rlim_cur = wanted;
    setrlimit(RLIMIT_AS, &limit); // best effort; a refusal just leaves the cap off
  }
};

const ArnmTestMemoryLimit g_arnm_test_memory_limit;

/**
 * True when a request of @p bytes is certain to be refused by the host in this process.
 *
 * Asked at call time, not at startup: setrlimit() above is best effort, and an environment that
 * already held a tighter or an unlimited cap decides instead. A request at or above the current
 * limit cannot be served whatever else the process has mapped, which is the only promise a test
 * on a failure path can build on.
 */
inline bool ArnmTestAllocationMustFail(unsigned long long bytes) {
  rlimit limit{};
  if (getrlimit(RLIMIT_AS, &limit) != 0) { return false; }
  if (limit.rlim_cur == RLIM_INFINITY) { return false; }
  return bytes >= static_cast<unsigned long long>(limit.rlim_cur);
}

} // namespace

#endif // !ARNM_TEST_SKIP_MEMORY_LIMIT
#endif // __linux__

#if !defined(__linux__) || defined(ARNM_TEST_SKIP_MEMORY_LIMIT)
namespace {
/** No cap here, so nothing can be promised to fail -- see the note above. */
inline bool ArnmTestAllocationMustFail(unsigned long long) {
  return false;
}
} // namespace
#endif

#endif // ARNM_TESTS_MEMORY_LIMIT_H

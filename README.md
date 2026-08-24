# arnm

C primitives for embedding. **Data lives in Memory-Arenas.**

A small C11 library meant to be linked into a program written in something else — Node.js, Go,
Rust, C++. It brings a bump allocator, containers built on it, and the conversions a host does
constantly and the standard library does slowly. Everything it hands out comes from an arena,
and the arena comes from you: `arnm` brings no memory policy of its own — you give it a blob,
it works inside that blob, and it gives the blob back.

```c
#include <stdalign.h>
#include "arnm/arena.h"

alignas(8) uint8_t blob[64 * 1024];   // wherever this comes from: the host decides
arnm mem;
arnm_init_arena_borrow(&mem, blob, sizeof(blob));

uint8_t *buffer = NULL;
arnm_alloc(&buffer, 128, &mem);
/* ... */
arnm_free(buffer, 128, &mem);

arnm_reset(&mem);              // the whole arena, in one move
```

A borrowed buffer is taken exactly as it is. Both halves of that line have to hold: `blob` must
start on an 8 byte boundary, which is what `alignas(8)` is for, and `sizeof(blob)` must be a
multiple of 8. Neither is rounded — anything else comes back as `ARNM_ERROR_INVALID_PARAM` and
no arena is set up.

The size is the half that surprises people, because the owned arena does round it:
`arnm_init_arena(&mem, 100)` reserves 104 bytes and succeeds, while
`arnm_init_arena_borrow(&mem, blob, 100)` is refused. The difference is who owns the memory —
rounding 100 up to 104 in a buffer you sized at 100 would let the arena hand out four bytes past
its end, so the size is questioned instead of corrected. Declare the blob at a multiple of
8 rather than trimming the call. `arnm_multi_arena_borrow()` borrows under the same rule.

Pass `NULL` instead of an allocator and every call falls back to malloc/free. That is the only
place in the library where `malloc` appears — `lint.sh` fails the build if a second one shows
up.

## What is in it

| Header | What it gives you |
|---|---|
| `arnm/memory.h` | the `arnm` handle and the calls every allocator answers: alloc, free, realloc, clone, reset |
| `arnm/arena.h` | make a handle an arena -- over memory it takes from the host, or memory you lend it |
| `arnm/memory_block.h` | pointer and size kept together, so freeing needs no bookkeeping from you |
| `arnm/multi_arena.h` | a chain of arenas that opens another one instead of running dry |
| `arnm/fixed_arena_pool.h` | a fixed set of equal sized arenas, lent out and returned; the peak is known at init |
| `arnm/bucket_vector.h` | growing sequence with stable element addresses; no copy on growth |
| `arnm/converter.h` | integer to decimal string, roughly 4× faster than `snprintf`; bytes to lowercase hex and back, uuid to its 8-4-4-4-12 form and back |
| `arnm/duration.h` | nanoseconds to a readable span |
| `arnm/mono_timer.h` | monotonic clock, one type, three units |
| `arnm/result.h` | one result code for everything, with a range reserved for you |

## The contract

- **Sizes are `uint32_t`.** Counts, indices, byte sizes. Anything that would not fit returns
  `ARNM_ERROR_ARITHMETIC_OVERFLOW` instead of wrapping.
- **Sizes are passed in, never stored.** Freeing and resizing need the size you allocated
  with. `arnm_memory_block` keeps the two together when you would rather not.
- **Every size rounds up to 8**, so every pointer the arena hands out is 8 byte aligned.
- **An arena only takes back its most recent allocation.** Anything before it stays until
  `arnm_reset`. Calls that could not reclaim return `ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED`
  — the operation happened, the memory did not come back. It is neither a failure nor a
  release; handle it where it appears.
- **Failures leave every output untouched.**

## When one arena is not enough

An arena has the capacity it was born with. A chain keeps a set of them and opens the next one
when the current stretch fills, so the size never has to be guessed right up front — and a
request larger than the arena capacity gets ground of its own instead of a refusal.

A chain is an `arnm *` like any other allocator: it is *built* through this header and then
*used* through the calls above.

```c
#include "arnm/multi_arena.h"

// 1 MiB per arena, and an arena drops out of the search once under 4 KiB is left.
// {0} would take every default: 1 KiB arenas, a 64 byte threshold, no ceiling on the count.
arnm_multi_arena_options options = {0};
options.arena_capacity = 1024 * 1024;
options.full_remaining = 4096;

arnm *chain = arnm_create_multi_arena(&options, NULL);
arnm_multi_arena_borrow(chain, blob, sizeof(blob));  // optional: lend it the host's blob,
                                                     // same alignas(8) and multiple-of-8 rule

uint8_t *buffer = NULL;
arnm_alloc(&buffer, 4096, chain);   // the same calls a single arena takes
arnm_free(buffer, 4096, chain);

arnm_reset(chain);                  // every allocation, in one move; the arenas stay
arnm_multi_arena_shrink(chain);     // and give the empty ones back to the host
arnm_destroy(chain, NULL);          // naming the allocator it was created with
```

`arnm_alloc`, `arnm_free`, `arnm_realloc`, `arnm_clone` and `arnm_reset` do not care which of
the two they were handed. What stays chain specific is what only a chain can answer:
`_reserve()`, `_borrow()`, `_shrink()`, `_arena_count()` and `_measure()` — the last of which
reports bytes reserved against bytes used, and how much of the chain the search still walks.

`NULL` returned from `arnm_create_multi_arena()` means the options were refused or the allocator
had no room; `arnm_multi_arena_options_validate()` tells the two apart, and fills a set of
options in with the defaults it would really run on. The allocator passed there carries the
chain's own bookkeeping only — the arenas always come from the host, because a chain drawing
them from a fixed allocator could not outgrow it, which is the one thing it exists to do.

Pointers stay put: an arena, once opened, is never moved or resized. A borrowed block stays the
host's: the chain fills it and never frees it. Set `arena_max_count` to cap the chain, and it
answers `ARNM_ERROR_RESOURCE_EXHAUSTED` instead of opening another arena — a bounded budget
with a known peak.

`full_remaining` is the field worth thinking about. A request is served first fit, and an arena
leaves that search for good once its remainder falls to the threshold. So the question it
answers is: what is the smallest request that should still land in a leftover? An arena holds a
request of `n` bytes while at least `n` are left, and is written off at or below the threshold,
so `n - 8` drops it out of the search exactly when it can no longer take that request.

For uniform records that is the whole story — one alignment step below the record size wastes
nothing and keeps the search short. For mixed sizes it is a trade: lower gives up nothing usable
but leaves arenas in the search holding remainders only the small requests fit, at around half a
nanosecond per arena walked, which only bites once a thousand of them have piled up; higher
keeps the search short and writes off up to a threshold worth of bytes per arena.
`bench_multi_arena` puts numbers on both ends.

## Build

```bash
zig build                                                    # the static library, host target
zig build -Dtests=true -Dbenchmarks=true
./run_all.sh                                                 # run everything in zig-out/bin
```

Options: `-Dtarget=` to cross compile, `-Dshared=true` for a dynamic library (what a language
binding usually wants), `-Dsanitize=undefined_behavior` or `-Dsanitize=thread`, and
`-DsingleOutputDir=true` to drop the artifacts without the `bin/` and `lib/` split.

Targets verified to build: `x86_64-linux-gnu`, `x86_64-linux-musl`, `x86_64-windows-gnu`,
`x86_64-macos`, `aarch64-macos`. The `-msvc` ABI targets need the MSVC SDK headers present on
the machine — zig does not ship them, so that combination is untested here.

Naming a target also side-steps an incomplete host toolchain: without `-Dtarget` zig compiles
against the system headers in `/usr/include`, with one it uses the headers it ships itself. If
a plain `zig build -Dtests=true` fails on something like a missing `asm/errno.h`, install your
distribution's kernel headers or pass `-Dtarget=x86_64-linux-gnu`.

That gap is what `CMakeLists.txt` is for: the MSVC ABI on Windows, and nothing else. `build.zig`
stays the master build — CMake mirrors it and never leads it.

```bash
cmake -B build -DENABLE_TESTS=ON -DENABLE_BENCHMARKS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

`ENABLE_TESTS`, `ENABLE_BENCHMARKS`, `ENABLE_SANITIZERS` and `ENABLE_THREAD_SANITIZER` are the
CMake spellings of the `-D` options above. AddressSanitizer is the one thing this route offers
that zig cannot — zig ships no asan runtime, MSVC does.

**Benchmark numbers need `-Doptimize=ReleaseFast`.** In a debug build the hand written digit
loop loses to `snprintf` by a factor of two, because libc ships optimised and your build does
not. With optimisation the picture turns around:

```
unsigned to string        snprintf   61.8 ns      arnm  16.3 ns
signed to string          snprintf   58.2 ns      arnm  13.8 ns
```

## Using it from another zig project

```zig
const arnm = b.dependency("arnm", .{ .target = target, .optimize = optimize });
lib.linkLibrary(arnm.artifact("arnm"));
lib.addIncludePath(arnm.path("include"));
```

## Changes

[CHANGELOG.md](CHANGELOG.md) -- what moved in each release, newest first.

## License

Apache 2.0

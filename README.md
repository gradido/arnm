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
up. The JSON parser vendored under `third_party/` is held to the same line: it is handed an
allocator that forwards to `arnm_alloc`/`arnm_realloc`/`arnm_free`, so its own default
allocator is on no path this library takes.

## What is in it

| Header | What it gives you |
|---|---|
| `arnm/memory.h` | the `arnm` handle and the calls every allocator answers: alloc, free, realloc, clone, reset |
| `arnm/arena.h` | make a handle an arena -- over memory it takes from the host, or memory you lend it |
| `arnm/memory_block.h` | pointer and size kept together, so freeing needs no bookkeeping from you |
| `arnm/multi_arena.h` | a chain of arenas that opens another one instead of running dry |
| `arnm/fixed_arena_pool.h` | a fixed set of equal sized arenas, lent out and returned; the peak is known at init |
| `arnm/bucket_vector.h` | growing sequence with stable element addresses; no copy on growth |
| `arnm/json_reader.h` | JSON parsed into your arena; one line per struct field, the first error kept with its field name, an in-situ path that copies nothing |
| `arnm/json_writer.h` | the way back: one line per struct field, strings borrowed rather than copied, and the output size known before the text exists |
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

## Asking an arena where it stands

Two figures, read from opposite sides of the same line, and both a field read rather than a
walk:

```c
uint32_t left = arnm_arena_remaining(&mem);   // what is still there
size_t   over = arnm_arena_overflow_total(&mem);  // what was already turned away
```

`arnm_arena_remaining()` is the largest request that would still be served -- an arena never
reaches past the block it was given. It is asked *before* a request, which is what makes it
useful: code with somewhere else to put a large block can look first instead of being refused
to find out. The figure is always a multiple of 8, and it holds until the next call that moves
the index.

`arnm_arena_overflow_total()` is the other half, and it is only ever read afterwards. Every
request the arena had to refuse adds its rounded up size, saturating at `UINT32_MAX` rather
than rolling over. Run the real workload once and the figure says how much larger the arena
would have had to be — the sizing question a guess only postpones. `arnm_reset()` and
`arnm_release()` clear it.

Both answer 0 for `NULL`, for host mode and for a chain, so a 0 from either is "full" only when
the handle really is a single arena. `arnm_is_arena()` and `arnm_is_multi_arena()` tell those
cases apart, and a chain is asked through `arnm_multi_arena_measure()` instead — it opens more
ground rather than running dry, so one remainder could not say what it appears to.

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

## Reading JSON

`arnm/json_reader.h` parses a document into the allocator you name and reads it back one field
per line. The parser is yyjson, vendored under `third_party/` as a submodule — it is compiled
into `libarnm` and never surfaces: the header is plain C11, and you link one library.

The reader keeps the **first** error and the field it happened at, so a struct is filled without
a test between the lines and asked about once, at the end:

```c
#include "arnm/json_reader.h"

arnm_json_reader reader;
arnm_json_reader_init(&reader, &mem, ARNM_JSON_READ_DEFAULT);  // NULL is the host, as everywhere
arnm_json_reader_parse(&reader, text, length);

config.host    = arnm_json_reader_get_string(&reader, "host");
config.port    = arnm_json_reader_get_uint32(&reader, "port");
config.timeout = arnm_json_reader_get_double(&reader, "timeout");
config.debug   = arnm_json_reader_get_bool(&reader, "debug");

if (ARNM_SUCCESS != arnm_json_reader_status(&reader)) {
  log("%s at '%s'", arnm_result_to_string(arnm_json_reader_status(&reader)),
      arnm_json_reader_error_field(&reader));
}

arnm_json_reader_release(&reader);              // every borrowed string dangles from here on
```

Once something is refused, every later getter answers its empty value — NULL, false, 0 — and
leaves the recorded error alone. A refusal in the middle of a struct therefore does not have to
stop the reading, and `arnm_json_reader_clear_error()` picks it up again where you want to go on
deliberately.

Nesting is two lines and no bookkeeping: `arnm_json_reader_enter()` steps into a member and
hands back the value it left, `arnm_json_reader_leave()` puts it back. Arrays go by
`arnm_json_reader_enter_at()` with `arnm_json_reader_count()` as the bound, and the reader
remembers where it stood, so walking one costs a step per element rather than a walk per
element. A key of `NULL` names the current value itself, which is how the elements of an array of
scalars are read.

```c
arnm_json_value *array = arnm_json_reader_enter(&reader, "peers");
for (uint32_t i = 0, n = arnm_json_reader_count(&reader); i < n; ++i) {
  arnm_json_value *element = arnm_json_reader_enter_at(&reader, i);
  config.peers[i].name = arnm_json_reader_get_string(&reader, "name");
  config.peers[i].port = arnm_json_reader_get_uint32(&reader, "port");
  arnm_json_reader_leave(&reader, element);
}
arnm_json_reader_leave(&reader, array);
```

A string a getter hands back points into the document and dies with it. Naming an output
allocator — `arnm_json_reader_set_output_allocator(&reader, &strings)` — copies each one there
instead, NUL terminated, so it outlives the document, the reader, and the buffer an in-situ
parse read through. Nothing else is copied; numbers and bools travel by value already.

The arena that receives those copies can be sized before it is built. Both measurements walk
the document's value array rather than its tree — a flat run, no recursion — and answer the exact
byte count, terminators and eight byte rounding included:

```c
uint32_t bytes = arnm_json_reader_output_size(&reader);              // every string in the tree
static const char *const wanted[] = {"host", "name"};
bytes = arnm_json_reader_output_size_for_keys(&reader, wanted, 2);   // only what you will read
```

The first is the cheaper call and the larger answer: a document carrying members you never read
pays for them. The second compares every member name against the list, at every depth, so an
array of objects is covered by naming its member once — and it reserves only what a mapper will
actually copy. Measured in `bench_json` on a document of 61 values, five of whose members are
read and twenty-five of which are not: the full walk takes about 62 ns and reserves 1200 bytes,
the keyed walk takes about 148 ns and reserves 200. Both sit well under the parse that precedes
them, which costs about 395 ns for the same document.

A read is refused rather than rounded: `arnm_json_reader_get_int64()` on `3.5` records
`ARNM_ERROR_ARITHMETIC_OVERFLOW` instead of handing back `3`, and reading a string as a number
records `ARNM_ERROR_INVALID_ENUM_TYPE`. Under the field level sit the value level calls the same
document is reachable through — `arnm_json_object_get()`, `arnm_json_read_uint32()`,
`arnm_json_array_iter_next()` and the rest — each answering an `arnm_result` of its own, for
code that wants every step checked where it happens.

`arnm_json_reader_parse_insitu()` is the cheaper path. It unescapes strings inside your own
buffer instead of copying it, which leaves a document at exactly one allocation — and behind an
arena that one sits at the tail, so releasing gives every byte back. The price is that the
buffer is modified, has to outlive the document, and has to carry
`ARNM_JSON_READER_INSITU_PADDING` spare bytes past the JSON. The copying `arnm_json_reader_parse()`
asks none of that and costs one input copy, which stays buried in an arena until `arnm_reset()`.

## Writing JSON

`arnm/json_writer.h` is the reader read backwards, and keeps its three habits: one line per
field, strings borrowed rather than copied, and one check at the end.

```c
#include "arnm/json_writer.h"

arnm_json_writer writer;
arnm_json_writer_init(&writer, scratch, ARNM_JSON_WRITE_DEFAULT);   // or ..._PRETTY

arnm_json_writer_add_string(&writer, "host", config.host);
arnm_json_writer_add_uint64(&writer, "port", config.port);
arnm_json_writer_add_bool(&writer, "debug", config.debug);

arnm_memory_block text;
if (ARNM_SUCCESS == arnm_json_writer_write(&writer, output, &text, NULL)) {
  send(text.data);                          // NUL terminated
  arnm_memory_block_free(&text, output);
}
```

No `begin` is needed for an object root; the first field opens one.
`arnm_json_writer_begin_array()` opens an array instead, and either one starts the next payload
through the same writer. A writer carrying an error refuses to write and answers that error, so
the result above stands in for a check after every field —
`arnm_json_writer_status()` and `arnm_json_writer_error_field()` are there when you want the
verdict earlier.

`arnm_json_writer_add_string()` keeps the pointer it is given and nothing else. Every string and
every key therefore has to stay where it is until the write, which is what makes serialising a
struct cost almost nothing: the payload is read once, at the end, straight out of your own
memory. `arnm_json_writer_add_string_copy()` is there for a value that will not stand still that
long.

Nesting is an open and a close, and the writer keeps the levels itself — up to
`ARNM_JSON_WRITER_MAX_DEPTH`, past which it records `ARNM_ERROR_RESOURCE_EXHAUSTED` rather than
reaching for memory mid-field. A key of `NULL` means "an element of the current array".

```c
arnm_json_writer_open_array(&writer, "peers");
for (uint32_t i = 0; i < config.peer_count; ++i) {
  arnm_json_writer_open_object(&writer, NULL);
  arnm_json_writer_add_string(&writer, "name", config.peers[i].name);
  arnm_json_writer_close(&writer);
}
arnm_json_writer_close(&writer);
```

**The size of the output is known before the text exists.** `arnm_json_writer_size()` reads a
number the writer has been keeping all along: every field knows its own length, its separator
and its indentation the moment it is added, so nothing is walked and nothing is rendered twice.
It is **exact** for a document of integers, booleans, nulls and valid UTF-8 strings, under every
layout — and `arnm_json_writer_write()` takes exactly that many bytes from the allocator you
name, so an arena sized by it comes out full to the byte. Two things make it an upper bound
instead, and only ever too large: a `double` is charged its longest form (25 bytes), and under
`ARNM_JSON_WRITE_ESCAPE_UNICODE` a byte outside ASCII is charged six. The slack goes back before
`write()` returns.

Asking costs 1.4 ns whatever the document holds. Keeping the number costs one table lookup per
string byte, about 0.2 ns — `bench_json` prices both against the work they precede, and runs
every payload through the reader and the writer alike so the two directions can be read against
each other.

## Build

```bash
git submodule update --init --recursive                      # third_party/yyjson, once per clone
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

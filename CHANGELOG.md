# Changelog

Every release of arnm, newest first. A date is the day the version was set in `build.zig.zon`,
which is not always the day a tag followed: 0.3.1 and 0.4.0 carry no tag yet.

The library was called hostmem until 0.5.0, which renamed every symbol. Entries below that
version name the symbols as they were spelled at the time, so the record still matches the
tags -- read a `hostmem_` there as today's `arnm_`.

The version lives in `build.zig.zon`; `Doxyfile` carries it a second time for the generated
documentation. Until 1.0 the minor number moves for new API, the patch number for fixes -- a
minor release may still change what is already there, and such a change is named here rather
than left to be discovered.

Entries before 0.4.0 were reconstructed from the git history after the fact, so they summarise
what the commits show rather than what was noted at the time.

## 0.7.1 -- 2026-08-24

An arena could always say how much it had already refused. Now it can also be asked how much is
still there, before anything is refused at all.

Additive only: nothing that was there behaves differently, so the patch number moves rather than
the minor one -- against the rule at the top of this file, which reserves the minor number for
new API. The exception is named here rather than left to be noticed: one function was added and
no existing one changed, so code built against 0.7.0 links and behaves the same.

### Added

- **`arnm_arena_remaining()` in `arnm/arena.h` -- the bytes an arena can still hand out.** What
  lies between the index and the end of the block, and therefore the largest request that would
  still be served. A subtraction of two fields: nothing is walked and nothing is counted, so it
  is cheap enough to ask before every allocation, which `arnm_multi_arena_measure()` is not.
  - **It is read before a request, where the overflow counter is read after one.** The two
    figures sit on opposite sides of the same line. A caller with somewhere else to put a large
    block can look first instead of having to be refused to find out;
    `arnm_arena_overflow_total()` stays the answer to the sizing question a whole run asks.
  - **0 where there is no single arena to measure** -- NULL, host mode, a chain, and an arena
    that was released or never initialized. That matches `arnm_arena_overflow_total()`, and it
    means a 0 reads as "full" only once the handle is known to be a single arena;
    `arnm_is_arena()` and `arnm_is_multi_arena()` tell the cases apart. A chain is asked through
    `arnm_multi_arena_measure()` instead: it opens more ground rather than running dry, so one
    remainder could not say what it appears to.
  - The figure is always a multiple of 8, being the difference of two figures that are, and it
    holds until the next call that moves the index -- `arnm_alloc()`, `arnm_realloc()`,
    `arnm_free()`, `arnm_reset()`.
- Seven tests. Five in `test_memory.cpp`: the remainder against allocation and reclaim, the
  exact fit at the boundary, the rounding an owned arena does against the exactness a borrowed
  one keeps, the four handles that answer 0, and the two figures read side by side. Two in
  `test_multi_arena.cpp`: a chain answers 0 while `_measure()` answers for real, and each arena
  inside a chain still answers on its own.
- README: a section on asking an arena where it stands, which also documents
  `arnm_arena_overflow_total()` -- it had only ever been described in the header.

## 0.7.0 -- 2026-08-24

JSON in both directions, over the allocator that is already there. A reader that fills a struct
one line per field and a writer that empties one the same way, both parsing and rendering into
memory arnm hands out -- and both keeping the whole of yyjson behind them, where a consumer
never has to know it is there.

Everything here landed after v0.6.0 was tagged. The section below it describes what that tag
carries, which is the allocator work and none of this.

### Added

- **`arnm/json_reader.h` — a JSON reader that parses into your allocator and fills a struct one
  line per field.** An opaque `arnm_json_reader` with `init`/`create`/`release`/`destroy`, where
  the allocator and the read flags are named once at `init` and every later parse reads under
  them.
  - **The first error stays, with the field it happened at.** Every
    `arnm_json_reader_get_string/_bool/_int64/_uint64/_int32/_uint32/_double()` reads a member of
    the current value and answers its empty value — NULL, false, 0 — on a refusal, without
    overwriting what was recorded. So a whole struct is read without a test between the lines and
    asked about once, through `arnm_json_reader_status()` and `arnm_json_reader_error_field()`.
    A parse that fails is that same first error. `arnm_json_reader_clear_error()` picks the
    reading up again.
  - **Nesting costs two lines and no bookkeeping.** `arnm_json_reader_enter()` steps into a
    member and hands back the value it left; `arnm_json_reader_leave()` puts it back, on every
    path, so a step that failed pairs exactly like one that did not. Arrays go by
    `arnm_json_reader_enter_at()` with `arnm_json_reader_count()` as the bound, and the reader
    remembers where it stood — walking an array costs a step per element rather than a walk per
    element. A key of `NULL` names the current value itself, which is how an array of scalars is
    read. `arnm_json_reader_has()` and `arnm_json_reader_type_of()` ask without recording
    anything, for optional members.
  - **Borrowed strings, or copied ones.** A string a getter hands back points into the document
    by default. `arnm_json_reader_set_output_allocator()` copies each one into the arena it names
    instead, NUL terminated, so it outlives the document and the buffer an in-situ parse read
    through. NULL there means borrow, and is the one place in arnm where NULL is not the host: a
    copy nobody can free one at a time belongs in an arena.
  - **The output arena can be sized before it is built.**
    `arnm_json_reader_output_size()` answers the bytes every string in the document costs;
    `arnm_json_reader_output_size_for_keys()` counts only the members named in a
    `const char *const *` of up to 255 names, at every depth, so an array of objects is covered
    by naming its member once. Both numbers are exact — terminator and eight byte rounding
    included — and both walk the document's flat value array rather than its tree, so nesting is
    a number rather than a call depth. `bench_json` puts the trade-off in figures: on a document
    of 61 values with five members read and twenty-five ignored, the full walk costs about 62 ns
    and reserves 1200 bytes while the keyed walk costs about 148 ns and reserves 200, against
    about 395 ns for the parse itself.
  - **The value level is still there underneath**, each call answering an `arnm_result` of its
    own: `arnm_json_read_null/_bool/_int64/_uint64/_int32/_uint32/_double/_string()`, plus arrays
    and objects by index, by key, or through an iterator. Values are opaque handles owned by the
    document; a `release` or the next parse invalidates every one of them.
  - A number is refused rather than rounded. `arnm_json_read_int64()` on `3.5` answers
    `ARNM_ERROR_ARITHMETIC_OVERFLOW`, as does one on a value outside the target type;
    `arnm_json_value_number_type()` says beforehand which of `uint64`, `int64` and `double`
    carries a given number whole.
  - `arnm_json_reader_parse_insitu()` unescapes strings inside the caller's buffer instead of
    copying it, which leaves a document at a single allocation — and that one sits at an arena's
    tail, so `arnm_json_reader_release()` gives every byte back. It asks for a writable buffer
    that outlives the document and carries `ARNM_JSON_READER_INSITU_PADDING` spare bytes.
    `arnm_json_reader_parse()` asks none of that and pays one input copy, which stays buried in
    an arena until `arnm_reset()` and is reported as
    `ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED`.
  - The flags are arnm's own bits, translated one at a time. A bit the header does not define is
    refused with `ARNM_ERROR_INVALID_PARAM` at `init`, before a byte of the reader is written,
    rather than passed through.
- **`arnm/json_writer.h` — the way back out, on the reader's terms.** An opaque
  `arnm_json_writer` with `init`/`create`/`release`/`destroy`, the flags named once at `init`,
  one `add` per struct member, and the first error kept with the field name it happened at.
  `arnm_json_writer_write()` renders into an allocation the caller owns and frees, and a writer
  carrying an error refuses to write, so its result stands in for a check after every field.
  - **Strings are borrowed.** `arnm_json_writer_add_string()` keeps the pointer and nothing
    else, which is what makes serialising a struct cost almost nothing -- and what makes every
    string and key the caller's to hold still until the write.
    `arnm_json_writer_add_string_copy()` takes a copy into the writer's own allocator for a
    value that will not.
  - **The output size is known before the text exists.** `arnm_json_writer_size()` reads a
    running total the writer keeps as fields arrive -- nothing is walked, nothing is rendered
    twice -- and `arnm_json_writer_write()` takes exactly that many bytes from the allocator it
    is handed. It is exact for integers, booleans, nulls and valid UTF-8 strings under every
    layout; a `double` is charged its longest form and an escaped non-ASCII byte six, both of
    which can only make it too large.
  - Nesting is `open_object`/`open_array` and `close`, with the levels kept by the writer up to
    `ARNM_JSON_WRITER_MAX_DEPTH`. A key of `NULL` is an element of the current array, mirroring
    the reader, where a NULL key is the current value itself.
- **`bench_json` — one benchmark for both directions, over one payload.** Each document is
  built by the writer, rendered, and then parsed back by the reader, so the two directions are
  measured on bytes neither of them merely resembles.
- **`third_party/yyjson` 0.12.0, the first and only vendored dependency.** A git submodule,
  pinned to a release tag rather than a branch head, compiled into `libarnm` and invisible
  from outside: no installed header names it, its include path is private to the library
  target in both builds, and every allocation it makes is forwarded to
  `arnm_alloc`/`arnm_realloc`/`arnm_free` — its own default allocator is on no path this library
  takes. A fresh clone needs `git submodule update --init --recursive`; both builds check for the
  source and name it when it is missing.

### Fixed

- **`arnm_uint64_to_string_size()` answered one digit short above 10^19.** The ladder stopped at
  nineteen, so every value from `10000000000000000000` up was reported as nineteen digits --
  and since `arnm_uint64_to_string()` fills its buffer from the back, the number it wrote was
  missing its first digit. `UINT64_MAX` came out as `8446744073709551615`. The test that was
  meant to catch it compared against a reference implementation carrying the same off-by-one;
  that reference is fixed too, and the twenty digit range is now checked against its own text.

## 0.6.0 -- 2026-08-24

The arena and the multi arena became one allocator behind one handle. A consumer that only ever
called `arnm_alloc()` and `arnm_free()` is unaffected; one that named the multi arena's own
functions or read fields out of the descriptor has work to do, and every such change is below.

### Added

- **A chain can be capped.** `arnm_multi_arena_options::arena_max_count` bounds the number of
  arenas; a chain at its cap answers `ARNM_ERROR_RESOURCE_EXHAUSTED` instead of opening another,
  which turns it into a budget with a known peak. 0 keeps the old behaviour of growing as long
  as the host gives ground.
- **`arnm/arena.h`** collects what belongs to a single arena: `arnm_init_arena()`,
  `arnm_init_arena_borrow()`, `arnm_reinit_arena()`, `arnm_is_arena()` and
  `arnm_arena_overflow_total()`. It includes `arnm/memory.h`, so an arena using host code needs
  this header alone.
- **`arnm_is_multi_arena()`** tells a chain from a plain arena -- `arnm_is_arena()` is true for
  both, a chain allocating by moving an index as well, only in more than one block.
- **`ARNM_BVEC_MAYBE_UNUSED`** in `arnm/bucket_vector.h` marks the wrappers
  `ARNM_BVEC_DEFINE` generates. The macro emits the whole typed set, so a caller who wants three
  of them got clang's `-Wunused-function` for the other sixteen -- noise no consumer could
  silence without giving up the macro. gcc never warned here and MSVC's C4514 is off at every
  level, so the fallback expands to nothing.

### Changed

- **`arnm` is opaque.** The descriptor is a sized 32 byte struct with no readable fields, so it
  can still live on the stack or inside another allocation while nothing outside the library
  depends on its layout. Code that read `last_index`, `capacity` or `data` asks
  `arnm_multi_arena_measure()` or `arnm_arena_overflow_total()` instead; code that reached for
  them to tell allocators apart asks `arnm_is_arena()` or `arnm_is_multi_arena()`.
- **The multi arena is reached through the same calls as everything else.** It has no public
  struct and no functions of its own for allocation: `arnm_multi_arena_init/_alloc/_clone/
  _free/_reset/_release/_destroy` are gone. A chain comes from
  `arnm_create_multi_arena()` as an `arnm *` and is then handed to `arnm_alloc()`,
  `arnm_free()`, `arnm_reset()`, `arnm_destroy()` -- the same names an arena takes. What remains
  chain specific is what only a chain can answer: `_reserve()`, `_borrow()`, `_shrink()`,
  `_arena_count()`, `_measure()`.
- **`arnm_create_multi_arena()` takes an options struct and one allocator.** It was
  `(capacity, full_remaining, bookkeeping_allocator, descriptor_allocator)`; it is now
  `(arnm_multi_arena_options *, arnm *allocator)`, where `{0}` selects every default and the one
  allocator carries both the handle and the descriptor vector. The arenas themselves still come
  from the host, since a chain that drew them from a fixed allocator could not outgrow it.
  `arnm_multi_arena_options_validate()` answers *why* a set was refused, which the old two-step
  could not.
- **Arena setup moved to `arnm/arena.h`,** and `arnm_overflow_total()` is now
  `arnm_arena_overflow_total()` -- it only ever answered for a single arena, and the name now
  says so.
- **`arnm_align8_u32()` returns the rounded size** instead of a bool with an out parameter; 0 is
  the refusal. Every caller already rejected a size of 0 first, so folding "nothing to round"
  into "cannot be rounded" costs nothing and removes a parameter.
- **A bucket vector names its element type at `_init`, not in its type.**
  `ARNM_BVEC_STATIC(name, type, log2)` is gone; `ARNM_BVEC_DEFINE(name, type)` generates the
  typed accessors and every vector is an `arnm_bvec`, with the bucket exponent and the growth
  step passed to `arnm_bvec_init()`. `ARNM_BVEC_FOREACH` is gone with it -- walk by index, or
  bucket by bucket through `arnm_bvec_bucket_data()`.
- **A bucket vector has a new ceiling, and it is the tighter one.**
  `ARNM_BVEC_MAX_INDEX_CAPACITY` is bounded by the counters the index array is measured in
  rather than by what the allocator would hand out in one go, which puts a vector at that many
  buckets times its bucket capacity -- 8191 x 2^log2 today. A bucket exponent is therefore a
  capacity decision, not only a memory one: below 2^9 no vector holds four million elements at
  all. The index array also grows by a named step now rather than by doubling, so a vector that
  knows its size should call `arnm_bvec_reserve()` -- behind an arena especially, where every
  regrowth strands the previous array.
- **A zeroed `arnm_bvec` is no longer a usable empty vector.** The element size lives in the
  descriptor now, so one that never saw `arnm_bvec_init()` reads as empty through every accessor
  and refuses every write with `ARNM_ERROR_INVALID_STATE`. `arnm_bvec_init()` also refuses a
  bucket exponent of 0; the smallest bucket holds two elements.
- **`arnm_bvec_reserve(v, 0)` is refused** with `ARNM_ERROR_INVALID_PARAM`, where it used to
  succeed as a no-op. A count that computed to nothing is the caller's arithmetic going wrong.
- **The full threshold is asked before the fit.** An arena whose remainder has fallen to the
  threshold leaves the chain's scan for good, even for a later request its remainder could still
  have held -- giving up that tail is what the threshold buys. The boundary is `remaining <=
  full_remaining`, which is the reading `arnm_multi_arena_options_validate()` already assumed
  when it refuses a threshold that reaches the capacity.
- **A chain tells a foreign address from a buried block.** `arnm_free()` and `arnm_realloc()`
  now find the arena a block came from, so freeing the tail of *any* arena in the chain reclaims
  it -- previously only the last arena was consulted, and everything else answered
  `ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED`. An arena that has room again rejoins the scan. An
  address no arena of the chain owns is now `ARNM_ERROR_INVALID_PARAM` rather than that same
  warning.
- **A tail block that outgrows its arena moves within the chain.** `arnm_realloc()` takes a
  fresh block from another arena, copies the contents and hands the old one back, answering
  `ARNM_SUCCESS`; it used to answer `ARNM_ERROR_OUT_OF_MEMORY`. A single arena still refuses --
  it has nowhere else to go -- and a buried block still moves without reclaiming, as before.
- **Every call in `arnm/bucket_vector.h` checks its pointers.** `arnm_bvec_clear()`,
  `_emplace()`, `_push_ptr()` and `_pop()` used to read the descriptor before anything else.
- **A chain's default arena is 1 KiB, and its default full threshold 64 bytes.**
  `ARNM_MULTI_ARENA_DEFAULT_CAPACITY` was 1 MiB and
  `ARNM_MULTI_ARENA_DEFAULT_FULL_REMAINING` 128. A megabyte per arena is a lot of ground to take
  for a chain that is barely used, and the defaults are for the caller who has not measured yet
  -- one who has names both at `arnm_create_multi_arena()`, where nothing changed.
  `ARNM_MULTI_ARENA_BUCKET_LOG2` is now `ARNM_MULTI_ARENA_DEFAULT_BUCKET_LOG2`, so all three
  read as what they are.
- **The bucket vector's macro surface is one macro.** `ARNM_BVEC_DECLARE`,
  `ARNM_BVEC_BUCKET_BYTES` and `ARNM_BVEC_INDEX_INITIAL_CAPACITY` are gone with
  `ARNM_BVEC_STATIC`: the accessors are generated in place by `ARNM_BVEC_DEFINE`, a bucket's
  byte size is `sizeof(type) << log2` in the caller's own code, and the index array grows by
  `ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE` slots at a time instead of doubling from an initial
  capacity.
- `ARNM_ALIGN8` rounds with `/8*8` rather than a `& ~7` mask, so the operand keeps its own type
  instead of being converted through `int`. Same value for everything the library rounds; it is
  the `-Wconversion` noise at every use site that goes away.
- **Both builds compile the first-party C with `-Wall -Wextra` beside `-Wconversion`**
  (`/W4` on MSVC), and the tree is clean under them on gcc and clang alike. The two do not
  overlap: gcc reports the signed/unsigned comparisons, clang the wrappers a macro expanded but
  the translation unit never called -- so a change is worth checking against both. The
  googletest translation units stay exempt.
- The public headers carry full reference documentation, and the doxygen run is warning free.

### Fixed

- **A multi arena leaked everything it held.** `arnm_release()` tested `is_arena()` first, which
  is true for a chain as well, so the branch that releases the arenas and the descriptor vector
  was never reached. Every arena body and the vector stayed with the host until the process
  ended.
- **`arnm_fixed_arena_pool` wrote its free list over its own descriptors.** The link belongs in
  the first bytes of a free arena's *buffer*; it was going into `arena->bytes`, which is the
  descriptor, whose first eight bytes are the pointer to that buffer. The first allocation out of
  a pooled arena landed on whatever address the link had held.
- **`arnm_bvec_reserve()` divided by zero** on a descriptor that never saw `arnm_bvec_init()`:
  the state check accepted an all-zero descriptor, and the bound is computed against the element
  size. It is now rejected with `ARNM_ERROR_INVALID_STATE`.
- **`arnm_free(buffer, 0, arena)` reported `ARNM_ERROR_ARITHMETIC_OVERFLOW`.** A size of 0 did
  not overflow; it is nothing to give back, and it now takes the same warning a buried block
  takes. `arnm_realloc()` had the same confusion, which made its documented "`new_size` 0
  releases the block" path unreachable and refused a fresh allocation from `old_size` 0.
- **`arnm_multi_arena_options_validate()` passed options that `arnm_create_multi_arena()` then
  refused.** It resolved only the capacity, so a threshold of 0 was compared as 0 there and as
  the default afterwards. It fills in every default now, which is what makes it answer about the
  values a chain is really built on.
- **`arnm_result_to_string(ARNM_ERROR_RESOURCE_SIZE_EXCEED)` answered
  `"ARNM_ERROR_UNKNOWN"`** -- the value had no entry in the message table. `test_result` now
  walks the whole range, so a value added without its string fails the suite.
- `arnm_duration_string()` compared a `size_t` buffer size against an `int` sum of `uint8_t`
  terms, across the sign boundary. The sum is widened before the comparison now; every term was
  small and non-negative, so no answer changes.
- `Doxyfile` carried absolute paths for `OUTPUT_DIRECTORY` and `INPUT`, so a checkout anywhere
  else documented nothing -- or, on the machine those paths pointed at, silently documented the
  other tree. Both are repository relative.

## 0.5.0 -- 2026-08-21

### Changed

- **The library is now `arnm`, and every public symbol was renamed with it.** `hostmem_` became
  `arnm_`, `HOSTMEM_` became `ARNM_`, and the allocator type `hostmem` is now `arnm` -- so the
  operations read as `arnm_init_arena`, `arnm_alloc`, `arnm_reset`. The rename is mechanical and
  complete: no function changed behaviour, no signature moved, no result code changed its value.
  A consumer updates by substituting the prefix in both cases.
- **Headers moved from `include/hostmem/` to `include/arnm/`.** An include reads
  `#include "arnm/memory.h"`. The directory stays flat, as before.
- **The build artefacts carry the new name.** The zig package is `.arnm` and the library it
  installs is `arnm`, so a dependent project writes
  `b.dependency("arnm", ...).artifact("arnm")`; the CMake target is `arnm`. The package
  fingerprint in `build.zig.zon` changed with the name, as zig derives it from one.
- **The test memory cap is read from `ARNM_TEST_MEMORY_LIMIT_MB`.** The old spelling is not
  consulted, so a shell that still exports it silently falls back to the 2048 MB default.
- The motto that heads the documentation is now **data lives in memory arenas**, which says the
  same thing from the arena's side rather than the host's: every byte the library hands out
  comes from an arena, and every arena comes from the caller. The memory contract behind it is
  unchanged.

### Fixed

- `arnm/memory.h` pointed at `utils/memory_block.h` in a group comment, a path left over from
  gradido-blockchain-core. It is `arnm/memory_block.h`.

## 0.4.1 -- 2026-08-19

### Fixed

- The wiping guarantee on `hostmem_binary_from_hex()` and `hostmem_uuid_from_string()` promised
  more than either function does. Both said the output is set to all zeros "when the string does
  not decode", which reads as covering every refusal -- but a length that is wrong is caught
  before anything is written, and on that path the buffer is left exactly as the caller had it.
  Only HOSTMEM_ERROR_DECODE_FAILED clears it, which is the path where the parser has already put
  something there or is about to. The documentation now says which path does which, and the tests
  pin both.

  The behaviour did not change and does not need to. A string of odd length is refused before
  anything is written, so clearing on that path would erase bytes the call never produced.
  Wiping a buffer that has served its purpose belongs to whoever owns it, which the group
  warning already says.

## 0.4.0 -- 2026-08-19

### Added

- `hostmem/converter.h` grew a second family beside the number conversions: raw bytes to
  lowercase hex and back, and a uuid to its canonical 8-4-4-4-12 form and back. All four come
  from gradido-blockchain-core, where they were the half of that project's converter that needed
  no crypto library; the constant time pair that does stayed behind with the project that links
  one.
  - `hostmem_binary_to_hex()`, `hostmem_binary_from_hex()` -- computed digits rather than a
    lookup, which lets the compiler vectorise both loops. Measured in a ReleaseFast build:
    4.5 ns for 16 bytes, 118 ns for 1024.
  - `hostmem_uuid_from_string()`, `hostmem_uuid_to_string()` -- table driven, because 16 bytes at
    fixed scattered positions are not a run a vectoriser can help with. 5.6 ns to write, 15.9 ns
    to read.
  - `HOSTMEM_UUID_BINARY_SIZE` (16) and `HOSTMEM_UUID_STRING_LENGTH` (36).
- `benchmarks/src/bench_binaryToString.c`, covering both directions of each pair: the hex
  conversions at 16, 32, 64 and 1024 bytes, the uuid conversions at the 16 bytes the format
  fixes them to. No baseline beside them: hostmem links no crypto library, so the constant time
  conversions such a library ships are not there to compare against.

### Notes

- None of the four runs in constant time, and the header says so in the group warning. Keys,
  seeds and passphrases belong in a crypto library's own conversion; hashes, transaction ids,
  public keys and uuids are what these are for.
- `hostmem/converter.h` now includes `hostmem/memory_block.h`, which the hex encoder takes its
  input through. A consumer that included only `converter.h` gets the block API along with it.

## 0.3.1 -- 2026-08-19

### Fixed

- The version in `build.zig.zon` had not followed the 0.3.0 release.

## 0.3.0 -- 2026-08-16

### Added

- `hostmem/multi_arena.h` -- a chain of arenas that opens another one instead of running dry,
  generalised from the meta area allocator the geosearch used.
- `hostmem/fixed_arena_pool.h` -- a fixed set of equal sized arenas, lent out and returned, with
  the peak known at init.
- `CMakeLists.txt` for the one case zig cannot serve: the MSVC ABI on Windows, whose SDK zig does
  not ship. It mirrors `build.zig` and never leads it.
- `compile_commands.json` is regenerated on every build rather than only on `zig build cdb`.

### Changed

- `hostmem_create()` and `hostmem_destroy()` take an allocator as an optional parameter, so a
  descriptor can come from a host's own memory rather than from malloc.
- Sources are ASCII only, enforced by `lint.sh`. A byte above 0x7F means what the compiler
  decides it means, and MSVC reads a file in the system codepage unless told otherwise.

## 0.2.0 -- 2026-08-15

### Changed

- The source tree was flattened: `src/*.c` and `include/hostmem/*.h` sit in one directory each,
  with no subfolders.

## 0.1.1 -- 2026-08-15

### Fixed

- Follow-up corrections to the invariants carried over from gradido-blockchain-core.

## 0.1.0 -- 2026-08-15

First release. The bump arena and the malloc path behind one allocator type, memory blocks,
bucket vector, the number conversions, duration and monotonic timer -- rewritten out of
gradido-blockchain-core so that a host owns every byte the library works in.

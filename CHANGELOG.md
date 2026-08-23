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

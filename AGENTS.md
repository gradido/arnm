# AGENTS.md - arnm

ARNM is a **C11 library designed to be linked into programs written in other languages**.

Its central idea comes from the allocator model used by Zig:

> **The caller chooses where memory comes from. ARNM provides the mechanism without hiding the decision.**

One sentence captures the memory model:

> **Data lives in memory arenas.**

The caller provides the memory or allocator. ARNM manages that memory according to the selected strategy, but never silently takes ownership of memory it was not given.

The same principle extends throughout the library:

* explicit ownership
* explicit lifetime
* explicit allocation strategy
* explicit sizes
* explicit transformations
* predictable performance
* meaningful warnings and errors
* low mental overhead when reading the code

ARNM abstracts **mechanisms, not decisions**.

---

## Core design principles

### Allocators are explicit

`arnm` is the common allocator handle. The caller can select the allocation strategy without changing code that consumes the allocator.

Do not introduce hidden allocators, global allocation state, thread-local allocation state, or implicit ownership.

`malloc` exists only in `src/memory.c`, on the NULL-allocator path. No other library source may call it.

### Ownership is explicit

ARNM never owns memory it was merely given.

This distinction is especially important for borrowed arena memory and fixed arena pools. A fixed pool, for example, owns the arenas that are currently free; once an arena is handed to the caller, the pool does not track or wrap that arena.

Do not add bookkeeping merely to make ownership implicit.

### Sizes are explicit

Sizes are `uint32_t`: counts, indices and byte sizes alike.

Anything that would overflow `uint32_t` must return `ARNM_ERROR_ARITHMETIC_OVERFLOW` rather than wrap. Where a bound is known at compile time, prefer `static_assert`.

Every allocation is rounded up to an 8-byte boundary.

ARNM does not store per-allocation size bookkeeping. The caller supplies the size again when freeing or resizing.

`arnm_memory_block` exists for cases where keeping pointer and allocated size together is useful:

```c
typedef struct arnm_memory_block {
    uint8_t *data;
    uint32_t size;
} arnm_memory_block;
```

The `size` is the **allocated size**, not the logical content length.

### Why the caller tracks the size

Arena memory can only be reclaimed from the tail.

To determine whether an allocation can be released, ARNM needs both:

* the pointer
* the original allocated size

The allocation is reclaimable when the end of that allocation is exactly the arena's current tail.

Conceptually:

```c
memory->data + memory->last_index - aligned_size == buffer
```

If the allocation is buried behind another allocation, the arena cannot reclaim it. The memory remains part of the arena until reset or another appropriate lifetime operation.

This is why replacing explicit size tracking with hidden allocation metadata would work against the fundamental memory model.

---

## Lifetime is part of the abstraction

ARNM provides several deliberately different lifetime models.

* **Arena:** fast bump allocation; reset keeps the memory, release returns it.
* **Multi-arena:** grows by adding arenas and can release empty trailing arenas.
* **Fixed arena pool:** reserves a known number of equal-sized arenas and lends them to callers.
* **Borrowed memory:** ARNM uses caller-owned memory without taking ownership.
* **Bucket/vector storage:** provides stable element storage according to its bucket model.

These are different abstractions because they express different lifetime and memory requirements.

Do not flatten them into a generic allocator abstraction merely to make their APIs look uniform.

For example, a fixed arena pool deliberately has a known memory ceiling and O(1) acquire/release operations. Its free list lives inside the free arenas themselves, requiring no separate allocation.

A multi-arena deliberately behaves differently: it grows when necessary and `shrink()` only releases empty arenas from the end.

---

## No unnecessary transformations

ARNM favors direct data flow.

If data can be produced directly into its final allocation, do so.

Avoid silently introducing:

* copies
* intermediate buffers
* escaping
* validation
* normalization
* ownership transfers
* unbounded growth

unless the operation explicitly promises that behavior.

The JSON writer is an important example: it renders directly into memory obtained from the supplied allocator, grows there as necessary, and returns that same allocation after shrinking it to the required size. The JSON text is not rendered into an intermediate buffer first.

### JSON writer input semantics

JSON string escaping is a transformation and must therefore be explicit.

The default string-writing semantics do not silently escape strings supplied by the caller.

Likewise, malformed UTF-8 is intentionally written unchecked rather than causing an implicit validation pass.

General rule:

> **Do not silently perform work the caller did not request.**

---

## Result semantics

`arnm_result` distinguishes successful completion, warnings and errors.

`ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED` means:

> The requested operation completed, but the arena could not return those bytes to the available tail.

It is neither ordinary success nor an error.

Handle it explicitly at the call site. Do not hide it behind a generic `ok()` helper because its meaning depends on the operation and caller.

Failures leave outputs untouched.

`ARNM_ERROR_USER_BASE` reserves codes above 1000 for the embedding application; ARNM owns the codes below it.

---

## Performance

Performance is part of ARNM's architecture, not merely an implementation detail.

Consider:

* allocation count
* memory movement
* copying
* cache behavior
* branches
* scans
* function-call overhead
* bounded versus growing memory
* compiler optimization

Every function call can have a cost depending on optimization and compiler decisions. Do not assume that a small helper is free merely because it looks like it should inline.

Performance-sensitive paths may therefore use specialized implementations or intentionally duplicate a small amount of code.

Measure performance claims in an optimized build. Benchmarks should use `-Doptimize=ReleaseFast`; debug numbers answer a different question.

---

## Mental overhead

Machine performance is not the only cost.

**Mental overhead while reading the code matters too.**

Prefer code whose actual behavior is immediately visible.

Do not extract a simple one-liner into a separate function merely to remove a line of code if the original expression makes the operation clearer than the resulting function name.

Do not introduce abstractions whose indirection makes data flow, ownership, lifetime or control flow harder to understand.

A small amount of local duplication can be preferable to an abstraction that increases cognitive load.

The goal is:

> **Low machine cost and low mental cost.**

---

## Internal functions

Functions declared `static` inside `.c` files are implementation details.

Their preconditions should reflect the actual internal contract.

Do not repeat defensive checks that the caller has already established.

For example, if an internal caller guarantees:

```c
buffer != NULL
aligned_size != 0
```

the callee does not need to check them again merely for defensive programming.

A check belongs in the internal function when the condition can genuinely occur there.

This keeps hot paths smaller, avoids unnecessary branches and makes the function's actual assumptions visible.

Public API boundaries have different requirements: public functions must validate their documented inputs.

---

## Abstraction and refactoring

Before adding an abstraction, first ask whether the existing ARNM mechanisms already express the required behavior.

Prefer:

1. existing ARNM abstraction
2. clear local code
3. new abstraction for a genuinely new concept

Do not create wrappers whose only purpose is renaming a simple operation.

When refactoring, evaluate both:

* runtime cost
* reader cost

A refactoring is not automatically an improvement because it reduces duplication or produces more layers.

Preserve code that is already clear, direct and efficient.

---

## Build and test

`build.zig` is the master build configuration. It defines targets, options and the verification matrix.

Typical development commands:

```bash
zig build -Dtests=true -Dbenchmarks=true
./run_all.sh
./run_all.sh --tests
./lint.sh
```

`-Dtarget=` is normally unnecessary because Zig resolves the host target itself. It can be useful when the system headers are incomplete; use a named target rather than modifying the build to compensate for a machine-specific environment.

`CMakeLists.txt` exists specifically for the MSVC ABI on Windows. It mirrors `build.zig` but does not lead it. When they disagree, fix CMake to match `build.zig`.

Supported build options include:

| Option                          | Meaning                  |
| ------------------------------- | ------------------------ |
| `-Dtarget=`                     | Cross compilation target |
| `-Dtests=true`                  | Build tests              |
| `-Dbenchmarks=true`             | Build benchmarks         |
| `-Dshared=true`                 | Build a dynamic library  |
| `-Dsanitize=undefined_behavior` | UBSan                    |
| `-Dsanitize=thread`             | TSan                     |

Tests must use the repository's memory limit mechanism. Boundary tests must not accidentally reserve unbounded host memory.

When fixing a bug, verify that the new test actually fails without the fix before considering the test sufficient.

---

## Portability

ARNM targets Linux (glibc and musl), Windows and macOS on supported architectures.

Never claim a target that has not actually been built.

### C portability

* Sources are ASCII only.
* `.c` files use C headers, never C++ headers.
* Every public header must compile independently as both C and C++.
* Every `.c` file includes its own header first.
* Platform-specific code carries the headers required by that branch.
* POSIX or legacy headers require appropriate platform guards.
* Compiler extensions should not be introduced when portable C can express the same operation.

The `static_assert` compatibility definition must not interfere with C++:

```c
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
```

The portability rules exist so that every toolchain exercises the same implementation wherever practical rather than maintaining an untested special path.

---

## Dependencies

`third_party/yyjson` is intentionally private to ARNM.

No public header exposes it. JSON memory management crosses the internal `src/json_memory.h` seam and routes allocations through ARNM. The vendored files remain unmodified and are kept at the recorded upstream version.

Do not turn implementation dependencies into public API dependencies without a strong architectural reason.

---

## Naming

Every public symbol starts with `arnm_`.

Every public macro starts with `ARNM_`.

The allocator type itself is `arnm`, giving APIs such as:

```c
arnm_init_arena(...)
arnm_alloc(...)
arnm_reset(...)
arnm_release(...)
```

### `unsafe_`, the one prefix that goes in front of the name

A call that deliberately skips the checks its ordinary twin performs carries `unsafe_` ahead of the whole arnm name:

```c
arnm_byte_buffer_copy(&log, record, length);          // checks, and answers
unsafe_arnm_byte_buffer_copy(&log, record, length);   // the caller already asked
```

yyjson spells its own pairs that way, and the reason it is worth the exception is where the word lands. The prefix is the first thing read at a call site, so the dangerous member of a pair cannot be taken for the ordinary one while skimming, and every unchecked call in a codebase is one grep. `arnm_byte_buffer_copy_unsafe()` would keep the rule and lose exactly that: the two names would then differ only at the end, after a reader has already decided what the line does.

The exception is narrow.

* It is for a checked/unchecked pair and nothing else. A call with no checked twin does not get the prefix; it gets the checks.
* The arnm name stays whole behind the prefix. `unsafe_arnm_byte_buffer_copy`, never `unsafe_byte_buffer_copy`.
* Both members take the same arguments in the same order and write the same bytes. The unchecked one answers nothing and asserts, where assertions are on, exactly what the checked one refuses -- see the pair in `arnm/byte_buffer.h`.
* Such a twin is a `static inline` in a header. It emits no external symbol -- even unoptimized it is a local `t`, never a global -- so nothing here reaches a linker, and the namespace the rule above protects is untouched.

The rule is about what a linker and a reader see of arnm as a whole. This exception is about what a reader sees at the one moment it matters most.

The only other name in the public headers without an `ARNM_` prefix is `static_assert`, the C11 fallback under Portability. It is spelled that way because it stands in for the keyword, and it is defined only where the keyword is missing.

Keep `include/arnm/` flat. A public header such as `arnm/bucket_vector.h` should not be hidden several directories deep.

---

## Doxygen modules

Every public header defines exactly one top-level Doxygen module using `@defgroup` and wraps its public API with `@{` and `@}`.

Example:

```c
/** @defgroup arnm_memory arnm_memory
 *  @brief Allocator that is either a bump arena or plain malloc/free
 *  @{
 */

/* API */

/** @} */
```

There are no parent groups and no `@ingroup` hierarchy. The public headers intentionally form one flat namespace.

---

## Commenting standard

ARNM comments use two aligned layers.

### Technical layer

This is the ground truth.

Document:

* parameters
* types
* constraints
* edge cases
* return behavior
* overflow and limits
* deterministic rules

The technical layer must be sufficient to implement the function without relying on the poetic layer.

### Semantic layer

The optional semantic layer describes the same behavior as a natural process: flow, growth, transition, settling, tide, stream, season, decay.

It must never alter or weaken the technical meaning.

### `@whisper`

`@whisper` is an optional poetic one-liner at the end of a Doxygen comment.

It is particularly suitable for functions with meaningful system behavior and less useful for trivial helpers.

Never delete an existing `@whisper` merely for stylistic preference. Rewrite it only when the function's behavior has changed enough that the old line is no longer accurate.

The technical specification always has priority over the poetic description.

---

## Repository workflow

Agents must not commit, tag or push.

Reading Git is encouraged:

```text
git status
git diff
git log
git show
```

Leave the working tree with the completed changes and report what was changed.

Keep diffs focused. Avoid drive-by formatting, unrelated renames and speculative refactoring.

Format files that are actually touched; a repository-wide formatting change belongs in its own change.

When something could not be verified, say so explicitly.

Never present an unbuilt platform, unmeasured performance claim or unexecuted test as verified.

---

## Final design rule

When in doubt, remember the ARNM model:

> **Give the caller control over the important decisions. Provide efficient mechanisms for implementing those decisions. Keep ownership, lifetime, memory cost, runtime cost and mental cost visible.**

Or, more compactly:

> **Explicit decisions. Explicit costs. Minimal hidden work.**

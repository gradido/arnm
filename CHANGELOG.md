# Changelog

Every release of arnm, newest first. A date is the day the version was set in `build.zig.zon`,
which is not always the day a tag followed: 0.3.1 and 0.4.0 carry no tag yet.

The library was called hostmem until 0.5.0, which renamed every symbol. Entries below that
version name the symbols as they were spelled at the time, so the record still matches the
tags -- read a `hostmem_` there as today's `arnm_`.

The version lives in `build.zig.zon`; `Doxyfile` carries it a second time for the generated
documentation. Until 1.0 the minor number moves when a release takes something away or changes
what a call already did -- when code built against the release before would go wrong rather than
simply stop compiling. The patch number carries everything else: fixes, API that is only added,
and a parameter appended to a setup call where passing nothing gives the old behaviour back.
While this library has a handful of consumers, a compile error that every call site answers with
`NULL` is not worth a minor; a call that quietly does something else always is. Either way, a
release that costs its callers an edit names it here rather than leaving it to be found at the
next build.

Entries before 0.4.0 were reconstructed from the git history after the fact, so they summarise
what the commits show rather than what was noted at the time.

## 0.7.5 -- 2026-08-30

The reader is a different reader. What 0.7.0 opened and 0.7.4 finished was a cursor: you entered
an object, asked for a member by name, read it, and left again, and where you were lived in the
reader rather than in your own code. That is gone. An object is now read by handing over a table
-- what a key is called, what it should become, where to put it -- and an array by handing over a
buffer of value handles. There is no third way in, because everything else is a member of one of
those two.

The point of the table is that the member chain is walked once. Each key is compared only against
the entries the walk has not filled yet, starting at the lowest of them, so a table written in the
document's own order is met at one comparison per member; once every entry is filled the rest of
the object is not looked at. A member the table does not name is not an error, and a member named
twice is read the first time and ignored after, which is the opposite of what most JSON readers do
and is a consequence of that shape rather than a preference.

**Every consumer of the old reader is rewritten rather than edited.** `arnm/json_reader.h` keeps
init, create, release and destroy, keeps the two parses with signatures that changed, and loses
the forty-odd calls that were the cursor and the value level. The compile error lands at every
call site and there is no mechanical answer to it: a loop that entered an object, asked for five
members and left becomes a table and one call. Code built against 0.7.4 stops compiling rather
than going quietly wrong, which is what this file's opening says the patch number carries -- but
it is by far the largest removal so far, and easier to meet knowing that in advance.

**One change does go quietly, and it is not in that header.** Both builds now set
`YYJSON_DISABLE_UTF8_VALIDATION`. A document whose strings hold malformed UTF-8 used to be refused
by the parse and is now carried through unexamined, and the writer used to refuse to write such a
string unless `ARNM_JSON_WRITE_ALLOW_INVALID_UNICODE` said otherwise and now always writes it.
Nothing about either is visible at a call site, and there is no switch to ask for the old
behaviour -- the flag is off at the source, not at run time. A caller who needs well formed text
has to check its own, on the way in or on the way out. It is named here because it is the one
thing in this release that a build will not tell you about -- and, by the rule at the top of this
file, the one thing in it that argues for the minor number rather than the patch: the removals
above all stop a build, this changes what two calls already did and stops nothing.

### Added

- **`arnm_json_read_object()`** in `arnm/json_reader.h`, with the table it is driven by:
  `ARNM_JSON_FIELD_BOOL()`, `_INT64()`, `_UINT64()`, `_INT32()`, `_UINT32()`, `_DOUBLE()`,
  `_STRING()`, `_HEX_FIXED()`, `_UUID()` and `_VALUE()`, plus `ARNM_JSON_FIELD_MAKE()` under
  them. `out_found` is a bit per entry, so a caller sees what the document actually carried and
  decides once, for the whole object, what was required.
  - **The table is type checked where the compiler can.** `ARNM_JSON_TARGET()` is a `_Generic`
    over the pointer, so an entry that says INT64 above an `int32_t *` matches no association and
    the translation unit does not compile. Without `_Generic` -- C99, C++, or a C11 without it --
    only the cast is left and the same mistake is a run time one instead. A `void *` beside a type
    tag is a contract the compiler otherwise cannot see, and it writes eight bytes into four.
  - **Nothing allocates and there is no allocator to hand over.** A string member is borrowed from
    the document, and HEX_FIXED and UUID decode into a buffer the caller already had, named
    through `ARNM_JSON_BLOCK_OF()`. Every target is either the caller's own storage or a view into
    a document that outlives the call.
  - **`ARNM_JSON_FIELDS_MAX` is 64** because the mask is a `uint64_t`, and a table longer than
    that is refused rather than truncated.
  - Measured in `bench_json`, nine mixed members -- a string, two 64 bit numbers, two 32 bit, a
    double, a bool, a 32 byte hex digest and a uuid -- read into a struct, the parse not counted:
    **79 ns** where the table is in the document's order, **78 ns** where 24 members the table does
    not name follow the nine, **213 ns** where those same 24 come first, and **98 ns** where the
    nine are written back to front. The spread is the walk meeting the table's order or not, and
    it is the one thing a caller can do something about.
- **`arnm_json_read_array()`**, the other half: value handles out, nothing converted, because an
  array's elements have no names to hang a type on. 64 objects come back in **77 ns**, and the
  caller reads each with a table of its own.
- **`arnm_binary_from_base64_insitu()`** in `arnm/converter.h` -- the base64 decode written over
  the string it was handed, three bytes going where four characters were, always behind the read
  that produced them. It is what a caller reaches for after
  `arnm_json_reader_parse_insitu()`, where the buffer the field is borrowed from is the caller's
  own; the copying call is still what a borrowed view into a reader's own pool needs.
  - **It saves the buffer and not the time.** Against the copying call it measures between 1.00x
    and 1.03x from 1 KiB to 1 MiB and 1.05x to 1.09x at 8 MiB, where the two buffers together are
    past any L3 this is likely to run on. The decode moves about 1.7 GB/s and the memory it stops
    touching answers an order of magnitude faster than that, so the second buffer was never what
    it waited on. `bench_binaryToString` carries the rows, a length sweep from 16 bytes to 8 MiB,
    and the reasoning.
- **`arnm_binary_from_hex_with_known_hex_size()`**, for a caller that already knows the length and
  should not pay a `strlen` to say so -- a hex field borrowed out of a document knows exactly how
  long it is.
- **`arnm_ctz()` and `arnm_ctzll()`** in `arnm/bitmap.h`, the lowest set bit spelled the same way
  on every toolchain. `arnm_json_read_object()` is the first caller: the entries still open are a
  mask, and the next one to compare against is that scan.
- **`bench_json` measures the writer as well as the reader**, over the same six documents: the
  building and the rendering apart, both with and without the pool hint, what each costs the arena
  where the clock cannot show it, and what `arnm_json_writer_size()` costs to ask. The hint is
  level against the clock and is worth 32 KiB of stranded chunks on the largest of them.

### Changed

- **`arnm_json_reader_init()` loses its flags parameter** and `arnm_json_reader_create()` with it.
  Every `ARNM_JSON_READ_*` switch it took is gone; see Removed.
- **Both parses hand back the root and take `stop_when_done` as an argument.** Each grew the same
  two parameters at the end -- a `bool` where the read flag used to be, and an
  `arnm_json_value **` the root is written to. There is no `arnm_json_reader_root()` to ask
  afterwards: the parse is where the root comes from, and a reader that no longer remembers where
  you are has nowhere to keep one.
- **`ARNM_JSON_READER_SIZE` falls from 256 to 72 bytes.** A reader holds an allocator, a document
  and the first refusal it saw; the cursor, the path and the output allocator it used to keep are
  no longer part of it.
- **yyjson is compiled with four of its features taken out** rather than defaulted off:
  `YYJSON_DISABLE_INCR_READER`, `YYJSON_DISABLE_UTILS`, `YYJSON_DISABLE_NON_STANDARD` and
  `YYJSON_DISABLE_UTF8_VALIDATION`, in `build.zig` and `CMakeLists.txt` alike. Comments, trailing
  commas, `Infinity`, `NaN` and a byte order mark are refused with no way to ask otherwise, which
  is why the read flags could go. The fourth is the silent one described above.
- **`ARNM_JSON_WRITE_ALLOW_INF_AND_NAN` and `ARNM_JSON_WRITE_ALLOW_INVALID_UNICODE` are gone**,
  and bits 4 and 6 are deliberately left empty so an old value is refused with
  `ARNM_ERROR_INVALID_PARAM` rather than landing on a neighbouring flag. Translating a bit into a
  serializer that no longer reads it would be a switch accepted here and ignored there.
  `ARNM_JSON_WRITE_INF_AND_NAN_AS_NULL` is unaffected: `null` is standard JSON and that path
  survives the build.
- **`arnm_binary_from_hex()` is now a `static inline` in the header** over the sized call.
  Source compatible and the same behaviour, but the symbol is no longer exported: a consumer that
  links `-Dshared=true` and does not recompile will not find it.

### Removed

- **The whole cursor level of the reader**: `arnm_json_reader_root()`, `_current()`, `_enter()`,
  `_enter_at()`, `_leave()`, `_has()`, `_count()`, `_type_of()`, `_clear_error()`,
  `_error_field()`, and the eight `arnm_json_reader_get_*()` calls. Where you are is now a
  variable in the caller's own code, which is where it is easiest to read and hardest to get
  quietly wrong.
- **The value level beside it**: `arnm_json_object_get()`, `_size()`, `_iter_init()`,
  `_iter_next()`, the four `arnm_json_array_*()` matching them, `arnm_json_value_type()`,
  `arnm_json_value_number_type()` and `arnm_json_type_to_string()`. A value is now something a
  table names or a handle an array read hands over.
- **The per value reads**: `arnm_json_read_bool()`, `_int32()`, `_int64()`, `_uint32()`,
  `_uint64()`, `_double()`, `_string()`, `_hex_fixed()` and `_uuid()` are field types in the table
  now, one entry each. `arnm_json_read_hex()` and `arnm_json_read_base64_block()` are not: a hex
  field of no fixed length and a base64 block have no entry, so a caller reads the string with
  `ARNM_JSON_FIELD_STRING()` and hands it to `arnm/converter.h` -- which is what
  `arnm_binary_from_base64_insitu()` above was added for. `arnm_base64_binary_size()` stays where
  0.7.4 put it, and is how the block is sized before it is decoded.
- **The output allocator and the sizes it went with**:
  `arnm_json_reader_set_output_allocator()`, `arnm_json_reader_output_size()` and
  `arnm_json_reader_output_size_for_keys()`. Nothing is copied out of a document any more --
  strings are borrowed and the two decoding field types write into the caller's own buffer -- so
  there is no output to size and no allocator to name for it.
- **Every read flag**: `ARNM_JSON_READ_DEFAULT`, `_ALLOW_COMMENTS`, `_ALLOW_TRAILING_COMMAS`,
  `_ALLOW_INF_AND_NAN`, `_ALLOW_INVALID_UNICODE`, `_ALLOW_BOM` and `_STOP_WHEN_DONE`, together
  with `ARNM_JSON_READER_FIELD_NAME_SIZE`. Six of them switched code the build no longer contains;
  the seventh is the `bool` the parses now take.

## 0.7.4 -- 2026-08-27

The reading side of what 0.7.3 gave the writer. A document has no type for bytes, so it spells
them -- hex, base64, the canonical 8-4-4-4-12 -- and until now every consumer wrote the same
three lines per field to read one back: borrow the string, check its length, convert it. The
length check is the part that is easy to leave out, and leaving it out is not visible: a field
reads as shorter than it is, or a string that merely starts like the right one passes.

`arnm_json_read_hex()`, `arnm_json_read_hex_fixed()`, `arnm_json_read_uuid()` and
`arnm_json_read_base64_block()` are those three lines, once. The fixed one is for a field whose
type says its length -- a key, a hash, a signature -- and refuses every other length before it
converts anything; the other reads whatever the document spells, into a buffer whose capacity it
respects. A string carrying a NUL of its own is refused by both: the converter underneath reads
to the terminator and would otherwise stop early and leave the rest of the field as the caller
had it. The base64 one takes its block from the allocator it is handed, at exactly the size the
string decodes to.

`arnm_base64_binary_size()` is that size on its own, for a caller that has to reserve before it
reads: `ARNM_BASE64_BINARY_SIZE()` answers what a length of characters can hold at most, this
answers what one particular string will write, which is up to two bytes less. An arena charged
for bytes it is never given back ends short of the read that follows it.

`arnm_json_read_null()` is gone. It never had anything to hand back -- the whole answer was in
the result code -- which made it a predicate wearing a read function's shape, and
`arnm_json_value_type()` was already that predicate. Every call site answers the compile error
with `ARNM_JSON_TYPE_NULL == arnm_json_value_type(value)`, which is the kind of removal this
file's opening says the patch number carries.

`arnm/json_reader.h` now includes `arnm/converter.h` and `arnm/memory_block.h`, which it needs
for the sizes and the block the four new calls speak in.

## 0.7.3 -- 2026-08-26

Four ways for a document to cost less. Three calls for the field types that cost the writer the
most -- binary as hex, binary as base64, and the uuid beside them -- all three formatted
straight into the document and written out verbatim. And a hint at init, so the document's two
pools open once at the size they will need instead of doubling their way there.

base64 arrives as a converter pair of its own as well, so a project that had to reach for a
crypto library to get it no longer does. And the uuid pair, which had been converting a byte at
a time through 784 bytes of its own tables, goes through the hex pair instead: writes a uuid in
a little over half the time and leaves that much L1 to whoever is calling.

**`arnm_json_writer_init()` takes a fourth parameter and `arnm_json_writer_create()` a third**,
both the hint, and both are the last one. Every existing call site answers with `NULL` and is
then exactly the writer it was: nothing that was there behaves differently and nothing was taken
away, which is why the patch number moves. Named here because it is still an edit at every call
site, and a build that stops is easier to meet knowing why.

### Added

- **`arnm_json_writer_add_hex()`** in `arnm/json_writer.h`, taking a pointer and a size and
  writing them as a lowercase hex string, two characters per byte. NULL or a size of 0 writes
  the empty string; past `(ARNM_MAX_ALLOC_SIZE - 3) / 2` the field is refused with
  `ARNM_ERROR_RESOURCE_SIZE_EXCEED`, which `arnm_json_writer_status()` answers with. The bytes
  are read where they are added and never again.
  - **It does not ask the serializer for six times its length.** Before writing a string,
    yyjson reserves `str_len * 6 + 16` -- room for the case where every byte escapes to
    `\uXXXX`. Hex escapes to nothing, and this is the one call that can say so, because it is
    what put the digits there: the text goes in already quoted, as a raw value, which is
    reserved for at `str_len + 2`. On a document whose longest field is a 1 KiB blob that is
    about 2 KiB of working buffer instead of about 12 KiB, and the working buffer is what
    decides the peak of the whole write.
  - **No scratch buffer, and no copy out of one.** The characters are formatted directly into
    the document's string pool. A caller doing this through
    `arnm_json_writer_add_string_copy()` had to allocate `2 * size + 1` somewhere, hex into it,
    and hand it back -- and against an arena that scratch never came back, because the
    document's own copy of it was allocated on top. Both the scratch and the copy are gone.
  - `arnm_json_writer_size()` stays exact over it: hex holds no character any write flag
    escapes, so the length is counted rather than bounded.

- **`arnm_binary_to_base64()` and `arnm_binary_from_base64()`** in `arnm/converter.h`, with
  `ARNM_BASE64_STRING_LENGTH()` and `ARNM_BASE64_BINARY_SIZE()` beside them. The standard
  alphabet with padding -- what `atob()` reads -- and nothing else: whitespace, a newline and
  the URL safe `-_` are refused rather than skipped, and padding anywhere but in the last group
  is refused too. The length macro is exact, which is what lets a writer count a field before
  it exists.
  - **Not constant time, deliberately.** Both halves branch on the data. libsodium's pair does
    not -- it computes every character from branchless masks either way -- and `bench_base64` in
    gradido-blockchain-core puts the two side by side: same text out of both, arnm's around
    eight times faster. That is the price of the property, and payloads that are already
    encrypted or already public do not need it. The group warning in `converter.h` says so, and
    says what does need it.
  - **Both directions read a lookup table, and that is a measured choice.** Computing the
    characters instead is the obvious idea -- the five runs of the alphabet are not contiguous,
    so it is four compares walking an offset, and a compare chain is something a vectoriser can
    turn into blends where a table load stops it. The argument is sound and the measurement
    does not follow it. Swapping only the character mapping, same loop, same build:
    - Encoder, CMake Release, one buffer over and over: 33 ns against 61 at 64 bytes, 241
      against 479 at 512, 1.9 us against 5.4 at 4096 -- **twice slower and worse as it grows**.
      In zig ReleaseFast over payloads past the size of L1 the two came out level, 694 ns
      against 684 at 1024 bytes, because there the loop is waiting on the payload either way.
    - Decoder: 1.8x slower for the plain arithmetic, 2.2x fully branchless. A compact table over
      `'+'..'z'`, 80 bytes instead of 256, cost 1.5x for the one bounds check per character it
      adds.
    - The tables are 64 and 256 bytes and stay in L1 across any loop that converts more than one
      block, which is why taking them out buys a caller less than it looks like it should. Both
      figures and the reasoning are written at the tables in `converter.c` so the experiment is
      not run a third time.
    - What did survive is the loop shape: the encoder walks a group index rather than two
      induction variables at 3 and at 4. It costs the table nothing and it is the form the
      arithmetic needed to be within reach at all -- with induction variables that one measured
      1.7 us where the group indexed form measured 0.68.
  - `bench_binaryToString` gains both base64 directions beside the hex rows it already had, at
    every length it already measured. They do not read the way the character counts suggest:
    base64 writes a third fewer characters and takes several times as long for them. hex maps
    one byte to two characters with no carry between them and the compiler vectorises it
    tightly; base64 has to shuffle bits across a three byte group, and that regrouping is what
    an auto vectoriser handles badly.
- **`arnm_json_writer_add_base64()`**, the same field written through the writer: encoded
  straight into the document, quoted, and written verbatim, exactly as add_hex() does it. Four
  characters per three bytes rather than two per one, so a payload field costs a third less
  than its hex.

- **A pool hint on `arnm_json_writer_init()`**, as `arnm_json_writer_hint` -- how many values
  the document will hold, keys counted, and how many bytes its copied strings will take. NULL
  says nothing and leaves the growth as it was.
  - The document is built in two pools that start at 384 and 256 bytes and double when they run
    out, keeping every chunk they ever opened. A document that needs four chunks pays for all
    four. On the largest transaction of a real ledger that is 2688 bytes of value pool where
    1968 were wanted, and 3840 of string pool where 2590 were: **1968 bytes of the 6712 the
    document occupied were chunks it had outgrown.** With the hint both pools open once.
  - Neither figure has to be right. Too low costs one extra chunk, which is where the growth
    would have started anyway; too high reserves room nothing uses. A hint sized for the largest
    document is paid by every small one, so a writer that builds documents of one shape can
    afford to be exact and one that does not should say nothing.
  - **A hint larger than the allocator behind it could ever serve is dropped, not attempted.**
    yyjson takes a chunk size in a `size_t` and only checks it against `SIZE_MAX`, so on a 64 bit
    host it accepts a figure no arnm allocator can hand out and then fails on the first
    allocation -- which would turn a hint into a write that does not happen. Both figures are
    bounded against `JSON_BLOCK_MAX_PAYLOAD` before they are passed on.
  - `ARNM_JSON_WRITER_SIZE` grows from 320 to 328 bytes for the two figures the writer now
    keeps. The static_assert in `json_writer.c` is what holds the two in step.

- **`arnm_json_writer_add_uuid()`**, the same for the one binary field that is not hex: 16 bytes
  in the canonical 8-4-4-4-12 form, formatted into the document and written verbatim, reserved
  for at 38 bytes rather than at `36 * 6 + 16`. No buffer to hold one uuid in on the way past.
  - **NULL is the literal `null`, not the empty string.** A uuid is sixteen bytes or it is not
    there; unlike `add_hex()`, which takes a size and can therefore tell a block of no bytes --
    written as `""` -- from an argument that is missing.
  - Whether it moves a peak depends on the document. Where a long hex field is present, that
    field decides the buffer and this changes nothing; where the uuids are among the longest
    strings, it is the same saving as above at a smaller scale.

### Changed

- **`arnm_uuid_to_string()` and `arnm_uuid_from_string()` go through the hex pair** instead of
  converting a byte at a time through tables of their own. Same output, same result codes, same
  refusals -- including the separator check by position that 0.4.0 fixed the heap overflow with.
  - **Writing a uuid is a little under twice as fast**: 6.0 ns to 3.7 in zig ReleaseFast, 6.8 to
    3.7 under CMake Release, both builds agreeing. Reading one comes out level either way.
  - **784 bytes of lookup table are gone** -- 512 to write a byte pair, 256 to read one, 16 for
    the scattered positions -- and with them about thirty lines that did what the hex pair
    already does.
  - The tables were there on the reasoning that a run broken by four dashes is not a run a
    vectoriser can help with. The dashes were the wrong thing to look at: the sixteen bytes are
    contiguous and only their text is not, so hexing them whole and placing five stretches of it
    costs less than converting them one at a time. `converter.c` says so where the tables were.

## 0.7.2 -- 2026-08-24

yyjson stops being a git submodule and becomes two files in this tree. Nothing an `arnm_` call
does changes; what changes is whether a project that depends on arnm gets a library that
compiles.

`zig fetch` takes the repository tree and nothing under it. A submodule arrives as an empty
directory, so a project depending on arnm through `build.zig.zon` got the JSON half as a missing
header three layers down -- and `git submodule update --init --recursive` is not something a
dependency can ask of the projects that use it. Fetching arnm now fetches all of arnm.

### Changed

- **`third_party/yyjson` is a copy, not a submodule.** `src/yyjson.c`, `src/yyjson.h` and the
  MIT `LICENSE` are in the tree unmodified, at the same release the submodule was pinned to
  (`0.12.0`, upstream commit `8b4a38dc994a110abaec8a400615567bd996105f`). Nothing else came
  with them: upstream's build files, tests, fuzzers and packaging are not here, because the one
  source is compiled straight into `libarnm` and yyjson is never built as a project of its own.
  - `.gitmodules` is gone, and with it `git submodule update --init --recursive` -- a clone is
    a clone again.
  - Both builds lost the check that named the missing submodule. There is nothing left for it
    to catch, and a file that is simply absent from the tree is a different problem with a
    different fix.
  - Neither build changed otherwise: the same source, the same private include path, the same
    `-Wconversion` exemption. `include/arnm/json_reader.h` and `include/arnm/json_writer.h` are
    plain C11 and still name nothing of what is underneath.
- **`third_party/yyjson/README.md`** records where the copy came from, which files were taken,
  and the three commands that move it to a newer release -- the pin a submodule used to hold on
  its own.

### Notes

- The tree carries about 700 KB it did not before. That is the price of a fetch that works, and
  it is paid once per clone rather than by every consumer's build configuration.
- A version bump is now a copy that nothing verifies for you. The commit recorded in that README
  is what makes an update a diff against a known point rather than a guess, which is why the two
  files are kept byte-identical to the tag -- everything arnm needs on top of them lives in
  `src/json_memory.h`.
- Verified by extracting the tracked tree into a clean directory with no `.git` and building a
  small consumer project against it through `b.dependency("arnm", ...)`: the library builds and
  the JSON reader parses. The same tree with `third_party/yyjson` emptied -- which is what a
  submodule looked like to a consumer -- fails to compile, which is the bug this release closes.
- The MSVC ABI route through `CMakeLists.txt` was not built here; the change to it is the
  removal of an existence check and no source or flag moved.

## 0.7.1 -- 2026-08-24

An arena could always say how much it had already refused. Now it can also be asked how much is
still there, before anything is refused at all.

Additive only: nothing that was there behaves differently, so the patch number moves. One
function was added and no existing one changed, so code built against 0.7.0 links and behaves
the same.

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

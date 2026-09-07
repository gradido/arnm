# yyjson -- vendored

The parser behind `arnm/json_reader.h` and the renderer behind `arnm/json_writer.h`, copied
into this tree rather than pulled in as a git submodule.

| | |
|---|---|
| Upstream | https://github.com/ibireme/yyjson |
| Version | 0.12.0 |
| Commit | `8b4a38dc994a110abaec8a400615567bd996105f` (tag `0.12.0`) |
| License | MIT, `LICENSE` beside this file |

```
sha256  ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a  src/yyjson.c
sha256  175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3  src/yyjson.h
```

## Why a copy and not a submodule

`zig fetch` takes the repository tree and nothing under it: a submodule arrives as an empty
directory, and a project that depends on arnm through `build.zig.zon` gets a library whose JSON
half will not compile, with an error three layers down that says nothing about the cause.
Cloning with `--recursive` is not something a dependency can ask of its consumers. The files are
here so that fetching arnm fetches all of arnm.

## What was copied

Only what is compiled: `src/yyjson.c`, `src/yyjson.h`, and the `LICENSE` that has to travel with
them. Upstream's build files, tests, fuzzers, documentation and packaging are not here -- arnm
compiles the one source straight into `libarnm` and never builds yyjson as a project of its own.

**Nothing in these two files is modified.** They are upstream's bytes, so the diff against a
fresh checkout of the tag is empty and an update is a copy rather than a merge. What arnm needs
on top of them is on arnm's side of the line: `src/json_memory.h` is the only place `yyjson.h` is
included, and it is where the allocator seam sits.

## Updating

```bash
git clone --depth 1 --branch <tag> https://github.com/ibireme/yyjson.git /tmp/yyjson
cp /tmp/yyjson/src/yyjson.c /tmp/yyjson/src/yyjson.h third_party/yyjson/src/
cp /tmp/yyjson/LICENSE third_party/yyjson/
sha256sum third_party/yyjson/src/yyjson.c third_party/yyjson/src/yyjson.h
```

Then update the table and the hashes above, and re-read what the memory contract rests on: every yyjson entry
point arnm calls is handed an allocator that forwards to `arnm_alloc`/`arnm_realloc`/
`arnm_free`, so yyjson's own default allocator is on no path this library takes. A new version
may add an entry point that takes an allocator arnm does not yet pass one to. Run the tests --
`test_json_reader` and `test_json_writer` -- and check `src/json_memory.h` against upstream's
changelog before taking the update.

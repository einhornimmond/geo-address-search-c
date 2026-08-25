# CRoaring

Roaring bitmaps — one posting list per word, and the intersection a query walks.
[github.com/RoaringBitmap/CRoaring](https://github.com/RoaringBitmap/CRoaring), dual licensed
Apache-2.0 / MIT; the `LICENSE` beside this file is the copy that came with the source.

## What is here, and what is not

Three files, and nothing else CRoaring ships:

| file | what it is |
| --- | --- |
| `roaring.c` | the amalgamation of every source file, as `amalgamation.sh` writes it |
| `roaring.h` | the amalgamation of every header, its counterpart |
| `LICENSE` | unmodified |

Taken at **4.7.2**, upstream commit `2e8395f1dbf286d7944a7276195a7c40cbcbfd4a`, amalgamated
on 2026-07-27. Both files are byte-identical to what that checkout produced, and they are
never edited here — a bug in a vendored library is worked around in our own code, as
`AGENTS.md` says.

Upstream's build files, tests, benchmarks, fuzzers, the C++ wrapper and the split headers
under `include/` are all absent. The one source is compiled straight into every artifact that
needs it, and the one header is what `search/geo_index.h` includes; nothing here is ever
built as a project of its own.

This used to be a git submodule of the whole repository — 298 MB, of which the build read
two files. `zig fetch` takes a repository tree and nothing under it, so a submodule reaches a
consumer as an empty directory; and a clone that needs `git submodule update --init` is a
clone that fails quietly for anyone who forgets. Two files in the tree are 1.2 MB and always
there.

## Moving to a newer release

```sh
git clone --depth 1 --branch v<x.y.z> https://github.com/RoaringBitmap/CRoaring.git /tmp/croaring
cd /tmp/croaring && ./amalgamation.sh
cp roaring.c roaring.h LICENSE <this directory>/
```

Then record the version and the commit in the table above — that record is what makes the
next update a diff against a known point rather than a guess. `amalgamation.sh` also writes
`roaring.hh` and `amalgamation_demo.*`; none of them are wanted here.

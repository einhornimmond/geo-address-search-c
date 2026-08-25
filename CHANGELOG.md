# Changelog

Every release of geo-address-search-c, newest first. A date is the day the tag was set.

The version lives in `build.zig.zon`; `Doxyfile` carries it a second time for the generated
documentation. The number moves by what a release asks of the people using it: the minor
number for new behaviour and for anything a build has to be told differently, the patch
number for fixes that ask nothing.

`client.h` and the two headers beside it are the surface this promises anything about. What
changes underneath — the collectors, the parser, the build files — is named here because a
build is affected by it, not because a program that links the library is.

Entries before 1.2.0 were reconstructed from the git history after the fact, so they
summarise what the commits show rather than what was noted at the time.

## 1.2.0 -- 2026-08-25

The JSON half of the program moved onto [arnm](https://github.com/gradido/arnm), which now
carries the parser as well as the allocator, and yyjson left the tree with it. A coordinate
the dump wrote without a fraction stopped being read as zero. And the parser thread count
became a figure a planet build has to choose rather than one it may leave alone — the
release says so before the second pass rather than halfway through it.

An index built by 1.1.0 is still read by 1.2.0: the file format stays at version 9 and
`client.h` is untouched. What changed is what a *build* needs, and one thing about what it
produces — see the centroid fix below.

### Changed

- **hostmem 0.4.0 is arnm 0.7.2.** The dependency is
  [`gradido/arnm`](https://github.com/gradido/arnm) now, and every symbol travelled with the
  rename: `hostmem_` became `arnm_`, `HOSTMEM_` became `ARNM_`, and the headers moved from
  `hostmem/` to `arnm/`. The mechanical half of that changes no behaviour. The rest is
  below.
- **A bucket vector is one type again, and its shape is an argument.**
  `HOSTMEM_BVEC_DECLARE`/`_DEFINE(name, type, log2, scope)` is `ARNM_BVEC_DEFINE(name, type)`;
  every vector is an `arnm_bvec` whatever it holds, and the bucket exponent moved to
  `_init()`. The generated wrappers are `static inline` and live in the header, so the
  declaration/definition split the collectors used to carry is gone.
- **A bucket vector reaches 268 173 312 elements and not one more.** arnm counts its buckets
  in a `uint16`, where hostmem counted them in a `uint32` — so what used to be a memory
  decision is now a capacity one. Every per-thread vector of the collectors sits at the
  largest exponent arnm takes (15), which is where that number comes from, and
  `GEO_VEC_CEILING` in `doc_collector.h` names it in one place.
  - Since a collector belongs to one parser thread, the ceiling is per thread and the thread
    count is what divides the work. The 2026 planet dump with 32 languages counts 1.87 G word
    occurrences and therefore needs **at least 8 parser threads**; four or six are refused.
  - `house_vec` holds the planet's 248 552 976 house numbers on a single thread with 7 % to
    spare. `name_vec` holds 4 190 208 names under one two-byte prefix per thread.
- **The build refuses a thread count it could not carry, right after the first pass.** The
  first pass counts every word occurrence, which is an upper bound on the postings the second
  pass produces, so the question can be settled where the figure appears rather than seven
  minutes later. The refusal names the count, the ceiling and the number of threads to use.
  An eighth of the ceiling is left unclaimed for the unevenness of batch work.
- **A collector that runs out of buckets says which vector did.** `doc_collector_limit()` and
  `house_collector_limit()` answer with the vector's name, what it held and what it holds;
  the build prints that instead of the bare `ARNM_ERROR_ARITHMETIC_OVERFLOW` it used to.
- **The parser reads through `arnm/json_reader.h` and writes through `arnm/json_writer.h`.**
  yyjson is behind arnm now and named nowhere in this tree.
  - `third_party/yyjson` is gone as a submodule, as an include path and as a compiled source.
    A clone is one submodule lighter and the `core` library some twenty-five thousand lines
    smaller.
  - The keys of an entry are walked once and dispatched in a switch, where nine separate
    lookups used to ask the object by name one at a time. The `name` object and the `address`
    block are walked the same way. In the address block the key decides before the value is
    read, so a house entry — two of every three in a planet dump — lets a role-free key go
    without touching its string.
  - Each thread parses into an arnm arena that grows to the longest line seen, where a
    thread-local `malloc` pool used to serve.
  - Measured over 150 000 lines of a planet-shaped corpus: 715 ns per line before, 646 after,
    identical output in all three language settings. The first pass of a real build runs
    2–3 % faster.
- **Every release build links with LTO; the debug build does not.** arnm's JSON reader is a
  thin layer — one call per key, one per value — and across a library boundary those calls
  stay calls. With LTO they are inlined and the layer costs nothing: the same parser is 27 %
  slower without it than with. `zig build` on its own keeps the plain path, where a slower
  link buys nothing.

### Fixed

- **A coordinate written without a fraction was read as zero.** JSON draws no line between
  `13` and `13.0`, and a serializer is free to drop a fraction that has none — but the parser
  took the fractional form alone, so every entry sitting on a whole degree was silently moved
  to the equator or the prime meridian. `has_point` was set all the same, so nothing about it
  looked wrong. Both forms are read as the same number now; only a value that is no number at
  all still answers 0.

  Whether a dump is affected decides whether an index has to be rebuilt. Nothing else about
  this release changes what a build produces.
- Three vectors of the document collector — documents, readings and word offsets — were
  sized against the merged document count when what they hold is segments: 64.1 M of them on
  the planet against a ceiling of 67.1 M, which one thread would have overrun and two would
  barely have cleared. All four sit at the ceiling now.

### Notes

- The bindings under `bindings/` are unchanged and keep their own version numbers.
- Verified by `./test_all.sh --clean`: 328 tests, every one of the four optimisation modes,
  from a cold cache. The parser was additionally checked against the implementation it
  replaces by folding every field of every entry into one fingerprint over a 74 MB corpus —
  172 528 entries, identical with two languages, with every language the dump offers, and
  with none named.

## 1.1.0 -- 2026-08-21

An index keeps the readings a build names rather than German alone, and the build stopped
walking the dump three times for the same bytes.

The file format moved from 6 to 9, so an index built by 1.0 is not read by 1.1.0 — version 8
widened the folding, version 9 gave the header two counters and the file two sections.

### Added

- **`--languages=de,en,fr` on a build, `--language=en` on a search.** The first tag is the
  default: it fills the document records and is what an answer shows unless a search asks
  otherwise. Every further one enters both the display dictionary and the search words, so a
  street in Prague is found by the name it is shown by. Without the option the index is
  exactly the index this program always built.
- **`--cache=<dir>`, a place cache.** The first pass writes the entries it needs as a binary
  file and the two behind it read that instead of unpacking the dump twice more. It needs
  twice the dump's size free and is passed over when it is not.
- A search may be narrowed to the places around a coordinate, which then orders what is left
  by distance after everything the query named outright.

### Changed

- The allocator, the bucket vector, the number conversion and the timer come from hostmem
  rather than from this tree.
- Folding widened to the whole Latin script: `é → e`, `ø → o`, `Č → c`.
- `src/` grew the folders the modules live in — `foundation`, `parser`, `search`, `types` —
  and every include names its path from `src/` downwards.

## 1.0 -- 2026-07-28

The first tag: a Photon dump in, a memory-mappable index out, and an address query answered
from it in well under a millisecond. File format version 6.

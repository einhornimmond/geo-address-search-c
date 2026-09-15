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

1.2.0 is tagged but cannot be fetched as a Zig package; 1.2.1 is the first that can, and
1.2.2 the first that builds once fetched.

## Unreleased

### Added

- **`zig build eval` and `tests/eval/`: a fixed set of queries to measure the search
  against.** 589 queries drawn from the German dump with a fixed seed plus 21 regression
  queries kept by hand, each asked with a map position and the last word read as a beginning,
  as production asks. `geo_eval` reports how often the expected place comes first, among the
  first three and among the first ten, per category, prints what went wrong with
  `--failures`, and writes per-query ranks for diffing two runs with `--ranks`. Baseline
  numbers are in the Readme, under *Measuring search quality*.
- **`tests/eval/external/`: geocoder-tester's suites and Nominatim's search features, asked
  of the index.** `fetch.sh` downloads both at pinned commits — nothing of them is committed —
  and two converters turn them into query files: 12 732 queries from geocoder-tester, 21 from
  the 191 scenarios of Nominatim's BDD search features, with every test they pass over counted
  by reason. `geo_eval` reads query files by their header now, so a file carries only the
  columns it needs, and learned what those suites expect: street, town and postcode besides
  name and number, alternatives separated by `|`, a coordinate only where one is given, a
  language, a per-test limit reported as the *pass* rate, loose comparison of texts, and
  `--absent` to list the places the index does not hold.

### Fixed

- **A house number is found the way people write it.** A letter written apart from its
  number was asked as a word of its own, and since no place carries the word *a*,
  `Osterstr. 42 A Hannover` and `Lister Meile 29 D Hannover` found nothing at all. A number
  was also compared byte for byte, so `1A` never met the door the dump writes `1 A`, and
  `Lister Meile 29D`, a door that was never mapped, answered with the bare street. Ranges and
  the houses behind a house — `Anderter Straße 1-3`, `Hauptstraße 12/1`, about 1 % of the
  German addresses — were cut into two numbers of which neither was a door, and whoever
  typed one number of a range found only the street.
  - **A single letter, or *bis*, *ter* or *quater*, right behind a number is its suffix**
    and is held back with the number instead of narrowing the search. Where every number
    is finally asked as a word, the letter is asked as one too.
  - **Two numbers with one dash or slash between them are one house number:** `1-3`,
    `1 - 3` and `1/3` are compared alike, while a space alone joins nothing, so
    `Hauptstraße 5 53111` stays a door and a postal code. `TextToken` carries a `joint` for
    it, what stood in front of the word, and `text_tokenize()` fills it in.
  - **A plain number finds the range written with a dash that holds it,** on its side of the
    street: `Anderter Straße 3` finds `1-3`, `Kirchweg 24` does not find `23-25`. It ranks like
    the door itself. A slash makes no range — `2/4` beside `2/3` and `2/12` on one street is
    a house behind the 2.
  - **Case and spaces inside a written number do not count:** `42A`, `42a` and `42 A` are
    one door, `12bis` and `12 bis` another.
  - **A suffix no door carries falls back to the plain number** in front of it, and such a
    door ranks behind one that carries the number as it was asked for: `36B Avenue du
    Général Leclerc` puts the 36B in Le Bouscat before the 36 in Bordeaux.
  - **Measured on the planet** over the 13 377 queries of the regression file, the drawn
    queries and the external suites: no query ranks worse. geocoder-tester's German suite
    passes 80.0 % instead of 78.2 %; 123 addresses such as `31BIS Avenue Victor Hugo` or
    `Anderter Straße 1-3`, counted as absent before because the search could not name their
    door, come first, and 46 more rank higher. Overall 73.6 % pass instead of 72.8 %. On 200
    ranges drawn from the German dump, 188 come first as written instead of none, and 183
    instead of 28 when only their first number is typed; on 200 numbers with a slash, 176
    instead of none. No format change and no rebuild.

- **An address followed by its country is found.** The dump names the country of an entry
  by its code alone, never in the address, so no place carried the word *Deutschland*.
  Every word of a query has to meet, and that one met no street and no town:
  `Marienplatz München Deutschland`, `Domstraße 3, 97070 Würzburg, Deutschland` and
  `Berlin Germany` found nothing at all, and `Berlin Deutschland` answered with *CEMEX
  Deutschland AG* in Bernau — the kind of text pasted from a letterhead.
  - **The build writes the country as a word nobody types:** `#de` on every document of
    an entry with that code, and `#*` beside it on the country itself, the way a position
    is written as a cell word. On the German dump that is 1.6 M postings and 336 bytes of
    file; the vocabulary pass counts the word only where a thread meets a new country, so
    the thread check a planet build passes at eight threads is not moved.
  - **The search recognises a country named in full** — a country document answers to
    the word, and one of its spellings, default or in a language of the index, stands in
    the query whole — and narrows through the country word instead of through the name.
    Named alone, the country is found as the place it is. The words are asked plainly
    first, and a place found that way whose own name or town holds the country's word
    among others typed in full keeps it a plain word: `28 Rue de Madagascar`, `West Jordan`
    and `11 Avenue Albert 1er de Belgique Grenoble` had turned to Antananarivo, Amman and
    Brussels without that. Where the country leaves nothing standing, the plain answer
    stands: `Atlanta Georgia` names the state.
  - **Measured on the planet** against the external suites and the regression file (13 372
    queries): the eight queries naming a country pass where one did, `6 Silum,
    Liechtenstein` and `london united kingdom` come first, and no query without a
    country name changes rank through the search.
  - **An index built before this has no country words** and answers as it always did;
    rebuild it to find what is written with its country.

- **The same dump builds the same index, whatever the thread count.** Two builds of the
  German dump with the same binary and four threads came out different — 1 605 497 and
  1 605 499 documents, 21 986 296 and 21 986 325 postings — while two builds with one thread
  were identical to the byte ([#11](https://github.com/einhornimmond/geo-address-search-c/issues/11)).
  The parser threads take batches off one queue as they come free, and four steps of the
  build decided ties by the order the threads' work was joined in:
  - **Joining segments into documents.** Records alike in name, town, postcode and kind were
    ordered by where they landed in the flattened array, and the join is greedy — the first
    founds a cluster, the next is measured against its moving centre. Every batch now carries
    its position in the dump (`ParseBatch::sequence`), each thread notes where its batches
    begin, and such records are joined in dump order. A build with ten threads lines them up
    as a build with one does.
  - **Localized readings.** Where segments of one place disagree about its name in a
    language, the first one gathered won, and `qsort` keeps no order among equals. The
    spelling that sorts first wins now.
  - **Doors with the same number on one street.** They were ordered by the number alone and a
    search takes the first, so which position it answered with was up to the threads. They
    are ordered by where they stand now.
  - **The term count in the file header.** It came from the first pass, whose repetition
    filter lets through a different set of texts depending on which thread met them. It is
    taken from the second pass now, which folds every text. The first pass's count still
    decides whether a build has threads enough, as before.

- **A city named from afar is no longer hidden by a street named after it.** With a
  position, the candidates are narrowed to the searcher's surroundings before the ranking
  sees them, and the position is let go of only when nothing nearby answers. Something
  nearby nearly always carries a city's name: `Würzburg` typed in Berlin met the
  Würzburger Straße there, `Wien` typed in Munich the Willi-Wien-Straße, `Hannover` typed
  in Berlin the Hannoverstraße — and the city itself never became a candidate, so the rule
  that a named town outranks nearness had nothing to rank. Read as a beginning, as a map's
  search box reads every query, it hit almost every city: `München` from Berlin answered
  with Münchener Straße, `Paris` with Pariser Platz, `Berlin` from Munich with the
  Leopoldstraße. Where the ring holds, the reading that answered is now asked once more
  without it, and the places found there that the query names by town or postcode join
  the ranking, at most 16 of them.
  - **Only places of weight are taken in: 40000 of 65535 or more.** A name alone would
    also lift every village called after a common word, because such a village carries
    the word as its own name just as a city does — `Bahnhof` in Berlin would answer with
    Gmünd-Bahnhof in Bohemia, `Mitte` with the Burkinabé region Mitte-Ost. The threshold
    was measured on the planet index from Berlin, Munich, Cologne and Vienna: Frankfurt
    (Oder) at 40527 is the lightest place that has to come up, Charlottenburg at 39243 the
    heaviest that must not come up (for `Berlin` asked from Munich). A small town typed
    from afar can therefore still stand behind a nearby street carrying its name.
  - **Cost:** a query whose ring held pays one more reading. Warm on the planet index the
    median grows by 0 to 0.5 ms — least for rare words and house numbers, most for
    `Berlin` asked from Munich (0.54 → 1.02 ms), whose word stands on hundreds of
    thousands of places.
  - The sample is now always the query's own and copied out at the end, including for
    limits above 64, where it used to be the caller's array.

- **A city filed as a county answers to its own name first.** The ranking asks whether the
  query names a place's town, and read that only from the town field. `Würzburg` is a
  kreisfreie Stadt, which the dump files as `county` with no town at all, so it scored
  nothing — while `Neubrunn bei Würzburg`, `Hausen bei Würzburg` and every street of the city
  scored through theirs and stood before it, whatever its weight. An area — country, state,
  county, city — now lets its own name answer that question too. On the planet index this
  lifts the city named (Würzburg), and the counties and states named alike (Landkreis
  München, Region Hannover, the state of Brandenburg) into the same tier; street and address
  queries keep their order.
- **A town named beside another no longer ties with the town itself.** `Neubrunn bei
  Würzburg` holds the word *Würzburg*, but as the place it lies beside. It counted exactly as
  much as Würzburg, and on the planet only a single point of weight — 3500 against 3499 —
  kept a street in Würzburg ahead of the one in the village; where the village had the house
  number, it won outright (`Bahnhofstraße 1 München` answered with Grafing bei München). A
  town now counts fully where its first word was typed or at most one word of it was not,
  and half otherwise. So `Halle (Saale)`, `Frankfurt am Main`, `Den Haag` and `Bad Tölz`
  still count fully, `Garching bei München` and `Wentorf bei Hamburg` half. A postcode still
  outweighs any town. Requiring the whole name instead was measured and rejected: `Halle`
  then answered with a village before Halle (Saale), `Haag` with Haag in Oberbayern before
  Den Haag.
- **A text after one wider than 64 bytes gets its own words.** The tokenizer remembers the
  input its words belong to, so that the same text again costs nothing — but only inputs of
  up to 64 bytes are remembered, and a wider one left the memory of the input *before* it
  standing. Asked for that earlier text again, the tokenizer handed out the wide input's
  words instead; an empty input in between did the same with no words at all. Only a
  tokenizer with the repetition filter cleared answered with anything, which is exactly the
  two that must count every text:
  - the ranking, which folds one candidate's postcode, town and name after another. On the
    planet, `Paris` lost the Paris entry whose postcode lists all 22 codes (`75000;75001;…`,
    over 64 bytes): the town behind it read as those codes, agreed with nothing, and fell out
    of the first ten. It is second again.
  - the second pass of a build, which turns every text of an entry into its postings. The
    same sequence — a short text, a wide one, the short one again — attached the wide text's
    words to the entry and dropped the short text's own. Counted on the German dump, the
    whole pass met that sequence once — `Grünhaid` right after a
    `Gartenbauverein der Belegschaft der Porzellanfabrik Schönwald e. V.` — so an index built
    before this fix is not wrong enough to need a rebuild; a rebuild removes what there is.
    The file format is unchanged, and an existing index opens and answers as before.

### Changed

- **The place cache moves to layout 4.** A document record carries the batch it arrived in,
  four bytes, so that a pass replayed from the cache joins its documents in dump order too,
  and the two letters of its country code, so that the replay writes the country words as
  well. The layout is part of the stamp a cache is sealed with and of every file header, so
  a cache of layout 2 — or 3, left by a build between releases — does not answer for the
  dump: the build removes it before measuring the room and writes layout 4 where the room
  suffices. Where it does not, the build walks the dump three times without a cache; a cache
  directory that cannot be made or written into stops the build, as it always did.
- **Joining the documents holds four more bytes per segment** while it sorts them — on the
  2026 planet dump with its 64 M segments about 250 MB, for the length of the join.
- `doc_collector_add_document()` takes the batch as a third argument, `place_cache_write()`
  and `place_cache_read()` take and return it. None of these are part of `client.h`.

## 1.2.2 -- 2026-08-25

`build.zig` looked for `src/` in the working directory rather than in its own, which is the
same directory for a build of this repository and a different one for every build that
depends on it. Nothing an index or a query does changes here.

### Fixed

- **A build run as a dependency finds its own sources.** `addDirSources` walked `src/`
  through `std.fs.cwd()` to collect its file names, and handed them to `addCSourceFiles`
  with `b.path("src")` as their root. `zig build` never changes directory, so for a package
  resolved as a dependency the two halves came from different projects: the names from the
  *consumer's* `src/`, the root from ours. The one word that fixes it is
  `b.build_root.handle`, which is how `addRoaring()` and every `b.path()` beside it already
  resolve.
  - A consumer with no `src/` of its own got `error.FileNotFound` from a directory in the
    wrong project, at the moment the package was resolved — before a single file of this one
    was compiled, whatever it had asked the package for.
  - A consumer *with* a `src/` got a `core` built from a file list that was neither
    project's: a name the consumer had and we do not failed as `FileNotFound` under our
    `src/`, and a file of ours the consumer had no name for was left out of the library
    without a word.
  - Neither reached anyone linking the `geoindex` client, which names its five sources
    explicitly and never calls `addDirSources`, and which is what leaves `core` out of the
    graph entirely. The builder is `core` plus `main.c`, so it got the full weight of it.
  - Both together are why 1.2.1 was verified as fetchable and still was not: the consumer it
    was tried against had a `src/` and linked the client, which is the one combination that
    says nothing.

### Notes

- The version is the only thing a consumer of 1.2.1 has to change; `.paths`, the file
  format, `client.h` and every artifact this builds are untouched.
- Verified by extracting the tracked tree into a directory with no `.git`, building it
  there, and then resolving it as a `.path` dependency from three consumer projects: one
  with no `src/` and one with, both linking `geoindex`, and a third with a `src/` holding a
  file that is not C at all, linking the builder. All three build and run. With the old line
  restored, the first panics, the second builds for the wrong reason, and the third fails
  naming `src/intruder.c` under *this* package's `src/`, where nothing of the sort exists.

## 1.2.1 -- 2026-08-25

1.2.0 could not be fetched. `zig fetch` takes a repository tree and nothing under it, so the
four git submodules reached a consumer as four empty directories and the build failed three
layers down on a header that was not there — whatever `build.zig` said about it. Nothing an
index or a query does changes here; what changes is whether a project that depends on this
one gets something it can build.

### Changed

- **There are no git submodules.** All four are gone and `.gitmodules` with them, so a clone
  is `git clone` and `zig build` with nothing in between. `third_party` went from 314 MB to
  1.3 MB.
  - **CRoaring is the two files its `amalgamation.sh` writes** — `roaring.c` and `roaring.h`,
    byte-identical to what the pinned checkout produced — beside the `LICENSE` and a README
    recording version 4.7.2, upstream commit `2e8395f1` and the three commands that move it
    to a newer release. The whole repository was carried for those two files. Upstream's
    build files, tests, benchmarks, fuzzers, the C++ wrapper and the split headers under
    `include/` are all absent, which is why `<roaring/roaring.h>` is now `<roaring.h>` —
    an include of `search/geo_index.h`, which is not a header this package installs.
  - **zstd left.** It had been a Zig dependency in `build.zig.zon` all along; the submodule
    beside it was never named by `build.zig`.
  - **stb and tiny-json left**, used by nothing. stb still had two include paths in
    `build.zig` and tiny-json did not even have that.
- **`third_party` is named in `.paths`,** so it travels with the package. It was not there
  before, so even without submodules the fetched tree would have arrived without CRoaring.
- **`CROARING_COMPILER_SUPPORTS_AVX512` is set on every unit that reads `roaring.h`,** not
  only on the one that compiles `roaring.c`. The amalgamated header carries CRoaring's
  internals and picks its own default where nobody names one, so a unit reading it under a
  different answer than the library was built with would disagree with it about what is
  inside a bitmap — harmless so far, and the kind of disagreement that surfaces later as a
  bitmap that makes no sense. `addRoaring()` and `addRoaringHeader()` in `build.zig` are the
  one place that decides it now.

### Notes

- **A consumer has to fetch this version anew.** `.paths` decides what a package contains and
  therefore what it hashes to, so the hash of 1.2.1 is not the hash of 1.2.0 — which could
  not have been used in any case.
- Verified by extracting the tracked tree into a directory with no `.git`, building it there
  with `-Dtests=true` and running the suite, then building a separate consumer project
  against it through `b.dependency("geo_address_search_c", …).artifact("geoindex")` and
  running what came out.
- The library's own behaviour is untouched: `./test_all.sh --clean` passes the same 328 tests
  in all four optimisation modes, and a built index answers the same queries.

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

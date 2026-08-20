# geo-address-search-c

Reads a compressed [Photon](https://github.com/komoot/photon) dump, builds a search index
for postal addresses from it, and writes it as a binary file that later runs are merely
mapped into memory.

```
photon_dump.jsonl.zst  ──►  geo_address_search_c  ──►  index.gdx  ──►  mmap
      24 GB                     three passes                              < 1 ms
```

## What it does

1. **Decompresses and reads** the zstd-compressed dump as a stream.
2. **Parses** every JSON line with [yyjson](https://github.com/ibireme/yyjson) and splits
   each entry in two: the fields an answer shows (street, house number, postal code,
   town, coordinate, `importance`), and the role-free text a query may match.
3. **Folds and splits** that text: lower case, diacritics over the whole Latin script
   (`é → e`, `ș → s`, `ộ → o`, `ǎ → a`, and a combining mark folded away with the letter
   it sits on), `ß → ss`, umlauts in both spellings (`ü → ue` and `ü → u`), abbreviations
   (`str. → strasse`, `St. → Sankt`) and compounds (`superstrasse → super + strasse`).
4. **Collects, sorts and deduplicates** the words — lock-free per thread, grouped by
   prefix, joined at the end in a k-way merge. A second dictionary keeps the original
   spellings for display.
5. **Walks the dump a second time** and writes documents (a place, a coordinate, a
   weight) and posting lists (word → places). A posting needs the rank of its word, and
   that exists only once all words are sorted.
6. **Walks it a third time** and hangs the house numbers on their streets. That, too, is
   only possible once the streets are documents with numbers — and a pass costs minutes
   while holding 292 million houses in memory would cost gigabytes.
7. **Writes** the result as a `.gdx` file in exactly the shape it will be read in.

Whatever carries a house number is an address, regardless of what `address_type` calls
it. The dump files a holiday camp with a number under `other` and a shipwreck without one
under `house`. What carries no number and belongs to no level of the address hierarchy is
a pond or a cycleway, and stays out.

## Usage

```sh
geo_address_search_c <photon_dump.jsonl.zst> [index.gdx] [parser_threads] [--languages=de,en]
geo_address_search_c <index.gdx> ["query"] [max_results] [lat,lon] [--language=en]
```

The extension decides which way it goes: a first argument ending in `.gdx` is loaded,
anything else is built.

| Argument | Meaning | Default |
| --- | --- | --- |
| `photon_dump.jsonl.zst` | Source dump | required |
| `index.gdx` | Destination when building | derived from the dump name |
| `parser_threads` | Parser threads (1–10) | 4 |
| `"query"` | Words in any order | without it: counts only |
| trailing space | Closes the last word | without it: it counts as still being typed |
| `max_results` | Results to show | 10 |
| `--languages` | Readings the index keeps; the first is the default | `de` |
| `--language` | Reading an answer shows | the index's first |

```sh
# build: planet.jsonl.zst -> planet.gdx
geo_address_search_c planet.jsonl.zst 8

# load and show the counts
geo_address_search_c planet.gdx

# search — order does not matter, spellings and abbreviations are folded
geo_address_search_c planet.gdx "Berlin, Superstr. 8"
geo_address_search_c planet.gdx "15328 Bleyen" 5
```

### Languages

The dump writes every place in as many languages as OSM knows it — `name:de`, `name:en`,
`city:fr` — and an index keeps the ones the build names:

```sh
geo_address_search_c planet.jsonl.zst planet.gdx 8 --languages=de,en,fr
geo_address_search_c planet.gdx "Praha " --language=en    # -> Prague
```

The **first** language is the default: it fills the document records, it is what an
answer shows when nothing is asked for, and without the option it is German — so an
index built without `--languages` is exactly the index this program always built.

Every further language costs twice. Its readings enter the *display* dictionary, and
they enter the *search* words as well, so that a street in Prague can be found by
`Wenceslas Square Prague` and not only by `Václavské náměstí Praha`. The second is the
expensive half: the dump repeats the whole address chain on every entry, so each
language adds roughly one term per ancestor per place. Name the languages you will
actually ask for.

What that buys, per place and per language, is a record in a side table — and only
where a language really writes the place differently. A village named the same
everywhere costs nothing, which is why the table is sparse and sits at the end of the
file rather than inside every document record.

A language the index does not hold is not an error: the same places answer, spelled the
way the index spells them. So is a place the language has no reading of — it keeps its
default spelling rather than coming back blank. `geo_address_search_c index.gdx` lists
what an index holds, and so does `info().languages` in the bindings.

A query walks the same folding as the index did: `Superstr.`, `superstrasse` and
`SUPERSTRASSE` meet the same word, and `München` is also found as `Muenchen` or
`Munchen`. The folding covers every language written in Latin letters — `București` is
found as `Bucuresti` whichever of its two s-letters the dump used, `Hồ Chí Minh` as
`Ho Chi Minh` — and it does not matter whether a name arrives composed or decomposed. A place is found where all words of the query meet; words the index does not
know are passed over rather than made to fail the whole query. Results are ordered by
Photon's own `importance`, and whoever asks for a house number gets the street that
carries it first.

A number in the query is a house number before it is a word: `Superstraße 8` looks for
the street without the 8 and resolves the number there. Only if that finds nothing may
the number appear as a word — otherwise `Straße des 17. Juni` would fail.

### Autocomplete

The **last** word counts as still being typed and is read as a beginning as well:
`Altlandsberg Bahnhofstr 12` finds `Bahnhofstraße 12`. A trailing space or comma closes
the word and searches it exactly as it stands. As a library this is the `prefix_last`
parameter — `true` for input in progress, `false` for a submitted query.

Two limits are known and intended:

- A beginning matching **more than 4096 words** is **refused rather than truncated**.
  Truncating would take the first thousand in alphabetical order and quietly drop the
  rest — `mar` would find `marabu` and never `marienplatz`. A refused word narrows
  nothing; if the word also exists on its own (`mar` is Spanish for sea), the exact
  reading carries the query.
- A **single** word as a beginning costs milliseconds instead of microseconds: it unites
  every matching word and looks through a large result space by weight.

Both could be fixed by intersecting the complete words first and checking the beginning
against the few candidates afterwards. As long as autocomplete is not in front of users,
that work does not pay.

## Numbers

Measured on the planet dump (24.21 GB compressed, roughly 500 GB of JSON) and the German
extract (2.10 GB), four threads:

| | Germany | World |
| --- | --- | --- |
| entries | 26.7 M | 356.3 M |
| places (documents) | 1.6 M | 34.7 M |
| house numbers | 20.6 M | 248.6 M |
| distinct words | 594 k | 7.83 M |
| index file | 390 MB | 5.6 GB |
| build time | 1.1 min | 15.8 min |
| open | 0.25 ms | 0.26 ms |
| query, warm | ~100 µs | ~120 µs |

Build time hangs on decompression — the same dump three times. Everything else together
costs about a minute on the planet. Peak memory while building is around 13 GB.

## Building

```sh
zig build -Dtarget=x86_64-linux-gnu
```

The binary lands in `./zig-out/bin/geo_address_search_c`, the client library in
`./zig-out/lib/` and its header in `./zig-out/include/geoindex/client.h`.

Only the library, as a shared object:

```sh
zig build client -Dshared=true --release=fast
```

There is **no** `-march=native` in the compiler flags any more: it silently overrode the
target Zig compiles for and produced artifacts that abort with "illegal instruction" on
foreign machines. To compile for your own machine use `-Dcpu=native`; for shipped modules
the baseline target stays. CRoaring picks its SIMD paths at runtime regardless.

Dependencies are fetched by the Zig build system:
[zstd](https://github.com/facebook/zstd),
[hostmem](https://github.com/einhornimmond/hostmem) (bucket vector, arena allocator,
number conversion, timer), [yyjson](https://github.com/ibireme/yyjson) and
[CRoaring](https://github.com/RoaringBitmap/CRoaring) (both vendored).

Requirements: Zig ≥ 0.15.1, pthreads, Linux.

## Embedding

`client.h` is the whole surface of the reading library — eight functions, an opaque
handle, its own status enum, `extern "C"` for C++. Beyond `stdbool`, `stddef` and
`stdint` it names only two headers of ours, and both hold plain types.

```c
#include <geoindex/client.h>

GeoClient *client = NULL;
if (geo_client_open(&client, "planet.gdx") != GEO_OK) { /* … */ }

GeoAddress found[10];
size_t count = geo_client_search(client, "Bahnhofstr 12 Altlandsberg", 26, true, found, 10);

/* the same, answered in English where the index holds an English reading */
GeoSearchOptions asked = {.prefix_last = true, .language = "en"};
count = geo_client_search_options(client, "Praha", 5, &asked, found, 10);

char json[4096];
geo_client_search_json(client, "Bahnhofstr 12 Altlandsberg", 26, true, 10, json, sizeof(json));

geo_client_close(client);
```

Three properties you can rely on:

- **Nothing fatal.** No `exit`, no output; every failure comes back as a `GeoStatus`. An
  unreadable file does not take the server down with it.
- **Concurrent reads.** Any number of threads may search the same client — the scratch a
  search needs lives on its own stack. Checked with ThreadSanitizer.
- **Borrowed text.** The strings in a result point into the mapping and are valid until
  `geo_client_close()`. Copy what should outlive it.

`geo_client_search_json()` exists for the language border: one crossing per query instead
of one per result field. A buffer that is too small reports the length needed and stays
NUL-terminated.

The library links against hostmem: `geo_index.h` names `hostmem_result` in its headers and
`client.c` writes its JSON numbers with hostmem's converter rather than carrying a second
copy of one. Five files of our own are compiled — `client.c`, `geo_cell.c`, `geo_index.c`,
`prefix_tree.c` and `text_tokenize.c` — and beside them `roaring.c`, vendored from CRoaring
and built with its own flags.

### From other languages

Bindings live under `bindings/`. Bun and Node present the **same** `GeoIndex` class,
described by one declaration file — [`bindings/index.d.ts`](bindings/index.d.ts) — and
sharing their constants through [`bindings/kinds.js`](bindings/kinds.js):

```ts
import { GeoIndex } from "./bindings/bun/index.ts";   // Bun, through bun:ffi
import { GeoIndex } from "./bindings/node/index.js";  // Node, through a N-API addon

const index = GeoIndex.open("planet.gdx");
const [best] = index.search("Bahnhofstr 12 Altlandsberg");

index.info().languages;                          // ["de", "en", "fr"]
index.search("Praha", { language: "en" });       // -> Prague

index.close();
```

A query crosses the border as a single call and comes back as JSON — measured at **~22 µs**
on a warm 390 MB index with Bun, `JSON.parse` included.

```sh
zig build client -Dshared=true --release=fast   # libgeoindex.so, for Bun
cd bindings/node && npm install                 # N-API headers
zig build node --release=fast                   # geoindex.node, for Node
```

Details in [bindings/bun/README.md](bindings/bun/README.md) and
[bindings/node/README.md](bindings/node/README.md).

## Architecture

```
┌──────────────┐     ┌──────────────┐     ┌────────────────┐
│  main thread │────►│  ParseQueue  │────►│ parser threads │
│  zstd stream │     │  (bounded)   │     │ yyjson + fold  │
└──────────────┘     └──────────────┘     └────────┬───────┘
                                                   │ each thread sorts its own
                                          ┌────────▼────────┐
                                          │  k-way merge    │
                                          │  over prefixes  │
                                          └────────┬────────┘
                                          ┌────────▼────────┐
                                          │ geo_index_write │
                                          └─────────────────┘
```

| Module | Purpose |
| --- | --- |
| `main` | Command line, zstd stream, threads, three passes |
| `json_parse` | Photon lines → `PhotonPlace` (answer fields + search texts) |
| `text_tokenize` | Folding, abbreviations, compounds — index and query walk the same path |
| `name_collector` | Collect, sort, deduplicate; k-way merge across threads |
| `prefix_tree` | Byte-wise index tree: one character per level, an index at the end |
| `doc_collector` | Documents and postings per thread, joined by counting sort |
| `house_collector` | House numbers on their streets, ordered per street |
| `geo_index` | File format, writer, `mmap` loader, word lookup, queries over bitmaps |
| `client` | The reading library: open, search, close — without the builder |
| `parse_queue`, `line_buffer`, `progress`, `format`, `error` | Infrastructure |

## File format

```
[ header          ] magic, version, layout hash, byte order, counts
[ sections        ] per section: kind, offset, length
[ word dictionary ] groups, offsets, text — folded words, what a query meets
[ display dict.   ] groups, offsets, text — original spelling, what an answer shows
[ documents       ] one fixed record per place: coordinate, weight, kind, display ranks
[ importance      ] one weight per place, apart from the records
[ posting offsets ] word_count + 1 byte offsets into the postings
[ postings        ] one frozen Roaring bitmap per word, 32-byte aligned
[ houses          ] house numbers with their own coordinate, ordered by street
[ house offsets   ] document_count + 1 entries into the houses
[ languages       ] one tag per language, and where its readings begin
[ variants        ] localized readings, by language then by document
```

The postings are Roaring bitmaps ([CRoaring](https://github.com/RoaringBitmap/CRoaring),
vendored): a word standing on millions of places then costs a bit per place instead of
four bytes, and asking whether one of them is *this* place is a bit test instead of a
walk through gigabytes. The *frozen* format is the memory image itself — nothing is
decoded when the file opens, a bitmap is viewed where it lies.

The last two sections are empty in a build that named one language. They are read by
kind rather than by position, so a file that gained a section is still a file an older
reader walks past — what it refuses is a section it needs and does not find.

No pointers, only `uint32` indices and offsets; fixed field widths with `static_assert`;
the header checks magic, version, byte order, layout hash and file size before a byte is
read. That is why loading is an `mmap` and costs nothing regardless of file size.

## Development

```sh
./lint.sh     # clang-format over src/
doxygen       # API documentation
```

Comments follow the two-layer standard from [AGENTS.md](AGENTS.md): a precise technical
specification, plus a `@whisper` line where one fits.

## License

See the license files in `third_party/` for the vendored dependencies.

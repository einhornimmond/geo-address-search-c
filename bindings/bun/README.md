# @gradido/geoindex — Bun binding

An address in, a coordinate out — no database, no server process, no network.
The index lives inside your process as a memory mapping.

```ts
import { GeoIndex } from "@gradido/geoindex";

using index = GeoIndex.open("planet.gdx");

const [best] = index.search("Bahnhofstr 12 Altlandsberg");
console.log(best?.lat, best?.lon); // 52.5475072 13.7175547
```

## Building

The library is produced from the C part of this project:

```sh
zig build client -Dshared=true --release=fast
```

It lands in `zig-out/lib/libgeoindex.{so,dylib,dll}` and is found from here. A different
location goes through `GEOINDEX_LIB` or as the second argument of `GeoIndex.open()`.

For a module you ship, do **not** use `-Dcpu=native` — it aborts with "illegal
instruction" on older machines. CRoaring picks its SIMD paths at runtime anyway.

## API

| | |
| --- | --- |
| `GeoIndex.open(path, libraryPath?)` | Map the file; throws on an unreadable or foreign one |
| `index.search(query, { limit, prefix })` | Search; returns `GeoAddress[]` |
| `index.info()` | Counts of the file |
| `index.close()` or `using` | Release the mapping |

### Searching

The query is free text: words in any order, any case, umlauts spelled however, German
abbreviations written out or not.

```ts
index.search("Berlin, Superstr. 8");
index.search("15328 Bleyen");
index.search("München Marienpl");                        // last word as a beginning
index.search("München Marienplatz", { prefix: false });  // submitted, exact
```

A number is read as a house number first and as a word only if that finds nothing.
Results come back heaviest first; a place where the house number was actually found
comes before one without it.

**`prefix`** decides whether the last word is read as a beginning as well. `true`
(the default) for input someone is still typing, `false` for a query they submitted —
a beginning always matches more than the word itself.

### Result

```ts
interface GeoAddress {
  name: string | null;      // street or place, as it is written
  number: string | null;    // house number, when the query named one
  postcode: string | null;
  city: string | null;
  lat: number | null;       // null when the place never carried a coordinate
  lon: number | null;
  kind: GeoPlaceKind;       // Street, City, District, …
  importance: number;       // 0 … 65535
  matched: number;          // words of the query this place carries
}
```

## Worth knowing

**Opening costs nothing, the first queries do.** The file is mapped, not read; the
operating system fetches only the pages a query touches. Measured against a 390 MB
index of Germany:

| | |
| --- | --- |
| open | < 1 ms |
| first query | ~360 µs |
| every one after | **~22 µs** |

A server should warm itself up at startup — a few dozen queries spread across the data
are enough.

**Concurrent reads are safe.** A client may be searched from several threads at once;
the library keeps no shared state for it. With Bun's single JS thread that does not
matter, with worker threads it does.

**One crossing per query.** The library writes the answer as JSON into a buffer that is
reused between calls and only grows when an answer does not fit. Crossing the language
border once per result field would cost a multiple of the search.

**The memory belongs to the operating system.** The mapping shows up in RSS as pages are
touched — that is page cache, not a leak, and the kernel reclaims it under pressure.

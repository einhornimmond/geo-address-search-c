# @gradido/geoindex-node — Node binding

The same `GeoIndex` as the [Bun binding](../bun/README.md), reached through a N-API addon
instead of `bun:ffi`. Both are described by one declaration file, [`../index.d.ts`](../index.d.ts),
and both answer with the same numbers — the constants live in [`../kinds.js`](../kinds.js).

```js
import { GeoIndex } from "@gradido/geoindex-node";

const index = GeoIndex.open("planet.gdx");
const [best] = index.search("Bahnhofstr 12 Altlandsberg");
console.log(best?.lat, best?.lon); // 52.5475072 13.7175547
index.close();
```

## Building

The addon needs the N-API headers. The usual way is the `node-api-headers` package,
which is a devDependency here:

```sh
cd bindings/node && npm install
cd ../.. && zig build node --release=fast
```

The result lands in `zig-out/lib/geoindex.node` and is found from there. A different
location goes through `GEOINDEX_ADDON` or as the second argument of `GeoIndex.open()`.

If the headers live elsewhere — a `node-gyp` cache, a distribution package — point the
build at them:

```sh
zig build node --release=fast -Dnode-headers=/usr/include/node
```

For a module you ship, do **not** use `-Dcpu=native`: it aborts with "illegal instruction"
on older machines. CRoaring picks its SIMD paths at runtime anyway.

**On macOS** the linker has to be told that the N-API symbols are resolved by the host
process (`-undefined dynamic_lookup`). On Linux that is the default for a shared library,
which is why the build file says nothing about it.

## How it differs from the Bun binding

Nothing a caller sees. Underneath:

| | Bun | Node |
| --- | --- | --- |
| way down | `dlopen` on `libgeoindex.so` | `require` on `geoindex.node` |
| handle | a pointer as a number | a N-API external |
| forgotten `close()` | leaks until the process ends | released when collected |
| `using` | works today | works from Node 20 on |

Both hand the answer over as JSON: the library writes it in one go, and one string
crossing the boundary costs less than nine property assignments per result.

## Worth knowing

**Opening costs nothing, the first queries do.** The file is mapped, not read; the
operating system fetches only the pages a query touches. A server should warm itself up
at startup — a few dozen queries spread across the data are enough.

**One index per process is enough.** The mapping is read-only; open it once and keep it.
Opening it twice maps the same file twice, which the operating system will happily share,
but there is no reason to.

**The memory belongs to the operating system.** The mapping shows up in RSS as pages are
touched — that is page cache, not a leak, and the kernel reclaims it under pressure.

# parse_photon_jsonl_dump

Most of the code was written by chatgpt and deepseek!

A high-performance, multi-threaded pipeline that reads a compressed [Photon](https://github.com/komoot/photon)
geocoder JSONL dump, extracts and deduplicates the full address hierarchy, and
writes it as a self-contained PostgreSQL script with PostGIS geometry and
pg_trgm indexes for fuzzy search.

```
photon_dump.jsonl.zst  ──►  parse_photon_jsonl_dump  ──►  output.sql
```

## What it does

1. **Streams & decompresses** a zstd-compressed Photon dump file.
2. **Parses** each JSON line with [yyjson](https://github.com/ibireme/yyjson),
   counting document types and address components along the way.
3. **Deduplicates** the seven-level address hierarchy (country → state →
   county → city → postcode → street → house) using binary-keyed hash tables
   backed by [stb_ds](https://github.com/nothings/stb).
4. **Exports** a normalized PostgreSQL script containing:
   - DDL for seven hierarchy tables with foreign keys
   - `COPY` data sections with resolved references
   - PostGIS `GEOMETRY(POINT, 4326)` columns
   - pg_trgm GIN indexes for fuzzy text search
   - A final `VACUUM ANALYZE`

Processing uses a producer-consumer pipeline: the main thread reads and
decompresses, while configurable parser threads (default 4, up to 10) parse
JSON and build per-thread statistics. Results are merged at completion.

## Requirements

- [Zig](https://ziglang.org/) ≥ 0.15.1
- A C compiler (Zig bundles one)
- pthreads (Linux)

Dependencies are fetched automatically by the Zig build system:
- [zstd](https://github.com/facebook/zstd) (via zig package)
- [gradido-blockchain-core](https://github.com/gradido/gradido-blockchain-core)
  (provides `mono_timer` utilities)
- [yyjson](https://github.com/ibireme/yyjson) (vendored in `third_party/`)
- [stb_ds](https://github.com/nothings/stb) (vendored in `third_party/`)

## Build

```sh
zig build
```

The binary lands at `./zig-out/bin/parse_photon_jsonl_dump`.

## Usage

```sh
parse_photon_jsonl_dump <photon_dump.jsonl.zst> <output.sql> [parser_threads]
```

| Argument              | Description                          | Default |
| --------------------- | ------------------------------------ | ------- |
| `photon_dump.jsonl.zst` | Path to the compressed Photon dump | required |
| `output.sql`          | Path for the generated SQL script    | required |
| `parser_threads`      | Number of parser threads (1–10)      | 4       |

### Example

```sh
parse_photon_jsonl_dump photon_dump.jsonl.zst addresses.sql 8
```

This reads `photon_dump.jsonl.zst`, parses with 8 worker threads, and writes
`addresses.sql`. A progress bar shows decompression throughput in real time.

### Output

The generated `.sql` file can be loaded directly into PostgreSQL with PostGIS:

```sh
psql -d mydb -f addresses.sql
```

The script is self-contained — it creates its own tables and indexes and
can be run against an empty or existing database (tables are dropped with
`IF EXISTS`).

## Architecture

```
┌─────────────-┐     ┌──────────────┐     ┌──────────────┐
│  Main thread │────►│  ParseQueue  │────►│ Parser thread│
│  reads zstd  │     │  (bounded)   │     │  (yyjson)    │
│  decompresses│     │  cap = 8     │     │              │
└─────────────-┘     └──────────────┘     └──────┬───────┘
                                                │
                                     ┌──────────▼──────────┐
                                     │  Per-thread         │
                                     │  JsonStats          │
                                     │  StorageStats       │
                                     └──────────┬──────────┘
                                                │ merge
                                     ┌──────────▼──────────┐
                                     │  storage_stats_     │
                                     │  write_sql()        │
                                     └─────────────────────┘
```

| Module          | Purpose                                               |
| --------------- | ----------------------------------------------------  |
| `main.c`        | Orchestration, zstd streaming, thread management      |
| `parse_queue`   | Bounded, thread-safe producer-consumer queue          |
| `line_buffer`   | Resizable byte buffer with line-aware append/reset    |
| `json_stats`    | Census of address types found in the dump             |
| `storage_stats` | Deduplicated seven-level address store                |
| `sql_export`    | PostgreSQL DDL + COPY + PostGIS output                |
| `progress`      | Terminal progress bar for decompression               |
| `format`        | Human-readable byte-unit formatting                   |
| `error`         | Fatal error reporting and informational logging       |

## Development

### Linting

```sh
./lint.sh
```

Requires `clang-format`. Formats all C source files in `src/`.

### Documentation

Doxygen with `Doxyfile`.

```sh
doxygen
```

API documentation follows the [AGENTS.md](AGENTS.md) dual-layer commenting
standard: every public function carries a precise technical specification
and, where fitting, a poetic `@whisper` signature.

## License

See the license files in `third_party/` for bundled dependencies. 

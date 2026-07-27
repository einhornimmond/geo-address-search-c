/**
 * Bun binding for the geo index library.
 *
 * The library keeps the index as a memory mapping — opening costs microseconds
 * whether the file is 300 MB or 6 GB. A query crosses the language border as
 * **one** call and comes back as JSON; crossing once per result field would
 * cost more than the search itself.
 *
 * A client may be searched from several threads at once.
 */

import { dlopen, FFIType, ptr, suffix } from "bun:ffi";
import { existsSync } from "node:fs";
import { join } from "node:path";

/** How a library call ended. Mirrors `GeoStatus` from client.h. */
export enum GeoStatus {
  Ok = 0,
  Argument = 1,
  File = 2,
  Format = 3,
  Memory = 4,
}

/** What kind of place a result is. The numbers are part of the file format. */
export enum GeoPlaceKind {
  None = 0,
  Country = 1,
  State = 2,
  County = 3,
  City = 4,
  Street = 5,
  House = 6,
  Other = 7,
  District = 8,
  Locality = 9,
  StateCity = 10,
  IndependentCity = 11,
}

/** A place that was found. Missing fields are `null`, not empty. */
export interface GeoAddress {
  /** Street or place name, spelled as the source data spells it. */
  name: string | null;
  /** House number — set only when the query named one and the place carries it. */
  number: string | null;
  postcode: string | null;
  city: string | null;
  /** Degrees; `null` when the place never carried a coordinate. */
  lat: number | null;
  lon: number | null;
  kind: GeoPlaceKind;
  /** Weight of the place, 0 … 65535 — the larger, the more significant. */
  importance: number;
  /** How many words of the query this place carries. */
  matched: number;
}

/** Counts of an opened index file. */
export interface GeoIndexInfo {
  fileSize: number;
  documents: number;
  houses: number;
  words: number;
  spellings: number;
  postings: number;
  format: number;
}

export interface SearchOptions {
  /** Most results to return (the library caps at 256). Defaults to 10. */
  limit?: number;
  /**
   * Read the last word as a beginning as well, for input someone is still
   * typing: `Marienpl` then finds `Marienplatz`. Defaults to `true`.
   *
   * Pass `false` for a query they submitted — a beginning always matches more
   * than the word itself.
   */
  prefix?: boolean;
}

const STATUS_TEXT: Record<number, string> = {
  [GeoStatus.Argument]: "invalid argument",
  [GeoStatus.File]: "file cannot be read or mapped",
  [GeoStatus.Format]: "not an index this build can read",
  [GeoStatus.Memory]: "out of memory",
};

/** Size of `GeoClientInfo`: six `uint64` and one `uint32`, padded. */
const INFO_SIZE = 56;

/** Initial size of the JSON buffer; it grows when an answer does not fit. */
const INITIAL_BUFFER = 16 * 1024;

function findLibrary(explicit?: string): string {
  const candidates = [
    explicit,
    process.env.GEOINDEX_LIB,
    join(import.meta.dir, `libgeoindex.${suffix}`),
    join(import.meta.dir, "..", "..", "zig-out", "lib", `libgeoindex.${suffix}`),
  ].filter((path): path is string => typeof path === "string" && path.length > 0);

  for (const candidate of candidates) {
    if (existsSync(candidate)) return candidate;
  }
  throw new Error(
    `libgeoindex.${suffix} not found. Looked in:\n  ${candidates.join("\n  ")}\n` +
      `Build it with: zig build client -Dshared=true --release=fast\n` +
      `or point GEOINDEX_LIB at it.`,
  );
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** A string as NUL-terminated bytes, the way C expects it. */
function cString(text: string): Uint8Array {
  return encoder.encode(text + "\0");
}

let library: ReturnType<typeof dlopen> | null = null;

function symbols(explicitPath?: string) {
  if (!library) {
    library = dlopen(findLibrary(explicitPath), {
      geo_client_open: {
        args: [FFIType.ptr, FFIType.ptr],
        returns: FFIType.i32,
      },
      geo_client_close: {
        args: [FFIType.ptr],
        returns: FFIType.void,
      },
      geo_client_info: {
        args: [FFIType.ptr, FFIType.ptr],
        returns: FFIType.i32,
      },
      geo_client_search_json: {
        args: [
          FFIType.ptr, // client
          FFIType.ptr, // query
          FFIType.u64, // query_size
          FFIType.bool, // prefix_last
          FFIType.u64, // limit
          FFIType.ptr, // buffer
          FFIType.u64, // buffer_size
        ],
        returns: FFIType.u64_fast,
      },
    });
  }
  return library.symbols;
}

/**
 * An opened index.
 *
 * ```ts
 * using index = GeoIndex.open("planet.gdx");
 * const [best] = index.search("Bahnhofstr 12 Altlandsberg");
 * console.log(best?.lat, best?.lon);
 * ```
 */
export class GeoIndex {
  #handle: number;
  #buffer: Uint8Array;
  #api: ReturnType<typeof symbols>;

  private constructor(handle: number, api: ReturnType<typeof symbols>) {
    this.#handle = handle;
    this.#api = api;
    this.#buffer = new Uint8Array(INITIAL_BUFFER);
  }

  /**
   * Map an index file.
   *
   * @param path         Path to the `.gdx` file.
   * @param libraryPath  Where the shared library lives, if it is not found by itself.
   */
  static open(path: string, libraryPath?: string): GeoIndex {
    const api = symbols(libraryPath);
    const out = new BigUint64Array(1);
    const status = api.geo_client_open(ptr(out), ptr(cString(path)));
    if (status !== GeoStatus.Ok) {
      throw new Error(
        `cannot open index '${path}': ${STATUS_TEXT[status] ?? `status ${status}`}`,
      );
    }
    return new GeoIndex(Number(out[0]), api);
  }

  /** Release the mapping. Every result from this index becomes invalid. */
  close(): void {
    if (this.#handle === 0) return;
    this.#api.geo_client_close(this.#handle);
    this.#handle = 0;
  }

  [Symbol.dispose](): void {
    this.close();
  }

  /** Counts of the opened file. */
  info(): GeoIndexInfo {
    this.#assertOpen();
    const raw = new Uint8Array(INFO_SIZE);
    const status = this.#api.geo_client_info(this.#handle, ptr(raw));
    if (status !== GeoStatus.Ok) {
      throw new Error(`cannot read index counts: ${STATUS_TEXT[status] ?? `status ${status}`}`);
    }
    const view = new DataView(raw.buffer);
    return {
      fileSize: Number(view.getBigUint64(0, true)),
      documents: Number(view.getBigUint64(8, true)),
      houses: Number(view.getBigUint64(16, true)),
      words: Number(view.getBigUint64(24, true)),
      spellings: Number(view.getBigUint64(32, true)),
      postings: Number(view.getBigUint64(40, true)),
      format: view.getUint32(48, true),
    };
  }

  /**
   * Search for a place — words in any order, house number optional.
   *
   * @returns Results, heaviest first; a place where the house number was
   *          actually found comes before one where it was not.
   */
  search(query: string, options: SearchOptions = {}): GeoAddress[] {
    this.#assertOpen();
    const limit = options.limit ?? 10;
    const prefix = options.prefix ?? true;
    const text = encoder.encode(query);
    if (text.length === 0) return [];

    // Twice at most: if the answer does not fit, the buffer grows to the size
    // the library reported and the call is repeated once.
    for (let attempt = 0; attempt < 2; ++attempt) {
      const needed = this.#api.geo_client_search_json(
        this.#handle,
        ptr(text),
        BigInt(text.length),
        prefix,
        BigInt(limit),
        ptr(this.#buffer),
        BigInt(this.#buffer.length),
      );
      if (needed < this.#buffer.length) {
        return JSON.parse(decoder.decode(this.#buffer.subarray(0, needed))) as GeoAddress[];
      }
      this.#buffer = new Uint8Array(needed + 1);
    }
    throw new Error("the answer buffer keeps growing unexpectedly");
  }

  #assertOpen(): void {
    if (this.#handle === 0) throw new Error("this index is already closed");
  }
}

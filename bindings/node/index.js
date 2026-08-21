/**
 * Node binding for the geo index library.
 *
 * Presents the same `GeoIndex` class as the Bun binding — see
 * `bindings/index.d.ts` for the shape both share. The way down differs: Bun
 * reaches the shared library through `bun:ffi`, Node through the N-API addon
 * next to this file.
 *
 * A query crosses the boundary as one call and comes back as JSON; crossing
 * once per result field would cost more than the search itself.
 */

import { existsSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { GeoPlaceKind, GeoStatus, STATUS_TEXT } from "../kinds.js";

export { GeoPlaceKind, GeoStatus };

const here = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

/** Node 18 has no `Symbol.dispose` yet; a stand-in keeps `using` working later. */
const disposeSymbol = Symbol.dispose ?? Symbol.for("Symbol.dispose");

function findAddon(explicit) {
  const candidates = [
    explicit,
    process.env.GEOINDEX_ADDON,
    join(here, "geoindex.node"),
    join(here, "..", "..", "zig-out", "lib", "geoindex.node"),
  ].filter((path) => typeof path === "string" && path.length > 0);

  for (const candidate of candidates) {
    if (existsSync(candidate)) return candidate;
  }
  throw new Error(
    `geoindex.node not found. Looked in:\n  ${candidates.join("\n  ")}\n` +
      `Build it with: zig build node --release=fast\n` +
      `or point GEOINDEX_ADDON at it.`,
  );
}

let addon = null;

function load(explicit) {
  if (!addon) addon = require(findAddon(explicit));
  return addon;
}

/**
 * An opened index.
 *
 * ```js
 * const index = GeoIndex.open("planet.gdx");
 * const [best] = index.search("Bahnhofstr 12 Altlandsberg");
 * index.close();
 * ```
 */
export class GeoIndex {
  #handle;
  #api;

  constructor(handle, api) {
    this.#handle = handle;
    this.#api = api;
  }

  /**
   * Map an index file.
   *
   * @param {string} path        Path to the `.gdx` file.
   * @param {string} [addonPath] Where `geoindex.node` lives, if it is not found by itself.
   * @returns {GeoIndex}
   */
  static open(path, addonPath) {
    const api = load(addonPath);
    return new GeoIndex(api.open(path), api);
  }

  /** Release the mapping. Calling it twice is harmless. */
  close() {
    if (!this.#handle) return;
    this.#api.close(this.#handle);
    this.#handle = null;
  }

  /** Counts of the opened file. */
  info() {
    this.#assertOpen();
    return this.#api.info(this.#handle);
  }

  /**
   * Search for a place — words in any order, house number optional.
   *
   * @param {string} query
   * @param {{ limit?: number, prefix?: boolean, language?: string }} [options]
   * @returns {Array<object>} Results, heaviest first.
   */
  search(query, options = {}) {
    this.#assertOpen();
    if (query.length === 0) return [];
    const limit = options.limit ?? 10;
    const prefix = options.prefix ?? true;
    const language = options.language ?? null;
    return JSON.parse(this.#api.searchJson(this.#handle, query, prefix, limit, language));
  }

  #assertOpen() {
    if (!this.#handle) throw new Error("this index is already closed");
  }
}

GeoIndex.prototype[disposeSymbol] = function dispose() {
  this.close();
};

/** Kept for symmetry with the Bun binding, where the same names are exported. */
export { STATUS_TEXT };

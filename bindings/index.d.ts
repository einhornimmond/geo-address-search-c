/**
 * The surface both bindings offer — Bun through `bun:ffi`, Node through a
 * N-API addon. Same class, same methods, same results; only the way the
 * library is reached differs.
 *
 * ```ts
 * import { GeoIndex } from "@gradido/geoindex";      // Bun
 * import { GeoIndex } from "@gradido/geoindex-node"; // Node
 *
 * const index = GeoIndex.open("planet.gdx");
 * const [best] = index.search("Bahnhofstr 12 Altlandsberg");
 * index.close();
 * ```
 */

/** How a library call ended. Mirrors `GeoStatus` from client.h. */
export declare enum GeoStatus {
  Ok = 0,
  Argument = 1,
  File = 2,
  Format = 3,
  Memory = 4,
}

/** What kind of place a result is. The numbers are part of the file format. */
export declare enum GeoPlaceKind {
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
  /**
   * The readings the index holds, as language tags — `["de", "en"]`. The
   * first is what an answer shows when `search()` is given no `language`.
   *
   * Empty for an index built without `--languages`, which holds one reading
   * and answers in it whatever is asked for.
   */
  languages: string[];
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
  /**
   * Which reading the answer shows — `"en"`, `"fr"`. Left out, the index's
   * own default is shown; `info().languages` says which readings there are.
   *
   * A tag the index does not hold is not an error and finds the same places:
   * the language changes how a result is spelled, never which results there
   * are. A place the language has no reading of keeps the default spelling,
   * so a result is never blank for the asking.
   */
  language?: string;
}

/**
 * An opened index.
 *
 * Opening maps the file; it costs microseconds whether the index is 300 MB or
 * 6 GB, and the operating system fetches only the pages a query touches. The
 * first queries after opening are therefore slower than the later ones.
 *
 * The strings in a result are copies — they outlive `close()`.
 */
export declare class GeoIndex {
  /**
   * Map an index file.
   *
   * @param path         Path to the `.gdx` file.
   * @param nativePath   Where the shared library or addon lives, if it is not
   *                     found by itself.
   * @throws When the file cannot be read or was written by another build.
   */
  static open(path: string, nativePath?: string): GeoIndex;

  /** Release the mapping. Calling it twice is harmless. */
  close(): void;

  /** Counts of the opened file. */
  info(): GeoIndexInfo;

  /**
   * Search for a place — words in any order, house number optional.
   *
   * @returns Results, heaviest first; a place where the house number was
   *          actually found comes before one where it was not.
   */
  search(query: string, options?: SearchOptions): GeoAddress[];

  /** Releases the mapping, so `using index = GeoIndex.open(…)` works. */
  [Symbol.dispose](): void;
}

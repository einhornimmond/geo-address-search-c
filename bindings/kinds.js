/**
 * The constants both bindings share, in the shape a TypeScript enum compiles
 * to: name to number and number back to name.
 *
 *   GeoPlaceKind.Street === 5
 *   GeoPlaceKind[5]     === "Street"
 *
 * Plain JavaScript, so Bun and Node can both import it. The numbers are part
 * of the index file format — they are read from it, not chosen here.
 */

/** How a library call ended. Mirrors `GeoStatus` from client.h. */
export const GeoStatus = Object.freeze({
  Ok: 0,
  Argument: 1,
  File: 2,
  Format: 3,
  Memory: 4,
  0: "Ok",
  1: "Argument",
  2: "File",
  3: "Format",
  4: "Memory",
});

/** What kind of place a result is. */
export const GeoPlaceKind = Object.freeze({
  None: 0,
  Country: 1,
  State: 2,
  County: 3,
  City: 4,
  Street: 5,
  House: 6,
  Other: 7,
  District: 8,
  Locality: 9,
  StateCity: 10,
  IndependentCity: 11,
  0: "None",
  1: "Country",
  2: "State",
  3: "County",
  4: "City",
  5: "Street",
  6: "House",
  7: "Other",
  8: "District",
  9: "Locality",
  10: "StateCity",
  11: "IndependentCity",
});

/** What went wrong, in words — for an error message a caller can read. */
export const STATUS_TEXT = Object.freeze({
  [GeoStatus.Argument]: "invalid argument",
  [GeoStatus.File]: "file cannot be read or mapped",
  [GeoStatus.Format]: "not an index this build can read",
  [GeoStatus.Memory]: "out of memory",
});

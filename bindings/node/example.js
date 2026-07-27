/**
 * Example: open an index, look a few addresses up, show the timings.
 *
 *   node example.js ../../planet.gdx "Bahnhofstr 12 Altlandsberg"
 */

import { GeoIndex, GeoPlaceKind } from "./index.js";

const [path, ...words] = process.argv.slice(2);
if (!path) {
  console.error("usage: node example.js <index.gdx> [query]");
  process.exit(2);
}

const index = GeoIndex.open(path);
try {
  const info = index.info();
  console.log(
    `index: ${(info.fileSize / 1e6).toFixed(1)} MB, ${info.documents.toLocaleString("en")} places, ` +
      `${info.houses.toLocaleString("en")} house numbers, format ${info.format}`,
  );

  const query = words.join(" ") || "Berlin";

  // The first call pays the page faults; a server should warm itself up.
  for (const round of ["cold", "warm"]) {
    const started = process.hrtime.bigint();
    const found = index.search(query, { limit: 5 });
    const micros = Number(process.hrtime.bigint() - started) / 1000;

    if (round === "warm") {
      console.log(`\nresults for "${query}":`);
      for (const address of found) {
        const number = address.number ? ` ${address.number}` : "";
        const where = address.lat !== null ? `${address.lat}, ${address.lon}` : "no coordinate";
        console.log(
          `  ${address.name ?? "—"}${number}, ${address.postcode ?? "—"} ${address.city ?? "—"}` +
            `  →  ${where}  (${GeoPlaceKind[address.kind]}, weight ${address.importance})`,
        );
      }
      if (found.length === 0) console.log("  (nothing found)");
    }
    console.log(`${round}: ${micros.toFixed(1)} µs`);
  }
} finally {
  index.close();
}

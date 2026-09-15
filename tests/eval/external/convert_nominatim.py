#!/usr/bin/env python3
"""Turn the search features of Nominatim's BDD tests into a query file for geo_eval.

    python3 convert_nominatim.py <Nominatim>/test/bdd/features/api/search/*.feature > nominatim.tsv

Nominatim runs these against a database of Liechtenstein, a county of Alabama and a
few hand-made objects.  The planet index holds Liechtenstein too, so every scenario
that asks for a place by what someone types can be asked of it.  Most scenarios ask
something else — whether an output format is well-formed, whether a bad parameter is
refused, what a class filter returns — and are passed over, counted on stderr with
their reason.

A scenario is carried over where it searches with free text and then says what the
first result contains:

    When geocoding "Austrasse 11, Vaduz"            →  query
      | accept-language | viewbox |                →  lang, lat/lon (the centre of the box)
    Then result 0 contains [in field address]        →  the expectations below
    Then all results contain                         →  the same, for result 0

  name, display_name (up to its first comma)  → expect_name
  house_number, housenumber (geocodejson)     → expect_number
  road, street (geocodejson)                  → expect_street
  postcode                                    → expect_postcode
  city, town, village, hamlet, municipality   → expect_city, as alternatives: Nominatim
                                                names a village and the town above it
                                                separately, the index knows one town

What is left out:

  * searches restricted by a parameter the index has no counterpart for — a bounded
    viewbox, country codes, a feature type, excluded places, deduplication switched
    off — and structured searches without free text;
  * searches whose text carries a coordinate or a class filter (`[amenity=bar]`,
    `restaurants in …`);
  * results described only by what the index does not carry: category, type,
    place_rank, OSM ids, country, a pattern over display_name;
  * checks on any result but the first, on their number, or on attributes being absent;
  * localisation through HTTP headers, which the index has no counterpart for.

Scenario Outlines are expanded over their Examples.  Every query is asked with the last
word read as a beginning as well, as the map in production asks it, and with a limit of
1: Nominatim's checks name result 0.

Standard library only.
"""

import collections
import re
import sys
from pathlib import Path

COLUMNS = [
    "category", "query", "prefix", "lat", "lon", "lang", "limit",
    "expect_name", "expect_number", "expect_street", "expect_city", "expect_postcode", "context",
]

TOWN_KEYS = ("city", "town", "village", "hamlet", "municipality")
UNSUPPORTED_PARAMS = {
    "bounded", "countrycodes", "featureType", "featuretype", "exclude_place_ids", "dedupe",
    "viewboxlbrt", "json_callback", "polygon_geojson", "polygon_text", "polygon_kml",
    "polygon_svg", "layer", "street", "city", "county", "state", "country", "postalcode",
    "amenity",
}
HARMLESS_PARAMS = {"format", "addressdetails", "limit", "extratags", "namedetails", "q", "accept-language", "viewbox"}
COORDINATE_IN_TEXT = re.compile(r"-?\d+\.\d+\s*,\s*-?\d+\.\d+|[NSEW]\s*\d+|\d+\s*°")


def table_rows(lines, start):
    """The Gherkin table starting at lines[start], as a list of cell lists, and the index after it."""
    rows = []
    i = start
    while i < len(lines) and lines[i].strip().startswith("|"):
        cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
        rows.append(cells)
        i += 1
    return rows, i


def table_as_dict(rows):
    """A two-row table (keys, values) or a param/value table, as one dict."""
    if not rows:
        return {}
    if rows[0][:2] == ["param", "value"]:
        return {r[0]: r[1] for r in rows[1:] if len(r) >= 2}
    if len(rows) == 2:
        return dict(zip(rows[0], rows[1]))
    return None  # several value rows: a list of results, not what result 0 contains


class Scenario:
    def __init__(self, feature, title, lines, examples):
        self.feature = feature
        self.title = title
        self.lines = lines
        self.examples = examples


def read_features(path):
    """Scenarios of one feature file, Outlines expanded over their Examples."""
    lines = path.read_text(encoding="utf-8").splitlines()
    scenarios = []
    current = None
    body = []
    examples = []
    i = 0

    def close():
        if current is None:
            return
        if examples:
            header, *values = examples
            for row in values:
                substituted = []
                for line in body:
                    for key, value in zip(header, row):
                        line = line.replace(f"<{key}>", value)
                    substituted.append(line)
                scenarios.append(Scenario(path.stem, current, substituted, None))
        else:
            scenarios.append(Scenario(path.stem, current, list(body), None))

    while i < len(lines):
        stripped = lines[i].strip()
        if stripped.startswith("Scenario"):
            close()
            current = stripped.split(":", 1)[1].strip() if ":" in stripped else stripped
            body, examples = [], []
            i += 1
            continue
        if stripped.startswith("Examples"):
            rows, i = table_rows(lines, i + 1)
            examples = rows
            continue
        if current is not None:
            body.append(lines[i])
        i += 1
    close()
    return scenarios


class Converter:
    def __init__(self):
        self.rows = []
        self.seen = set()
        self.skipped = collections.Counter()

    def convert(self, scenario):
        lines = scenario.lines
        searches = 0
        i = 0
        search = None  # (query, params) of the step being looked at
        expectations = {}

        def finish():
            nonlocal search, expectations
            if search is not None:
                self.emit(scenario, search, expectations)
            search, expectations = None, {}

        while i < len(lines):
            stripped = lines[i].strip()
            if stripped.startswith("Given"):
                if "HTTP header" in stripped:
                    self.skipped["localisation through HTTP headers"] += 1
                    return
                i += 1
                continue
            match = re.match(r'When geocoding(?: "(.*)")?\s*$', stripped)
            sending = re.match(r"When sending (?:v1/)?search", stripped)
            if match or sending:
                finish()
                searches += 1
                rows, i = table_rows(lines, i + 1)
                params = table_as_dict(rows) or {}
                query = match.group(1) if match and match.group(1) is not None else params.get("q")
                search = (query, params)
                continue
            if stripped.startswith(("Then", "And")) and search is not None:
                body = re.sub(r"^(Then|And)\s+", "", stripped)
                rows, i = table_rows(lines, i + 1)
                # what all results contain, the first one contains as well
                if re.match(r"(result 0|all results) contains?( in field address)?$", body):
                    table = table_as_dict(rows)
                    if table is None:
                        expectations.setdefault("_unsupported", []).append("a table of several results")
                    else:
                        in_address = body.endswith("in field address")
                        for key, value in table.items():
                            expectations.setdefault("_pairs", []).append((key, value, in_address))
                elif re.match(r"(a HTTP 200 is returned|the result is valid \w+)$", body):
                    pass
                else:
                    expectations.setdefault("_other", []).append(body)
                continue
            i += 1
        finish()
        if not searches:
            self.skipped["no free-text search"] += 1

    def emit(self, scenario, search, expectations):
        query, params = search
        if not query:
            self.skipped["structured search without free text"] += 1
            return
        unsupported = sorted(set(params) & UNSUPPORTED_PARAMS)
        if unsupported:
            self.skipped[f"restricted by {', '.join(unsupported)}"] += 1
            return
        if params.get("viewbox") and params.get("bounded") == "1":
            self.skipped["restricted by bounded viewbox"] += 1
            return
        if COORDINATE_IN_TEXT.search(query):
            self.skipped["coordinate in the query text"] += 1
            return
        if "[" in query or re.search(r"\b(in|near)\b", query):
            self.skipped["class filter or special phrase in the query"] += 1
            return

        row = dict.fromkeys(COLUMNS, "")
        row.update(category=f"nominatim.{scenario.feature}", query=" ".join(query.split()),
                   prefix="1", limit="1", context=scenario.title)
        towns = []
        for key, value, in_address in expectations.get("_pairs", []):
            if "!" in key:  # a pattern or a geometry check, not a value
                continue
            field = key[len("address+"):] if key.startswith("address+") else key
            is_address = in_address or key.startswith("address+")
            if field in ("house_number", "housenumber"):
                row["expect_number"] = value
            elif field in ("road", "street"):
                row["expect_street"] = value
            elif field == "postcode":
                row["expect_postcode"] = value
            elif field in TOWN_KEYS and (is_address or field == "city"):
                towns.append(value)
            elif field == "name" and not is_address:
                row["expect_name"] = value
            elif field == "display_name" and not is_address:
                row["expect_name"] = value.split(",")[0].strip()
        if towns:
            row["expect_city"] = "|".join(dict.fromkeys(towns))

        if not any(row[c] for c in ("expect_name", "expect_number", "expect_street", "expect_city", "expect_postcode")):
            if expectations.get("_other"):
                self.skipped["checks all results, another result or their number"] += 1
            else:
                self.skipped["expects only what the index does not carry (category, type, rank, ids)"] += 1
            return

        if params.get("accept-language"):
            row["lang"] = params["accept-language"].split(",")[0].split(";")[0].strip()
        if params.get("viewbox"):
            x1, y1, x2, y2 = (float(v) for v in params["viewbox"].split(","))
            row["lat"], row["lon"] = f"{(y1 + y2) / 2:.6f}", f"{(x1 + x2) / 2:.6f}"
        key = tuple(row[c] for c in COLUMNS if c != "context")
        if key in self.seen:
            # an Outline repeated once per output format asks the same question again
            self.skipped["the same question again (an Outline over output formats)"] += 1
            return
        self.seen.add(key)
        self.rows.append(row)


def main():
    if len(sys.argv) < 2:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    converter = Converter()
    scenarios = 0
    for name in sys.argv[1:]:
        for scenario in read_features(Path(name)):
            scenarios += 1
            converter.convert(scenario)

    print("# converted from Nominatim's BDD search features by convert_nominatim.py; see fetch.sh")
    print("\t".join(COLUMNS))
    for row in converter.rows:
        print("\t".join(row[c].replace("\t", " ") for c in COLUMNS))

    print(f"nominatim: {scenarios} scenarios, {len(converter.rows)} queries kept", file=sys.stderr)
    for reason, count in converter.skipped.most_common():
        print(f"  passed over: {count:4d}  {reason}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

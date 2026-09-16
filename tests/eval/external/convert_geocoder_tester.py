#!/usr/bin/env python3
"""Turn geocoder-tester's search tests into query files for geo_eval.

    python3 convert_geocoder_tester.py <geocoder_tester/world> <out dir> [--with-poi]

One file per country comes out — build/geocoder-tester-germany.tsv, …-france.tsv — and
the tests at the top of world/ go to …-world.tsv.  The category of a query is
`gt-<country>.<path>`, so geo_eval groups by country and lists every source file.

What a test expects is carried over field by field, the way geocoder-tester's
check_results() reads it: every expected key has to hold on one answer within the
test's limit, which is 1 where the test names none.

  expected_name        → expect_name
  expected_housenumber → expect_number
  expected_street      → expect_street
  expected_city        → expect_city
  expected_postcode    → expect_postcode
  expected_coordinate  → expect_lat, expect_lon, radius_m   ("lat,lon,metres")

What the index cannot answer is left out, and counted on stderr with its reason:

  * keys the index does not carry — osm_id, osm_key, osm_value, type, country.  A test
    left with no key the index carries is passed over whole; one that keeps a name or
    a town keeps being asked.
  * reverse geocoding — a test without a query.
  * tests written as Python, which name their places by OSM id.
  * points of interest — airports, museums, stations — which the index does not hold
    as places of their own.  --with-poi keeps them; they then mostly count as absent.

Every query is asked with the last word read as a beginning as well, as the map in
production asks it.  A position is passed on where the test gives one, and a language
likewise — where a test names none, the suite's own country says which reading its
expectations are written in (see SUITE_LANGUAGE).
YAML tests need PyYAML; without it they are passed over, and stderr says so.

Standard library otherwise.
"""

import argparse
import collections
import csv
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # the CSV tests are most of them; say what is missing and go on
    yaml = None

COLUMNS = [
    "category", "query", "prefix", "lat", "lon", "lang", "limit",
    "expect_name", "expect_number", "expect_street", "expect_city", "expect_postcode",
    "expect_lat", "expect_lon", "radius_m", "context",
]

# geocoder-tester's key → ours
CARRIED = {
    "name": "expect_name",
    "housenumber": "expect_number",
    "street": "expect_street",
    "city": "expect_city",
    "postcode": "expect_postcode",
}
# keys the index has no field for
DROPPED = {"osm_id", "osm_key", "osm_value", "type", "country", "label", "state", "district", "county"}

# The reading a suite writes its expectations in.  An index built with several
# languages answers in the first of them — the German planet index calls
# Strasbourg "Straßburg" — while the French suite expects "Strasbourg", so every
# such test would count as a miss over a spelling.  A language the index does not
# hold is passed over by the search, which then answers in its default reading,
# so naming one here costs nothing where it is not there.
SUITE_LANGUAGE = {
    "france": "fr",
    "germany": "de",
    "italy": "it",
    "netherlands": "nl",
    "poland": "pl",
    "usa": "en",
    "world": "en",
}

POI_PARTS = {"poi"}
POI_STEMS = {"test_airports", "test_museum", "test_wikivoyage_fr", "test_pois", "test_train_stations",
             "test_public_culture_buildings", "test_theatre_paris", "test_tourist"}


def clean(value):
    return " ".join(str(value).split()) if value is not None else ""


def coordinate(text):
    """`lat,lon,metres` → three strings, or None where the text is not one."""
    parts = [p.strip() for p in str(text).split(",")]
    if len(parts) != 3:
        return None
    try:
        float(parts[0]), float(parts[1]), float(parts[2])
    except ValueError:
        return None
    return parts


class Converter:
    def __init__(self, with_poi):
        self.with_poi = with_poi
        self.rows = collections.defaultdict(list)  # country → rows
        self.skipped = collections.Counter()
        self.dropped_keys = collections.Counter()
        self.kept = 0

    def category(self, world, path):
        relative = path.relative_to(world)
        parts = list(relative.parts)
        country = parts[0] if len(parts) > 1 else "world"
        inner = parts[1:-1] if len(parts) > 1 else []
        stem = path.stem[5:] if path.stem.startswith("test_") else path.stem
        return country, ".".join([f"gt-{country}", *inner, stem])

    def is_poi(self, world, path):
        return bool(POI_PARTS & set(path.relative_to(world).parts)) or path.stem in POI_STEMS

    def add(self, country, category, query, expected, lat=None, lon=None, lang=None, limit=None,
            skip=None, comment=None):
        """One test; `expected` maps geocoder-tester's keys to their values."""
        if skip:
            self.skipped["marked skip in the suite"] += 1
            return
        query = clean(query)
        if not query:
            self.skipped["reverse geocoding (no query)"] += 1
            return

        row = dict.fromkeys(COLUMNS, "")
        row.update(category=category, query=query, prefix="1", context=clean(comment))
        for key, value in expected.items():
            value = clean(value)
            if not value:
                continue
            if key in CARRIED:
                row[CARRIED[key]] = value
            elif key == "coordinate":
                point = coordinate(value)
                if point is None:
                    self.skipped["unreadable coordinate"] += 1
                    return
                row["expect_lat"], row["expect_lon"], row["radius_m"] = point
            else:
                self.dropped_keys[key] += 1

        if not any(row[c] for c in ("expect_name", "expect_number", "expect_street", "expect_city",
                                    "expect_postcode", "expect_lat")):
            self.skipped["expects only what the index does not carry (osm_id, country, …)"] += 1
            return
        if clean(lat) and clean(lon):
            row["lat"], row["lon"] = clean(lat), clean(lon)
        row["lang"] = clean(lang) or SUITE_LANGUAGE.get(country, "")
        if clean(limit):
            row["limit"] = clean(limit)
        self.rows[country].append(row)
        self.kept += 1

    def read_csv(self, world, path):
        country, category = self.category(world, path)
        with path.open(encoding="utf-8", newline="") as handle:
            sample = handle.read(4096)
            handle.seek(0)
            try:
                dialect = csv.Sniffer().sniff(sample, delimiters=",;")
            except csv.Error:
                dialect = csv.excel
            for row in csv.DictReader(handle, dialect=dialect):
                expected = {k[len("expected_"):]: v for k, v in row.items() if k and k.startswith("expected_")}
                # test_basic.csv writes the coordinate and its tolerance apart, under plural names
                if expected.get("coordinates"):
                    expected["coordinate"] = f"{expected.pop('coordinates')},{expected.pop('location_tolerance', '') or 1000}"
                expected.pop("location_tolerance", None)
                self.add(country, category, row.get("query"), expected,
                         lat=row.get("lat"), lon=row.get("lon"),
                         lang=row.get("lang") or row.get("tried_language"),
                         limit=row.get("limit"), skip=row.get("skip"), comment=row.get("comment"))

    def read_yaml(self, world, path):
        if yaml is None:
            self.skipped["YAML test file, PyYAML not installed"] += 1
            return
        country, category = self.category(world, path)
        tests = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        for name, spec in tests.items():
            spec = spec or {}
            expected = spec.get("expected") or {}
            # a list of expectations: each has to be found, so each is asked as a query of its own
            for one in expected if isinstance(expected, list) else [expected]:
                self.add(country, category, spec.get("query", name), one,
                         lat=spec.get("lat"), lon=spec.get("lon"), lang=spec.get("lang"),
                         limit=spec.get("limit"), skip=spec.get("skip"), comment=spec.get("comment"))

    def walk(self, world):
        for path in sorted(world.rglob("test*")):
            if not path.is_file():
                continue
            if not self.with_poi and self.is_poi(world, path):
                self.skipped[f"point of interest file ({path.suffix})"] += 1
                continue
            if path.suffix == ".csv":
                self.read_csv(world, path)
            elif path.suffix == ".yml":
                self.read_yaml(world, path)
            elif path.suffix == ".py":
                self.skipped["Python test file (places named by OSM id)"] += 1

    def write(self, out):
        out.mkdir(parents=True, exist_ok=True)
        for country, rows in sorted(self.rows.items()):
            target = out / f"geocoder-tester-{country}.tsv"
            with target.open("w", encoding="utf-8") as handle:
                handle.write("# converted from geocoder-tester by convert_geocoder_tester.py; see fetch.sh\n")
                handle.write("\t".join(COLUMNS) + "\n")
                for row in rows:
                    handle.write("\t".join(row[c].replace("\t", " ") for c in COLUMNS) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("world", type=Path, help="geocoder_tester/world of a checkout")
    parser.add_argument("out", type=Path, help="directory the query files are written to")
    parser.add_argument("--with-poi", action="store_true", help="keep points of interest")
    args = parser.parse_args()

    converter = Converter(args.with_poi)
    converter.walk(args.world)
    converter.write(args.out)

    print(f"geocoder-tester: {converter.kept} queries kept", file=sys.stderr)
    for reason, count in converter.skipped.most_common():
        print(f"  passed over: {count:6d}  {reason}", file=sys.stderr)
    for key, count in converter.dropped_keys.most_common():
        print(f"  key dropped: {count:6d}  expected_{key}", file=sys.stderr)


if __name__ == "__main__":
    main()

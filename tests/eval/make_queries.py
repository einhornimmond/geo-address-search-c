#!/usr/bin/env python3
"""Draw search targets from a Photon dump and write the queries people would type for them.

    zstdcat photon-dump-germany-1.0-latest.jsonl.zst | python3 tests/eval/make_queries.py > tests/eval/queries.tsv

The truth comes from the dump, not from an index: every target is an entry the dump
holds — an address, a street, a town, a quarter — with the coordinate the dump gives
it.  Asking an index which answer is right would only write down what it already
answers, and a ranking change could never show up as better or worse.

What is drawn is fixed by the seed, so the same dump gives the same file.  Every
query is asked the way the map in production asks it: with the last word read as a
beginning as well, and with a position — either near the target, as someone looking
at the place on the map, or in a large city at least 150 km away, as someone who has
just opened the map somewhere else.

Standard library only; the dump arrives on stdin.
"""

import argparse
import json
import math
import random
import sys
import unicodedata

# ---------------------------------------------------------------------------
#  What is drawn
# ---------------------------------------------------------------------------

ADDRESS_TARGETS = 50
STREET_TARGETS = 30
TOWN_TARGETS_PER_SIZE = 15  # large, medium, small
DISTRICT_TARGETS = 20

# How close a result has to stand to count as the target, in metres.  An address is a
# door; a street is merged from its segments and its centre moves; a town is a point
# the dump sets somewhere in its middle, and a county-filed city stands elsewhere
# than the town record of the same name.
RADIUS_ADDRESS = 150
RADIUS_STREET = 3000
RADIUS_TOWN = 8000
RADIUS_DISTRICT = 3000

# Where someone might have opened the map, far away from what they are looking for.
FAR_VIEWPOINTS = [
    ("Berlin", 52.5200, 13.4050),
    ("Hamburg", 53.5511, 9.9937),
    ("München", 48.1374, 11.5755),
    ("Köln", 50.9375, 6.9603),
    ("Frankfurt am Main", 50.1109, 8.6821),
    ("Stuttgart", 48.7758, 9.1829),
    ("Leipzig", 51.3397, 12.3731),
    ("Hannover", 52.3759, 9.7320),
    ("Nürnberg", 49.4521, 11.0767),
]
FAR_MIN_KM = 150

STREET_KINDS = {"residential", "living_street", "tertiary", "secondary", "unclassified", "primary", "pedestrian"}
TOWN_KINDS = {"village", "town", "city", "municipality"}


# ---------------------------------------------------------------------------
#  Sampling
# ---------------------------------------------------------------------------


class Reservoir:
    """Algorithm R: a uniform sample of `size` out of a stream of unknown length."""

    def __init__(self, size, rng):
        self.size = size
        self.rng = rng
        self.items = []
        self.seen = 0

    def offer(self, item):
        self.seen += 1
        if len(self.items) < self.size:
            self.items.append(item)
            return
        slot = self.rng.randrange(self.seen)
        if slot < self.size:
            self.items[slot] = item


def plain_housenumber(number):
    """A single door: digits, perhaps one letter.  Ranges and lists are left out."""
    if not number or len(number) > 5:
        return False
    digits = number.rstrip("abcdefghABCDEFGH")
    return digits.isdigit() and len(number) - len(digits) <= 1


def centroid(entry):
    point = entry.get("centroid")
    if not point or len(point) != 2:
        return None
    return float(point[1]), float(point[0])  # the dump writes [lon, lat]


def read_targets(stream, rng):
    """One walk over the dump; returns the drawn targets by kind."""
    # Most entries are addresses and streets, and parsing every one of them in Python
    # would take hours.  A cheap coin toss before the JSON keeps a few thousand
    # candidates of each, spread over the whole file, and the reservoir picks from those.
    addresses = Reservoir(ADDRESS_TARGETS, rng)
    streets = Reservoir(STREET_TARGETS, rng)
    districts = Reservoir(DISTRICT_TARGETS, rng)
    towns = []

    for raw in stream:
        if b'"type":"Place"' not in raw[:24]:
            continue
        if b'"housenumber"' in raw:
            if rng.random() >= 0.0005:
                continue
            kind = "address"
        elif b'"address_type":"street"' in raw:
            if rng.random() >= 0.0005:
                continue
            kind = "street"
        elif b'"address_type":"city"' in raw or b'"address_type":"county"' in raw:
            kind = "town"
        elif b'"address_type":"district"' in raw and b'"osm_value":"suburb"' in raw:
            if rng.random() >= 0.05:
                continue
            kind = "district"
        else:
            continue

        try:
            place = json.loads(raw)["content"][0]
        except (ValueError, KeyError, IndexError):
            continue
        point = centroid(place)
        if point is None or place.get("country_code") != "de":
            continue
        address = place.get("address", {})
        name = place.get("name", {}).get("name")

        if kind == "address":
            street = address.get("street")
            number = place.get("housenumber")
            city = address.get("city")
            postcode = place.get("postcode")
            if place.get("address_type") != "house" or not (street and city and postcode):
                continue
            if not plain_housenumber(number) or not (len(postcode) == 5 and postcode.isdigit()):
                continue
            addresses.offer({
                "street": street, "number": number, "postcode": postcode, "city": city,
                "suburb": address.get("suburb"), "lat": point[0], "lon": point[1],
            })
        elif kind == "street":
            city = address.get("city")
            if place.get("osm_value") not in STREET_KINDS or not (name and city):
                continue
            streets.offer({"name": name, "city": city, "lat": point[0], "lon": point[1]})
        elif kind == "town":
            value = place.get("osm_value")
            if place.get("address_type") == "county":
                # a kreisfreie Stadt is filed as a county; a Landkreis is not a town
                lowered = (name or "").lower()
                if value != "administrative" or "kreis" in lowered or "region" in lowered:
                    continue
            elif value not in TOWN_KINDS and value != "administrative":
                continue
            if not name:
                continue
            towns.append({
                "name": name, "importance": float(place.get("importance") or 0),
                "lat": point[0], "lon": point[1],
            })
        elif kind == "district":
            city = address.get("city")
            if not (name and city) or name == city:
                continue
            districts.offer({"name": name, "city": city, "lat": point[0], "lon": point[1]})

    return addresses.items, streets.items, towns, districts.items


def split_towns(towns, rng):
    """Large, medium and small towns, by the weight the dump gives them."""
    # one record per name and place; a town filed twice would otherwise be drawn twice
    unique = {}
    for town in towns:
        key = (town["name"], round(town["lat"], 1), round(town["lon"], 1))
        if key not in unique or town["importance"] > unique[key]["importance"]:
            unique[key] = town
    ordered = sorted(unique.values(), key=lambda t: (-t["importance"], t["name"]))
    n = len(ordered)
    large = ordered[: max(TOWN_TARGETS_PER_SIZE * 3, n // 100)]
    medium = ordered[len(large): n // 5]
    small = ordered[n // 5:]
    picks = []
    for size, pool in (("large", large), ("medium", medium), ("small", small)):
        for town in rng.sample(pool, min(TOWN_TARGETS_PER_SIZE, len(pool))):
            picks.append((size, town))
    return picks


# ---------------------------------------------------------------------------
#  What people type
# ---------------------------------------------------------------------------


def distance_km(lat1, lon1, lat2, lon2):
    rad = math.pi / 180
    x = (lon2 - lon1) * rad * math.cos((lat1 + lat2) / 2 * rad)
    y = (lat2 - lat1) * rad
    return math.hypot(x, y) * 6371.0


def near(rng, lat, lon, km):
    """A point up to `km` from the target, as the centre of a map looking at it."""
    angle = rng.random() * 2 * math.pi
    reach = rng.random() * km
    return (
        lat + reach / 111.32 * math.sin(angle),
        lon + reach / (111.32 * math.cos(lat * math.pi / 180)) * math.cos(angle),
    )


def far(rng, lat, lon):
    """A large city at least FAR_MIN_KM away, as a map opened somewhere else."""
    choices = [v for v in FAR_VIEWPOINTS if distance_km(lat, lon, v[1], v[2]) >= FAR_MIN_KM]
    _, vlat, vlon = rng.choice(choices)
    return vlat, vlon


def abbreviated(street):
    for long, short in (("straße", "str."), ("Straße", "Str."), ("strasse", "str."), ("Strasse", "Str.")):
        if street.endswith(long):
            return street[: -len(long)] + short
    return None


def without_umlauts(text):
    for umlaut, plain in (("ä", "ae"), ("ö", "oe"), ("ü", "ue"), ("Ä", "Ae"), ("Ö", "Oe"), ("Ü", "Ue"), ("ß", "ss")):
        text = text.replace(umlaut, plain)
    return text


def typing(query):
    """The query as it stands while the last word is still being typed."""
    head, _, last = query.rpartition(" ")
    if len(last) < 6:
        return None
    cut = last[: max(3, math.ceil(len(last) * 0.6))]
    return (head + " " + cut).strip()


def typo(rng, word):
    """Two neighbouring letters swapped inside the word, as a fast finger does."""
    letters = [i for i in range(1, len(word) - 2) if word[i].isalpha() and word[i + 1].isalpha() and word[i] != word[i + 1]]
    if len(word) < 6 or not letters:
        return None
    i = rng.choice(letters)
    return word[:i] + word[i + 1] + word[i] + word[i + 2:]


def spoken(name):
    """A town as it is typed: *Cottbus* for "Cottbus - Chóśebuz", *Weißwasser* for "Weißwasser/O.L."."""
    for separator in (" - ", "/"):
        if separator in name:
            name = name.split(separator)[0]
    return name.strip()


def clean(text):
    return unicodedata.normalize("NFC", str(text)).replace("\t", " ").replace("\n", " ")


def row(category, query, position, expect_name, expect_number, target, radius, context):
    lat, lon = position if position else ("", "")
    fields = [
        category, query, "1",
        f"{lat:.6f}" if lat != "" else "", f"{lon:.6f}" if lon != "" else "",
        expect_name, expect_number or "", f"{target['lat']:.7f}", f"{target['lon']:.7f}",
        str(radius), context,
    ]
    return "\t".join(clean(f) for f in fields)


def queries_for_address(rng, a):
    street, number, postcode, city = a["street"], a["number"], a["postcode"], spoken(a["city"])
    full = f"{street} {number}, {postcode} {city}"
    here = (a["lat"], a["lon"])
    context = f"{postcode} {city}"
    rows = [
        row("address.full.near", full, near(rng, *here, 2), street, number, a, RADIUS_ADDRESS, context),
        row("address.full.far", full, far(rng, *here), street, number, a, RADIUS_ADDRESS, context),
        row("address.no_postcode.far", f"{street} {number} {city}", far(rng, *here), street, number, a, RADIUS_ADDRESS, context),
    ]
    short = abbreviated(street)
    if short:
        rows.append(row("address.abbreviated.near", without_umlauts(f"{short} {number} {city}").lower(),
                        near(rng, *here, 2), street, number, a, RADIUS_ADDRESS, context))
    typed = typing(f"{street} {number} {city}")
    if typed:
        rows.append(row("address.typing.near", typed, near(rng, *here, 2), street, number, a, RADIUS_ADDRESS, context))
    if a["suburb"] and a["suburb"] != a["city"] and spoken(a["suburb"]) != city:
        rows.append(row("address.suburb.far", f"{street} {number} {spoken(a['suburb'])}", far(rng, *here),
                        street, number, a, RADIUS_ADDRESS, context))
    wrong = typo(rng, street)
    if wrong:
        rows.append(row("address.typo.near", f"{wrong} {number} {city}", near(rng, *here, 2),
                        street, number, a, RADIUS_ADDRESS, context))
    return rows


def queries_for_street(rng, s):
    here = (s["lat"], s["lon"])
    city = spoken(s["city"])
    rows = [
        row("street.city.far", f"{s['name']} {city}", far(rng, *here), s["name"], None, s, RADIUS_STREET, s["city"]),
        row("street.alone.near", s["name"], near(rng, *here, 0.5), s["name"], None, s, RADIUS_STREET, s["city"]),
    ]
    typed = typing(f"{s['name']} {city}")
    if typed:
        rows.append(row("street.typing.far", typed, far(rng, *here), s["name"], None, s, RADIUS_STREET, s["city"]))
    return rows


def queries_for_town(rng, size, t):
    here = (t["lat"], t["lon"])
    context = f"importance {t['importance']:.3f}"
    rows = [
        row(f"town.{size}.far", t["name"], far(rng, *here), t["name"], None, t, RADIUS_TOWN, context),
        row(f"town.{size}.near", t["name"], near(rng, *here, 5), t["name"], None, t, RADIUS_TOWN, context),
    ]
    if len(t["name"]) >= 6 and " " not in t["name"]:
        cut = t["name"][: max(4, math.ceil(len(t["name"]) * 0.7))]
        rows.append(row("town.typing.far", cut, far(rng, *here), t["name"], None, t, RADIUS_TOWN, context))
    wrong = typo(rng, t["name"])
    if wrong and " " not in t["name"]:
        rows.append(row("town.typo.far", wrong, far(rng, *here), t["name"], None, t, RADIUS_TOWN, context))
    return rows


def queries_for_district(rng, d):
    here = (d["lat"], d["lon"])
    return [
        row("district.city.far", f"{d['name']} {spoken(d['city'])}", far(rng, *here), d["name"], None, d, RADIUS_DISTRICT, d["city"]),
        row("district.alone.near", d["name"], near(rng, *here, 3), d["name"], None, d, RADIUS_DISTRICT, d["city"]),
    ]


HEADER = "\t".join([
    "category", "query", "prefix", "lat", "lon",
    "expect_name", "expect_number", "expect_lat", "expect_lon", "radius_m", "context",
])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--seed", type=int, default=12, help="fixes what is drawn (default: 12)")
    args = parser.parse_args()
    rng = random.Random(args.seed)

    addresses, streets, towns, districts = read_targets(sys.stdin.buffer, rng)
    print(f"drew {len(addresses)} addresses, {len(streets)} streets, {len(towns)} towns seen, "
          f"{len(districts)} districts", file=sys.stderr)

    print(f"# generated by make_queries.py --seed {args.seed}; do not edit by hand, see Readme.md")
    print(HEADER)
    for a in sorted(addresses, key=lambda a: (a["city"], a["street"], a["number"])):
        print("\n".join(queries_for_address(rng, a)))
    for s in sorted(streets, key=lambda s: (s["city"], s["name"])):
        print("\n".join(queries_for_street(rng, s)))
    for size, t in split_towns(towns, rng):
        print("\n".join(queries_for_town(rng, size, t)))
    for d in sorted(districts, key=lambda d: (d["city"], d["name"])):
        print("\n".join(queries_for_district(rng, d)))


if __name__ == "__main__":
    main()

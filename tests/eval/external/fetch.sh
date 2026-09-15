#!/usr/bin/env bash
#
# Fetch other geocoders' search suites and convert them into query files for geo_eval.
#
# Nothing of them is kept in this repository.  They are fetched at the commits named
# below — so the same commit of this repository always measures against the same
# tests — into download/, converted into build/, and both are ignored by git:
#
#   * geocoder-tester, the suite Photon and Nominatim are measured with, as a tarball
#     of about a megabyte.  MIT for the tool; its README names ODbL and CC0 for some of
#     the data.
#   * the search features of Nominatim's own BDD tests, seven files out of a 160 MB
#     repository.  GPL-3.0, which is one more reason not to copy them in.
#
# Usage:
#   tests/eval/external/fetch.sh               fetch what is missing, then convert
#   tests/eval/external/fetch.sh --with-poi    keep geocoder-tester's points of interest
#
# Then:
#   zig-out/bin/geo_eval planet.gdx tests/eval/external/build/*.tsv
#
# To move to newer tests, change a commit below and run again; the old download stays
# beside the new one until download/ is removed.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

GEOCODER_TESTER_COMMIT=5384d1534bc3c59e8d280be3d951a92356ce470b # 2026-07-31
NOMINATIM_COMMIT=12a56d207c2b549f898c09ee538d1f99c0fb4324       # 2026-09-11
NOMINATIM_FEATURES=(language params postcode queries simple structured v1_geocodejson)

options=()
for argument in "$@"; do
  case "$argument" in
    --with-poi) options+=(--with-poi) ;;
    -h|--help)
      awk 'NR>2 && /^#/ { sub(/^# ?/, ""); print; next } NR>2 { exit }' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "unknown option: $argument" >&2
      exit 2
      ;;
  esac
done

mkdir -p download build

tester="download/geocoder-tester-$GEOCODER_TESTER_COMMIT"
if [ ! -d "$tester" ]; then
  echo "fetching geocoder-tester at ${GEOCODER_TESTER_COMMIT:0:12}"
  curl -fsSL "https://github.com/geocoders/geocoder-tester/archive/$GEOCODER_TESTER_COMMIT.tar.gz" \
    | tar -xz -C download
fi

nominatim="download/nominatim-$NOMINATIM_COMMIT"
if [ ! -d "$nominatim" ]; then
  echo "fetching Nominatim's search features at ${NOMINATIM_COMMIT:0:12}"
  mkdir -p "$nominatim.partial"
  for feature in "${NOMINATIM_FEATURES[@]}"; do
    curl -fsSL -o "$nominatim.partial/$feature.feature" \
      "https://raw.githubusercontent.com/osm-search/Nominatim/$NOMINATIM_COMMIT/test/bdd/features/api/search/$feature.feature"
  done
  # renamed only once complete, so an interrupted fetch is not taken for a whole one
  mv "$nominatim.partial" "$nominatim"
fi

rm -f build/*.tsv
python3 convert_geocoder_tester.py "${options[@]}" "$tester/geocoder_tester/world" build
python3 convert_nominatim.py "$nominatim"/*.feature > build/nominatim.tsv
echo "query files in $(pwd)/build:"
wc -l build/*.tsv

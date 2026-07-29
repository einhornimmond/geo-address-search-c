#include "parser/json_stats.h"
#include "foundation/error.h"
#include "parser/json_parse.h"
#include <inttypes.h>
#include <stdio.h>

static void count_address_type(JsonStats *stats, PhotonPlaceType address_type) {
  switch (address_type) {
  case PHOTON_PLACE_TYPE_HOUSE:
    ++stats->houses;
    break;
  case PHOTON_PLACE_TYPE_STREET:
    ++stats->streets;
    break;
  case PHOTON_PLACE_TYPE_CITY:
    ++stats->cities;
    break;
  case PHOTON_PLACE_TYPE_STATE:
    ++stats->states;
    break;
  case PHOTON_PLACE_TYPE_COUNTY:
    ++stats->counties;
    break;
  case PHOTON_PLACE_TYPE_LOCALITY:
    ++stats->localities;
    break;
  case PHOTON_PLACE_TYPE_DISTRICT:
    ++stats->districts;
    break;
  case PHOTON_PLACE_TYPE_COUNTRY:
    ++stats->countries;
    break;
  case PHOTON_PLACE_TYPE_OTHER:
    ++stats->other;
    break;
  case PHOTON_PLACE_TYPE_STATE_COUNTY_CITY:
    ++stats->state_cities;
    break;
  case PHOTON_PLACE_TYPE_INDEPENDENT_CITY:
    ++stats->independent_cities;
    break;
  default:
    fatal(ERROR_ASSERT, "None or Unknown address type");
  }
}

void json_stats_count_document(JsonStats *stats, const void *result) {
  const JsonParseResult *r = result;
  ++stats->records;
  if (!r->is_valid) {
    ++stats->invalid_records;
    return;
  }
  if (!r->is_place) return;
  ++stats->place_records;
  stats->place_entries += r->entry_count;
}

void json_stats_count_place(JsonStats *stats, const PhotonPlace *place) {
  count_address_type(stats, place->typeEnum);
  stats->search_terms += place->search_count;
  stats->search_dropped += place->search_dropped;
}

void json_stats_add(JsonStats *total, const JsonStats *addend) {
  total->records += addend->records;
  total->place_records += addend->place_records;
  total->place_entries += addend->place_entries;
  total->countries += addend->countries;
  total->states += addend->states;
  total->counties += addend->counties;
  total->cities += addend->cities;
  total->localities += addend->localities;
  total->districts += addend->districts;
  total->streets += addend->streets;
  total->houses += addend->houses;
  total->other += addend->other;
  total->search_terms += addend->search_terms;
  total->search_dropped += addend->search_dropped;
  total->state_cities += addend->state_cities;
  total->independent_cities += addend->independent_cities;
  total->invalid_records += addend->invalid_records;
  if (addend->postcode_checked) total->postcode_checked = 1;
}

void json_stats_print(const JsonStats *stats) {
  printf("\nAddress hierarchy kept:\n");
  printf("  countries:      %" PRIu64 "\n", stats->countries);
  printf("  states:         %" PRIu64 "\n", stats->states);
  printf("  counties:       %" PRIu64 "\n", stats->counties);
  printf("  cities:         %" PRIu64 "\n", stats->cities);
  printf("  districts:      %" PRIu64 "\n", stats->districts);
  printf("  localities:     %" PRIu64 "\n", stats->localities);
  printf("  streets:        %" PRIu64 "\n", stats->streets);
  printf("  addresses:      %" PRIu64 "\n", stats->houses);
  printf("  other:          %" PRIu64 "\n", stats->other);
  printf(
      "\nPlace objects: %" PRIu64 ", entries kept: %" PRIu64 ", JSON records: %" PRIu64 "\n",
      stats->place_records, stats->place_entries, stats->records
  );
  if (stats->invalid_records) {
    printf("Malformed or incomplete entries: %" PRIu64 "\n", stats->invalid_records);
  }
  if (stats->search_dropped) {
    printf(
        "Search terms cut off: %" PRIu64 " of %" PRIu64 " — raise PHOTON_PLACE_SEARCH_MAX\n",
        stats->search_dropped, stats->search_terms + stats->search_dropped
    );
  }
}

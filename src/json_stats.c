#include "json_stats.h"
#include "json_parse.h"
#include "error.h"
#include <inttypes.h>
#include <stdio.h>

static void count_address_type(JsonStats *stats, PhotonPlaceType address_type) {
  switch(address_type) {
    case PHOTON_PLACE_TYPE_HOUSE: ++stats->houses; break;
    case PHOTON_PLACE_TYPE_STREET: ++stats->streets; break;
    case PHOTON_PLACE_TYPE_CITY: ++stats->cities; break;
    case PHOTON_PLACE_TYPE_STATE: ++stats->states; break;
    case PHOTON_PLACE_TYPE_COUNTY: ++stats->counties; break;
    case PHOTON_PLACE_TYPE_LOCALITY: ++stats->localities; break;
    case PHOTON_PLACE_TYPE_DISTRICT: ++stats->districts; break;
    case PHOTON_PLACE_TYPE_COUNTRY: ++stats->countries; break;
    case PHOTON_PLACE_TYPE_OTHER: ++stats->other; break;
    default: fatal(ERROR_ASSERT, "None or Unknown address type");
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

  /* --- postcode sanity check (fatal if missing after threshold) --- */
  if (!stats->postcode_checked) {
    if (place->postcode) {
      stats->postcode_checked = 1;
    } else if (stats->place_entries > 50000) {
      /*fatal(
          ERROR_JSON,
          "postcode field not found in Photon data after %" PRIu64
          " entries – dataset may lack postcode information.",
          stats->place_entries
          );*/
    }
  }
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
  total->invalid_records += addend->invalid_records;
  if (addend->postcode_checked) total->postcode_checked = 1;
}

void json_stats_print(const JsonStats *stats) {
  printf("\nGespeicherte Adresshierarchie:\n");
  printf("  Länder:         %" PRIu64 "\n", stats->countries);
  printf("  Bundesländer:   %" PRIu64 "\n", stats->states);
  printf("  Landkreise:     %" PRIu64 "\n", stats->counties);
  printf("  Städte:         %" PRIu64 "\n", stats->cities);
  printf("  Bezirke:        %" PRIu64 "\n", stats->districts);
  printf("  Locales:        %" PRIu64 "\n", stats->localities);
  printf("  Straßen:        %" PRIu64 "\n", stats->streets);
  printf("  Adressen:       %" PRIu64 "\n", stats->houses);
  printf("  Sonstige:  %" PRIu64 "\n", stats->other);
  printf(
      "\nPlace-Objekte: %" PRIu64 ", gespeicherte Einträge: %" PRIu64 ", JSON-Datensätze: %" PRIu64
      "\n",
      stats->place_records, stats->place_entries, stats->records
  );
  if (stats->invalid_records) {
    printf("Ungültige oder unvollständige Einträge: %" PRIu64 "\n", stats->invalid_records);
  }
}

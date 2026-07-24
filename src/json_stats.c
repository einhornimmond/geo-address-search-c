#include "json_stats.h"
#include "error.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void count_address_type(JsonStats *stats, const char *address_type) {
  if (strcmp(address_type, "country") == 0) {
    ++stats->countries;
  } else if (strcmp(address_type, "state") == 0) {
    ++stats->states;
  } else if (strcmp(address_type, "county") == 0) {
    ++stats->counties;
  } else if (strcmp(address_type, "city") == 0) {
    ++stats->cities;
  } else if (strcmp(address_type, "street") == 0) {
    ++stats->streets;
  } else if (strcmp(address_type, "house") == 0) {
    ++stats->houses;
  } else {
    ++stats->other;
  }
}

void json_stats_record(JsonStats *stats, yyjson_val *root) {
  static int postcode_found = 0;

  ++stats->records;
  if (!yyjson_is_obj(root)) {
    ++stats->invalid_records;
    return;
  }

  yyjson_val *type = yyjson_obj_get(root, "type");
  if (!yyjson_is_str(type) || strcmp(yyjson_get_str(type), "Place") != 0) { return; }
  ++stats->place_records;

  yyjson_val *content = yyjson_obj_get(root, "content");
  if (!yyjson_is_arr(content)) {
    ++stats->invalid_records;
    return;
  }

  size_t index, max;
  yyjson_val *entry;
  yyjson_arr_foreach(content, index, max, entry) {
    if (!yyjson_is_obj(entry)) {
      ++stats->invalid_records;
      continue;
    }
    ++stats->place_entries;

    yyjson_val *address_type = yyjson_obj_get(entry, "address_type");
    if (!yyjson_is_str(address_type)) {
      ++stats->other;
      continue;
    }
    count_address_type(stats, yyjson_get_str(address_type));

    if (!postcode_found) {
      yyjson_val *postcode = yyjson_obj_get(entry, "postcode");
      if (!yyjson_is_str(postcode)) {
        yyjson_val *addr = yyjson_obj_get(entry, "address");
        if (yyjson_is_obj(addr)) { postcode = yyjson_obj_get(addr, "postcode"); }
      }
      if (yyjson_is_str(postcode)) {
        postcode_found = 1;
      } else if (stats->place_entries > 50000) {
        fatal(
            ERROR_JSON,
            "postcode field not found in Photon data after %" PRIu64
            " entries – dataset may lack postcode information.",
            stats->place_entries
        );
      }
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
  total->streets += addend->streets;
  total->houses += addend->houses;
  total->other += addend->other;
  total->invalid_records += addend->invalid_records;
}

void json_stats_print(const JsonStats *stats) {
  printf("\nGespeicherte Adresshierarchie:\n");
  printf("  Länder:     %" PRIu64 "\n", stats->countries);
  printf("  Bundesländer: %" PRIu64 "\n", stats->states);
  printf("  Landkreise: %" PRIu64 "\n", stats->counties);
  printf("  Städte:     %" PRIu64 "\n", stats->cities);
  printf("  Straßen:    %" PRIu64 "\n", stats->streets);
  printf("  Adressen:  %" PRIu64 "\n", stats->houses);
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

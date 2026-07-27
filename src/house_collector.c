/** @cond INTERNAL */

#include "house_collector.h"

#include <stdlib.h>
#include <string.h>

GRDU_BVEC_DEFINE(house_vec, HouseEntry, 11, )

/* =========================================================================
 *  Per-thread collecting
 * ========================================================================= */

grd_result house_collector_init(HouseCollector *collector) {
  if (!collector) return GRD_ERROR_NULL_POINTER;
  collector->homeless = 0;
  collector->pointless = 0;
  collector->without_number = 0;
  collector->unknown_street = 0;
  collector->unknown_key = 0;
  collector->recovered_city = 0;
  collector->recovered_postcode = 0;
  collector->recovered_nearest = 0;
  return house_vec_init(&collector->houses, NULL);
}

void house_collector_free(HouseCollector *collector) {
  if (!collector) return;
  house_vec_free(&collector->houses);
}

grd_result house_collector_add(
    HouseCollector *collector,
    uint32_t document,
    const GeoDocument *street,
    uint32_t number_rank,
    int32_t lat_e7,
    int32_t lon_e7,
    int has_point
) {
  if (!collector || !street) return GRD_ERROR_NULL_POINTER;

  HouseEntry entry = {
      .document = document,
      .house = {.number_rank = number_rank, .lat_e7 = lat_e7, .lon_e7 = lon_e7},
  };
  /* a house without a coordinate simply stands where its street stands */
  if (!has_point) {
    ++collector->pointless;
    entry.house.lat_e7 = street->lat_e7;
    entry.house.lon_e7 = street->lon_e7;
  }
  return house_vec_push(&collector->houses, entry);
}

size_t house_collector_count(const HouseCollector *collector) {
  return collector ? house_vec_size(&collector->houses) : 0;
}

/* =========================================================================
 *  Joining
 * ========================================================================= */

/** Order houses of one street by the rank of their number. */
static int compare_house(const void *lhs, const void *rhs) {
  uint32_t a = ((const GeoHouse *)lhs)->number_rank;
  uint32_t b = ((const GeoHouse *)rhs)->number_rank;
  return a < b ? -1 : (a > b ? 1 : 0);
}

grd_result house_collector_merge(
    HouseSet *out, HouseCollector *const *collectors, size_t collector_count, size_t document_count
) {
  if (!out) return GRD_ERROR_NULL_POINTER;
  memset(out, 0, sizeof(*out));
  if (collector_count && !collectors) return GRD_ERROR_NULL_POINTER;
  out->document_count = document_count;

  size_t house_count = 0;
  for (size_t c = 0; c < collector_count; ++c) {
    if (!collectors[c]) return GRD_ERROR_NULL_POINTER;
    house_count += house_collector_count(collectors[c]);
    out->homeless += collectors[c]->homeless;
    out->pointless += collectors[c]->pointless;
    out->without_number += collectors[c]->without_number;
    out->unknown_street += collectors[c]->unknown_street;
    out->unknown_key += collectors[c]->unknown_key;
    out->recovered_city += collectors[c]->recovered_city;
    out->recovered_postcode += collectors[c]->recovered_postcode;
    out->recovered_nearest += collectors[c]->recovered_nearest;
  }

  uint32_t *offsets = calloc(document_count + 1, sizeof(*offsets));
  if (!offsets) return GRD_ERROR_OUT_OF_MEMORY;
  if (!house_count) {
    out->offsets = offsets;
    return GRD_SUCCESS;
  }
  if (house_count > UINT32_MAX) {
    free(offsets);
    return GRD_ERROR_ARITHMETIC_OVERFLOW;
  }

  /* --- how many houses stand on each street --- */
  for (size_t c = 0; c < collector_count; ++c) {
    const house_vec *vec = &collectors[c]->houses;
    size_t total = house_vec_size(vec);
    for (size_t i = 0; i < total; ++i) {
      uint32_t document = house_vec_get(vec, i)->document;
      if (document < document_count) ++offsets[document];
    }
  }

  uint32_t running = 0;
  for (size_t d = 0; d < document_count; ++d) {
    uint32_t count = offsets[d];
    offsets[d] = running;
    running += count;
  }
  offsets[document_count] = running;

  GeoHouse *houses = malloc((size_t)running * sizeof(*houses));
  uint32_t *cursor = malloc(document_count * sizeof(*cursor));
  if (!houses || !cursor) {
    free(houses);
    free(cursor);
    free(offsets);
    return GRD_ERROR_OUT_OF_MEMORY;
  }
  memcpy(cursor, offsets, document_count * sizeof(*cursor));

  for (size_t c = 0; c < collector_count; ++c) {
    house_vec *vec = &collectors[c]->houses;
    size_t total = house_vec_size(vec);
    for (size_t i = 0; i < total; ++i) {
      const HouseEntry *entry = house_vec_get(vec, i);
      if (entry->document >= document_count) continue;
      houses[cursor[entry->document]++] = entry->house;
    }
    house_vec_free(vec); /* placed — give the memory back before the next thread */
  }
  free(cursor);

  /* --- within a street the numbers line up, so one can be found by halving --- */
  for (size_t d = 0; d < document_count; ++d) {
    uint32_t start = offsets[d];
    uint32_t count = offsets[d + 1] - start;
    if (count > 1) { qsort(houses + start, count, sizeof(*houses), compare_house); }
  }

  out->houses = houses;
  out->house_count = running;
  out->offsets = offsets;
  return GRD_SUCCESS;
}

void house_set_free(HouseSet *set) {
  if (!set) return;
  free(set->houses);
  free(set->offsets);
  memset(set, 0, sizeof(*set));
}

/** @endcond */

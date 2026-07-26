#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "error.h"
#include "format.h"
#include "storage_stats.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_stats_internal.h"

/* =========================================================================
 *  Hashing — fused strlen + FNV-1a, one pass per string
 * ========================================================================= */

/**
 * @brief Compute a 32-char hex hash from value array — fused strlen + FNV-1a.
 *
 *  Walks each string once, feeding bytes into two 64-bit FNV-1a hashes.
 *  The length of each string follows as raw little-endian bytes,
 *  interleaved between the two hashes for dispersion.  Replaces the
 *  previous three-function split (hash_bytes → hash_key → snprintf)
 *  with a single fused pass and direct nibble-to-hex output.
 *
 *  @whisper One walk, two streams — the shape condenses into 32 hex
 */
static BinKey hash_key_bin(const char *const *values, size_t count, uint32_t *fp_out) {
  uint64_t h1 = UINT64_C(14695981039346656037);
  uint64_t h2 = UINT64_C(1099511628211);
  const uint64_t prime = UINT64_C(1099511628211);
  uint32_t djb = 5381; /* independent djb2 fingerprint for collision guard */
  for (size_t i = 0; i < count; ++i) {
    const char *s = values[i];
    if (!s) s = ""; /* guard against NULL — missing parent levels */
    uint64_t len = 0;
    uint8_t c;
    while ((c = (uint8_t)*s++) != 0) {
      h1 ^= c;
      h1 *= prime;
      h2 ^= c;
      h2 *= prime;
      ++len;
      djb = ((djb << 5) + djb) + (uint32_t)c;
    }
    for (int b = 0; b < 8; ++b) {
      uint8_t lb = (uint8_t)(len >> (b * 8));
      h1 ^= lb;
      h1 *= prime;
    }
    for (int b = 0; b < 8; ++b) {
      uint8_t lb = (uint8_t)(len >> (b * 8));
      h2 ^= lb;
      h2 *= prime;
    }
    djb = ((djb << 5) + djb) + (uint32_t)len;
  }
  if (fp_out) *fp_out = djb;
  return (BinKey){h1, h2};
}

/* =========================================================================
 *  Key-set operations
 * ========================================================================= */

/**
 * @brief Arena-safe string copy — like @c strdup but from a MetaAreaAllocator.
 *
 *  Returns NULL when @p s is NULL or the allocator is exhausted.
 */
static char *meta_strdup(MetaAreaAllocator *alloc, const char *s) {
  if (!s) return NULL;
  if (!alloc) fatal(ERROR_MEMORY, "Missing Area alloc in %s:%d", __FILE__, __LINE__);
  size_t len = strlen(s) + 1;
  uint8_t *buf = NULL;
  grd_result r = meta_area_alloc(alloc, &buf, len);
  if (r != GRD_SUCCESS) fatal(ERROR_MEMORY, "Couldn't get memory from Area alloc in %s:%d", __FILE__, __LINE__);
  memcpy(buf, s, len);
  return (char *)buf;
}

/**
 * @brief Insert or update an entity in a key set.
 *
 *  Accepts pre-computed binary @p key and @p parent_key.  Routes the
 *  entity into the prefix bucket derived from the first two bytes of
 *  @p name, then either allocates a new Entity or enriches an existing
 *  one.  The entity's name suffix (name + 2) is stored — the prefix
 *  lives implicitly in the bucket index, saving two bytes per entry.
 *
 *  @whisper A name arrives, splits into guide and payload, and settles
 */
static void key_set_add(
    KeySet *set,
    BinKey key,
    BinKey parent_key,
    uint32_t fingerprint,
    const char *code,
    const char *name,
    int32_t lon_e7,
    int32_t lat_e7,
    int has_point,
    MetaAreaAllocator *alloc
) {
  if (!name) return;

  /* --- bucket index from first two bytes of name --- */
  unsigned char c0 = (unsigned char)name[0];
  unsigned char c1 = (unsigned char)name[1]; /* \0 for 1-char names */
  unsigned short bucket_idx = (unsigned short)(c0 | (c1 << 8));
  PrefixBucket *bucket = &set->buckets[bucket_idx];
  const char *name_suffix = name + 2; /* points to "" if name ≤ 2 chars */

  ptrdiff_t index = hmgeti(bucket->entries, key);

  if (index < 0) {
    Entity e = {
        .key = key,
        .parent_key = parent_key,
        .code = meta_strdup(alloc, code),
        .name = meta_strdup(alloc, name_suffix),
        .centroid_lon_e7 = lon_e7,
        .centroid_lat_e7 = lat_e7,
        .has_point = (uint8_t)has_point,
        .fingerprint = fingerprint,
    };
    hmputs(bucket->entries, e);
  } else {
    Entity *e = &bucket->entries[index];
    if (e->fingerprint != fingerprint) {
      fatal(
          ERROR_HASH_COLLISION,
          "BinKey collision: existing fp=%" PRIu32 " new fp=%" PRIu32 " name=\"%s\"",
          e->fingerprint, fingerprint, name
      );
    }
    if (has_point && !e->has_point) {
      e->centroid_lon_e7 = lon_e7;
      e->centroid_lat_e7 = lat_e7;
      e->has_point = 1;
    }
  }
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

StorageStats *storage_stats_create(MetaAreaAllocator *alloc) {
  StorageStats *stats = calloc(1, sizeof(StorageStats));
  if (stats) stats->alloc = alloc;
  return stats;
}

/**
 * @brief Release all entries across all prefix buckets of one key set.
 *
 *  Walks the 65536 buckets, releasing hash tables.  String payloads
 *  (@c code, @c name) live in the arena and are reclaimed with it —
 *  no individual @c free calls.
 *
 *  Buckets that were never populated remain NULL and cost nothing.
 */
static void key_set_destroy(KeySet *set) {
  for (unsigned i = 0; i < PREFIX_BUCKET_COUNT; ++i) {
    Entity *entries = set->buckets[i].entries;
    if (!entries) continue;
    hmfree(entries);
    set->buckets[i].entries = NULL;
  }
}

void storage_stats_destroy(StorageStats *stats) {
  if (!stats) return;
  key_set_destroy(&stats->countries);
  key_set_destroy(&stats->states);
  key_set_destroy(&stats->counties);
  key_set_destroy(&stats->cities);
  key_set_destroy(&stats->postcodes);
  key_set_destroy(&stats->streets);
  key_set_destroy(&stats->houses);
  free(stats);
}

/* =========================================================================
 *  Record one place entry (HOT PATH — minimise work per call)
 * ========================================================================= */

void storage_stats_record(StorageStats *stats, const PhotonPlace *place) {
  MetaAreaAllocator *alloc = stats->alloc;
  if (place->unsupported) ++stats->unsupported_addresslines;

  /* --- country code fallback --- */
  const char *country_code = place->country_code;
  if (!country_code) country_code = place->country;
  if (!country_code) return;

  /* --- resolve type category for has_pt filtering --- */
  enum { TC_NONE, TC_COUNTRY, TC_STATE, TC_COUNTY, TC_CITY, TC_POSTCODE, TC_STREET } tc = TC_NONE;
  if (place->type) {
    switch (strlen(place->type)) {
    case 8:
      if (place->type[0] == 'p') tc = TC_POSTCODE;
      break;
    case 7:
      if (place->type[0] == 'c') tc = TC_COUNTRY;
      break;
    case 5:
      if (place->type[0] == 's') tc = TC_STATE;
      break;
    case 6:
      if (place->type[0] == 'c')
        tc = TC_COUNTY;
      else if (place->type[0] == 's')
        tc = TC_STREET;
      break;
    case 4:
      if (place->type[0] == 'c') tc = TC_CITY;
      break;
    }
  }

  /* --- alias fields for compact key construction --- */
  const char *country = place->country;
  const char *state_str = place->state;
  const char *county_str = place->county;
  const char *city_str = place->city;
  const char *postcode_str = place->postcode;
  const char *street_str = place->street;
  const char *house_str = place->house;
  int32_t lon_e7 = place->lon_e7;
  int32_t lat_e7 = place->lat_e7;
  int has_pt = place->has_point;

  /* --- parent hashes (pre-compute on the stack) --- */
  BinKey parent_country = BINKEY_NULL;
  uint32_t fp_country = 0, fp_state = 0, fp_county = 0, fp_city = 0;
  uint32_t fp_postcode = 0, fp_street = 0, fp_house = 0;
  BinKey parent_state = BINKEY_NULL;
  BinKey parent_county = BINKEY_NULL;
  BinKey parent_city = BINKEY_NULL;
  BinKey parent_postcode = BINKEY_NULL;
  BinKey parent_street = BINKEY_NULL;
  BinKey parent_house = BINKEY_NULL;

  {
    const char *k1[] = {country_code};
    parent_country = hash_key_bin(k1, 1, &fp_country);
  }

  /* --- insert into each level --- */
  key_set_add(
      &stats->countries, parent_country, BINKEY_NULL, /* no parent */
      fp_country, country_code,                       /* code  */
      country ? country : country_code,               /* name */
      lon_e7, lat_e7, has_pt && (tc == TC_COUNTRY), alloc
  );

  {
    const char *k2[] = {country_code, state_str};
    if (state_str) parent_state = hash_key_bin(k2, 2, &fp_state);
  }
  if (state_str)
    key_set_add(
        &stats->states, parent_state, parent_country, fp_state, NULL, state_str, lon_e7, lat_e7,
        has_pt && (tc == TC_STATE), alloc
    );

  {
    const char *k3[] = {country_code, state_str, county_str};
    if (county_str) parent_county = hash_key_bin(k3, 3, &fp_county);
  }
  if (county_str)
    key_set_add(
        &stats->counties, parent_county, parent_state, fp_county, NULL, county_str, lon_e7, lat_e7,
        has_pt && (tc == TC_COUNTY), alloc
    );

  {
    const char *k4[] = {country_code, state_str, county_str, city_str};
    if (city_str) parent_city = hash_key_bin(k4, 4, &fp_city);
  }
  if (city_str)
    key_set_add(
        &stats->cities, parent_city, parent_county, fp_city, NULL, city_str, lon_e7, lat_e7,
        has_pt && (tc == TC_CITY), alloc
    );

  {
    const char *k5[] = {country_code, state_str, county_str, city_str, postcode_str};
    if (postcode_str) parent_postcode = hash_key_bin(k5, 5, &fp_postcode);
  }
  if (postcode_str)
    key_set_add(
        &stats->postcodes, parent_postcode, parent_city, fp_postcode, NULL, postcode_str, lon_e7,
        lat_e7, has_pt && (tc == TC_POSTCODE), alloc
    );

  {
    const char *k6[] = {country_code, state_str, county_str, city_str, postcode_str, street_str};
    if (street_str) parent_street = hash_key_bin(k6, 6, &fp_street);
  }
  if (street_str)
    key_set_add(
        &stats->streets, parent_street, parent_postcode, fp_street, NULL, street_str, lon_e7,
        lat_e7, has_pt && (tc == TC_STREET), alloc
    );

  {
    if (house_str) {
      const char *k7[] = {country_code, state_str,  county_str, city_str,
                          postcode_str, street_str, house_str};
      parent_house = hash_key_bin(k7, 7, &fp_house);
      key_set_add(
          &stats->houses, parent_house, parent_street, fp_house, NULL, house_str, lon_e7, lat_e7,
          has_pt, alloc
      );
    }
  }
}

/* =========================================================================
 *  Merge
 * ========================================================================= */

/**
 * @brief Merge one source KeySet into a destination KeySet.
 *
 *  Walks all 65536 prefix buckets.  Entries that share the same name
 *  prefix land in the corresponding destination bucket — no bucket
 *  reassignment needed.
 */
static void key_set_merge(KeySet *dst, const KeySet *src, MetaAreaAllocator *alloc) {
  for (unsigned b = 0; b < PREFIX_BUCKET_COUNT; ++b) {
    Entity *src_entries = src->buckets[b].entries;
    if (!src_entries) continue;

    for (ptrdiff_t i = 0; i < hmlen(src_entries); ++i) {
      const Entity *se = &src_entries[i];
      ptrdiff_t found = -1;
      if (dst->buckets[b].entries) found = hmgeti(dst->buckets[b].entries, se->key);
      if (found < 0) {
        Entity e = {
            .key = se->key,
            .parent_key = se->parent_key,
            .code = meta_strdup(alloc, se->code),
            .name = meta_strdup(alloc, se->name),
            .centroid_lon_e7 = se->centroid_lon_e7,
            .centroid_lat_e7 = se->centroid_lat_e7,
            .has_point = se->has_point,
            .fingerprint = se->fingerprint,
        };
        hmputs(dst->buckets[b].entries, e);
      } else {
        Entity *de = &dst->buckets[b].entries[found];
        if (se->has_point && !de->has_point) {
          de->centroid_lon_e7 = se->centroid_lon_e7;
          de->centroid_lat_e7 = se->centroid_lat_e7;
          de->has_point = 1;
        }
      }
    }
  }
}

void storage_stats_merge(StorageStats *dst, const StorageStats *src) {
  MetaAreaAllocator *alloc = dst->alloc;
  key_set_merge(&dst->countries, &src->countries, alloc);
  key_set_merge(&dst->states, &src->states, alloc);
  key_set_merge(&dst->counties, &src->counties, alloc);
  key_set_merge(&dst->cities, &src->cities, alloc);
  key_set_merge(&dst->postcodes, &src->postcodes, alloc);
  key_set_merge(&dst->streets, &src->streets, alloc);
  key_set_merge(&dst->houses, &src->houses, alloc);
  dst->unsupported_addresslines += src->unsupported_addresslines;
}

/* =========================================================================
 *  Estimation helpers  (unchanged logic, adjusted for new Entity layout)
 * ========================================================================= */

static uint64_t align8(uint64_t value) {
  return (value + 7u) & ~UINT64_C(7);
}

static uint64_t estimated_table_bytes(const KeySet *set, unsigned parent_count) {
  uint64_t bytes = 0;
  for (unsigned b = 0; b < PREFIX_BUCKET_COUNT; ++b) {
    Entity *entries = set->buckets[b].entries;
    if (!entries) continue;
    for (ptrdiff_t i = 0; i < hmlen(entries); ++i) {
      const Entity *e = &entries[i];
      size_t name_len = e->name ? strlen(e->name) + 2 : 0; /* +2 for implicit prefix */
      bytes += align8(24 + parent_count * 4 + 4 + name_len + (e->has_point ? 16 : 0));
    }
  }
  return bytes;
}

static uint64_t estimated_index_bytes(const KeySet *set, unsigned parent_count) {
  uint64_t bytes = 0;
  for (unsigned b = 0; b < PREFIX_BUCKET_COUNT; ++b) {
    Entity *entries = set->buckets[b].entries;
    if (!entries) continue;
    for (ptrdiff_t i = 0; i < hmlen(entries); ++i) {
      size_t name_len =
          entries[i].name ? strlen(entries[i].name) + 2 : 0; /* +2 for implicit prefix */
      bytes += align8(16 + parent_count * 4 + 4 + name_len);
    }
  }
  return bytes * 13 / 10;
}

void storage_stats_print(const StorageStats *stats) {
  /*const KeySet *sets[] = {&stats->countries, &stats->states,  &stats->counties, &stats->cities,
                          &stats->postcodes, &stats->streets, &stats->houses};
  const char *labels[] = {"Länder",         "Bundesländer", "Landkreise", "Städte",
                          "Postleitzahlen", "Straßen",      "Adressen"};

  printf("\nNormalisierte deutsche Adressdaten (weltweit):\n");
  for (size_t i = 0; i < 7; ++i) {
    uint64_t rows = 0;
    for (unsigned b = 0; b < PREFIX_BUCKET_COUNT; ++b) {
      if (sets[i]->buckets[b].entries) rows += (uint64_t)hmlen(sets[i]->buckets[b].entries);
    }
    printf("  %-14s %" PRIu64 "\n", labels[i], rows);
  }

  if (stats->unsupported_addresslines)
    printf(
        "  Noch nicht über addresslines aufgelöste Einträge: %" PRIu64 "\n",
        stats->unsupported_addresslines
        );*/
  if (stats->alloc) {
    size_t arena_bytes = meta_area_total_allocated(stats->alloc);
    size_t arena_count = meta_area_arena_count(stats->alloc);
    char arena_buf[32];
    format_byte_units(arena_buf, sizeof(arena_buf), arena_bytes, 2);
    printf(
        "\nArena-Allokator: %s in %zu × 32 MiB-Blöcken reserviert\n", arena_buf, arena_count
    );
  }
}

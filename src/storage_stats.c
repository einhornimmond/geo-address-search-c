#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "storage_stats.h"
#include "error.h"
#include "format.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_stats_internal.h"


/* =========================================================================
 *  Localisation helpers  (unchanged)
 * ========================================================================= */

static const char *localized(yyjson_val *object, const char *german_key, const char *fallback_key)
{
    yyjson_val *value = yyjson_obj_get(object, german_key);
    if (!yyjson_is_str(value) && strcmp(german_key, fallback_key) != 0)
        value = yyjson_obj_get(object, fallback_key);
    return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static const char *place_name(yyjson_val *place)
{
    yyjson_val *names = yyjson_obj_get(place, "name");
    return yyjson_is_obj(names) ? localized(names, "name:de", "name") : NULL;
}

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
static BinKey hash_key_bin(const char *const *values, size_t count, uint32_t *fp_out)
{
    uint64_t h1 = UINT64_C(14695981039346656037);
    uint64_t h2 = UINT64_C(1099511628211);
    const uint64_t prime = UINT64_C(1099511628211);
    uint32_t djb = 5381;  /* independent djb2 fingerprint for collision guard */
    for (size_t i = 0; i < count; ++i) {
        const char *s = values[i];
        uint64_t len = 0;
        uint8_t c;
        while ((c = (uint8_t)*s++) != 0) {
            h1 ^= c; h1 *= prime; h2 ^= c; h2 *= prime; ++len;
            djb = ((djb << 5) + djb) + (uint32_t)c;
        }
        for (int b = 0; b < 8; ++b) { uint8_t lb = (uint8_t)(len >> (b * 8)); h1 ^= lb; h1 *= prime; }
        for (int b = 0; b < 8; ++b) { uint8_t lb = (uint8_t)(len >> (b * 8)); h2 ^= lb; h2 *= prime; }
        djb = ((djb << 5) + djb) + (uint32_t)len;
    }
    if (fp_out) *fp_out = djb;
    return (BinKey){h1, h2};
}

/* =========================================================================
 *  Key-set operations
 * ========================================================================= */

/**
 * @brief Insert or update an entity in a key set.
 *
 *  Accepts pre-computed binary @p key and @p parent_key, then either
 *  allocates a new Entity or enriches an existing one.
 */
static void key_set_add(
    KeySet *set,
    BinKey key,
    BinKey parent_key,
    uint32_t fingerprint,
    const char *code,
    const char *name,
    int32_t lon_e7, int32_t lat_e7,
    int has_point)
{
    if (!name) return;
    ptrdiff_t index = hmgeti(set->entries, key);

    if (index < 0) {
        Entity e = {
            .key             = key,
            .parent_key      = parent_key,
            .code            = code      ? strdup(code)       : NULL,
            .name            = strdup(name),
            .centroid_lon_e7 = lon_e7,
            .centroid_lat_e7 = lat_e7,
            .has_point       = (uint8_t)has_point,
            .fingerprint     = fingerprint,
        };
        hmputs(set->entries, e);
    } else {
        Entity *e = &set->entries[index];
        if (e->fingerprint != fingerprint) {
            fatal(ERROR_HASH_COLLISION,
                  "BinKey collision: existing fp=%" PRIu32 " new fp=%" PRIu32 " name=\"%s\"",
                  e->fingerprint, fingerprint, name);
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

StorageStats *storage_stats_create(void)
{
    return calloc(1, sizeof(StorageStats));
}

static void key_set_destroy(KeySet *set)
{
    if (!set->entries) return;
    for (ptrdiff_t i = 0; i < hmlen(set->entries); ++i) {
        Entity *e = &set->entries[i];
        free(e->code);
        free(e->name);
    }
    hmfree(set->entries);
    set->entries = NULL;
}

void storage_stats_destroy(StorageStats *stats)
{
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

void storage_stats_record(StorageStats *stats, yyjson_val *place)
{
    /* --- address sub-object --- */
    yyjson_val *address = yyjson_obj_get(place, "address");
    if (!yyjson_is_obj(address)) {
        if (yyjson_is_arr(yyjson_obj_get(place, "addresslines")))
            ++stats->unsupported_addresslines;
        address = NULL;
    }

    const char *type         = localized(place, "address_type", "address_type");
    const char *own_name     = place_name(place);
    const char *country_code = localized(place, "country_code", "country_code");
    /* --- single-pass address field extraction (replaces 5 localized calls) --- */
    const char *country = NULL, *state_str = NULL, *county_str = NULL,
               *city_str = NULL, *postcode_str = NULL, *street_str = NULL;
    if (address) {
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(address, &iter);
        yyjson_val *key, *val;
        while ((key = yyjson_obj_iter_next(&iter))) {
            val = yyjson_obj_iter_get_val(key);
            if (!yyjson_is_str(val)) continue;
            const char *k = yyjson_get_str(key);
            size_t      kl = yyjson_get_len(key);
            const char *v  = yyjson_get_str(val);

            /* length-first dispatch — no strcmp on known Photon keys */
            switch (kl) {
            case 4: /* "city" — only set when no German key seen */
                if (!city_str && k[0]=='c') city_str = v;
                break;
            case 5: /* "state" */
                if (!state_str && k[0]=='s') state_str = v;
                break;
            case 6: /* "county" (c) | "street" (s) */
                if      (k[0]=='c' && !county_str) county_str = v;
                else if (k[0]=='s' && !street_str) street_str = v;
                break;
            case 7: /* "country" (co, guarded) | "city:de" (ci, always) */
                if (k[0]=='c') {
                    if      (k[1]=='o' && !memcmp(k,"country",7) && !country) country = v;
                    else if (k[1]=='i' && !memcmp(k,"city:de",7)) city_str = v;
                }
                break;
            case 8: /* "state:de" | "postcode" (p)  */
                if (k[0] == 'p' && !memcmp(k, "postcode", 8) && !postcode_str) {
                  postcode_str = v;
                } else {
                  if (!memcmp(k,"state:de",8)) state_str = v;
                }
                break;
            case 9: /* "county:de" (c) | "street:de" (s) — always overwrite */
                if      (k[0]=='c' && !memcmp(k,"county:de",9)) county_str = v;
                else if (k[0]=='s' && !memcmp(k,"street:de",9)) street_str = v;
                break;
            case 10: /* "country:de" — always overwrites */
                if (!memcmp(k,"country:de",10)) country = v;
                break;
            }
        }
    }

    /* --- resolve type category once (replaces 10 strcmp below) --- */
    enum { TC_NONE, TC_COUNTRY, TC_STATE, TC_COUNTY, TC_CITY, TC_POSTCODE, TC_STREET } tc = TC_NONE;
    if (type) {
        switch (strlen(type)) {
        case 8: if (type[0]=='p') tc = TC_POSTCODE; break;
        case 7: if (type[0]=='c') tc = TC_COUNTRY; break;
        case 5: if (type[0]=='s') tc = TC_STATE;   break;
        case 6:
            if      (type[0]=='c') tc = TC_COUNTY;
            else if (type[0]=='s') tc = TC_STREET;
            break;
        case 4: if (type[0]=='c') tc = TC_CITY;    break;
        }
    }

    if (tc == TC_COUNTRY) country    = own_name;
    if (tc == TC_STATE)   state_str  = own_name;
    if (tc == TC_COUNTY)  county_str = own_name;
    if (tc == TC_CITY)    city_str   = own_name;
    if (tc == TC_POSTCODE) postcode_str = own_name;
    if (tc == TC_STREET)  street_str = own_name;

    if (!country_code) country_code = country;
    if (!country_code) return;

    /* --- centroid --- */
    int     has_pt = 0;
    int32_t lon_e7 = 0, lat_e7 = 0;
    yyjson_val *centroid = yyjson_obj_get(place, "centroid");
    if (yyjson_is_arr(centroid) && yyjson_arr_size(centroid) == 2) {
        double lon = yyjson_get_real(yyjson_arr_get(centroid, 0));
        double lat = yyjson_get_real(yyjson_arr_get(centroid, 1));
        has_pt = 1;
        lon_e7 = (int32_t)round(lon * 1.0e7);
        lat_e7 = (int32_t)round(lat * 1.0e7);
    }

    /* --- parent hashes (pre-compute on the stack) --- */
    BinKey parent_country = BINKEY_NULL;
    uint32_t fp_country = 0, fp_state = 0, fp_county = 0, fp_city = 0;
    uint32_t fp_postcode = 0, fp_street = 0, fp_house = 0;
    BinKey parent_state   = BINKEY_NULL;
    BinKey parent_county  = BINKEY_NULL;
    BinKey parent_city    = BINKEY_NULL;
    BinKey parent_postcode = BINKEY_NULL;
    BinKey parent_street  = BINKEY_NULL;

    {
        const char *k1[] = {country_code};
        parent_country = hash_key_bin(k1, 1, &fp_country);
    }

    /* --- insert into each level --- */
    key_set_add(&stats->countries, parent_country,
        BINKEY_NULL,            /* no parent */
        fp_country,
        country_code,           /* code  */
        country ? country : country_code,  /* name */
        lon_e7, lat_e7, has_pt && (tc == TC_COUNTRY));

    if (state_str) {
        const char *k2[] = {country_code, state_str};
        parent_state = hash_key_bin(k2, 2, &fp_state);
        key_set_add(&stats->states, parent_state,
            parent_country,
        fp_state, NULL, state_str,
            lon_e7, lat_e7, has_pt && (tc == TC_STATE));
    }
    if (county_str) {
        const char *k3[] = {country_code, state_str ? state_str : "", county_str};
        parent_county = hash_key_bin(k3, 3, &fp_county);
        key_set_add(&stats->counties, parent_county,
            memcmp(&parent_state, &BINKEY_NULL, 16) ? parent_state : parent_country,
        fp_county,
            NULL, county_str,
            lon_e7, lat_e7, has_pt && (tc == TC_COUNTY));
    }
    if (city_str) {
        const char *k4[] = {country_code, state_str ? state_str : "",
                            county_str ? county_str : "", city_str};
        parent_city = hash_key_bin(k4, 4, &fp_city);
        key_set_add(&stats->cities, parent_city,
            memcmp(&parent_county, &BINKEY_NULL, 16) ? parent_county
                : (memcmp(&parent_state, &BINKEY_NULL, 16) ? parent_state : parent_country),
        fp_city,
            NULL, city_str,
            lon_e7, lat_e7, has_pt && (tc == TC_CITY));
    }
    /* --- postcode level (new: between city and street) --- */
    if (postcode_str && city_str) {
        const char *k5[] = {country_code, state_str ? state_str : "",
                            county_str ? county_str : "",
                            city_str, postcode_str};
        parent_postcode = hash_key_bin(k5, 5, &fp_postcode);
        key_set_add(&stats->postcodes, parent_postcode,
            memcmp(&parent_city, &BINKEY_NULL, 16) ? parent_city
                : (memcmp(&parent_county, &BINKEY_NULL, 16) ? parent_county
                    : (memcmp(&parent_state, &BINKEY_NULL, 16) ? parent_state : parent_country)),
        fp_postcode,
            NULL, postcode_str,
            lon_e7, lat_e7, has_pt && (tc == TC_NONE) /* postcode has no own address_type */);
    }
    if (street_str) {
        /* street key now includes postcode (6 elements) */
        BinKey street_parent;
        if (postcode_str) {
            const char *k6[] = {country_code, state_str ? state_str : "",
                                county_str ? county_str : "",
                                city_str ? city_str : "", postcode_str, street_str};
            parent_street = hash_key_bin(k6, 6, &fp_street);
            street_parent = parent_postcode;
        } else {
            const char *k6[] = {country_code, state_str ? state_str : "",
                                county_str ? county_str : "",
                                city_str ? city_str : "", "", street_str};
            parent_street = hash_key_bin(k6, 6, &fp_street);
            street_parent = memcmp(&parent_city, &BINKEY_NULL, 16) ? parent_city
                : (memcmp(&parent_county, &BINKEY_NULL, 16) ? parent_county
                    : (memcmp(&parent_state, &BINKEY_NULL, 16) ? parent_state : parent_country));
        }
        key_set_add(&stats->streets, parent_street,
            street_parent,
        fp_street,
            NULL, street_str,
            lon_e7, lat_e7, has_pt && (tc == TC_STREET));
    }
    yyjson_val *house_number = yyjson_obj_get(place, "housenumber");
    if (street_str && yyjson_is_str(house_number)) {
        const char *hn = yyjson_get_str(house_number);
        BinKey house_key;
        if (postcode_str) {
            const char *k7[] = {country_code, state_str ? state_str : "",
                                county_str ? county_str : "",
                                city_str ? city_str : "", postcode_str, street_str, hn};
            house_key = hash_key_bin(k7, 7, &fp_house);
        } else {
            const char *k7[] = {country_code, state_str ? state_str : "",
                                county_str ? county_str : "",
                                city_str ? city_str : "", "", street_str, hn};
            house_key = hash_key_bin(k7, 7, &fp_house);
        }
        key_set_add(&stats->houses, house_key,
            memcmp(&parent_street, &BINKEY_NULL, 16) ? parent_street
                : (memcmp(&parent_postcode, &BINKEY_NULL, 16) ? parent_postcode
                    : (memcmp(&parent_city, &BINKEY_NULL, 16) ? parent_city
                        : (memcmp(&parent_county, &BINKEY_NULL, 16) ? parent_county
                            : (memcmp(&parent_state, &BINKEY_NULL, 16) ? parent_state : parent_country)))),
        fp_house,
            NULL, hn,
            lon_e7, lat_e7, has_pt);
    }
}

/* =========================================================================
 *  Merge
 * ========================================================================= */

static void key_set_merge(KeySet *dst, const KeySet *src)
{
    for (ptrdiff_t i = 0; i < hmlen(src->entries); ++i) {
        Entity *se = &src->entries[i];
        ptrdiff_t found = hmgeti(dst->entries, se->key);
        if (found < 0) {
            Entity e = {
                .key             = se->key,
                .parent_key      = se->parent_key,
                .code            = se->code       ? strdup(se->code)       : NULL,
                .name            = strdup(se->name),
                .centroid_lon_e7 = se->centroid_lon_e7,
                .centroid_lat_e7 = se->centroid_lat_e7,
                .has_point       = se->has_point,
                .fingerprint     = se->fingerprint,
            };
            hmputs(dst->entries, e);
        } else {
            Entity *de = &dst->entries[found];
            if (se->has_point && !de->has_point) {
                de->centroid_lon_e7 = se->centroid_lon_e7;
                de->centroid_lat_e7 = se->centroid_lat_e7;
                de->has_point = 1;
            }
        }
    }
}

void storage_stats_merge(StorageStats *dst, const StorageStats *src)
{
    key_set_merge(&dst->countries, &src->countries);
    key_set_merge(&dst->states,    &src->states);
    key_set_merge(&dst->counties,  &src->counties);
    key_set_merge(&dst->cities,    &src->cities);
    key_set_merge(&dst->postcodes, &src->postcodes);
    key_set_merge(&dst->streets,   &src->streets);
    key_set_merge(&dst->houses,    &src->houses);
    dst->unsupported_addresslines += src->unsupported_addresslines;
}

/* =========================================================================
 *  Estimation helpers  (unchanged logic, adjusted for new Entity layout)
 * ========================================================================= */

static uint64_t align8(uint64_t value)
{
    return (value + 7u) & ~UINT64_C(7);
}

static uint64_t estimated_table_bytes(const KeySet *set, unsigned parent_count)
{
    uint64_t bytes = 0;
    for (ptrdiff_t i = 0; i < hmlen(set->entries); ++i) {
        const Entity *e = &set->entries[i];
        size_t name_len = e->name ? strlen(e->name) : 0;
        bytes += align8(24 + parent_count * 4 + 4 + name_len + (e->has_point ? 16 : 0));
    }
    return bytes;
}

static uint64_t estimated_index_bytes(const KeySet *set, unsigned parent_count)
{
    uint64_t bytes = 0;
    for (ptrdiff_t i = 0; i < hmlen(set->entries); ++i) {
        size_t name_len = set->entries[i].name ? strlen(set->entries[i].name) : 0;
        bytes += align8(16 + parent_count * 4 + 4 + name_len);
    }
    return bytes * 13 / 10;
}

void storage_stats_print(const StorageStats *stats)
{
    const KeySet *sets[] = {&stats->countries, &stats->states,  &stats->counties,
                             &stats->cities,    &stats->postcodes, &stats->streets, &stats->houses};
    const char *labels[] = {"Länder", "Bundesländer", "Landkreise",
                             "Städte", "Postleitzahlen", "Straßen", "Adressen"};
    uint64_t heap = 0, indexes = 0;
    printf("\nNormalisierte deutsche Adressdaten (weltweit):\n");
    for (size_t i = 0; i < 7; ++i) {
        uint64_t rows = (uint64_t)hmlen(sets[i]->entries);
        printf("  %-14s %" PRIu64 "\n", labels[i], rows);
        heap    += estimated_table_bytes(sets[i], (unsigned)i);
        indexes += estimated_index_bytes(sets[i], (unsigned)i) + rows * 16;
    }
    printf("\nPostgreSQL-Schätzung (Heap + PK/Unique-Indizes): %.2f GiB\n",
           (double)(heap + indexes) / (1024.0 * 1024.0 * 1024.0));
    printf("  Heap: %.2f GiB, Indizes: %.2f GiB\n",
           (double)heap / (1024.0 * 1024.0 * 1024.0),
           (double)indexes / (1024.0 * 1024.0 * 1024.0));
    if (stats->unsupported_addresslines)
        printf("  Noch nicht über addresslines aufgelöste Einträge: %" PRIu64 "\n",
               stats->unsupported_addresslines);
}

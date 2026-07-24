#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "storage_stats.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Entity {
    char* key;
    uint64_t name_bytes;
    uint8_t has_point;
} Entity;

typedef struct KeySet {
    Entity* entries;
} KeySet;

struct StorageStats {
    KeySet countries, states, counties, cities, streets, houses;
    uint64_t unsupported_addresslines;
};

static const char* localized(yyjson_val* object, const char* key)
{
    char localized_key[64];
    int written = snprintf(localized_key, sizeof(localized_key), "%s:de", key);
    yyjson_val* value = written > 0 && (size_t)written < sizeof(localized_key)
        ? yyjson_obj_get(object, localized_key) : NULL;
    if (!yyjson_is_str(value)) value = yyjson_obj_get(object, key);
    return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static const char* place_name(yyjson_val* place)
{
    yyjson_val* names = yyjson_obj_get(place, "name");
    return yyjson_is_obj(names) ? localized(names, "name") : NULL;
}

static char* make_key(const char* const* values, size_t count)
{
    size_t length = 1;
    for (size_t i = 0; i < count; ++i) length += strlen(values[i]) + 24;
    char* key = malloc(length);
    if (!key) return NULL;
    char* cursor = key;
    for (size_t i = 0; i < count; ++i) {
        size_t value_length = strlen(values[i]);
        cursor += sprintf(cursor, "%zu:", value_length);
        memcpy(cursor, values[i], value_length);
        cursor += value_length;
        *cursor++ = ';';
    }
    *cursor = '\0';
    return key;
}

static void key_set_add(KeySet* set, const char* const* values, size_t count,
                        const char* name, int has_point)
{
    if (!name) return;
    char* key = make_key(values, count);
    if (!key) return;
    if (!set->entries) sh_new_strdup(set->entries);
    ptrdiff_t index = shgeti(set->entries, key);
    if (index < 0) {
        shputs(set->entries, ((Entity){ .key = key, .name_bytes = strlen(name), .has_point = has_point }));
    } else if (has_point) {
        set->entries[index].has_point = 1;
    }
    free(key);
}

static int has_centroid(yyjson_val* place)
{
    yyjson_val* centroid = yyjson_obj_get(place, "centroid");
    return yyjson_is_arr(centroid) && yyjson_arr_size(centroid) == 2;
}

StorageStats* storage_stats_create(void)
{
    return calloc(1, sizeof(StorageStats));
}

void storage_stats_destroy(StorageStats* stats)
{
    if (!stats) return;
    shfree(stats->countries.entries); shfree(stats->states.entries);
    shfree(stats->counties.entries); shfree(stats->cities.entries);
    shfree(stats->streets.entries); shfree(stats->houses.entries);
    free(stats);
}

void storage_stats_record(StorageStats* stats, yyjson_val* place)
{
    yyjson_val* address = yyjson_obj_get(place, "address");
    if (!yyjson_is_obj(address)) {
        if (yyjson_is_arr(yyjson_obj_get(place, "addresslines"))) ++stats->unsupported_addresslines;
        address = NULL;
    }
    const char* type = localized(place, "address_type");
    const char* own_name = place_name(place);
    const char* country_code = localized(place, "country_code");
    const char* country = address ? localized(address, "country") : NULL;
    const char* state = address ? localized(address, "state") : NULL;
    const char* county = address ? localized(address, "county") : NULL;
    const char* city = address ? localized(address, "city") : NULL;
    const char* street = address ? localized(address, "street") : NULL;
    if (type && strcmp(type, "country") == 0) country = own_name;
    if (type && strcmp(type, "state") == 0) state = own_name;
    if (type && strcmp(type, "county") == 0) county = own_name;
    if (type && strcmp(type, "city") == 0) city = own_name;
    if (type && strcmp(type, "street") == 0) street = own_name;
    if (!country_code) country_code = country;
    if (!country_code) return;

    int point = has_centroid(place);
    const char* country_key[] = { country_code };
    key_set_add(&stats->countries, country_key, 1, country, point);
    if (state) {
        const char* key[] = { country_code, state };
        key_set_add(&stats->states, key, 2, state, point);
    }
    if (county) {
        const char* key[] = { country_code, state ? state : "", county };
        key_set_add(&stats->counties, key, 3, county, point);
    }
    if (city) {
        const char* key[] = { country_code, state ? state : "", county ? county : "", city };
        key_set_add(&stats->cities, key, 4, city, point);
    }
    if (street) {
        const char* key[] = { country_code, state ? state : "", county ? county : "", city ? city : "", street };
        key_set_add(&stats->streets, key, 5, street, point);
    }
    yyjson_val* house_number = yyjson_obj_get(place, "housenumber");
    if (street && yyjson_is_str(house_number)) {
        const char* key[] = { country_code, state ? state : "", county ? county : "", city ? city : "", street, yyjson_get_str(house_number) };
        key_set_add(&stats->houses, key, 6, yyjson_get_str(house_number), point);
    }
}

static void key_set_merge(KeySet* destination, const KeySet* source)
{
    for (ptrdiff_t i = 0; i < shlen(source->entries); ++i) {
        Entity* entry = &source->entries[i];
        if (!destination->entries) sh_new_strdup(destination->entries);
        ptrdiff_t found = shgeti(destination->entries, entry->key);
        if (found < 0) shputs(destination->entries, *entry);
        else if (entry->has_point) destination->entries[found].has_point = 1;
    }
}

void storage_stats_merge(StorageStats* destination, const StorageStats* source)
{
    key_set_merge(&destination->countries, &source->countries);
    key_set_merge(&destination->states, &source->states);
    key_set_merge(&destination->counties, &source->counties);
    key_set_merge(&destination->cities, &source->cities);
    key_set_merge(&destination->streets, &source->streets);
    key_set_merge(&destination->houses, &source->houses);
    destination->unsupported_addresslines += source->unsupported_addresslines;
}

static uint64_t align8(uint64_t value) { return (value + 7u) & ~UINT64_C(7); }

static uint64_t estimated_table_bytes(const KeySet* set, unsigned parent_count)
{
    uint64_t bytes = 0;
    for (ptrdiff_t i = 0; i < shlen(set->entries); ++i) {
        const Entity* entry = &set->entries[i];
        bytes += align8(24 + parent_count * 4 + 4 + entry->name_bytes + (entry->has_point ? 16 : 0));
    }
    return bytes;
}

static uint64_t estimated_index_bytes(const KeySet* set, unsigned parent_count)
{
    uint64_t bytes = 0;
    for (ptrdiff_t i = 0; i < shlen(set->entries); ++i)
        bytes += align8(16 + parent_count * 4 + 4 + set->entries[i].name_bytes);
    return bytes * 13 / 10;
}

void storage_stats_print(const StorageStats* stats)
{
    const KeySet* sets[] = { &stats->countries, &stats->states, &stats->counties, &stats->cities, &stats->streets, &stats->houses };
    const char* labels[] = { "Länder", "Bundesländer", "Landkreise", "Städte", "Straßen", "Adressen" };
    uint64_t heap = 0, indexes = 0;
    printf("\nNormalisierte deutsche Adressdaten (weltweit):\n");
    for (size_t i = 0; i < 6; ++i) {
        uint64_t rows = (uint64_t)shlen(sets[i]->entries);
        printf("  %-14s %" PRIu64 "\n", labels[i], rows);
        heap += estimated_table_bytes(sets[i], (unsigned)i);
        indexes += estimated_index_bytes(sets[i], (unsigned)i) + rows * 16;
    }
    printf("\nPostgreSQL-Schätzung (Heap + PK/Unique-Indizes): %.2f GiB\n",
           (double)(heap + indexes) / (1024.0 * 1024.0 * 1024.0));
    printf("  Heap: %.2f GiB, Indizes: %.2f GiB\n",
           (double)heap / (1024.0 * 1024.0 * 1024.0), (double)indexes / (1024.0 * 1024.0 * 1024.0));
    if (stats->unsupported_addresslines)
        printf("  Noch nicht über addresslines aufgelöste Einträge: %" PRIu64 "\n", stats->unsupported_addresslines);
}

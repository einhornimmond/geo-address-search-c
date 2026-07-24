#pragma once

#include <stdint.h>

#include <yyjson.h>

typedef struct JsonStats {
    uint64_t records;
    uint64_t place_records;
    uint64_t place_entries;
    uint64_t countries;
    uint64_t states;
    uint64_t counties;
    uint64_t cities;
    uint64_t streets;
    uint64_t houses;
    uint64_t other;
    uint64_t invalid_records;
} JsonStats;

void json_stats_record(JsonStats* stats, yyjson_val* root);
void json_stats_add(JsonStats* total, const JsonStats* addend);
void json_stats_print(const JsonStats* stats);

#pragma once

#include <stdint.h>

#include <yyjson.h>

typedef struct StorageStats StorageStats;

StorageStats* storage_stats_create(void);
void storage_stats_destroy(StorageStats* stats);
void storage_stats_record(StorageStats* stats, yyjson_val* place);
void storage_stats_merge(StorageStats* destination, const StorageStats* source);
void storage_stats_print(const StorageStats* stats);

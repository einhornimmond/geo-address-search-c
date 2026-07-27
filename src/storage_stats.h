/** @defgroup storage_stats Storage statistics
 *  @ingroup data
 *  @brief Normalised German address data — counting unique entities and
 *         estimating their PostgreSQL footprint.
 *  @{
 */

#pragma once

#include <stdint.h>

#include "json_parse.h"
#include "meta_area_allocator.h"
#include "storage_stats_internal.h"

/** Opaque handle for the deduplicated, seven-level address store. */
typedef struct StorageStats StorageStats;

/**
 * @brief Create an empty storage statistics collector.
 *
 *  Allocates and zero-initialises the internal key sets for all seven
 *  hierarchy levels (country through house).  All future string copies
 *  (@c strdup) within this collector are served from @p alloc.
 *
 *  @param[in] alloc   Allocator for string storage; must outlive the collector.
 *  @return   New StorageStats pointer, or NULL on allocation failure.
 */
StorageStats *storage_stats_create(MetaAreaAllocator *alloc);

/**
 * @brief Release the collector and all its hash tables.
 *
 *  Safe to call with NULL.
 *
 *  @param[in] stats   Collector to destroy (NULL is a no-op).
 */
void storage_stats_destroy(StorageStats *stats);

/**
 * @brief Register one Photon place entry in the deduplicated store.
 *
 *  Extracts the hierarchical key (country → state → county → city →
 *  postcode → street → housenumber), the display name, centroid
 *  coordinates, and country code from the pre-parsed place. Inserts a
 *  unique entry into each relevant level, preserving all fields for
 *  later SQL export.
 *
 *  @param[in,out] stats   Collector to update.
 *  @param[in]     place   Pre-extracted place data from the JSON parser.
 *  @param[out]    result  0 if ok, <> 0 on error
 *
 *  @whisper One more place finds its home in the hierarchy
 */
int storage_stats_record(StorageStats *stats, const PhotonPlace *place);

/**
 * @brief Combine two collectors into one.
 *
 *  Merges every key set from @p source into @p destination,
 *  preserving centroid coordinates when either side carries them. Used to consolidate per-thread
 * tallies.
 *
 *  @param[in,out] destination   Accumulator receiving the merge.
 *  @param[in]     source        Partial collector to fold in.
 */
void storage_stats_merge(StorageStats *destination, const StorageStats *source);

AddrTreeNode* storage_stats_find_state_by_city(AddrTreeNode* root, const char* cityName, const char* countryCode);

/**
 * @brief Print estimated row counts and PostgreSQL storage size.
 *
 *  Renders a table of unique entries per hierarchy level and
 *  estimates the resulting heap + index storage in GiB for a
 *  fully normalised schema.
 *
 *  @param[in] stats   Collector to display.
 */
void storage_stats_print(const StorageStats *stats);

/** @} */

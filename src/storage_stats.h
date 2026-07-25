/** @defgroup storage_stats Storage statistics
  *  @ingroup data
  *  @brief Normalised German address data — counting unique entities and
  *         estimating their PostgreSQL footprint.
  *  @{
  */

#pragma once

#include <stdint.h>

#include <yyjson.h>

/** Opaque handle for the deduplicated, seven-level address store. */
typedef struct StorageStats StorageStats;

/**
 * @brief Create an empty storage statistics collector.
 *
 *  Allocates and zero-initialises the internal key sets for all seven
 *  hierarchy levels (country through house).
 *
 *  @return   New StorageStats pointer, or NULL on allocation failure.
 */
StorageStats* storage_stats_create(void);

/**
 * @brief Release the collector and all its hash tables.
 *
 *  Safe to call with NULL.
 *
 *  @param[in] stats   Collector to destroy (NULL is a no-op).
 */
void storage_stats_destroy(StorageStats* stats);

/**
 * @brief Register one Photon place entry in the deduplicated store.
 *
 *  Extracts the hierarchical key (country → state → county → city →
 *  postcode → street → housenumber), the display name, centroid
 *  coordinates, and country code from the JSON value. Inserts a unique
 *  entry into each relevant level, preserving all fields for later
 *  SQL export.
 *
 *  @param[in,out] stats   Collector to update.
 *  @param[in]     place   JSON value of a single content entry from a
 *                         Place document.
 *
 *  @whisper One more place finds its home in the hierarchy
 */
void storage_stats_record(StorageStats* stats, yyjson_val* place);

/**
 * @brief Combine two collectors into one.
 *
 *  Merges every key set from @p source into @p destination,
 *  preserving centroid coordinates when either side carries them. Used to consolidate per-thread tallies.
 *
 *  @param[in,out] destination   Accumulator receiving the merge.
 *  @param[in]     source        Partial collector to fold in.
 */
void storage_stats_merge(StorageStats* destination, const StorageStats* source);

/**
 * @brief Print estimated row counts and PostgreSQL storage size.
 *
 *  Renders a table of unique entries per hierarchy level and
 *  estimates the resulting heap + index storage in GiB for a
 *  fully normalised schema.
 *
 *  @param[in] stats   Collector to display.
 */
void storage_stats_print(const StorageStats* stats);


/** @} */

/** @defgroup storage_stats_internal Storage statistics internals
  *  @ingroup data
  *  @brief Shared struct definitions for the deduplicated address store —
  *         visible to storage_stats.c and sql_export.c.
  *  @{
  */

#pragma once

#include <stdint.h>

/** 16-byte binary hash key — faster than 32-char hex strings. */
typedef struct { uint64_t h1, h2; } BinKey;

/** Sentinel for an empty / unset BinKey. */
#define BINKEY_NULL ((BinKey){0, 0})

/** One deduplicated address entity at any hierarchy level.
 *
 *  The @p key is a 16-byte binary hash stored inline — no allocation,
 *  no strcmp.  @p parent_key references the parent entity's key.
 */
typedef struct Entity {
    BinKey     key;               /**< binary hash key (16 bytes inline)              */
    BinKey     parent_key;        /**< parent's binary hash (BINKEY_NULL for root)   */
    char       *code;             /**< country code (strdup'd, NULL otherwise)        */
    char       *name;             /**< display name or housenumber (strdup'd)         */
    int32_t     centroid_lon_e7;  /**< longitude × 10⁷                               */
    int32_t     centroid_lat_e7;  /**< latitude  × 10⁷                               */
    uint8_t     has_point;        /**< centroid data is present                       */
    char       *postcode;         /**< strdup'd, only for houses                      */
    uint32_t    fingerprint;      /**< djb2 over value strings — collision guard     */
} Entity;

/** A binary-keyed hash set of entities (backed by stb_ds hm* family).
 *
 *  @p entries is an stb_ds array — initialise to NULL and use
 *  hmputs / hmgeti with BinKey keys.
 */
typedef struct KeySet {
    Entity *entries;              /**< stb_ds binary-keyed hash: BinKey → Entity     */
} KeySet;

/** Six-level deduplicated address store.
 *
 *  The hierarchy flows: countries → states → counties → cities →
 *  streets → houses. Each level is a KeySet with parent references
 *  linking entries together.
 */
struct StorageStats {
    KeySet   countries, states, counties, cities, streets, houses;
    uint64_t unsupported_addresslines;
};

typedef struct StorageStats StorageStats;

/** @} */

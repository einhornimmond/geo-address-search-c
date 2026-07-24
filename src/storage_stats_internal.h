/** @defgroup storage_stats_internal Storage statistics internals
  *  @ingroup data
  *  @brief Shared struct definitions for the deduplicated address store —
  *         visible to storage_stats.c and sql_export.c.
  *  @{
  */

#pragma once

#include <stdint.h>

/** One deduplicated address entity at any hierarchy level.
 *
 *  The @p key field points into stb_ds internal string storage
 *  and must not be freed. All other owning pointers are strdup'd.
 */
typedef struct Entity {
    const char *key;              /**< hash key (points into stb_ds key storage)      */
    const char *parent_key;       /**< parent's hex hash (strdup'd, NULL for root)    */
    char       *code;             /**< country code (strdup'd, NULL otherwise)        */
    char       *name;             /**< display name or housenumber (strdup'd)         */
    int32_t     centroid_lon_e7;  /**< longitude × 10⁷                               */
    int32_t     centroid_lat_e7;  /**< latitude  × 10⁷                               */
    uint8_t     has_point;        /**< centroid data is present                       */
    char       *postcode;         /**< strdup'd, only for houses                      */
} Entity;

/** A string-keyed hash set of entities (backed by stb_ds).
 *
 *  @p entries is an stb_ds array acting as a hash table with
 *  the entity's @p key field as the lookup key.
 */
typedef struct KeySet {
    Entity *entries;              /**< stb_ds string-keyed hash: key_hex → Entity    */
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

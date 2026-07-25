/** @defgroup storage_stats_internal Storage statistics internals
 *  @ingroup data
 *  @brief Shared struct definitions for the deduplicated address store —
 *         visible to storage_stats.c and sql_export.c.
 *  @{
 */

#pragma once

#include <stdint.h>

/** 16-byte binary hash key — faster than 32-char hex strings. */
typedef struct {
  uint64_t h1, h2;
} BinKey;

/** Sentinel for an empty / unset BinKey. */
#define BINKEY_NULL ((BinKey){0, 0})

/** One deduplicated address entity at any hierarchy level.
 *
 *  The @p key is a 16-byte binary hash stored inline — no allocation,
 *  no strcmp.  @p parent_key references the parent entity's key.
 */
typedef struct Entity {
  BinKey key;              /**< binary hash key (16 bytes inline)              */
  BinKey parent_key;       /**< parent's binary hash (BINKEY_NULL for root)   */
  char *code;              /**< country code (strdup'd, NULL otherwise)        */
  char *name;              /**< display name suffix — prefix stored implicitly
                                in the bucket index (saves 2 bytes per entry)  */
  int32_t centroid_lon_e7; /**< longitude × 10⁷                               */
  int32_t centroid_lat_e7; /**< latitude  × 10⁷                               */
  uint8_t has_point;       /**< centroid data is present                       */
  uint32_t fingerprint;    /**< djb2 over value strings — collision guard     */
} Entity;

/** Number of prefix buckets — one per possible unsigned short (2-byte prefix). */
#define PREFIX_BUCKET_COUNT 65536

/** A bucket of entities sharing the same 2-byte name prefix.
 *
 *  The prefix is the first two bytes of the display name reinterpreted as
 *  an unsigned short.  Each bucket carries its own stb_ds hash table,
 *  initialised lazily to NULL — only populated when an entry lands here.
 *
 *  @whisper One bucket stays silent until the first name falls into it
 */
typedef struct {
  Entity *entries; /**< stb_ds binary-keyed hash: BinKey → Entity     */
} PrefixBucket;

/** A binary-keyed hash set, pre-sorted into 65536 prefix buckets.
 *
 *  Bucket index = (unsigned char)name[0] | ((unsigned char)name[1] << 8).
 *  Entity.name stores only the suffix (name + 2); the full name is
 *  reconstructed on the stack where needed (sql_export, print, estimates).
 */
typedef struct KeySet {
  PrefixBucket buckets[PREFIX_BUCKET_COUNT];
} KeySet;

/** Seven-level deduplicated address store.
 *
 *  The hierarchy flows: countries → states → counties → cities →
 *  postcodes → streets → houses. Each level is a KeySet with parent
 *  references linking entries together.
 */
struct StorageStats {
  KeySet countries, states, counties, cities, postcodes, streets, houses;
  uint64_t unsupported_addresslines;
};

typedef struct StorageStats StorageStats;

/** @} */

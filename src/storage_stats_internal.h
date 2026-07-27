/** @defgroup storage_stats_internal Storage statistics internals
 *  @ingroup data
 *  @brief Tree-based address hierarchy — every node carries its children
 *         directly instead of routing through flat key sets and parent_key
 *         indirection.
 *  @{
 */

#pragma once

#include <stdint.h>

/** Opaque handle — owned by the meta-area allocator module. */
typedef struct MetaAreaAllocator MetaAreaAllocator;


/**
 * @brief One node in the geo-hierarchy tree.
 *
 *  Each node lives at a specific hierarchy level (country, state, county,
 *  city, street, or house).  @p name_hash is the FNV-1a 64-bit hash of
 *  @p name and doubles as the stb_ds hashmap key — it must remain the
 *  first field so stb_ds can use it for keyed lookup.
 *
 *  @p children is a lazy stb_ds hashmap mapping child name_hash to child
 *  node.  It stays NULL until the first child knocks, saving memory for
 *  the millions of leaf nodes.
 *
 *  @p postcode rides as an optional attribute — not a separate tree level.
 *  It is stored on whichever node the Photon entry attaches it to
 *  (typically city, street, or house).
 *
 *  @whisper Each node holds its name and waits for children to arrive —
 *           no flat key sets, no parent_key indirection
 */
typedef struct AddrTreeNode {
    uint64_t key;                    /**< FNV-1a hash of name — stb_ds key (must be first)       */
    char *name;                      /**< display name (arena-allocated, never freed individually) */
    char *postcode;                  /**< postcode attribute (arena-allocated, NULL if absent)     */
    int32_t lon_e7;                  /**< longitude × 10⁷                                         */
    int32_t lat_e7;                  /**< latitude  × 10⁷                                         */
    uint8_t has_point;               /**< centroid is present                                     */
    struct AddrTreeNode *children;   /**< stb_ds hm: child name_hash → AddrTreeNode (NULL = leaf) */
} AddrTreeNode;

/**
 * @brief Root of the address hierarchy tree.
 *
 *  @p root is a stb_ds hashmap mapping country name_hash → AddrTreeNode.
 *  The hierarchy flows naturally through nested children maps:
 *
 *  root → country.children → state.children → county.children →
 *         city.children → street.children → house (leaf)
 *
 *  @p unsupported_addresslines counts Photon entries that carry address
 *  data in the legacy addresslines array instead of a structured address
 *  object — they are logged but not inserted into the tree.
 *
 *  @whisper The tree is the hierarchy — seven levels collapse into nested children maps
 */
struct StorageStats {
    AddrTreeNode *root;              /**< stb_ds hm of country name_hash → AddrTreeNode           */
    uint64_t unsupported_addresslines; /**< entries skipped due to legacy addresslines format       */
    MetaAreaAllocator *alloc;        /**< arena allocator for all string copies                   */
};

typedef struct StorageStats StorageStats;

/** @} */

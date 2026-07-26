#include "json_parse.h"
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "error.h"
#include "format.h"
#include "meta_area_allocator.h"
#include "storage_stats.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_stats_internal.h"

/* =========================================================================
 *  Hashing — FNV-1a 64-bit, one pass per string
 * ========================================================================= */

/**
 * @brief Compute the 64-bit FNV-1a hash of a string.
 *
 *  Feeds each byte through the standard FNV-1a cycle.  The output is
 *  the stb_ds hashmap key for tree lookups — collisions are rare and
 *  handled by stb_ds chaining.
 *
 *  @whisper Eight bytes condense a name into a look-up key
 */
static uint64_t name_hash_fnv1a(const char *s) {
    if (!s) return 0;
    uint64_t h = UINT64_C(14695981039346656037);
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* =========================================================================
 *  Arena string copy
 * ========================================================================= */

/**
 * @brief Arena-safe string copy — like @c strdup but from a MetaAreaAllocator.
 *
 *  Returns NULL when @p s is NULL.  Fatal error on allocator exhaustion.
 */
static char *meta_strdup(MetaAreaAllocator *alloc, const char *s) {
    if (!s) return NULL;
    if (!alloc) fatal(ERROR_MEMORY, "Missing Area alloc");
    size_t len = strlen(s) + 1;
    uint8_t *buf = NULL;
    grd_result r = meta_area_alloc(alloc, &buf, len);
    if (r != GRD_SUCCESS) fatal(ERROR_MEMORY, "Arena exhausted");
    memcpy(buf, s, len);
    return (char *)buf;
}

/* =========================================================================
 *  Tree operations
 * ========================================================================= */

/**
 * @brief Get or create a child node in a stb_ds children hashmap.
 *
 *  Looks up @p name by FNV-1a hash.  When absent, allocates a new node
 *  and inserts it — the children hashmap may grow (stb_ds realloc).
 *  When present, enriches the existing node with missing centroid or
 *  postcode.
 *
 *  The returned pointer is stable until the next insert into the *same*
 *  hashmap.  Since the caller advances to a different parent for the
 *  next hierarchy level, there is no realloc conflict across levels.
 *
 *  @param[in,out] children_ptr  Pointer to stb_ds hm (may point to NULL).
 *  @param[in]     name          Node display name (copied into arena).
 *  @param[in]     postcode      Optional postcode (copied if node lacks one).
 *  @param[in]     lon_e7        Longitude × 10⁷.
 *  @param[in]     lat_e7        Latitude  × 10⁷.
 *  @param[in]     has_point     Whether to store the centroid on this node.
 *  @param[in]     alloc         Arena allocator for string storage.
 *  @return Pointer to the existing or newly-created node.
 *
 *  @whisper A name knocks — the node opens if it's the first, nods if familiar
 */
static AddrTreeNode *tree_get_or_create(
    AddrTreeNode **children_ptr,
    const char *name,
    const char *postcode,
    AddrTreeNodeType type,
    int32_t lon_e7,
    int32_t lat_e7,
    int has_point,
    MetaAreaAllocator *alloc
) {
    if (!name) return NULL;

    uint64_t h = name_hash_fnv1a(name);

    /* --- try existing --- */
    AddrTreeNode *node = *children_ptr ? hmgetp_null(*children_ptr, h) : NULL;

    if (!node) {
        AddrTreeNode new_node = {
            .key       = h,
            .name      = meta_strdup(alloc, name),
            .postcode  = meta_strdup(alloc, postcode),
            .lon_e7    = lon_e7,
            .lat_e7    = lat_e7,
            .has_point = (uint8_t)(has_point ? 1 : 0),
            .type = type,
            .children  = NULL,
        };
        hmputs(*children_ptr, new_node);
        node = hmgetp_null(*children_ptr, h); /* re-fetch after potential realloc */
    } else {
        if (node->type != type) {
            fatal(ERROR_ASSERT, "Node type mismatch: expected %d, got %d, name: %s, existing name: %s", node->type, type, name, node->name);
        }
        /* --- enrich existing node --- */
        if (has_point && !node->has_point) {
            node->lon_e7   = lon_e7;
            node->lat_e7   = lat_e7;
            node->has_point = 1;
        }
        if (postcode && !node->postcode) {
            node->postcode = meta_strdup(alloc, postcode);
        }
    }

    return node;
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

StorageStats *storage_stats_create(MetaAreaAllocator *alloc) {
    StorageStats *stats = calloc(1, sizeof(StorageStats));
    if (stats) stats->alloc = alloc;
    return stats;
}

/**
 * @brief Recursively free a children hashmap and all its subtrees.
 *
 *  String payloads live in the arena — only stb_ds array headers are freed.
 *
 *  @whisper The tree lets go branch by branch
 */
static void tree_destroy(AddrTreeNode *children) {
    if (!children) return;
    for (ptrdiff_t i = 0; i < hmlen(children); ++i) {
        tree_destroy(children[i].children);
    }
    hmfree(children);
}

void storage_stats_destroy(StorageStats *stats) {
    if (!stats) return;
    tree_destroy(stats->root);
    free(stats);
}

/* =========================================================================
 *  Record one place entry (HOT PATH)
 * ========================================================================= */

void storage_stats_record(StorageStats *stats, const PhotonPlace *place) {
    MetaAreaAllocator *alloc = stats->alloc;

    /* --- unsupported addresslines entries are counted but skipped --- */
    if (place->unsupported) {
        ++stats->unsupported_addresslines;
        return;
    }

    /* --- country is the root key — nothing to do without it --- */
    const char *country_name = place->country;
    if (!country_name) country_name = place->country_code;
    if (!country_name) {
      fatal(ERROR_ASSERT, "Missing country name in: %s", photon_place_to_json(place));
    }

    const char *postcode = place->postcode;
    int32_t lon = place->lon_e7, lat = place->lat_e7;
    int has_pt = place->has_point;

    AddrTreeNode **cur = &stats->root;

    /* --- level 0: country --- */
    if (!country_name) {
      fatal(ERROR_ASSERT, "Missing country");
    }
    
    AddrTreeNode *node = tree_get_or_create(cur, country_name, NULL, ADDR_TREE_NODE_TYPE_COUNTRY,
        lon, lat, has_pt && (place->typeEnum == PHOTON_PLACE_TYPE_COUNTRY), alloc);
    cur = &node->children;

    /* --- level 1: state --- */
    if (place->state) {
        bool isState = place->typeEnum == PHOTON_PLACE_TYPE_STATE;
        AddrTreeNode *node = tree_get_or_create(cur, place->state, NULL, ADDR_TREE_NODE_TYPE_STATE,
            lon, lat, has_pt && isState, alloc);
        cur = &node->children;
        if (isState) { return; }
    } else {
        // printf("missing state for %s\n", place->own_name);
    }

    /* --- level 2: county --- */
    if (place->county || (!place->county && place->city)) {
        bool isCounty = place->typeEnum == PHOTON_PLACE_TYPE_COUNTY;
        AddrTreeNode *node = tree_get_or_create(cur, place->county ? place->county : place->city, NULL, ADDR_TREE_NODE_TYPE_COUNTY,
            lon, lat, has_pt && isCounty, alloc);
        cur = &node->children;
        if (isCounty) { return; }
    }

    /* --- level 3: city or suburb (postcode may ride here) --- */
    if (place->city) {
        bool isCity = place->typeEnum == PHOTON_PLACE_TYPE_CITY;
        AddrTreeNode *node = tree_get_or_create(cur, place->city, postcode, ADDR_TREE_NODE_TYPE_CITY,
            lon, lat, has_pt && isCity, alloc);
        cur = &node->children;
        if (isCity) { return ; }
    } else if (place->suburb) {
        AddrTreeNode *node = tree_get_or_create(cur, place->suburb, postcode, ADDR_TREE_NODE_TYPE_SUBURB,
            lon, lat, false, alloc);
        cur = &node->children;
    } else if(place->typeEnum == PHOTON_PLACE_TYPE_LOCALITY) {
        AddrTreeNode *node = tree_get_or_create(cur, place->suburb, postcode, ADDR_TREE_NODE_TYPE_LOCALITY,
            lon, lat, false, alloc);
        cur = &node->children;
    } else if(place->typeEnum == PHOTON_PLACE_TYPE_DISTRICT) {
        AddrTreeNode *node = tree_get_or_create(cur, place->suburb, postcode, ADDR_TREE_NODE_TYPE_DISTRICT,
            lon, lat, false, alloc);
        cur = &node->children;
    } else {
        fatal(ERROR_ASSERT, "Unhandled place type: %s", place->type);
    }

    /* --- level 4: street (postcode may ride here as well) --- */
    if (place->street) {
        bool isStreet = place->typeEnum == PHOTON_PLACE_TYPE_STREET;
        AddrTreeNode *node = tree_get_or_create(cur, place->street, postcode, ADDR_TREE_NODE_TYPE_STREET,
            lon, lat, has_pt && isStreet, alloc);
        cur = &node->children;
        if (isStreet) { return; }
    }

    /* --- level 5: house (leaf — postcode rides here too) --- */
    if (place->house) {
        tree_get_or_create(cur, place->house, postcode, ADDR_TREE_NODE_TYPE_HOUSE,
            lon, lat, has_pt && (place->typeEnum == PHOTON_PLACE_TYPE_HOUSE), alloc);
    }
}

/* =========================================================================
 *  Merge
 * ========================================================================= */

/**
 * @brief Recursively merge one source node into a destination children hashmap.
 *
 *  Finds or creates the matching node in @p dst_children, enriches it
 *  with missing centroid or postcode, then recurses into its children.
 *
 *  @whisper Two trees touch — branches that match entwine, new ones graft
 */
static void tree_merge_node(
    AddrTreeNode **dst_children,
    const AddrTreeNode *src_node,
    MetaAreaAllocator *alloc
) {
    AddrTreeNode *dst_node = *dst_children ? hmgetp_null(*dst_children, src_node->key) : NULL;

    if (!dst_node) {
        AddrTreeNode copy = {
            .key       = src_node->key,
            .name      = meta_strdup(alloc, src_node->name),
            .postcode  = meta_strdup(alloc, src_node->postcode),
            .lon_e7    = src_node->lon_e7,
            .lat_e7    = src_node->lat_e7,
            .has_point = src_node->has_point,
            .type      = src_node->type,
            .children  = NULL,
        };
        hmputs(*dst_children, copy);
        dst_node = hmgetp_null(*dst_children, src_node->key);
    } else {
        if (dst_node->type != src_node->type) {
            fatal(
                ERROR_ASSERT, 
                "Node type mismatch: expected %d, got %d, name: %s, existing name: %s",
                dst_node->type, src_node->type, src_node->name, dst_node->name  
            );
        }
        if (src_node->has_point && !dst_node->has_point) {
            dst_node->lon_e7   = src_node->lon_e7;
            dst_node->lat_e7   = src_node->lat_e7;
            dst_node->has_point = 1;
        }
        if (src_node->postcode && !dst_node->postcode) {
            dst_node->postcode = meta_strdup(alloc, src_node->postcode);
        }
    }

    /* --- descend into children --- */
    if (src_node->children) {
        for (ptrdiff_t i = 0; i < hmlen(src_node->children); ++i) {
            tree_merge_node(&dst_node->children, &src_node->children[i], alloc);
        }
    }
}

void storage_stats_merge(StorageStats *dst, const StorageStats *src) {
    if (!src->root) return;
    for (ptrdiff_t i = 0; i < hmlen(src->root); ++i) {
        tree_merge_node(&dst->root, &src->root[i], dst->alloc);
    }
    dst->unsupported_addresslines += src->unsupported_addresslines;
}

/* =========================================================================
 *  Print — walk the tree, count nodes per level
 * ========================================================================= */

/**
 * @brief Recursively count nodes at each depth level.
 *
 *  @p depth 0 = countries, 1 = states, …, 5 = houses.
 */
static void tree_count_levels(const AddrTreeNode *nodes, uint64_t counts[ADDR_TREE_NODE_COUNT]) {
    if (!nodes) return;

    ptrdiff_t childrenCount = hmlen(nodes);    
    for (ptrdiff_t i = 0; i < childrenCount; ++i) {
        ++counts[nodes[i].type];    
        if (nodes[i].type == ADDR_TREE_NODE_TYPE_STATE) {
            printf("state: %s\n", nodes[i].name);
        }
        tree_count_levels(nodes[i].children, counts);
    }
}

void storage_stats_print(const StorageStats *stats) {
    uint64_t counts[ADDR_TREE_NODE_COUNT] = {0};
    
    tree_count_levels(stats->root, counts);
    
    const char *labels[] = {
        "Länder", "Bundesländer", "Landkreise", "Städte", "Suburbs", "Localities", "Districts", "Streets", "Häuser"
    };

    printf("\nAdresshierarchie (Baum):\n");
    for (int i = 0; i < 9; ++i) {
        printf("  %-14s %" PRIu64 "\n", labels[i], counts[i]);
    }

    if (stats->unsupported_addresslines) {
        printf("  %-14s %" PRIu64 " (nicht aufgelöst)\n",
               "addresslines", stats->unsupported_addresslines);
    }

    /* --- arena stats --- */
    if (stats->alloc) {
        size_t arena_bytes = meta_area_total_allocated(stats->alloc);
        size_t arena_count = meta_area_arena_count(stats->alloc);
        char arena_buf[32];
        format_byte_units(arena_buf, sizeof(arena_buf), arena_bytes, 2);
        printf(
            "\nArena-Allokator: %s in %zu × 32 MiB-Blöcken reserviert\n",
            arena_buf, arena_count
        );
    }
}

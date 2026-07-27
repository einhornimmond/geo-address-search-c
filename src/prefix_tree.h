/** @defgroup prefix_tree Prefix tree
 *  @ingroup data
 *  @brief Byte-wise index tree — one level per character, an index at the end.
 *
 *  A key is the first @c depth bytes of a string.  Each level of the tree
 *  consumes one of them: the first character chooses a slot in the root, the
 *  second a slot in the node below it, and at the last level the slot holds a
 *  dense index instead of another pointer.  Indices are handed out in
 *  encounter order, 0, 1, 2, … — the caller keeps its payload in a plain
 *  array beside the tree and reaches it through that index.
 *
 *  Only travelled paths exist.  A level is a block of @ref PREFIX_TREE_FANOUT
 *  slots (2 KiB on a 64-bit machine) and is created on first use, so a
 *  vocabulary touching 150 first characters costs 150 blocks, not 65536
 *  empty ones.
 *
 *  Because slots are visited in ascending byte order, prefix_tree_foreach()
 *  walks the keys in exactly the order a byte-wise comparison would give —
 *  the tree is a sorting of the prefixes, not merely a lookup structure.
 *
 *  Complexity: intern / find O(depth), foreach O(nodes · fanout),
 *  free O(nodes).  Nothing here is synchronised; a tree belongs to one thread
 *  at a time.
 *
 *  @whisper Each character opens one door, and behind the last one waits a number
 *  @{
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gradido_blockchain_core/result.h"

/** Slots per level — one for every possible byte value. */
#define PREFIX_TREE_FANOUT 256

/** Longest key the tree accepts, in bytes. */
#define PREFIX_TREE_DEPTH_MAX 4

/** Scratch buffer type for a key of at most @ref PREFIX_TREE_DEPTH_MAX bytes. */
typedef uint8_t PrefixKey[PREFIX_TREE_DEPTH_MAX];

/**
 * @brief A tree mapping fixed-length byte keys to dense indices.
 *
 *  Fields are internal; reach them through the API.
 */
typedef struct PrefixTree {
  void *root;       /**< First level, or NULL while no key has arrived. */
  size_t count;     /**< Distinct keys, and the next index to hand out. */
  size_t levels;    /**< Level blocks allocated — the tree's whole footprint. */
  unsigned depth;   /**< Bytes consumed per key, in [1, PREFIX_TREE_DEPTH_MAX]. */
} PrefixTree;

/**
 * @brief Prepare an empty tree of @p depth characters.
 *
 *  Allocates nothing; the first key opens the first level.
 *
 *  @param[in,out] tree   Tree to initialise; must not be NULL.
 *  @param[in]     depth  Key length in bytes, in [1, PREFIX_TREE_DEPTH_MAX].
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER if @p tree is NULL, or
 *          GRD_ERROR_INVALID_PARAM if @p depth is out of range.
 */
grd_result prefix_tree_init(PrefixTree *tree, unsigned depth);

/**
 * @brief Release every level, leaving a reusable empty tree.
 *
 *  The payload the indices point at is none of the tree's business and stays
 *  untouched.  Safe to call with NULL.
 *
 *  @param[in,out] tree  Tree to empty (NULL is a no-op).
 */
void prefix_tree_free(PrefixTree *tree);

/**
 * @brief Fill a key with the first @p depth bytes of @p name.
 *
 *  Bytes beyond the name's end count as 0.  The padding keeps byte order
 *  intact: a shorter name sorts before every longer one sharing its start,
 *  because 0 is below every other byte value.
 *
 *  @param[in]  name       Name bytes; may be NULL only if @p name_size is 0.
 *  @param[in]  name_size  Byte length of @p name without a terminating NUL.
 *  @param[in]  depth      Bytes to read, in [1, PREFIX_TREE_DEPTH_MAX].
 *  @param[out] key        Receives @p depth bytes.
 */
static inline void prefix_tree_key(
    const char *name, size_t name_size, unsigned depth, uint8_t *key
) {
  for (unsigned i = 0; i < depth; ++i) { key[i] = i < name_size ? (uint8_t)name[i] : 0u; }
}

/**
 * @brief Find @p key, or give it the next free index.
 *
 *  Creates the levels the key travels through if they do not exist yet.  On
 *  allocation failure the tree keeps whatever it already held and no index is
 *  handed out.
 *
 *  @param[in,out] tree         Tree to search; must not be NULL.
 *  @param[in]     key          @c tree->depth bytes.
 *  @param[out]    out_index    Receives the index belonging to @p key.
 *  @param[out]    out_created  Set true if the key was new; may be NULL.
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER on a NULL argument, or
 *          GRD_ERROR_OUT_OF_MEMORY when a level could not be opened.
 *
 *  @whisper A new word is given its number, an old one recognised
 */
grd_result prefix_tree_intern(
    PrefixTree *tree, const uint8_t *key, size_t *out_index, bool *out_created
);

/**
 * @brief Look @p key up without changing the tree.
 *
 *  @param[in]  tree       Tree to search; NULL yields false.
 *  @param[in]  key        @c tree->depth bytes.
 *  @param[out] out_index  Receives the index if the key exists; may be NULL.
 *  @return true if @p key is present.
 */
bool prefix_tree_find(const PrefixTree *tree, const uint8_t *key, size_t *out_index);

/** @brief Number of distinct keys the tree holds. */
size_t prefix_tree_count(const PrefixTree *tree);

/** @brief Bytes occupied by the tree's levels — its whole footprint. */
size_t prefix_tree_memory(const PrefixTree *tree);

/**
 * @brief Callback for prefix_tree_foreach().
 *
 *  @param[in] key        The visited key, @c depth bytes, valid for this call.
 *  @param[in] index      Index belonging to the key.
 *  @param[in] user_data  Forwarded verbatim.
 *  @return 0 to continue the walk, non-zero to stop and return that value.
 */
typedef int (*PrefixTreeVisitor)(const uint8_t *key, size_t index, void *user_data);

/**
 * @brief Walk every key in ascending byte order.
 *
 *  @param[in] tree       Tree to walk; NULL visits nothing.
 *  @param[in] visit      Callback; must not be NULL.
 *  @param[in] user_data  Forwarded to @p visit on every call.
 *  @return 0 after a complete walk, or the value that stopped it.
 *
 *  @whisper The doors open in the order they were carved
 */
int prefix_tree_foreach(const PrefixTree *tree, PrefixTreeVisitor visit, void *user_data);

/** @} */

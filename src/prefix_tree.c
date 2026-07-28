/** @cond INTERNAL */

#include "prefix_tree.h"

#include <stdlib.h>

/** Interior level: one child pointer per byte value. */
typedef struct PrefixNode {
  void *slots[PREFIX_TREE_FANOUT];
} PrefixNode;

/** Last level: one index per byte value, stored as index + 1 so 0 means empty. */
typedef struct PrefixLeaf {
  size_t slots[PREFIX_TREE_FANOUT];
} PrefixLeaf;

/** Open a fresh level for depth @p level — a leaf at the last character. */
static void *level_alloc(PrefixTree *tree, unsigned level) {
  void *block =
      level + 1 == tree->depth ? calloc(1, sizeof(PrefixLeaf)) : calloc(1, sizeof(PrefixNode));
  if (block) ++tree->levels;
  return block;
}

/** Release @p block and everything below it. */
static void level_free(void *block, unsigned level, unsigned depth) {
  if (!block) return;
  if (level + 1 < depth) {
    PrefixNode *node = block;
    for (unsigned slot = 0; slot < PREFIX_TREE_FANOUT; ++slot) {
      level_free(node->slots[slot], level + 1, depth);
    }
  }
  free(block);
}

grd_result prefix_tree_init(PrefixTree *tree, unsigned depth) {
  if (!tree) return GRD_ERROR_NULL_POINTER;
  if (depth < 1 || depth > PREFIX_TREE_DEPTH_MAX) return GRD_ERROR_INVALID_PARAM;
  tree->root = NULL;
  tree->count = 0;
  tree->levels = 0;
  tree->depth = depth;
  return GRD_SUCCESS;
}

void prefix_tree_free(PrefixTree *tree) {
  if (!tree) return;
  level_free(tree->root, 0, tree->depth);
  tree->root = NULL;
  tree->count = 0;
  tree->levels = 0;
}

grd_result prefix_tree_intern(
    PrefixTree *tree, const uint8_t *key, size_t *out_index, bool *out_created
) {
  if (!tree || !key || !out_index) return GRD_ERROR_NULL_POINTER;
  if (out_created) *out_created = false;

  if (!tree->root) {
    tree->root = level_alloc(tree, 0);
    if (!tree->root) return GRD_ERROR_OUT_OF_MEMORY;
  }

  /* --- one character, one door --- */
  void *level = tree->root;
  for (unsigned d = 0; d + 1 < tree->depth; ++d) {
    PrefixNode *node = level;
    void **slot = &node->slots[key[d]];
    if (!*slot) {
      *slot = level_alloc(tree, d + 1);
      if (!*slot) return GRD_ERROR_OUT_OF_MEMORY;
    }
    level = *slot;
  }

  PrefixLeaf *leaf = level;
  size_t *cell = &leaf->slots[key[tree->depth - 1]];
  if (!*cell) {
    *cell = ++tree->count; /* stored as index + 1 */
    if (out_created) *out_created = true;
  }
  *out_index = *cell - 1;
  return GRD_SUCCESS;
}

bool prefix_tree_find(const PrefixTree *tree, const uint8_t *key, size_t *out_index) {
  if (!tree || !key || !tree->root) return false;

  const void *level = tree->root;
  for (unsigned d = 0; d + 1 < tree->depth; ++d) {
    const PrefixNode *node = level;
    level = node->slots[key[d]];
    if (!level) return false;
  }

  const PrefixLeaf *leaf = level;
  size_t cell = leaf->slots[key[tree->depth - 1]];
  if (!cell) return false;
  if (out_index) *out_index = cell - 1;
  return true;
}

size_t prefix_tree_count(const PrefixTree *tree) {
  return tree ? tree->count : 0;
}

size_t prefix_tree_memory(const PrefixTree *tree) {
  /* nodes and leaves hold FANOUT machine words either way */
  return tree ? tree->levels * PREFIX_TREE_FANOUT * sizeof(void *) : 0;
}

/** Walk one level in ascending slot order, carrying the key built so far. */
static int level_walk(
    const void *block,
    unsigned level,
    unsigned depth,
    uint8_t *key,
    PrefixTreeVisitor visit,
    void *user_data
) {
  if (level + 1 == depth) {
    const PrefixLeaf *leaf = block;
    for (unsigned slot = 0; slot < PREFIX_TREE_FANOUT; ++slot) {
      if (!leaf->slots[slot]) continue;
      key[level] = (uint8_t)slot;
      int stop = visit(key, leaf->slots[slot] - 1, user_data);
      if (stop) return stop;
    }
    return 0;
  }

  const PrefixNode *node = block;
  for (unsigned slot = 0; slot < PREFIX_TREE_FANOUT; ++slot) {
    if (!node->slots[slot]) continue;
    key[level] = (uint8_t)slot;
    int stop = level_walk(node->slots[slot], level + 1, depth, key, visit, user_data);
    if (stop) return stop;
  }
  return 0;
}

int prefix_tree_foreach(const PrefixTree *tree, PrefixTreeVisitor visit, void *user_data) {
  if (!tree || !tree->root || !visit) return 0;
  PrefixKey key = {0};
  return level_walk(tree->root, 0, tree->depth, key, visit, user_data);
}

/** @endcond */

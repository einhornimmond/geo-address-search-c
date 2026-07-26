/** @cond INTERNAL */

#include "meta_area_allocator.h"
#include "error.h"

#include "gradido_blockchain_core/memory.h"
#include "gradido_blockchain_core/utils/bucket_vector.h"

#include <stdlib.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

/** Capacity of a single arena — 32 MiB. */
#define META_ARENA_CAPACITY ((size_t)32 * 1024 * 1024)

/** Remaining threshold below which an arena is considered full — 16 KiB. */
#define META_ARENA_FULL_REMAINING ((size_t)256)

/** log2 bucket size for the arena vector: 256 arenas per bucket. */
#define META_ARENA_BUCKET_LOG2 8

/* =========================================================================
 *  Arena descriptor stored in the bucket vector
 * ========================================================================= */

typedef struct {
  grd_memory arena; /**< 32 MiB bump allocator */
} MetaArena;

/** Generate a header-only bucket vector for MetaArena. */
GRDU_BVEC_STATIC(meta_arena_vec, MetaArena, META_ARENA_BUCKET_LOG2)

/* =========================================================================
 *  Meta-area allocator struct
 * ========================================================================= */

struct MetaAreaAllocator {
  meta_arena_vec arenas; /**< Ordered list of arenas */
  size_t last_hint;      /**< Index of the last arena that had space */
  size_t total_alloc;    /**< Sum of all @c size arguments ever passed in */
};

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

MetaAreaAllocator *meta_area_allocator_create(void) {
  MetaAreaAllocator *m = calloc(1, sizeof(*m));
  if (!m) return NULL;
  meta_arena_vec_init(&m->arenas, NULL); /* NULL → malloc/free for internal bookkeeping */
  return m;
}

void meta_area_allocator_destroy(MetaAreaAllocator *m) {
  if (!m) return;

  /* Free each arena's backing buffer */
  for (size_t i = 0; i < meta_arena_vec_size(&m->arenas); ++i) {
    MetaArena *arena = meta_arena_vec_get(&m->arenas, i);
    if (arena) grd_memory_free(&arena->arena);
  }
  meta_arena_vec_free(&m->arenas);
  free(m);
}

/* =========================================================================
 *  Allocation — hot path
 * ========================================================================= */

/**
 * @brief Try to serve @p size from arena at index @p i.
 *
 *  Returns GRD_SUCCESS if the arena had enough space (> 16 KiB remaining
 *  AND ≥ @p size).  Updates @c m->last_hint on success.
 */
static grd_result try_alloc_from(MetaAreaAllocator *m, size_t i, uint8_t **out, size_t size) {
  MetaArena *arena = meta_arena_vec_get(&m->arenas, i);
  if (!arena) return GRD_ERROR_NULL_POINTER;

  grd_result result = grd_memory_buffer_alloc(out, &arena->arena, size);
  if (result == GRD_SUCCESS) {
    m->total_alloc += size;
  } else {
    if (arena->arena.out_of_memory_capacity) {
      m->last_hint = i+1;
    }
  }
  return result;
}

grd_result meta_area_alloc(MetaAreaAllocator *m, uint8_t **out, size_t size) {
  if (!m || !out) return GRD_ERROR_NULL_POINTER;
  if (size == 0) return GRD_ERROR_INVALID_PARAM;

  /* Reject oversized single requests */
  if (size >= META_ARENA_CAPACITY / 2) {
    fatal(
        ERROR_ASSERT,
        "MetaAreaAllocator: request of %zu bytes exceeds half the arena capacity (%zu bytes). "
        "Re-think the data structure — single objects this large don't belong in a "
        "bucket-vector arena.",
        size, META_ARENA_CAPACITY / 2
    );
  }

  size_t arena_count = meta_arena_vec_size(&m->arenas);

  /* --- scan forward from last_hint --- */
  for (size_t i = m->last_hint; i < arena_count; ++i) {
    if (try_alloc_from(m, i, out, size) == GRD_SUCCESS) return GRD_SUCCESS;
  }

  /* --- no arena has space → open a fresh one --- */
  MetaArena new_arena;
  grd_result result = grd_memory_init_arena(&new_arena.arena, META_ARENA_CAPACITY);
  if (result != GRD_SUCCESS) return result;

  result = meta_arena_vec_push(&m->arenas, new_arena);
  if (result != GRD_SUCCESS) {
    grd_memory_free(&new_arena.arena);
    return result;
  }

  /* The push copied new_arena; allocate from the live copy in the vector */
  size_t new_index = meta_arena_vec_size(&m->arenas) - 1;
  return try_alloc_from(m, new_index, out, size);
}

/* =========================================================================
 *  Queries
 * ========================================================================= */

size_t meta_area_total_allocated(const MetaAreaAllocator *m) {
  return m ? m->total_alloc : 0;
}

size_t meta_area_arena_count(const MetaAreaAllocator *m) {
  return m ? meta_arena_vec_size(&m->arenas) : 0;
}

/** @endcond */

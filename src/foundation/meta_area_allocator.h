/** @defgroup meta_area_allocator Meta-Area Allocator
 *  @ingroup foundation
 *  @brief Multi-arena bump allocator - chains 32 MiB arenas backed by
 *         @ref grd_memory, managed through a @ref grdu_bucket_vector.
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gradido_blockchain_core/result.h"

/** Opaque handle for the chained-arena allocator. */
typedef struct MetaAreaAllocator MetaAreaAllocator;

/**
 * @brief Create an empty meta-arena allocator.
 *
 *  No arenas are pre-allocated; the first allocation request opens the
 *  first 32 MiB block lazily.
 *
 *  @return   New MetaAreaAllocator pointer, or NULL on allocation failure.
 */
MetaAreaAllocator *meta_area_allocator_create(void);

/**
 * @brief Release all arenas and the allocator itself.
 *
 *  Safe to call with NULL.
 *
 *  @param[in] m   Allocator to destroy (NULL is a no-op).
 */
void meta_area_allocator_destroy(MetaAreaAllocator *m);

/**
 * @brief Allocate @p size bytes from the best-fit arena.
 *
 *  Scans existing arenas starting from the last one that had space;
 *  skips arenas with ≤ 16 KiB remaining.  If no arena can serve the
 *  request a fresh 32 MiB block is opened.
 *
 *  Requests ≥ 16 MiB (half the arena capacity) are rejected with a
 *  fatal error — they indicate a design mistake, not a runtime shortage.
 *
 *  @param[in,out] m     Allocator to draw from.
 *  @param[out]    out   Receives the allocated pointer.
 *  @param[in]     size  Bytes requested; must be in (0, 16 MiB).
 *  @return grd_result indicating success or failure.
 *  @retval GRD_SUCCESS              Memory allocated.
 *  @retval GRD_ERROR_OUT_OF_MEMORY  malloc failed for a new arena.
 *  @retval GRD_ERROR_NULL_POINTER   @p m or @p out is NULL.
 *  @retval GRD_ERROR_INVALID_PARAM  @p size is 0.
 */
grd_result meta_area_alloc(MetaAreaAllocator *m, uint8_t **out, size_t size);

/**
 * @brief Total bytes requested since creation.
 *
 *  Useful for capacity planning and progress reporting.
 *
 *  @param[in] m   Allocator to query (NULL returns 0).
 *  @return Sum of all @p size arguments passed to meta_area_alloc().
 */
size_t meta_area_total_allocated(const MetaAreaAllocator *m);

/**
 * @brief Number of 32 MiB arenas currently managed.
 *
 *  @param[in] m   Allocator to query (NULL returns 0).
 */
size_t meta_area_arena_count(const MetaAreaAllocator *m);

/** @} */

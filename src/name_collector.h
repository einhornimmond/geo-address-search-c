/** @defgroup name_collector Name collector
 *  @ingroup data
 *  @brief Gathering every `own_name` a parser thread meets — one bucket
 *         vector per thread, later merged into a single sorted set of
 *         distinct names.
 *
 *  Each parser thread owns one @ref NameCollector and never touches a
 *  foreign one, so collecting stays lock-free.  The name text itself is
 *  copied into a @ref MetaAreaAllocator — the pointers in the bucket
 *  vector therefore outlive the JSON document they were read from and
 *  stay valid until that allocator is destroyed.
 *
 *  After all threads have joined, name_collector_merge() folds the
 *  per-thread streams into one flat array, sorts it and lets the
 *  duplicates fall away.
 *  @{
 */

#pragma once

#include <stddef.h>

#include "gradido_blockchain_core/result.h"
#include "gradido_blockchain_core/utils/bucket_vector.h"
#include "meta_area_allocator.h"

/** A collected name — points into meta-arena memory, never owned by the vector. */
typedef const char *NameRef;

/** log2 bucket size for the name vector: 4096 pointers (32 KiB) per bucket. */
#define NAME_VEC_BUCKET_LOG2 12

/** Bucket vector over collected name pointers, shared across translation units. */
GRDU_BVEC_DECLARE(name_vec, NameRef, NAME_VEC_BUCKET_LOG2, extern)

/**
 * @brief One thread's growing stream of place names.
 *
 *  Holds the bucket vector of pointers and the arena the name bytes are
 *  copied into.  Both must belong to a single thread; nothing here is
 *  synchronised.
 */
typedef struct NameCollector {
  name_vec names;         /**< Pointers to the copied names, in encounter order. */
  MetaAreaAllocator *alloc; /**< Arena the name bytes are drawn from; not owned. */
} NameCollector;

/**
 * @brief Prepare an empty collector bound to @p alloc.
 *
 *  Allocates nothing — the first name opens the first bucket.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @param[in]     alloc      Arena for the name bytes; must not be NULL and
 *                            must not be shared with another thread.
 *  @return GRD_SUCCESS, or GRD_ERROR_NULL_POINTER if an argument is NULL.
 */
grd_result name_collector_init(NameCollector *collector, MetaAreaAllocator *alloc);

/**
 * @brief Copy @p name into the arena and remember it.
 *
 *  The string is copied including its terminating NUL, so the source may
 *  vanish with the JSON document right after the call.  A NULL @p name is
 *  silently ignored — a place without a name simply leaves no trace.
 *
 *  @param[in,out] collector  Collector receiving the name.
 *  @param[in]     name       NUL-terminated name, or NULL.
 *  @return GRD_SUCCESS on success (including the NULL case),
 *          GRD_ERROR_NULL_POINTER if @p collector is NULL,
 *          or the allocator's error when the arena could not serve the copy.
 *
 *  @whisper Every name spoken once is kept, even before it is known to be new
 */
grd_result name_collector_add(NameCollector *collector, const char *name, size_t name_size);

/** @brief Number of names collected so far (duplicates included). */
size_t name_collector_size(const NameCollector *collector);

/**
 * @brief Release the bucket vector.
 *
 *  The names themselves live in the arena and are released with it, not
 *  here.  Safe to call with NULL.
 *
 *  @param[in,out] collector  Collector to empty (NULL is a no-op).
 */
void name_collector_free(NameCollector *collector);

/**
 * @brief The merged result — every distinct name, in ascending byte order.
 *
 *  @c names is a flat array owned by the set; the strings it points to
 *  remain the property of the arenas they were copied into.
 */
typedef struct NameSet {
  const char **names; /**< Sorted, duplicate-free array of @c count entries. */
  size_t count;       /**< Distinct names. */
  size_t total;       /**< Names seen before duplicates were dropped. */
} NameSet;

/**
 * @brief Merge, sort and dedupe the streams of every thread.
 *
 *  Flattens all collectors into one contiguous array (malloc, roughly
 *  `total * sizeof(char *)` bytes), sorts it with `strcmp` order and
 *  collapses runs of equal names to their first occurrence.
 *
 *  The strings are not copied — @p out stays valid only as long as the
 *  arenas behind the collectors live.  Call this after every parser
 *  thread has been joined; concurrent collecting would race.
 *
 *  @param[out] out              Receives the merged set; zeroed on failure.
 *  @param[in]  collectors       Array of @p collector_count collector pointers.
 *  @param[in]  collector_count  Number of collectors; 0 yields an empty set.
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER on a NULL argument, or
 *          GRD_ERROR_OUT_OF_MEMORY when the flat array could not be taken.
 *
 *  @whisper Many streams reach the same lake, and what was said twice becomes one
 */
grd_result name_collector_merge(
    NameSet *out, const NameCollector *const *collectors, size_t collector_count
);

/**
 * @brief Release the flat array of a merged set.
 *
 *  Leaves the strings untouched — they belong to the arenas.  Safe to call
 *  with NULL or on an already emptied set.
 *
 *  @param[in,out] set  Set to empty (NULL is a no-op).
 */
void name_set_free(NameSet *set);

/** @} */

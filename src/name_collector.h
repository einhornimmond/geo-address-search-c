/** @defgroup name_collector Name collector
 *  @ingroup data
 *  @brief Gathering every `own_name` a parser thread meets — pre-sorted by
 *         its first two bytes, later merged into one sorted set of distinct
 *         names.
 *
 *  Each parser thread owns one @ref NameCollector holding
 *  @ref NAME_PREFIX_COUNT (65536) bucket vectors — one per possible
 *  `unsigned short` formed from the first two bytes of a name.  A name is
 *  filed into the vector of its prefix and stored **without** those two
 *  bytes: the index already carries them, so only the remainder is copied
 *  into a @ref MetaAreaAllocator.  Nothing here is synchronised; a
 *  collector belongs to exactly one thread and collecting stays lock-free.
 *
 *  Prefix layout — with `c0`, `c1` the first two bytes as `unsigned char`:
 *
 *  | name length | index                 | stored suffix     |
 *  |-------------|-----------------------|-------------------|
 *  | 0           | 0                     | `""`              |
 *  | 1           | `c0 << 8`             | `""`              |
 *  | ≥ 2         | `(c0 << 8) \| c1`     | name + 2          |
 *
 *  Because the index is exactly the big-endian reading of the first two
 *  bytes, ascending prefix order *is* byte order — the groups need no
 *  sorting among themselves, only within.  Names carrying an embedded NUL
 *  in their second byte are the one ambiguity this trade accepts; Photon
 *  place names do not contain one.
 *
 *  The way from stream to set runs in three movements:
 *
 *  1. **Collect** — name_collector_add(), lock-free, in the parser thread.
 *  2. **Settle** — name_collector_finish(), still in that thread once its
 *     queue has run dry: every group sorted, the thread's own duplicates
 *     dissolved, the bucket vectors released.  All threads do this at the
 *     same time, so the sorting costs almost no wall clock.
 *  3. **Join** — name_run_merge(), a k-way merge over the already sorted
 *     runs, spread over worker threads by prefix.  No comparison sort left,
 *     and the pre-sorting is preserved in the result.
 *  @{
 */

#pragma once

#include <stddef.h>

#include "gradido_blockchain_core/result.h"
#include "gradido_blockchain_core/utils/bucket_vector.h"
#include "meta_area_allocator.h"

/** A stored name remainder — points into meta-arena memory, never owned by the vector. */
typedef const char *NameRef;

/** log2 bucket size for one prefix vector: 64 pointers (512 B) per bucket.
 *  Kept small because most of the 65536 prefixes hold only a handful of names. */
#define NAME_VEC_BUCKET_LOG2 6

/** Number of prefix buckets — the full value range of an `unsigned short`. */
#define NAME_PREFIX_COUNT ((size_t)0x10000)

/** Upper bound for runs and merge workers — one per parser thread, generously rounded. */
#define NAME_RUN_MAX 16

/** Bucket vector over stored name remainders, shared across translation units. */
GRDU_BVEC_DECLARE(name_vec, NameRef, NAME_VEC_BUCKET_LOG2, extern)

/**
 * @brief One thread's growing stream of place names, pre-sorted by prefix.
 *
 *  @c prefixes is an array of @ref NAME_PREFIX_COUNT vectors indexed by the
 *  first two bytes of each name.  Both the vectors and the arena belong to
 *  a single thread.
 */
typedef struct NameCollector {
  name_vec *prefixes;       /**< NAME_PREFIX_COUNT vectors, indexed by prefix. */
  MetaAreaAllocator *alloc; /**< Arena the name remainders are drawn from; not owned. */
  size_t size;              /**< Names collected so far, duplicates included. */
} NameCollector;

/**
 * @brief Compute the prefix index of a name.
 *
 *  Reads the first two bytes as an unsigned big-endian pair; missing bytes
 *  count as 0.  Deterministic and total — every name has a prefix.
 *
 *  @param[in] name  Name bytes; may be NULL only if @p name_size is 0.
 *  @param[in] name_size  Byte length of @p name without the terminating NUL.
 *  @return Index in [0, NAME_PREFIX_COUNT).
 */
static inline unsigned name_prefix_index(const char *name, size_t name_size) {
  unsigned c0 = name_size > 0 ? (unsigned char)name[0] : 0u;
  unsigned c1 = name_size > 1 ? (unsigned char)name[1] : 0u;
  return (c0 << 8) | c1;
}

/**
 * @brief Prepare an empty collector bound to @p alloc.
 *
 *  Allocates the array of 65536 prefix vectors (≈ 4 MiB) up front; the
 *  vectors themselves stay empty until their first name arrives.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @param[in]     alloc      Arena for the name bytes; must not be NULL and
 *                            must not be shared with another thread.
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER if an argument is NULL, or
 *          GRD_ERROR_OUT_OF_MEMORY if the prefix array could not be taken.
 */
grd_result name_collector_init(NameCollector *collector, MetaAreaAllocator *alloc);

/**
 * @brief File @p name under its prefix, keeping only the remainder.
 *
 *  Copies `name + 2` (including the terminating NUL) into the arena and
 *  pushes it into the vector of prefix `name_prefix_index(name, name_size)`.
 *  Names of one or two bytes leave nothing but their prefix behind and
 *  share one static empty string instead of arena space.  A NULL @p name is
 *  silently ignored — a place without a name leaves no trace.
 *
 *  @param[in,out] collector  Collector receiving the name.
 *  @param[in]     name       NUL-terminated name, or NULL.
 *  @param[in]     name_size  Byte length of @p name without the NUL.
 *  @return GRD_SUCCESS on success (including the NULL case),
 *          GRD_ERROR_NULL_POINTER if @p collector is NULL,
 *          or the allocator's error when the arena could not serve the copy.
 *
 *  @whisper Every name is kept, its first two letters carried by the shelf it rests on
 */
grd_result name_collector_add(NameCollector *collector, const char *name, size_t name_size);

/** @brief Number of names collected so far (duplicates included). */
size_t name_collector_size(const NameCollector *collector);

/** @brief Number of prefix buckets holding at least one name. */
size_t name_collector_used_prefixes(const NameCollector *collector);

/**
 * @brief Release the prefix vectors.
 *
 *  The name bytes live in the arena and are released with it, not here.
 *  Safe to call with NULL and on a collector already emptied by
 *  name_collector_finish().
 *
 *  @param[in,out] collector  Collector to empty (NULL is a no-op).
 */
void name_collector_free(NameCollector *collector);

/**
 * @brief One thread's finished stream — sorted per prefix, free of its own doubles.
 *
 *  @c names is a flat array; group @c p occupies `[offsets[p], offsets[p + 1])`
 *  and is sorted in byte order without duplicates.  The array belongs to the
 *  run, the strings stay the property of the arena they were copied into.
 */
typedef struct NameRun {
  const char **names; /**< Remainders, grouped by prefix; @c count entries. */
  size_t *offsets;    /**< NAME_PREFIX_COUNT + 1 group boundaries into @c names. */
  size_t count;       /**< Distinct names in this run. */
  size_t total;       /**< Names the collector had seen before dedupe. */
} NameRun;

/**
 * @brief Let a collector settle into a sorted run.
 *
 *  Flattens the bucket vectors into one array, sorts each prefix group with
 *  `strcmp` order, drops the duplicates this thread produced on its own, and
 *  releases the vectors — from here on the run carries the names.  Call this
 *  in the collecting thread after its work queue has run dry, so all threads
 *  sort in parallel.
 *
 *  On failure the collector is left untouched and @p run zeroed.
 *
 *  @param[in,out] collector  Collector to drain; must not be NULL.
 *  @param[out]    run        Receives the sorted run.
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER on a NULL argument, or
 *          GRD_ERROR_OUT_OF_MEMORY when the flat arrays could not be taken.
 *
 *  @whisper The stream stops running and lets its sediment lie in order
 */
grd_result name_collector_finish(NameCollector *collector, NameRun *run);

/**
 * @brief Release the arrays of a run; the strings belong to the arena.
 *
 *  @param[in,out] run  Run to empty (NULL is a no-op).
 */
void name_run_free(NameRun *run);

/**
 * @brief The merged result — every distinct name, still grouped by prefix.
 *
 *  @c names is one flat array of remainders; group @c p occupies the range
 *  `[offsets[p], offsets[p + 1])` and is sorted in byte order without
 *  duplicates.  Groups follow each other in ascending prefix order, so the
 *  array read front to back is the whole set in lexicographic order.
 *
 *  The arrays belong to the set; the strings remain the property of the
 *  arenas they were copied into.
 */
typedef struct NameSet {
  const char **names;   /**< Remainders, grouped by prefix; @c count entries. */
  size_t *offsets;      /**< NAME_PREFIX_COUNT + 1 group boundaries into @c names. */
  size_t count;         /**< Distinct names. */
  size_t total;         /**< Names seen before any duplicate was dropped. */
  size_t used_prefixes; /**< Prefix buckets holding at least one distinct name. */
} NameSet;

/**
 * @brief Join the sorted runs of all threads into one set.
 *
 *  Every prefix group is a k-way merge over the runs' already sorted groups —
 *  linear in the number of names, with the last duplicates dissolving as the
 *  streams flow together.  Groups are handed to @p worker_count threads split
 *  by cumulative element count, not by equal prefix ranges: the distribution
 *  over the 65536 prefixes is heavily skewed, and equal ranges would leave
 *  one worker carrying the crowded ones alone.
 *
 *  The strings are not copied — @p out stays valid only as long as the arenas
 *  behind the runs live.  Call this after every parser thread has been joined.
 *
 *  @param[out] out           Receives the merged set; zeroed on failure.
 *  @param[in]  runs          Array of @p run_count run pointers.
 *  @param[in]  run_count     Number of runs, at most @ref NAME_RUN_MAX;
 *                            0 yields an empty set.
 *  @param[in]  worker_count  Merge threads to use; clamped to
 *                            [1, NAME_RUN_MAX].  Threads that cannot be
 *                            created have their share done by the caller.
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER on a NULL argument,
 *          GRD_ERROR_INVALID_PARAM if @p run_count exceeds NAME_RUN_MAX, or
 *          GRD_ERROR_OUT_OF_MEMORY when the flat arrays could not be taken.
 *
 *  @whisper Many streams reach the same lake, and what was said twice becomes one
 */
grd_result name_run_merge(
    NameSet *out, const NameRun *const *runs, size_t run_count, unsigned worker_count
);

/**
 * @brief Borrow one prefix group of a merged set.
 *
 *  @param[in]  set        Merged set; must not be NULL.
 *  @param[in]  prefix     Prefix index in [0, NAME_PREFIX_COUNT).
 *  @param[out] out_count  Receives the number of names in the group.
 *  @return Pointer to the group's first remainder, or NULL if the group is
 *          empty or @p prefix is out of range (@p out_count is 0 then).
 */
const char *const *name_set_group(const NameSet *set, unsigned prefix, size_t *out_count);

/**
 * @brief Write a stored name back in full, prefix bytes restored.
 *
 *  Reverses the split: prepends the one or two bytes carried by @p prefix
 *  and appends the remainder.  The result is always NUL-terminated as long
 *  as @p buffer_size is at least 1.
 *
 *  @param[in]  set          Merged set the entry came from; must not be NULL.
 *  @param[in]  prefix       Prefix index the entry is filed under.
 *  @param[in]  index        Position inside the group.
 *  @param[out] buffer       Destination for the reassembled name.
 *  @param[in]  buffer_size  Capacity of @p buffer in bytes.
 *  @return Byte length of the full name without the NUL, regardless of
 *          truncation; 0 if the entry does not exist.  A return value ≥
 *          @p buffer_size means the name was cut short.
 *
 *  @whisper What was split for order returns whole for reading
 */
size_t name_set_compose(
    const NameSet *set, unsigned prefix, size_t index, char *buffer, size_t buffer_size
);

/**
 * @brief Release the flat arrays of a merged set.
 *
 *  Leaves the strings untouched — they belong to the arenas.  Safe to call
 *  with NULL or on an already emptied set.
 *
 *  @param[in,out] set  Set to empty (NULL is a no-op).
 */
void name_set_free(NameSet *set);

/** @} */

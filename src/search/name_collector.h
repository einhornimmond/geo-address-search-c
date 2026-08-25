/** @defgroup name_collector Name collector
 *  @ingroup search
 *  @brief Gathering every `own_name` a parser thread meets — filed by its
 *         first characters through a @ref prefix_tree, later merged into one
 *         sorted set of distinct names.
 *
 *  Each parser thread owns one @ref NameCollector.  A name's first
 *  @ref NAME_PREFIX_DEPTH bytes travel down the index tree and come back as a
 *  dense group index; the name itself is stored **without** those bytes,
 *  copied into an arnm arena chain.  Only prefixes that actually occur
 *  exist — no empty space is reserved for the ones that never come.  Nothing
 *  here is synchronised; a collector belongs to exactly one thread and
 *  collecting stays lock-free.
 *
 *  Prefix layout — with `c0`, `c1`, … the leading bytes as `unsigned char`
 *  and a depth of 2:
 *
 *  | name length | key         | stored remainder |
 *  |-------------|-------------|------------------|
 *  | 0           | `{0, 0}`    | `""`             |
 *  | 1           | `{c0, 0}`   | `""`             |
 *  | ≥ 2         | `{c0, c1}`  | name + 2         |
 *
 *  Missing bytes are padded with 0, which sits below every other byte value —
 *  so ascending key order is byte order, and a shorter name keeps its place
 *  before every longer one sharing its start.  Names carrying an embedded NUL
 *  among their first bytes are the one ambiguity this trade accepts; Photon
 *  place names do not contain one.  Raising @ref NAME_PREFIX_DEPTH adds a
 *  level to the tree and shortens the stored remainders; nothing else changes.
 *
 *  The way from stream to set runs in three movements:
 *
 *  1. **Collect** — name_collector_add(), lock-free, in the parser thread.
 *  2. **Settle** — name_collector_finish(), still in that thread once its
 *     queue has run dry: every group sorted, the thread's own duplicates
 *     dissolved, the bucket vectors released.  All threads do this at the
 *     same time, so the sorting costs almost no wall clock.
 *  3. **Join** — name_run_merge(), a k-way merge over the already sorted
 *     runs, spread over worker threads by prefix group.  No comparison sort
 *     left, and the pre-sorting is preserved in the result.
 *  @{
 */

#pragma once

#include <stddef.h>

#include "arnm/bucket_vector.h"
#include "arnm/multi_arena.h"
#include "arnm/result.h"
#include "search/prefix_tree.h"

/** Bytes an arena of the name chain reserves — 32 MiB.
 *
 *  Name remainders are short, so what this figure decides is how often the chain asks the
 *  host for ground and how many arenas a first-fit scan can end up walking.  One arena per
 *  32 MiB of text keeps both small.  The full threshold stays at arnm's default: a
 *  remainder of 64 bytes or less holds no name worth the walk past it. */
#define NAME_ARENA_CAPACITY ((uint32_t)32 * 1024 * 1024)

/** A stored name remainder — points into arena memory, never owned by the vector. */
typedef const char *NameRef;

/** Leading bytes carried by the tree instead of by the stored strings. */
#define NAME_PREFIX_DEPTH 2

static_assert(
    NAME_PREFIX_DEPTH >= 1 && NAME_PREFIX_DEPTH <= PREFIX_TREE_DEPTH_MAX,
    "NAME_PREFIX_DEPTH must fit into a PrefixKey"
);

/** log2 bucket size for one prefix group: 512 pointers (4 KiB) per bucket.
 *
 *  A bucket vector holds at most `ARNM_BVEC_MAX_INDEX_CAPACITY` buckets, so this
 *  exponent is also the ceiling of a group: 4 190 208 names under one prefix, per
 *  thread.  Smaller buckets would suit the many groups that hold a handful of names, but a
 *  two-byte prefix is a coarse sieve and the busiest group of a planet build carries far more
 *  than a bucket count of that size could address.  The floor this costs is one bucket per
 *  occurring prefix. */
#define NAME_VEC_BUCKET_LOG2 9

/** log2 bucket size for the group array: 64 vectors (2.5 KiB) per bucket.
 *
 *  A key is @ref NAME_PREFIX_DEPTH bytes, so there are at most 65536 groups — well inside
 *  the 523 776 this exponent addresses. */
#define NAME_GROUP_VEC_BUCKET_LOG2 6

/** Upper bound for runs and merge workers — one per parser thread, generously rounded. */
#define NAME_RUN_MAX 16

/** Recently added names kept for the repetition filter — see @ref NameRecent.
 *  Must exceed the number of strings one entry contributes, otherwise the
 *  repetition of the *previous* entry is already overwritten when it returns. */
#define NAME_RECENT_SLOTS 64

/** Bucket vector over stored name remainders. */
ARNM_BVEC_DEFINE(name_vec, NameRef)

/** Bucket vector over the prefix groups — pointer-stable, so growth never
 *  disturbs a vector a caller is currently filling. */
ARNM_BVEC_DEFINE(name_group_vec, arnm_bvec)

/**
 * @brief A name the collector has just stored, held for comparison.
 *
 *  The dump arrives geographically ordered, so the same city, county and
 *  state come by again and again in immediate succession.  Remembering the
 *  last few names lets those repetitions pass without arena bytes and
 *  without a slot — the deduplication would drop them later anyway, this
 *  only refuses to carry them there.
 */
typedef struct NameRecent {
  PrefixKey key;    /**< Leading bytes of the name. */
  const char *rest; /**< Stored remainder, as it went into the arena. */
  size_t size;      /**< Full name length; SIZE_MAX while the slot is unused. */
} NameRecent;

/**
 * @brief One thread's growing stream of place names, filed by prefix.
 *
 *  @c prefixes maps the leading bytes of a name to a dense index into
 *  @c groups.  Both the tree and the vectors belong to a single thread.
 */
typedef struct NameCollector {
  PrefixTree prefixes;                  /**< Leading bytes → group index. */
  arnm_bvec groups;                     /**< One vector of remainders per occurring prefix. */
  arnm *alloc;                          /**< Arena the remainders are drawn from; not owned. */
  size_t size;                          /**< Names stored, duplicates included. */
  size_t seen;                          /**< Names offered, including those the filter absorbed. */
  NameRecent recent[NAME_RECENT_SLOTS]; /**< Ring of the last names stored. */
  unsigned recent_next;                 /**< Slot the next name overwrites. */
} NameCollector;

/**
 * @brief Prepare an empty collector bound to @p alloc.
 *
 *  Allocates nothing — the first name opens the first level of the tree and
 *  the first group.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @param[in]     alloc      Arena for the name bytes; must not be NULL and
 *                            must not be shared with another thread.
 *  @return ARNM_SUCCESS, or ARNM_ERROR_NULL_POINTER if an argument is NULL.
 */
arnm_result name_collector_init(NameCollector *collector, arnm *alloc);

/**
 * @brief File @p name under its prefix, keeping only the remainder.
 *
 *  Sends the first @ref NAME_PREFIX_DEPTH bytes down the tree and copies what
 *  is left (including the terminating NUL) into the arena.  Names no longer
 *  than the prefix leave nothing but their key behind and share one static
 *  empty string instead of arena space.  A NULL @p name is silently ignored —
 *  a place without a name leaves no trace.
 *
 *  @param[in,out] collector  Collector receiving the name.
 *  @param[in]     name       NUL-terminated name, or NULL.
 *  @param[in]     name_size  Byte length of @p name without the NUL.  What is stored
 *                            is the remainder after the prefix, and the arena measures
 *                            that in `uint32_t` — so a name of
 *                            `UINT32_MAX + NAME_PREFIX_DEPTH` bytes or more is refused
 *                            rather than truncated.  No dump comes near it; the bound
 *                            is there because a truncated size would reserve short and
 *                            copy long.
 *  @retval ARNM_SUCCESS            The name is filed, or @p name was NULL and there
 *                                  was nothing to file.
 *  @retval ARNM_ERROR_NULL_POINTER @p collector is NULL.
 *  @retval ARNM_ERROR_ARITHMETIC_OVERFLOW @p name_size is at or above the bound above.
 *  @retval Anything the prefix tree, the group vectors or the arena answer with when
 *          they cannot take what this name needs, passed on unchanged.
 *  @note A refusal is not undone.  A name that fails partway may leave a key in the
 *       tree, an empty group behind it or a few arena bytes nobody points at — all of
 *       it released with the collector.  The counts stay honest either way: @c size
 *       rises only once the name is really filed.
 *
 *  @whisper Every name is kept, its first letters carried by the branch it hangs on
 */
arnm_result name_collector_add(NameCollector *collector, const char *name, size_t name_size);

/** @brief Number of names stored so far (duplicates included). */
size_t name_collector_size(const NameCollector *collector);

/** @brief Number of names offered so far, including the repetitions filtered out. */
size_t name_collector_seen(const NameCollector *collector);

/** @brief Number of distinct prefixes encountered so far. */
size_t name_collector_prefix_count(const NameCollector *collector);

/**
 * @brief Release the tree and the group vectors.
 *
 *  The name bytes live in the arena and are released with it, not here.
 *  Safe to call with NULL and on a collector already emptied by
 *  name_collector_finish().
 *
 *  @param[in,out] collector  Collector to empty (NULL is a no-op).
 */
void name_collector_free(NameCollector *collector);

/**
 * @brief One prefix group inside a run or a set.
 *
 *  The key holds the leading bytes the group's names share; the remainders
 *  live in `[start, start + count)` of the owning array, sorted and distinct.
 */
typedef struct NameGroup {
  PrefixKey key; /**< Leading bytes, zero-padded beyond NAME_PREFIX_DEPTH. */
  size_t start;  /**< First remainder of the group. */
  size_t count;  /**< Remainders in the group; never 0. */
} NameGroup;

/**
 * @brief One thread's finished stream — sorted per group, free of its own doubles.
 *
 *  @c groups is ordered ascending by key, so reading the run front to back
 *  yields the names in lexicographic order.  The arrays belong to the run,
 *  the strings stay the property of the arena they were copied into.
 */
typedef struct NameRun {
  const char **names; /**< Remainders, grouped by prefix; @c count entries. */
  NameGroup *groups;  /**< @c group_count groups, ascending by key. */
  size_t group_count; /**< Distinct prefixes in this run. */
  size_t count;       /**< Distinct names in this run. */
  size_t total;       /**< Names the collector had seen before dedupe. */
} NameRun;

/**
 * @brief Let a collector settle into a sorted run.
 *
 *  Walks the tree in ascending key order, flattens each group into one array,
 *  sorts it with `strcmp` order, drops the duplicates this thread produced on
 *  its own, and releases tree and vectors — from here on the run carries the
 *  names.  Call this in the collecting thread after its work queue has run
 *  dry, so all threads sort in parallel.
 *
 *  On failure the collector is left untouched and @p run zeroed.
 *
 *  @param[in,out] collector  Collector to drain; must not be NULL.
 *  @param[out]    run        Receives the sorted run.
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER on a NULL argument, or
 *          ARNM_ERROR_OUT_OF_MEMORY when the flat arrays could not be taken.
 *
 *  @whisper The stream stops running and lets its sediment lie in order
 */
arnm_result name_collector_finish(NameCollector *collector, NameRun *run);

/**
 * @brief Release the arrays of a run; the strings belong to the arena.
 *
 *  @param[in,out] run  Run to empty (NULL is a no-op).
 */
void name_run_free(NameRun *run);

/**
 * @brief The merged result — every distinct name, still grouped by prefix.
 *
 *  @c groups is ordered ascending by key and @c prefixes finds a group by its
 *  leading bytes in @ref NAME_PREFIX_DEPTH steps.  Reading @c names front to
 *  back yields the whole set in lexicographic order.
 *
 *  The arrays belong to the set; the strings remain the property of the
 *  arenas they were copied into.
 */
typedef struct NameSet {
  const char **names;  /**< Remainders, grouped by prefix; @c count entries. */
  NameGroup *groups;   /**< @c group_count groups, ascending by key. */
  PrefixTree prefixes; /**< Leading bytes → index into @c groups. */
  size_t group_count;  /**< Distinct prefixes. */
  size_t count;        /**< Distinct names. */
  size_t total;        /**< Names seen before any duplicate was dropped. */
} NameSet;

/**
 * @brief Join the sorted runs of all threads into one set.
 *
 *  The runs' group lists are merged by key, and every shared key is a k-way
 *  merge over the runs' already sorted names — linear in the number of names,
 *  with the last duplicates dissolving as the streams flow together.  Groups
 *  are handed to @p worker_count threads split by cumulative element count,
 *  not by equal group ranges: the distribution over prefixes is heavily
 *  skewed, and equal ranges would leave one worker carrying the crowded ones
 *  alone.
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
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER on a NULL argument,
 *          ARNM_ERROR_INVALID_PARAM if @p run_count exceeds NAME_RUN_MAX, or
 *          ARNM_ERROR_OUT_OF_MEMORY when the flat arrays could not be taken — or
 *          when the size one of them would need cannot be expressed at all, which is
 *          refused before the allocator is asked for anything.
 *
 *  @whisper Many streams reach the same lake, and what was said twice becomes one
 */
arnm_result name_run_merge(
    NameSet *out, const NameRun *const *runs, size_t run_count, unsigned worker_count
);

/**
 * @brief Find the group a name's prefix belongs to.
 *
 *  @param[in] set        Merged set; must not be NULL.
 *  @param[in] name       Name whose leading bytes are looked up.
 *  @param[in] name_size  Byte length of @p name without a terminating NUL.
 *  @return The group, or NULL if no name with this prefix was collected.
 */
const NameGroup *name_set_find(const NameSet *set, const char *name, size_t name_size);

/**
 * @brief Find a word and learn its position in the whole set.
 *
 *  The rank is what a posting list refers to: words are numbered by their
 *  place in byte order, so the number outlives every rebuild of the same
 *  input.  Two steps — the tree chooses the group, a binary search finds the
 *  word inside it.
 *
 *  @param[in]  set       Merged set; must not be NULL.
 *  @param[in]  word      Word to look up, exactly as it was collected.
 *  @param[in]  size      Byte length of @p word.
 *  @param[out] out_rank  Receives the rank if the word exists; may be NULL.
 *  @return true if the word is present.
 *
 *  @whisper A word asks for its number, and the order answers
 */
bool name_set_rank(const NameSet *set, const char *word, size_t size, size_t *out_rank);

/**
 * @brief Borrow one group by position, groups being ordered by key.
 *
 *  @param[in] set          Merged set; must not be NULL.
 *  @param[in] group_index  Position in [0, group_count).
 *  @return The group, or NULL if @p group_index is out of range.
 */
const NameGroup *name_set_group_at(const NameSet *set, size_t group_index);

/**
 * @brief Borrow the remainders of a group.
 *
 *  @param[in] set    Merged set the group came from; must not be NULL.
 *  @param[in] group  Group to open; must not be NULL.
 *  @return Pointer to the group's first remainder — @c group->count entries,
 *          sorted, distinct; NULL on a NULL argument.
 */
const char *const *name_set_group_names(const NameSet *set, const NameGroup *group);

/**
 * @brief Write a stored name back in full, prefix bytes restored.
 *
 *  Reverses the split: prepends the leading bytes carried by the group's key
 *  (trailing zero padding dropped) and appends the remainder.  The result is
 *  always NUL-terminated as long as @p buffer_size is at least 1.
 *
 *  @param[in]  set          Merged set the group came from; must not be NULL.
 *  @param[in]  group        Group holding the entry; must not be NULL.
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
    const NameSet *set, const NameGroup *group, size_t index, char *buffer, size_t buffer_size
);

/**
 * @brief Release the arrays and the tree of a merged set.
 *
 *  Leaves the strings untouched — they belong to the arenas.  Safe to call
 *  with NULL or on an already emptied set.
 *
 *  @param[in,out] set  Set to empty (NULL is a no-op).
 */
void name_set_free(NameSet *set);

/** @} */

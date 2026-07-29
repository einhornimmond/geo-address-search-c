/** @cond INTERNAL */

#include "search/name_collector.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** Bucket vector bodies — exactly one translation unit defines them. */
GRDU_BVEC_DEFINE(name_vec, NameRef, NAME_VEC_BUCKET_LOG2, )
GRDU_BVEC_DEFINE(name_group_vec, name_vec, NAME_GROUP_VEC_BUCKET_LOG2, )

/** Shared remainder of every name that is fully carried by its prefix. */
static const char name_empty_suffix[] = "";

/** Keys are compared over their whole width; the padding beyond the depth is always 0. */
static inline int key_compare(const uint8_t *lhs, const uint8_t *rhs) {
  return memcmp(lhs, rhs, sizeof(PrefixKey));
}

/* =========================================================================
 *  Per-thread collecting
 * ========================================================================= */

grd_result name_collector_init(NameCollector *collector, MetaAreaAllocator *alloc) {
  if (!collector || !alloc) return GRD_ERROR_NULL_POINTER;

  grd_result result = prefix_tree_init(&collector->prefixes, NAME_PREFIX_DEPTH);
  if (result != GRD_SUCCESS) return result;
  /* NULL → malloc/free for the vectors' own bookkeeping */
  result = name_group_vec_init(&collector->groups, NULL);
  if (result != GRD_SUCCESS) return result;
  collector->alloc = alloc;
  collector->size = 0;
  collector->seen = 0;
  collector->recent_next = 0;
  /* an unused slot must not match an empty name */
  for (unsigned slot = 0; slot < NAME_RECENT_SLOTS; ++slot) {
    collector->recent[slot].size = SIZE_MAX;
    collector->recent[slot].rest = NULL;
  }
  return GRD_SUCCESS;
}

grd_result name_collector_add(NameCollector *collector, const char *name, size_t name_size) {
  if (!collector) return GRD_ERROR_NULL_POINTER;
  if (!name) return GRD_SUCCESS;
  ++collector->seen;

  PrefixKey key;
  prefix_tree_key(name, name_size, NAME_PREFIX_DEPTH, key);
  size_t carried = name_size < NAME_PREFIX_DEPTH ? name_size : NAME_PREFIX_DEPTH;
  size_t rest_size = name_size - carried;

  /* --- the same name again, right behind itself: let it pass --- */
  for (unsigned slot = 0; slot < NAME_RECENT_SLOTS; ++slot) {
    const NameRecent *recent = &collector->recent[slot];
    if (recent->size != name_size) continue;
    if (memcmp(recent->key, key, NAME_PREFIX_DEPTH) != 0) continue;
    if (rest_size && memcmp(recent->rest, name + carried, rest_size) != 0) continue;
    return GRD_SUCCESS;
  }

  size_t group_index = 0;
  grd_result result = prefix_tree_intern(&collector->prefixes, key, &group_index, NULL);
  if (result != GRD_SUCCESS) return result;

  /* indices are handed out densely, so at most one group is ever missing —
     opening it here also repairs a group left behind by an earlier failure */
  while (name_group_vec_size(&collector->groups) <= group_index) {
    name_vec *group = NULL;
    result = name_group_vec_emplace(&collector->groups, &group);
    if (result != GRD_SUCCESS) return result;
    name_vec_init(group, NULL);
  }

  NameRef stored = name_empty_suffix;
  if (rest_size) {
    uint8_t *copy = NULL;
    result = meta_area_alloc(collector->alloc, &copy, rest_size + 1);
    if (result != GRD_SUCCESS) return result;
    memcpy(copy, name + carried, rest_size);
    copy[rest_size] = '\0';
    stored = (NameRef)copy;
  }

  result = name_vec_push(name_group_vec_get(&collector->groups, group_index), stored);
  if (result != GRD_SUCCESS) return result;
  ++collector->size;

  NameRecent *recent = &collector->recent[collector->recent_next];
  memcpy(recent->key, key, sizeof(PrefixKey));
  recent->rest = stored;
  recent->size = name_size;
  collector->recent_next = (collector->recent_next + 1) % NAME_RECENT_SLOTS;
  return GRD_SUCCESS;
}

size_t name_collector_size(const NameCollector *collector) {
  return collector ? collector->size : 0;
}

size_t name_collector_seen(const NameCollector *collector) {
  return collector ? collector->seen : 0;
}

size_t name_collector_prefix_count(const NameCollector *collector) {
  return collector ? prefix_tree_count(&collector->prefixes) : 0;
}

void name_collector_free(NameCollector *collector) {
  if (!collector) return;
  for (size_t g = 0, groups = name_group_vec_size(&collector->groups); g < groups; ++g) {
    name_vec_free(name_group_vec_get(&collector->groups, g));
  }
  name_group_vec_free(&collector->groups);
  prefix_tree_free(&collector->prefixes);
  collector->alloc = NULL;
  collector->size = 0;
  collector->recent_next = 0;
  for (unsigned slot = 0; slot < NAME_RECENT_SLOTS; ++slot) {
    collector->recent[slot].size = SIZE_MAX;
    collector->recent[slot].rest = NULL;
  }
}

/* =========================================================================
 *  Shared ordering primitives
 * ========================================================================= */

/** Byte order over the stored remainder; qsort's view of two pointer slots. */
static int compare_names(const void *lhs, const void *rhs) {
  return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

/** Collapse a sorted range to its distinct names, in place; returns what remains. */
static size_t unique_in_place(const char **group, size_t count) {
  size_t unique = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!unique || strcmp(group[unique - 1], group[i]) != 0) { group[unique++] = group[i]; }
  }
  return unique;
}

/* =========================================================================
 *  Settling — the collecting thread puts its own stream in order
 * ========================================================================= */

/** What the tree walk fills while the run takes shape. */
typedef struct FinishContext {
  const NameCollector *collector;
  const char **flat;
  NameGroup *groups;
  size_t written;     /**< Names placed so far. */
  size_t group_count; /**< Groups closed so far. */
} FinishContext;

/** Gather one prefix group, sort it, let this thread's doubles fall away. */
static int finish_visit(const uint8_t *key, size_t index, void *user_data) {
  FinishContext *ctx = user_data;
  const name_vec *vec = name_group_vec_get(&ctx->collector->groups, index);
  size_t start = ctx->written;

  for (size_t b = 0, buckets = name_vec_bucket_count(vec); b < buckets; ++b) {
    size_t count = name_vec_bucket_size(vec, b);
    memcpy(ctx->flat + ctx->written, name_vec_bucket_data(vec, b), count * sizeof(*ctx->flat));
    ctx->written += count;
  }

  size_t raw = ctx->written - start;
  if (!raw) return 0;
  qsort(ctx->flat + start, raw, sizeof(*ctx->flat), compare_names);
  ctx->written = start + unique_in_place(ctx->flat + start, raw);

  NameGroup *group = &ctx->groups[ctx->group_count++];
  memcpy(group->key, key, sizeof(PrefixKey));
  group->start = start;
  group->count = ctx->written - start;
  return 0;
}

grd_result name_collector_finish(NameCollector *collector, NameRun *run) {
  if (!collector || !run) return GRD_ERROR_NULL_POINTER;
  memset(run, 0, sizeof(*run));

  size_t total = collector->size; /* names actually stored */
  size_t seen = collector->seen;  /* names offered, for the caller's report */
  size_t group_count = prefix_tree_count(&collector->prefixes);
  if (!total || !group_count) {
    name_collector_free(collector);
    return GRD_SUCCESS;
  }
  if (total > SIZE_MAX / sizeof(const char *)) return GRD_ERROR_OUT_OF_MEMORY;

  const char **flat = malloc(total * sizeof(*flat));
  NameGroup *groups = malloc(group_count * sizeof(*groups));
  if (!flat || !groups) {
    free(flat);
    free(groups);
    return GRD_ERROR_OUT_OF_MEMORY;
  }

  FinishContext ctx = {.collector = collector, .flat = flat, .groups = groups};
  prefix_tree_foreach(&collector->prefixes, finish_visit, &ctx);

  /* tree and vectors have handed everything over — give their memory back now,
     while the other threads are still sorting and the peak matters */
  name_collector_free(collector);

  if (ctx.written < total) {
    const char **shrunk = realloc(flat, ctx.written * sizeof(*flat));
    if (shrunk) flat = shrunk;
  }

  run->names = flat;
  run->groups = groups;
  run->group_count = ctx.group_count;
  run->count = ctx.written;
  run->total = seen;
  return GRD_SUCCESS;
}

void name_run_free(NameRun *run) {
  if (!run) return;
  free(run->names);
  free(run->groups);
  memset(run, 0, sizeof(*run));
}

/* =========================================================================
 *  Joining — k-way merge over the sorted runs, group by group
 * ========================================================================= */

/** One key of the union, with the runs that carry it. */
typedef struct MergeGroup {
  PrefixKey key;
  size_t start;                /**< Where the group may write into the flat array. */
  size_t raw;                  /**< Names entering the merge — the group's weight. */
  size_t produced;             /**< Distinct names written. */
  size_t source[NAME_RUN_MAX]; /**< Group index per run, SIZE_MAX when absent. */
} MergeGroup;

/** One worker's share of the union, plus everything it writes into. */
typedef struct MergeWorker {
  const NameRun *const *runs;
  size_t run_count;
  const char **flat; /**< Destination, shared but never overlapping. */
  MergeGroup *union_groups;
  size_t first_group; /**< First group of this share. */
  size_t end_group;   /**< One past the last group of this share. */
} MergeWorker;

/**
 * @brief Merge one key's names across all runs into @p dst.
 *
 *  Linear scan over the k heads — with k ≤ 16 that is cheaper and kinder to
 *  the cache than a heap.  Equal names collapse as they are emitted.
 *
 *  @return Number of distinct names written.
 */
static size_t merge_group_names(
    const MergeWorker *worker, const MergeGroup *group, const char **dst
) {
  const char *const *slices[NAME_RUN_MAX];
  size_t heads[NAME_RUN_MAX];
  size_t sizes[NAME_RUN_MAX];

  for (size_t r = 0; r < worker->run_count; ++r) {
    size_t source = group->source[r];
    if (source == SIZE_MAX) {
      slices[r] = NULL;
      sizes[r] = 0;
    } else {
      const NameRun *run = worker->runs[r];
      slices[r] = run->names + run->groups[source].start;
      sizes[r] = run->groups[source].count;
    }
    heads[r] = 0;
  }

  size_t written = 0;
  const char *last = NULL;
  for (;;) {
    size_t best = worker->run_count;
    for (size_t r = 0; r < worker->run_count; ++r) {
      if (heads[r] >= sizes[r]) continue;
      if (best == worker->run_count || strcmp(slices[r][heads[r]], slices[best][heads[best]]) < 0) {
        best = r;
      }
    }
    if (best == worker->run_count) break;

    const char *name = slices[best][heads[best]++];
    if (!last || (last != name && strcmp(last, name) != 0)) {
      dst[written++] = name;
      last = name;
    }
  }
  return written;
}

/** Merge every group of one share; the entry point of a merge thread. */
static void *merge_worker_run(void *arg) {
  MergeWorker *worker = arg;
  for (size_t g = worker->first_group; g < worker->end_group; ++g) {
    MergeGroup *group = &worker->union_groups[g];
    group->produced = merge_group_names(worker, group, worker->flat + group->start);
  }
  return NULL;
}

/**
 * @brief Build the union of all runs' group lists, in ascending key order.
 *
 *  Every run's list is already sorted, so one linear pass suffices.  Each
 *  union entry records where the key appears and how much weight it carries.
 *
 *  @return Number of union entries; @p raw_total receives the total weight.
 */
static size_t build_union(
    const NameRun *const *runs, size_t run_count, MergeGroup *union_groups, size_t *raw_total
) {
  size_t heads[NAME_RUN_MAX] = {0};
  size_t union_count = 0;
  size_t total = 0;

  for (;;) {
    const uint8_t *smallest = NULL;
    for (size_t r = 0; r < run_count; ++r) {
      if (heads[r] >= runs[r]->group_count) continue;
      const uint8_t *key = runs[r]->groups[heads[r]].key;
      if (!smallest || key_compare(key, smallest) < 0) smallest = key;
    }
    if (!smallest) break;

    MergeGroup *group = &union_groups[union_count++];
    memcpy(group->key, smallest, sizeof(PrefixKey));
    group->start = total;
    group->raw = 0;
    group->produced = 0;
    for (size_t r = 0; r < run_count; ++r) {
      group->source[r] = SIZE_MAX;
      if (heads[r] < runs[r]->group_count &&
          key_compare(runs[r]->groups[heads[r]].key, group->key) == 0) {
        group->source[r] = heads[r];
        group->raw += runs[r]->groups[heads[r]].count;
        ++heads[r];
      }
    }
    total += group->raw;
  }

  *raw_total = total;
  return union_count;
}

/**
 * @brief Cut the union into @p worker_count shares of similar weight.
 *
 *  Walks the groups and closes a share whenever the accumulated weight
 *  reaches its fair portion of @p total.  Boundaries stay monotonic; empty
 *  shares are legitimate when few groups carry everything.
 */
static void partition_groups(
    const MergeGroup *union_groups,
    size_t union_count,
    size_t total,
    unsigned worker_count,
    size_t *bounds
) {
  bounds[0] = 0;
  unsigned closed = 0;
  size_t acc = 0;
  for (size_t g = 0; g < union_count && closed + 1 < worker_count; ++g) {
    acc += union_groups[g].raw;
    if (acc * worker_count >= total * (size_t)(closed + 1)) { bounds[++closed] = g + 1; }
  }
  while (closed < worker_count) { bounds[++closed] = union_count; }
}

grd_result name_run_merge(
    NameSet *out, const NameRun *const *runs, size_t run_count, unsigned worker_count
) {
  if (!out) return GRD_ERROR_NULL_POINTER;
  memset(out, 0, sizeof(*out));
  grd_result result = prefix_tree_init(&out->prefixes, NAME_PREFIX_DEPTH);
  if (result != GRD_SUCCESS) return result;
  if (run_count && !runs) return GRD_ERROR_NULL_POINTER;
  if (run_count > NAME_RUN_MAX) return GRD_ERROR_INVALID_PARAM;

  size_t input = 0;       /* names entering the merge, per-thread doubles already gone */
  size_t total = 0;       /* names ever collected, for the caller's report */
  size_t group_bound = 0; /* upper bound for the union: no key can appear more often */
  for (size_t r = 0; r < run_count; ++r) {
    if (!runs[r]) return GRD_ERROR_NULL_POINTER;
    input += runs[r]->count;
    total += runs[r]->total;
    group_bound += runs[r]->group_count;
  }
  out->total = total;
  if (!input || !group_bound) return GRD_SUCCESS;
  if (input > SIZE_MAX / sizeof(const char *)) return GRD_ERROR_OUT_OF_MEMORY;

  if (worker_count < 1) worker_count = 1;
  if (worker_count > NAME_RUN_MAX) worker_count = NAME_RUN_MAX;

  const char **flat = malloc(input * sizeof(*flat));
  MergeGroup *union_groups = malloc(group_bound * sizeof(*union_groups));
  if (!flat || !union_groups) {
    free(flat);
    free(union_groups);
    return GRD_ERROR_OUT_OF_MEMORY;
  }

  size_t raw_total = 0;
  size_t union_count = build_union(runs, run_count, union_groups, &raw_total);

  size_t bounds[NAME_RUN_MAX + 1];
  partition_groups(union_groups, union_count, raw_total, worker_count, bounds);

  MergeWorker workers[NAME_RUN_MAX];
  pthread_t threads[NAME_RUN_MAX];
  int started[NAME_RUN_MAX] = {0};
  for (unsigned w = 0; w < worker_count; ++w) {
    workers[w] = (MergeWorker){
        .runs = runs,
        .run_count = run_count,
        .flat = flat,
        .union_groups = union_groups,
        .first_group = bounds[w],
        .end_group = bounds[w + 1],
    };
  }
  /* share 0 stays with the caller; a thread that refuses to start is simply
     carried by the caller as well, so the merge never depends on the pool */
  for (unsigned w = 1; w < worker_count; ++w) {
    started[w] = pthread_create(&threads[w], NULL, merge_worker_run, &workers[w]) == 0;
  }
  merge_worker_run(&workers[0]);
  for (unsigned w = 1; w < worker_count; ++w) {
    if (started[w]) {
      pthread_join(threads[w], NULL);
    } else {
      merge_worker_run(&workers[w]);
    }
  }

  /* --- close the gaps the dissolved duplicates left between the groups --- */
  NameGroup *groups = malloc(union_count * sizeof(*groups));
  if (!groups) {
    free(flat);
    free(union_groups);
    return GRD_ERROR_OUT_OF_MEMORY;
  }

  size_t written = 0;
  size_t group_count = 0;
  for (size_t g = 0; g < union_count; ++g) {
    const MergeGroup *merged = &union_groups[g];
    if (!merged->produced) continue;
    if (written != merged->start) {
      memmove(flat + written, flat + merged->start, merged->produced * sizeof(*flat));
    }
    NameGroup *group = &groups[group_count];
    memcpy(group->key, merged->key, sizeof(PrefixKey));
    group->start = written;
    group->count = merged->produced;

    size_t index = 0;
    result = prefix_tree_intern(&out->prefixes, group->key, &index, NULL);
    if (result != GRD_SUCCESS) {
      free(flat);
      free(groups);
      free(union_groups);
      prefix_tree_free(&out->prefixes);
      memset(out, 0, sizeof(*out));
      return result;
    }
    ++group_count;
    written += merged->produced;
  }
  free(union_groups);

  if (written < input) {
    const char **shrunk = realloc(flat, written * sizeof(*flat));
    if (shrunk) flat = shrunk;
  }

  out->names = flat;
  out->groups = groups;
  out->group_count = group_count;
  out->count = written;
  return GRD_SUCCESS;
}

/* =========================================================================
 *  Reading the merged set
 * ========================================================================= */

const NameGroup *name_set_find(const NameSet *set, const char *name, size_t name_size) {
  if (!set || !set->groups) return NULL;
  PrefixKey key;
  prefix_tree_key(name, name_size, NAME_PREFIX_DEPTH, key);
  size_t index = 0;
  if (!prefix_tree_find(&set->prefixes, key, &index) || index >= set->group_count) return NULL;
  return &set->groups[index];
}

bool name_set_rank(const NameSet *set, const char *word, size_t size, size_t *out_rank) {
  const NameGroup *group = name_set_find(set, word, size);
  if (!group) return false;

  /* the group carries the leading bytes; only the remainder is compared, and
     the caller's word need not be NUL-terminated — its length is the truth */
  size_t carried = size < NAME_PREFIX_DEPTH ? size : NAME_PREFIX_DEPTH;
  const char *rest = word + carried;
  size_t rest_size = size - carried;

  size_t low = group->start;
  size_t high = group->start + group->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const char *candidate = set->names[middle];
    size_t candidate_size = strlen(candidate);
    size_t shared = candidate_size < rest_size ? candidate_size : rest_size;
    int order = shared ? memcmp(candidate, rest, shared) : 0;
    if (order == 0 && candidate_size != rest_size) { order = candidate_size < rest_size ? -1 : 1; }
    if (order == 0) {
      if (out_rank) *out_rank = middle;
      return true;
    }
    if (order < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return false;
}

const NameGroup *name_set_group_at(const NameSet *set, size_t group_index) {
  if (!set || !set->groups || group_index >= set->group_count) return NULL;
  return &set->groups[group_index];
}

const char *const *name_set_group_names(const NameSet *set, const NameGroup *group) {
  if (!set || !set->names || !group) return NULL;
  return set->names + group->start;
}

size_t name_set_compose(
    const NameSet *set, const NameGroup *group, size_t index, char *buffer, size_t buffer_size
) {
  if (buffer && buffer_size) buffer[0] = '\0';
  if (!set || !set->names || !group || index >= group->count) return 0;

  /* the key gives back what it carried; trailing padding was never part of a name */
  char head[PREFIX_TREE_DEPTH_MAX];
  size_t head_len = 0;
  for (unsigned d = 0; d < NAME_PREFIX_DEPTH && group->key[d]; ++d) {
    head[head_len++] = (char)group->key[d];
  }

  const char *rest = set->names[group->start + index];
  size_t rest_len = strlen(rest);
  size_t full_len = head_len + rest_len;
  if (!buffer || !buffer_size) return full_len;

  size_t copied = head_len < buffer_size - 1 ? head_len : buffer_size - 1;
  memcpy(buffer, head, copied);
  size_t room = buffer_size - 1 - copied;
  size_t rest_copied = rest_len < room ? rest_len : room;
  memcpy(buffer + copied, rest, rest_copied);
  buffer[copied + rest_copied] = '\0';
  return full_len;
}

void name_set_free(NameSet *set) {
  if (!set) return;
  free(set->names);
  free(set->groups);
  prefix_tree_free(&set->prefixes);
  memset(set, 0, sizeof(*set));
}

/** @endcond */

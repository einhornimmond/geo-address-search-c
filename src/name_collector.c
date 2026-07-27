/** @cond INTERNAL */

#include "name_collector.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** Bucket vector bodies — exactly one translation unit defines them. */
GRDU_BVEC_DEFINE(name_vec, NameRef, NAME_VEC_BUCKET_LOG2, )

/** Shared remainder of every name that is fully carried by its prefix. */
static const char name_empty_suffix[] = "";

/* =========================================================================
 *  Per-thread collecting
 * ========================================================================= */

grd_result name_collector_init(NameCollector *collector, MetaAreaAllocator *alloc) {
  if (!collector || !alloc) return GRD_ERROR_NULL_POINTER;

  collector->prefixes = calloc(NAME_PREFIX_COUNT, sizeof(*collector->prefixes));
  if (!collector->prefixes) return GRD_ERROR_OUT_OF_MEMORY;

  /* NULL → malloc/free for the vectors' own bookkeeping */
  for (size_t p = 0; p < NAME_PREFIX_COUNT; ++p) { name_vec_init(&collector->prefixes[p], NULL); }
  collector->alloc = alloc;
  collector->size = 0;
  return GRD_SUCCESS;
}

grd_result name_collector_add(NameCollector *collector, const char *name, size_t name_size) {
  if (!collector) return GRD_ERROR_NULL_POINTER;
  if (!name) return GRD_SUCCESS;

  unsigned prefix = name_prefix_index(name, name_size);
  size_t carried = name_size < 2 ? name_size : 2; /* bytes the index already holds */
  size_t rest = name_size - carried;

  NameRef stored = name_empty_suffix;
  if (rest) {
    uint8_t *copy = NULL;
    grd_result result = meta_area_alloc(collector->alloc, &copy, rest + 1);
    if (result != GRD_SUCCESS) return result;
    memcpy(copy, name + carried, rest);
    copy[rest] = '\0';
    stored = (NameRef)copy;
  }

  grd_result result = name_vec_push(&collector->prefixes[prefix], stored);
  if (result != GRD_SUCCESS) return result;
  ++collector->size;
  return GRD_SUCCESS;
}

size_t name_collector_size(const NameCollector *collector) {
  return collector ? collector->size : 0;
}

size_t name_collector_used_prefixes(const NameCollector *collector) {
  if (!collector || !collector->prefixes) return 0;
  size_t used = 0;
  for (size_t p = 0; p < NAME_PREFIX_COUNT; ++p) {
    if (name_vec_size(&collector->prefixes[p])) ++used;
  }
  return used;
}

void name_collector_free(NameCollector *collector) {
  if (!collector) return;
  if (collector->prefixes) {
    for (size_t p = 0; p < NAME_PREFIX_COUNT; ++p) { name_vec_free(&collector->prefixes[p]); }
    free(collector->prefixes);
    collector->prefixes = NULL;
  }
  collector->alloc = NULL;
  collector->size = 0;
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

grd_result name_collector_finish(NameCollector *collector, NameRun *run) {
  if (!collector || !run) return GRD_ERROR_NULL_POINTER;
  memset(run, 0, sizeof(*run));
  if (!collector->prefixes) return GRD_ERROR_NULL_POINTER;

  size_t total = collector->size;
  if (!total) {
    /* an empty run still needs its boundaries, so readers need no special case */
    run->offsets = calloc(NAME_PREFIX_COUNT + 1, sizeof(*run->offsets));
    return run->offsets ? GRD_SUCCESS : GRD_ERROR_OUT_OF_MEMORY;
  }
  if (total > SIZE_MAX / sizeof(const char *)) return GRD_ERROR_OUT_OF_MEMORY;

  const char **flat = malloc(total * sizeof(*flat));
  size_t *offsets = malloc((NAME_PREFIX_COUNT + 1) * sizeof(*offsets));
  if (!flat || !offsets) {
    free(flat);
    free(offsets);
    return GRD_ERROR_OUT_OF_MEMORY;
  }

  /* --- group by group: gather, sort, let this thread's doubles fall away --- */
  size_t written = 0;
  for (size_t p = 0; p < NAME_PREFIX_COUNT; ++p) {
    const name_vec *vec = &collector->prefixes[p];
    size_t start = written;
    offsets[p] = start;
    for (size_t b = 0, buckets = name_vec_bucket_count(vec); b < buckets; ++b) {
      size_t count = name_vec_bucket_size(vec, b);
      memcpy(flat + written, name_vec_bucket_data(vec, b), count * sizeof(*flat));
      written += count;
    }
    size_t raw = written - start;
    if (!raw) continue;
    qsort(flat + start, raw, sizeof(*flat), compare_names);
    written = start + unique_in_place(flat + start, raw);
  }
  offsets[NAME_PREFIX_COUNT] = written;

  /* the vectors have handed everything over — give their memory back now,
     while the other threads are still sorting and the peak matters */
  for (size_t p = 0; p < NAME_PREFIX_COUNT; ++p) { name_vec_free(&collector->prefixes[p]); }
  free(collector->prefixes);
  collector->prefixes = NULL;

  if (written < total) {
    const char **shrunk = realloc(flat, written * sizeof(*flat));
    if (shrunk) flat = shrunk;
  }

  run->names = flat;
  run->offsets = offsets;
  run->count = written;
  run->total = total;
  return GRD_SUCCESS;
}

void name_run_free(NameRun *run) {
  if (!run) return;
  free(run->names);
  free(run->offsets);
  memset(run, 0, sizeof(*run));
}

/* =========================================================================
 *  Joining — k-way merge over the sorted runs, prefix by prefix
 * ========================================================================= */

/** One worker's share of the prefix range, plus everything it writes into. */
typedef struct MergeWorker {
  const NameRun *const *runs; /**< Sorted runs to merge. */
  size_t run_count;           /**< Number of runs, ≤ NAME_RUN_MAX. */
  const char **flat;          /**< Destination array, shared but never overlapping. */
  const size_t *starts;       /**< Raw start offset of every prefix group. */
  size_t *produced;           /**< Receives the distinct count per prefix. */
  size_t first_prefix;        /**< First prefix of this share. */
  size_t end_prefix;          /**< One past the last prefix of this share. */
} MergeWorker;

/**
 * @brief Merge one prefix group of all runs into @p dst.
 *
 *  Linear scan over the k heads — with k ≤ 16 that is cheaper and kinder to
 *  the cache than a heap.  Equal names collapse as they are emitted.
 *
 *  @return Number of distinct names written.
 */
static size_t merge_prefix_group(const MergeWorker *worker, size_t prefix, const char **dst) {
  const char *const *groups[NAME_RUN_MAX];
  size_t heads[NAME_RUN_MAX];
  size_t sizes[NAME_RUN_MAX];

  for (size_t r = 0; r < worker->run_count; ++r) {
    const NameRun *run = worker->runs[r];
    size_t start = run->offsets[prefix];
    groups[r] = run->names + start;
    heads[r] = 0;
    sizes[r] = run->offsets[prefix + 1] - start;
  }

  size_t written = 0;
  const char *last = NULL;
  for (;;) {
    size_t best = worker->run_count;
    for (size_t r = 0; r < worker->run_count; ++r) {
      if (heads[r] >= sizes[r]) continue;
      if (best == worker->run_count ||
          strcmp(groups[r][heads[r]], groups[best][heads[best]]) < 0) {
        best = r;
      }
    }
    if (best == worker->run_count) break;

    const char *name = groups[best][heads[best]++];
    if (!last || (last != name && strcmp(last, name) != 0)) {
      dst[written++] = name;
      last = name;
    }
  }
  return written;
}

/** Merge every prefix of one share; the entry point of a merge thread. */
static void *merge_worker_run(void *arg) {
  MergeWorker *worker = arg;
  for (size_t p = worker->first_prefix; p < worker->end_prefix; ++p) {
    worker->produced[p] = merge_prefix_group(worker, p, worker->flat + worker->starts[p]);
  }
  return NULL;
}

/**
 * @brief Cut the prefix range into @p worker_count shares of similar weight.
 *
 *  Walks the per-prefix counts and closes a share whenever the accumulated
 *  weight reaches its fair portion of @p total.  Boundaries stay monotonic;
 *  empty shares are legitimate when few prefixes carry everything.
 */
static void partition_prefixes(
    const size_t *counts, size_t total, unsigned worker_count, size_t *bounds
) {
  bounds[0] = 0;
  unsigned closed = 0;
  size_t acc = 0;
  for (size_t p = 0; p < NAME_PREFIX_COUNT && closed + 1 < worker_count; ++p) {
    acc += counts[p];
    if (acc * worker_count >= total * (size_t)(closed + 1)) { bounds[++closed] = p + 1; }
  }
  while (closed < worker_count) { bounds[++closed] = NAME_PREFIX_COUNT; }
}

grd_result name_run_merge(
    NameSet *out, const NameRun *const *runs, size_t run_count, unsigned worker_count
) {
  if (!out) return GRD_ERROR_NULL_POINTER;
  memset(out, 0, sizeof(*out));
  if (run_count && !runs) return GRD_ERROR_NULL_POINTER;
  if (run_count > NAME_RUN_MAX) return GRD_ERROR_INVALID_PARAM;

  size_t input = 0;  /* names entering the merge, per-thread duplicates already gone */
  size_t total = 0;  /* names ever collected, for the caller's report */
  for (size_t r = 0; r < run_count; ++r) {
    if (!runs[r] || !runs[r]->offsets) return GRD_ERROR_NULL_POINTER;
    input += runs[r]->count;
    total += runs[r]->total;
  }
  out->total = total;
  if (!input) return GRD_SUCCESS;
  if (input > SIZE_MAX / sizeof(const char *)) return GRD_ERROR_OUT_OF_MEMORY;

  if (worker_count < 1) worker_count = 1;
  if (worker_count > NAME_RUN_MAX) worker_count = NAME_RUN_MAX;

  const char **flat = malloc(input * sizeof(*flat));
  size_t *offsets = malloc((NAME_PREFIX_COUNT + 1) * sizeof(*offsets));
  size_t *produced = calloc(NAME_PREFIX_COUNT, sizeof(*produced));
  if (!flat || !offsets || !produced) {
    free(flat);
    free(offsets);
    free(produced);
    return GRD_ERROR_OUT_OF_MEMORY;
  }

  /* --- where each group may write: the raw sum of its runs, gaps included --- */
  size_t raw_total = 0;
  for (size_t p = 0; p < NAME_PREFIX_COUNT; ++p) {
    offsets[p] = raw_total;
    for (size_t r = 0; r < run_count; ++r) {
      raw_total += runs[r]->offsets[p + 1] - runs[r]->offsets[p];
    }
    produced[p] = raw_total - offsets[p]; /* weight of the group, for the partition */
  }
  offsets[NAME_PREFIX_COUNT] = raw_total;

  size_t bounds[NAME_RUN_MAX + 1];
  partition_prefixes(produced, raw_total, worker_count, bounds);
  memset(produced, 0, NAME_PREFIX_COUNT * sizeof(*produced));

  MergeWorker workers[NAME_RUN_MAX];
  pthread_t threads[NAME_RUN_MAX];
  int started[NAME_RUN_MAX] = {0};
  for (unsigned w = 0; w < worker_count; ++w) {
    workers[w] = (MergeWorker){
        .runs = runs,
        .run_count = run_count,
        .flat = flat,
        .starts = offsets,
        .produced = produced,
        .first_prefix = bounds[w],
        .end_prefix = bounds[w + 1],
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
  size_t written = 0;
  size_t used_prefixes = 0;
  for (size_t p = 0; p < NAME_PREFIX_COUNT; ++p) {
    size_t start = offsets[p];
    size_t count = produced[p];
    offsets[p] = written;
    if (!count) continue;
    if (written != start) { memmove(flat + written, flat + start, count * sizeof(*flat)); }
    written += count;
    ++used_prefixes;
  }
  offsets[NAME_PREFIX_COUNT] = written;
  free(produced);

  if (written < input) {
    const char **shrunk = realloc(flat, written * sizeof(*flat));
    if (shrunk) flat = shrunk;
  }

  out->names = flat;
  out->offsets = offsets;
  out->count = written;
  out->used_prefixes = used_prefixes;
  return GRD_SUCCESS;
}

/* =========================================================================
 *  Reading the merged set
 * ========================================================================= */

const char *const *name_set_group(const NameSet *set, unsigned prefix, size_t *out_count) {
  if (out_count) *out_count = 0;
  if (!set || !set->offsets || prefix >= NAME_PREFIX_COUNT) return NULL;

  size_t start = set->offsets[prefix];
  size_t count = set->offsets[prefix + 1] - start;
  if (!count) return NULL;
  if (out_count) *out_count = count;
  return set->names + start;
}

size_t name_set_compose(
    const NameSet *set, unsigned prefix, size_t index, char *buffer, size_t buffer_size
) {
  size_t count = 0;
  const char *const *group = name_set_group(set, prefix, &count);
  if (buffer && buffer_size) buffer[0] = '\0';
  if (!group || index >= count) return 0;

  /* the prefix gives back what it carried: one byte, two, or none at all */
  char head[2];
  size_t head_len = 0;
  unsigned first = (prefix >> 8) & 0xffu;
  unsigned second = prefix & 0xffu;
  if (first) head[head_len++] = (char)first;
  if (second) head[head_len++] = (char)second;

  const char *rest = group[index];
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
  free(set->offsets);
  memset(set, 0, sizeof(*set));
}

/** @endcond */

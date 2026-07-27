/** @cond INTERNAL */

#include "name_collector.h"

#include <stdlib.h>
#include <string.h>

/** Bucket vector bodies — exactly one translation unit defines them. */
GRDU_BVEC_DEFINE(name_vec, NameRef, NAME_VEC_BUCKET_LOG2, )

/* =========================================================================
 *  Per-thread collecting
 * ========================================================================= */

grd_result name_collector_init(NameCollector *collector, MetaAreaAllocator *alloc) {
  if (!collector || !alloc) return GRD_ERROR_NULL_POINTER;
  collector->alloc = alloc;
  return name_vec_init(&collector->names, NULL); /* NULL → malloc/free for the index array */
}

grd_result name_collector_add(NameCollector *collector, const char *name, size_t name_size) {
  if (!collector) return GRD_ERROR_NULL_POINTER;
  if (!name) return GRD_SUCCESS;

  size_t size = name_size + 1;
  uint8_t *copy = NULL;
  grd_result result = meta_area_alloc(collector->alloc, &copy, size);
  if (result != GRD_SUCCESS) return result;

  memcpy(copy, name, size);
  return name_vec_push(&collector->names, (NameRef)copy);
}

size_t name_collector_size(const NameCollector *collector) {
  return collector ? name_vec_size(&collector->names) : 0;
}

void name_collector_free(NameCollector *collector) {
  if (!collector) return;
  name_vec_free(&collector->names);
  collector->alloc = NULL;
}

/* =========================================================================
 *  Merging — after the threads have come to rest
 * ========================================================================= */

/** Byte order over the name text; qsort's view of two pointer slots. */
static int compare_names(const void *lhs, const void *rhs) {
  return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

grd_result name_collector_merge(
    NameSet *out, const NameCollector *const *collectors, size_t collector_count
) {
  if (!out) return GRD_ERROR_NULL_POINTER;
  out->names = NULL;
  out->count = 0;
  out->total = 0;
  if (collector_count && !collectors) return GRD_ERROR_NULL_POINTER;

  /* --- how much water do all streams carry together --- */
  size_t total = 0;
  for (size_t c = 0; c < collector_count; ++c) {
    if (!collectors[c]) return GRD_ERROR_NULL_POINTER;
    total += name_vec_size(&collectors[c]->names);
  }
  if (!total) return GRD_SUCCESS;
  if (total > SIZE_MAX / sizeof(const char *)) return GRD_ERROR_OUT_OF_MEMORY;

  const char **flat = malloc(total * sizeof(*flat));
  if (!flat) return GRD_ERROR_OUT_OF_MEMORY;

  /* --- flatten bucket by bucket, over contiguous memory --- */
  size_t written = 0;
  for (size_t c = 0; c < collector_count; ++c) {
    const name_vec *vec = &collectors[c]->names;
    for (size_t b = 0, buckets = name_vec_bucket_count(vec); b < buckets; ++b) {
      size_t count = name_vec_bucket_size(vec, b);
      memcpy(flat + written, name_vec_bucket_data(vec, b), count * sizeof(*flat));
      written += count;
    }
  }

  qsort(flat, written, sizeof(*flat), compare_names);

  /* --- equal neighbours collapse into their first occurrence --- */
  size_t unique = 0;
  for (size_t i = 0; i < written; ++i) {
    if (!unique || strcmp(flat[unique - 1], flat[i]) != 0) { flat[unique++] = flat[i]; }
  }

  out->names = flat;
  out->count = unique;
  out->total = written;
  return GRD_SUCCESS;
}

void name_set_free(NameSet *set) {
  if (!set) return;
  free(set->names);
  set->names = NULL;
  set->count = 0;
  set->total = 0;
}

/** @endcond */

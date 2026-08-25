/** @cond INTERNAL */

#include "search/doc_collector.h"

#include "parser/json_parse.h" /* for PhotonPlaceType — a document keeps the kind it came from */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 *  Per-thread collecting
 * ========================================================================= */

/**
 * @brief Keep the name of the vector that ran out of buckets, and pass @p result on.
 *
 *  Only @c ARNM_ERROR_ARITHMETIC_OVERFLOW is a ceiling; everything else that can come
 *  back from a push is the allocator, and that says what it is on its own.  The first
 *  refusal is the one kept — a vector that fills after another has already filled says
 *  nothing about how the build should be sized.
 */
static arnm_result note_limit(
    DocCollector *collector, arnm_result result, const char *vector, const arnm_bvec *vec
) {
  if (ARNM_ERROR_ARITHMETIC_OVERFLOW == result && !collector->limit.vector) {
    collector->limit.vector = vector;
    collector->limit.held = arnm_bvec_size(vec);
    collector->limit.ceiling = GEO_VEC_CEILING;
  }
  return result;
}

bool doc_collector_limit(const DocCollector *collector, CollectorLimit *out) {
  if (!collector || !collector->limit.vector) return false;
  if (out) *out = collector->limit;
  return true;
}

arnm_result doc_collector_init(DocCollector *collector) {
  if (!collector) return ARNM_ERROR_NULL_POINTER;
  collector->dropped_words = 0;
  collector->dropped_doubles = 0;
  collector->seen_count = 0;
  collector->limit = (CollectorLimit){NULL, 0, 0};
  arnm_result result =
      geo_document_vec_init(&collector->documents, GEO_DOCUMENT_VEC_BUCKET_LOG2, 0, NULL);
  if (result != ARNM_SUCCESS) return result;
  result = geo_variant_vec_init(&collector->variants, GEO_VARIANT_VEC_BUCKET_LOG2, 0, NULL);
  if (result != ARNM_SUCCESS) return result;
  result = geo_word_vec_init(&collector->words, GEO_WORD_VEC_BUCKET_LOG2, 0, NULL);
  if (result != ARNM_SUCCESS) return result;
  return geo_start_vec_init(&collector->starts, GEO_START_VEC_BUCKET_LOG2, 0, NULL);
}

void doc_collector_free(DocCollector *collector) {
  if (!collector) return;
  geo_document_vec_free(&collector->documents);
  geo_variant_vec_free(&collector->variants);
  geo_word_vec_free(&collector->words);
  geo_start_vec_free(&collector->starts);
  collector->dropped_words = 0;
}

arnm_result doc_collector_add_document(
    DocCollector *collector, const GeoDocument *document, uint32_t *out_number
) {
  if (!collector || !document || !out_number) return ARNM_ERROR_NULL_POINTER;
  size_t number = geo_document_vec_size(&collector->documents);

  /* the words of this document begin where the words so far end */
  arnm_result result = note_limit(
      collector,
      geo_start_vec_push(&collector->starts, (uint32_t)geo_word_vec_size(&collector->words)),
      "geo_start_vec", &collector->starts
  );
  if (result != ARNM_SUCCESS) return result;
  result = note_limit(
      collector, geo_document_vec_push_ptr(&collector->documents, document), "geo_document_vec",
      &collector->documents
  );
  if (result != ARNM_SUCCESS) return result;

  collector->seen_count = 0; /* a fresh document has heard nothing yet */
  *out_number = (uint32_t)number;
  return ARNM_SUCCESS;
}

arnm_result doc_collector_add_variant(
    DocCollector *collector, uint32_t language, uint32_t name_rank, uint32_t city_rank
) {
  if (!collector) return ARNM_ERROR_NULL_POINTER;
  if (name_rank == GEO_RANK_NONE && city_rank == GEO_RANK_NONE) return ARNM_SUCCESS;
  size_t documents = geo_document_vec_size(&collector->documents);
  if (!documents) return ARNM_SUCCESS; /* nothing is open to hang it on */

  GeoVariantRecord reading = {
      .record = (uint32_t)(documents - 1),
      .name_rank = name_rank,
      .city_rank = city_rank,
      .language = language,
  };
  return note_limit(
      collector, geo_variant_vec_push_ptr(&collector->variants, &reading), "geo_variant_vec",
      &collector->variants
  );
}

arnm_result doc_collector_add_posting(DocCollector *collector, uint32_t word) {
  if (!collector) return ARNM_ERROR_NULL_POINTER;

  /* --- the same word, still the same document: it has already been noted --- */
  for (size_t s = 0; s < collector->seen_count; ++s) {
    if (collector->seen[s] == word) {
      ++collector->dropped_doubles;
      return ARNM_SUCCESS;
    }
  }
  if (collector->seen_count < POSTING_RUN_MAX) collector->seen[collector->seen_count++] = word;

  return note_limit(
      collector, geo_word_vec_push(&collector->words, word), "geo_word_vec", &collector->words
  );
}

size_t doc_collector_document_count(const DocCollector *collector) {
  return collector ? geo_document_vec_size(&collector->documents) : 0;
}

size_t doc_collector_posting_count(const DocCollector *collector) {
  return collector ? geo_word_vec_size(&collector->words) : 0;
}

/* =========================================================================
 *  Joining
 * ========================================================================= */

/** One record, keyed by what makes two of them the same place. */
typedef struct MergeKey {
  uint32_t name;
  uint32_t city;
  uint32_t postcode;
  uint32_t type;
  uint32_t record; /**< Where the record sits in the flattened array. */
} MergeKey;

/** Order by the key, then by record so a group's members stay in their old order. */
static int compare_merge_key(const void *lhs, const void *rhs) {
  const MergeKey *a = lhs;
  const MergeKey *b = rhs;
  if (a->name != b->name) return a->name < b->name ? -1 : 1;
  if (a->city != b->city) return a->city < b->city ? -1 : 1;
  if (a->postcode != b->postcode) return a->postcode < b->postcode ? -1 : 1;
  if (a->type != b->type) return a->type < b->type ? -1 : 1;
  return a->record < b->record ? -1 : (a->record > b->record ? 1 : 0);
}

/** 0.0027° ≈ 300 m — the distance below which two records of the same name in
 *  the same town are taken for the same place.  Longitude is compared with the
 *  same number of degrees, so the box widens towards the poles; for telling a
 *  duplicate from a different street that is close enough. */
#define MERGE_DISTANCE_E7 27000

/** Clusters one name-and-town group may hold before the rest fall into the last. */
#define MERGE_CLUSTER_MAX 64

/** Do these two records carry the same name in the same town? */
static bool same_name(const MergeKey *a, const MergeKey *b) {
  if (a->name == GEO_RANK_NONE || b->name == GEO_RANK_NONE) return false;
  return a->name == b->name && a->city == b->city;
}

/** Does @p document stand where the given point stands? */
static bool near(const GeoDocument *document, int32_t lat_e7, int32_t lon_e7) {
  if (!(document->flags & GEO_DOCUMENT_HAS_POINT)) return false;
  int64_t lat_gap = (int64_t)document->lat_e7 - lat_e7;
  int64_t lon_gap = (int64_t)document->lon_e7 - lon_e7;
  if (lat_gap < 0) lat_gap = -lat_gap;
  if (lon_gap < 0) lon_gap = -lon_gap;
  return lat_gap <= MERGE_DISTANCE_E7 && lon_gap <= MERGE_DISTANCE_E7;
}

/** A record with a postal code tells a reader more than one without. */
static bool better_display(const GeoDocument *candidate, const GeoDocument *held) {
  bool candidate_has = candidate->postcode_rank != GEO_RANK_NONE;
  bool held_has = held->postcode_rank != GEO_RANK_NONE;
  if (candidate_has != held_has) return candidate_has;
  return candidate->importance > held->importance;
}

/** Do these two records describe the same place? A nameless one never does. */
static bool same_place(const MergeKey *a, const MergeKey *b) {
  if (a->name == GEO_RANK_NONE || b->name == GEO_RANK_NONE) return false;
  return a->name == b->name && a->city == b->city && a->postcode == b->postcode &&
         a->type == b->type;
}

/** Which thread a flattened record came from — the ranges are contiguous. */
static size_t thread_of(const uint32_t *base, size_t collector_count, uint32_t record) {
  size_t thread = 0;
  while (thread + 1 < collector_count && base[thread + 1] <= record) { ++thread; }
  return thread;
}

/** A localized reading with its final document number, waiting to be ordered. */
typedef struct VariantSlot {
  uint32_t language;
  uint32_t document;
  uint32_t name_rank;
  uint32_t city_rank;
} VariantSlot;

/** By language first, then by document — the order the file is searched in. */
static int compare_variant_slot(const void *left, const void *right) {
  const VariantSlot *a = left, *b = right;
  if (a->language != b->language) return a->language < b->language ? -1 : 1;
  if (a->document != b->document) return a->document < b->document ? -1 : 1;
  return 0;
}

/** Where one record's words lie, and in which thread's vector. */
typedef struct WordRange {
  const arnm_bvec *words;
  uint32_t start;
  uint32_t count;
} WordRange;

/** Read the range of a record straight from the thread that collected it. */
static WordRange word_range_of(
    DocCollector *const *collectors, const uint32_t *base, size_t collector_count, uint32_t record
) {
  size_t thread = thread_of(base, collector_count, record);
  DocCollector *collector = collectors[thread];
  uint32_t local = record - base[thread];

  WordRange range = {.words = &collector->words, .start = 0, .count = 0};
  size_t documents = geo_start_vec_size(&collector->starts);
  if (local >= documents) return range;

  range.start = *geo_start_vec_get(&collector->starts, local);
  uint32_t end = local + 1 < documents ? *geo_start_vec_get(&collector->starts, local + 1)
                                       : (uint32_t)geo_word_vec_size(&collector->words);
  range.count = end - range.start;
  return range;
}

arnm_result doc_collector_merge(
    DocSet *out,
    DocCollector *const *collectors,
    size_t collector_count,
    size_t word_count,
    size_t language_count
) {
  if (!out) return ARNM_ERROR_NULL_POINTER;
  memset(out, 0, sizeof(*out));
  if (collector_count && !collectors) return ARNM_ERROR_NULL_POINTER;
  out->word_count = word_count;

  size_t record_count = 0;
  for (size_t c = 0; c < collector_count; ++c) {
    if (!collectors[c]) return ARNM_ERROR_NULL_POINTER;
    record_count += doc_collector_document_count(collectors[c]);
  }
  if (record_count > UINT32_MAX) return ARNM_ERROR_ARITHMETIC_OVERFLOW;
  if (collector_count > 64) return ARNM_ERROR_INVALID_PARAM;
  out->segment_count = record_count;
  if (!record_count) {
    out->posting_offsets = calloc(word_count + 1, sizeof(uint32_t));
    if (!out->posting_offsets) return ARNM_ERROR_OUT_OF_MEMORY;
    if (language_count) {
      out->language_offsets = calloc(language_count + 1, sizeof(uint32_t));
      if (!out->language_offsets) {
        free(out->posting_offsets);
        memset(out, 0, sizeof(*out));
        return ARNM_ERROR_OUT_OF_MEMORY;
      }
      out->language_count = language_count;
    }
    return ARNM_SUCCESS;
  }

  /* --- the records move into one array, thread after thread --- */
  GeoDocument *documents = NULL;
  uint32_t *assign = NULL;
  GeoStreetKey *streets = NULL;
  GeoVariant *variants = NULL;
  uint32_t *language_offsets = NULL;
  VariantSlot *slots = NULL;
  size_t variant_count = 0;
  GeoDocument *records = malloc(record_count * sizeof(*records));
  MergeKey *keys = malloc(record_count * sizeof(*keys));
  uint32_t *counts = calloc(word_count + 1, sizeof(*counts));
  uint32_t *stamp = calloc(word_count, sizeof(*stamp));
  uint32_t base[64];
  if (!records || !keys || !counts || !stamp) { goto out_of_memory; }

  size_t written = 0;
  for (size_t c = 0; c < collector_count; ++c) {
    base[c] = (uint32_t)written;
    arnm_bvec *vec = &collectors[c]->documents;
    for (uint16_t b = 0, buckets = geo_document_vec_bucket_count(vec); b < buckets; ++b) {
      size_t count = geo_document_vec_bucket_size(vec, b);
      memcpy(records + written, geo_document_vec_bucket_data(vec, b), count * sizeof(*records));
      written += count;
    }
    geo_document_vec_free(vec); /* the records live in the flat array now */
  }

  /* --- sort the records so that equal places stand together --- */
  for (size_t r = 0; r < record_count; ++r) {
    keys[r].name = records[r].name_rank;
    keys[r].city = records[r].city_rank;
    keys[r].postcode = records[r].postcode_rank;
    keys[r].type = records[r].type;
    keys[r].record = (uint32_t)r;
  }
  qsort(keys, record_count, sizeof(*keys), compare_merge_key);

  /* --- one document per place: first the segments of one key, then the
         neighbours that describe the same spot under another kind --- */
  documents = malloc(record_count * sizeof(*documents));
  assign = malloc(record_count * sizeof(*assign));
  if (!documents || !assign) { goto out_of_memory; }

  size_t document_count = 0;
  for (size_t i = 0; i < record_count;) {
    size_t group_end = i + 1;
    while (group_end < record_count && same_name(&keys[i], &keys[group_end])) { ++group_end; }

    /* clusters of this name-and-town group; their numbers run consecutively */
    size_t cluster_base = document_count;
    int64_t lat_sum[MERGE_CLUSTER_MAX], lon_sum[MERGE_CLUSTER_MAX];
    size_t points[MERGE_CLUSTER_MAX];
    size_t cluster_count = 0;

    for (size_t s = i; s < group_end;) {
      /* A street is a long thing: its pieces belong together however far apart
         their centres lie.  A town is a point — and towns of the same name are
         scattered over a country without having anything to do with each other,
         so those may only join when they stand in the same spot. */
      size_t part_end = s + 1;
      if (keys[s].type == PHOTON_PLACE_TYPE_STREET) {
        while (part_end < group_end && same_place(&keys[s], &keys[part_end])) { ++part_end; }
      }

      /* the pieces gathered here are averaged into one */
      GeoDocument piece = records[keys[s].record];
      int64_t piece_lat = 0, piece_lon = 0;
      size_t piece_points = 0;
      for (size_t m = s; m < part_end; ++m) {
        const GeoDocument *segment = &records[keys[m].record];
        if (segment->importance > piece.importance) piece.importance = segment->importance;
        if (!(segment->flags & GEO_DOCUMENT_HAS_POINT)) continue;
        piece_lat += segment->lat_e7;
        piece_lon += segment->lon_e7;
        ++piece_points;
      }

      /* --- does this piece stand where one of the clusters already stands? --- */
      size_t cluster = SIZE_MAX;
      if (piece_points) {
        int32_t lat = (int32_t)(piece_lat / (int64_t)piece_points);
        int32_t lon = (int32_t)(piece_lon / (int64_t)piece_points);
        for (size_t c = 0; c < cluster_count; ++c) {
          if (!points[c]) continue;
          if (near(&documents[cluster_base + c], lat, lon)) {
            cluster = c;
            break;
          }
        }
      }

      if (cluster == SIZE_MAX) {
        if (cluster_count >= MERGE_CLUSTER_MAX) {
          cluster = cluster_count - 1; /* a name this crowded settles into the last one */
        } else {
          cluster = cluster_count++;
          lat_sum[cluster] = 0;
          lon_sum[cluster] = 0;
          points[cluster] = 0;
          documents[cluster_base + cluster] = piece;
          ++document_count;
        }
      } else if (better_display(&piece, &documents[cluster_base + cluster])) {
        GeoDocument *held = &documents[cluster_base + cluster];
        uint16_t importance =
            held->importance > piece.importance ? held->importance : piece.importance;
        *held = piece;
        held->importance = importance;
      } else if (piece.importance > documents[cluster_base + cluster].importance) {
        documents[cluster_base + cluster].importance = piece.importance;
      }

      lat_sum[cluster] += piece_lat;
      lon_sum[cluster] += piece_lon;
      points[cluster] += piece_points;

      GeoDocument *held = &documents[cluster_base + cluster];
      if (points[cluster]) {
        held->lat_e7 = (int32_t)(lat_sum[cluster] / (int64_t)points[cluster]);
        held->lon_e7 = (int32_t)(lon_sum[cluster] / (int64_t)points[cluster]);
        held->flags |= GEO_DOCUMENT_HAS_POINT;
      } else {
        held->lat_e7 = 0;
        held->lon_e7 = 0;
        held->flags = (uint8_t)(held->flags & ~(unsigned)GEO_DOCUMENT_HAS_POINT);
      }

      for (size_t m = s; m < part_end; ++m) {
        assign[keys[m].record] = (uint32_t)(cluster_base + cluster);
      }
      s = part_end;
    }
    i = group_end;
  }
  /* --- the streets, as the houses will ask for them.  The keys are already
         sorted by name, town and postal code, so one walk collects them --- */
  streets = malloc(record_count * sizeof(*streets));
  if (!streets) { goto out_of_memory; }
  size_t street_count = 0;
  for (size_t i = 0; i < record_count;) {
    size_t end = i + 1;
    while (end < record_count && same_place(&keys[i], &keys[end])) { ++end; }
    if (keys[i].type == PHOTON_PLACE_TYPE_STREET && keys[i].name != GEO_RANK_NONE) {
      streets[street_count].name = keys[i].name;
      streets[street_count].city = keys[i].city;
      streets[street_count].postcode = keys[i].postcode;
      streets[street_count].document = assign[keys[i].record];
      ++street_count;
    }
    i = end;
  }
  if (street_count < record_count) {
    GeoStreetKey *shrunk = realloc(streets, (street_count ? street_count : 1) * sizeof(*streets));
    if (shrunk) streets = shrunk;
  }

  /* --- the localized readings follow their records into the new numbering ---
         A reading was collected against a thread-local record; that record has
         since been shifted into one range and possibly merged with others into
         a single document.  Both moves are the same lookup, so the readings
         travel through `assign` exactly as the postings do.  Where several
         segments of one street each brought a reading, the fields are gathered
         rather than fought over: the first segment that names the street in
         English names it for the whole street. */
  if (language_count) {
    language_offsets = calloc(language_count + 1, sizeof(*language_offsets));
    if (!language_offsets) { goto out_of_memory; }

    size_t gathered = 0;
    for (size_t c = 0; c < collector_count; ++c) {
      gathered += geo_variant_vec_size(&collectors[c]->variants);
    }
    if (gathered) {
      slots = malloc(gathered * sizeof(*slots));
      if (!slots) { goto out_of_memory; }
      size_t taken = 0;
      for (size_t c = 0; c < collector_count; ++c) {
        arnm_bvec *vec = &collectors[c]->variants;
        for (size_t v = 0, held = geo_variant_vec_size(vec); v < held; ++v) {
          const GeoVariantRecord *reading = geo_variant_vec_get(vec, v);
          if (reading->language >= language_count) continue; /* not a language of this build */
          uint32_t record = base[c] + reading->record;
          if (record >= record_count) continue;
          slots[taken].language = reading->language;
          slots[taken].document = assign[record];
          slots[taken].name_rank = reading->name_rank;
          slots[taken].city_rank = reading->city_rank;
          ++taken;
        }
      }
      qsort(slots, taken, sizeof(*slots), compare_variant_slot);

      /* one entry per language and document, the fields of its segments joined */
      for (size_t i = 0; i < taken;) {
        size_t end = i + 1;
        while (end < taken && slots[end].language == slots[i].language &&
               slots[end].document == slots[i].document) {
          ++end;
        }
        uint32_t name = GEO_RANK_NONE, city = GEO_RANK_NONE;
        for (size_t m = i; m < end; ++m) {
          if (name == GEO_RANK_NONE) name = slots[m].name_rank;
          if (city == GEO_RANK_NONE) city = slots[m].city_rank;
        }
        if (name != GEO_RANK_NONE || city != GEO_RANK_NONE) {
          slots[variant_count].language = slots[i].language;
          slots[variant_count].document = slots[i].document;
          slots[variant_count].name_rank = name;
          slots[variant_count].city_rank = city;
          ++variant_count;
        }
        i = end;
      }
    }

    if (variant_count) {
      variants = malloc(variant_count * sizeof(*variants));
      if (!variants) { goto out_of_memory; }
      for (size_t v = 0; v < variant_count; ++v) {
        variants[v].document = slots[v].document;
        variants[v].name_rank = slots[v].name_rank;
        variants[v].city_rank = slots[v].city_rank;
        ++language_offsets[slots[v].language + 1];
      }
    }
    for (size_t l = 0; l < language_count; ++l) { language_offsets[l + 1] += language_offsets[l]; }
    free(slots);
    slots = NULL;
  }
  for (size_t c = 0; c < collector_count; ++c) { geo_variant_vec_free(&collectors[c]->variants); }

  free(records);
  records = NULL;
  if (document_count < record_count) { /* the merged places left room behind */
    GeoDocument *shrunk = realloc(documents, document_count * sizeof(*documents));
    if (shrunk) documents = shrunk;
  }

  /* --- count, then place: both walks visit the documents in their new order,
         so every word's list ends up ascending and without doubles --- */
  size_t posting_count = 0;
  for (int pass = 0; pass < 2; ++pass) {
    memset(stamp, 0, word_count * sizeof(*stamp));
    for (size_t i = 0; i < record_count;) {
      size_t group_end = i + 1;
      while (group_end < record_count && same_name(&keys[i], &keys[group_end])) { ++group_end; }

      uint32_t lowest = UINT32_MAX, highest = 0;
      for (size_t m = i; m < group_end; ++m) {
        uint32_t document = assign[keys[m].record];
        if (document < lowest) lowest = document;
        if (document > highest) highest = document;
      }

      for (uint32_t document = lowest; document <= highest; ++document) {
        for (size_t m = i; m < group_end; ++m) {
          uint32_t record = keys[m].record;
          if (assign[record] != document) continue;
          WordRange range = word_range_of(collectors, base, collector_count, record);
          for (uint32_t k = 0; k < range.count; ++k) {
            uint32_t word = *geo_word_vec_get(range.words, range.start + k);
            if (stamp[word] == document + 1) continue; /* this document already has it */
            stamp[word] = document + 1;
            if (pass == 0) {
              ++counts[word];
              ++posting_count;
            } else {
              out->postings[counts[word]++] = document;
            }
          }
        }
      }
      i = group_end;
    }

    if (pass == 0) {
      /* counts become starting places */
      uint32_t running = 0;
      for (size_t w = 0; w < word_count; ++w) {
        uint32_t count = counts[w];
        counts[w] = running;
        running += count;
      }
      counts[word_count] = running;
      out->postings = malloc(posting_count * sizeof(*out->postings));
      out->posting_offsets = malloc((word_count + 1) * sizeof(*out->posting_offsets));
      if (!out->postings || !out->posting_offsets) { goto out_of_memory; }
      memcpy(out->posting_offsets, counts, (word_count + 1) * sizeof(*counts));
    }
  }

  for (size_t c = 0; c < collector_count; ++c) {
    geo_word_vec_free(&collectors[c]->words);
    geo_start_vec_free(&collectors[c]->starts);
  }
  free(keys);
  free(assign);
  free(counts);
  free(stamp);

  out->documents = documents;
  out->document_count = document_count;
  out->posting_count = posting_count;
  out->streets = streets;
  out->street_count = street_count;
  out->variants = variants;
  out->variant_count = variant_count;
  out->language_offsets = language_offsets;
  out->language_count = language_count;
  return ARNM_SUCCESS;

out_of_memory:
  free(records);
  free(documents);
  free(streets);
  free(variants);
  free(language_offsets);
  free(slots);
  free(assign);
  free(keys);
  free(counts);
  free(stamp);
  free(out->postings);
  free(out->posting_offsets);
  memset(out, 0, sizeof(*out));
  return ARNM_ERROR_OUT_OF_MEMORY;
}

/**
 * @brief Search the street table for one key.
 *
 *  @return Index of the entry, or the count when the key is absent.
 */
static size_t street_search(
    const GeoStreetKey *streets, size_t count, uint32_t name, uint32_t city, uint32_t postcode
) {
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const GeoStreetKey *entry = &streets[middle];
    int order = 0;
    if (entry->name != name) {
      order = entry->name < name ? -1 : 1;
    } else if (entry->city != city) {
      order = entry->city < city ? -1 : 1;
    } else if (entry->postcode != postcode) {
      order = entry->postcode < postcode ? -1 : 1;
    }
    if (!order) return middle;
    if (order < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return count;
}

/** First entry carrying this name and town, whatever its postal code. */
static size_t street_first_of_town(
    const GeoStreetKey *streets, size_t count, uint32_t name, uint32_t city
) {
  size_t low = 0, high = count;
  while (low < high) { /* lower bound of (name, city, anything) */
    size_t middle = low + (high - low) / 2;
    const GeoStreetKey *entry = &streets[middle];
    bool before = entry->name < name || (entry->name == name && entry->city < city);
    if (before) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low < count && streets[low].name == name && streets[low].city == city) return low;
  return count;
}

/** Lower bound of the entries carrying this name, whatever town they name. */
static size_t street_first_of_name(const GeoStreetKey *streets, size_t count, uint32_t name) {
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (streets[middle].name < name) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

/**
 * @brief Among the streets of this name, the one standing nearest the house.
 *
 *  @return Index into the street table, or the count when none is near enough.
 */
static size_t street_nearest(const DocSet *set, uint32_t name, int32_t lat_e7, int32_t lon_e7) {
  size_t best = set->street_count;
  int64_t best_gap = 0;
  size_t examined = 0;

  for (size_t i = street_first_of_name(set->streets, set->street_count, name);
       i < set->street_count && set->streets[i].name == name && examined < STREET_NEAREST_MAX;
       ++i, ++examined) {
    const GeoDocument *street = &set->documents[set->streets[i].document];
    if (!(street->flags & GEO_DOCUMENT_HAS_POINT)) continue;

    int64_t lat_gap = (int64_t)street->lat_e7 - lat_e7;
    int64_t lon_gap = (int64_t)street->lon_e7 - lon_e7;
    if (lat_gap < 0) lat_gap = -lat_gap;
    if (lon_gap < 0) lon_gap = -lon_gap;
    if (lat_gap > STREET_NEAREST_E7 || lon_gap > STREET_NEAREST_E7) continue;

    int64_t gap = lat_gap + lon_gap;
    if (best == set->street_count || gap < best_gap) {
      best = i;
      best_gap = gap;
    }
  }
  return best;
}

uint32_t doc_set_find_street(
    const DocSet *set,
    uint32_t name,
    uint32_t city,
    uint32_t postcode,
    int32_t lat_e7,
    int32_t lon_e7,
    int has_point,
    int *out_relaxed
) {
  if (out_relaxed) *out_relaxed = 0;
  if (!set || !set->streets || name == GEO_RANK_NONE) return GEO_RANK_NONE;

  size_t found = street_search(set->streets, set->street_count, name, city, postcode);
  if (found == set->street_count && postcode != GEO_RANK_NONE) {
    /* the dump gives the code to the house and withholds it from the street */
    found = street_search(set->streets, set->street_count, name, city, GEO_RANK_NONE);
    if (found < set->street_count && out_relaxed) *out_relaxed = 1;
  }
  if (found == set->street_count) {
    /* A street running through several postal codes carries one per segment,
       and the house may name a third.  Within one town a street name means one
       street, so the code is let go last of all. */
    found = street_first_of_town(set->streets, set->street_count, name, city);
    if (found < set->street_count && out_relaxed) *out_relaxed = 2;
  }
  if (found == set->street_count && has_point) {
    /* House and street name different towns.  Only the distance is left, and
       it is the more trustworthy witness. */
    found = street_nearest(set, name, lat_e7, lon_e7);
    if (found < set->street_count && out_relaxed) *out_relaxed = 3;
  }
  return found < set->street_count ? set->streets[found].document : GEO_RANK_NONE;
}

void doc_set_free(DocSet *set) {
  if (!set) return;
  free(set->documents);
  free(set->postings);
  free(set->posting_offsets);
  free(set->streets);
  free(set->variants);
  free(set->language_offsets);
  memset(set, 0, sizeof(*set));
}

/** @endcond */

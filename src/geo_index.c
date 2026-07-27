/** @cond INTERNAL */

#include "geo_index.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/** Sections begin on an 8-byte boundary so every record stays naturally aligned. */
#define GEO_INDEX_ALIGNMENT 8u

/** A frozen bitmap is a memory image and must begin on a 32-byte boundary. */
#define GEO_INDEX_BITMAP_ALIGNMENT 32u

/* =========================================================================
 *  Layout fingerprint
 * ========================================================================= */

/**
 * @brief Fold the sizes that define the format into one number.
 *
 *  A build whose records grew or whose prefix depth changed produces a
 *  different value and refuses a file it would misread.
 */
static uint32_t layout_hash(void) {
  const uint32_t parts[] = {
      (uint32_t)sizeof(GeoIndexHeader),  (uint32_t)sizeof(GeoIndexSection),
      (uint32_t)sizeof(GeoIndexGroup),   (uint32_t)sizeof(GeoDocument),
      (uint32_t)PREFIX_TREE_DEPTH_MAX,   (uint32_t)NAME_PREFIX_DEPTH,
      (uint32_t)GEO_INDEX_SECTION_COUNT,
  };
  uint32_t hash = 2166136261u; /* FNV-1a */
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
    hash ^= parts[i];
    hash *= 16777619u;
  }
  return hash;
}

/** Length of the leading bytes a key really carries — padding was never text. */
static size_t key_length(const uint8_t *key) {
  size_t length = 0;
  while (length < NAME_PREFIX_DEPTH && key[length]) { ++length; }
  return length;
}

/* =========================================================================
 *  Writing
 * ========================================================================= */

/** Pad the file until @p position sits on a multiple of @p alignment. */
static grd_result pad_to(FILE *file, uint64_t *position, uint64_t alignment) {
  static const char zeros[GEO_INDEX_BITMAP_ALIGNMENT] = {0};
  uint64_t misaligned = *position % alignment;
  if (!misaligned) return GRD_SUCCESS;
  size_t padding = (size_t)(alignment - misaligned);
  if (fwrite(zeros, 1, padding, file) != padding) return GRD_ERROR_ENCODE_FAILED;
  *position += padding;
  return GRD_SUCCESS;
}

/** Pad the file to the next section boundary. */
static grd_result pad_to_alignment(FILE *file, uint64_t *position) {
  return pad_to(file, position, GEO_INDEX_ALIGNMENT);
}

/** Open one section at the current position. */
static grd_result section_begin(
    FILE *file, uint64_t *position, GeoIndexSection *section, GeoIndexSectionKind kind
) {
  grd_result result = pad_to_alignment(file, position);
  if (result != GRD_SUCCESS) return result;
  section->kind = (uint32_t)kind;
  section->flags = 0;
  section->offset = *position;
  section->size = 0;
  return GRD_SUCCESS;
}

/** Append bytes to the open section. */
static grd_result section_put(
    FILE *file, uint64_t *position, GeoIndexSection *section, const void *data, size_t size
) {
  if (size && fwrite(data, 1, size, file) != size) return GRD_ERROR_ENCODE_FAILED;
  *position += size;
  section->size = *position - section->offset;
  return GRD_SUCCESS;
}

/**
 * @brief Write one dictionary as its three sections.
 *
 *  Words go in whole: the leading bytes their group carries, then the
 *  remainder that was stored.  What was split for order is joined again for
 *  reading.
 */
static grd_result write_dictionary(
    FILE *file,
    uint64_t *position,
    const NameSet *set,
    GeoIndexSection *sections,
    GeoIndexSectionKind groups_kind,
    GeoIndexSectionKind offsets_kind,
    GeoIndexSectionKind text_kind
) {
  /* --- the offset table has to exist before the text it describes --- */
  uint32_t *offsets = malloc((set->count + 1) * sizeof(*offsets));
  if (!offsets) return GRD_ERROR_OUT_OF_MEMORY;

  uint64_t text_size = 0;
  size_t counted = 0;
  for (size_t g = 0; g < set->group_count; ++g) {
    const NameGroup *group = &set->groups[g];
    size_t head = key_length(group->key);
    for (size_t i = 0; i < group->count; ++i) {
      offsets[counted++] = (uint32_t)text_size;
      text_size += head + strlen(set->names[group->start + i]);
      if (text_size > UINT32_MAX) { /* offsets are 32 bit — say so instead of wrapping */
        free(offsets);
        return GRD_ERROR_ARITHMETIC_OVERFLOW;
      }
    }
  }
  offsets[counted] = (uint32_t)text_size;

  grd_result result = section_begin(file, position, &sections[0], groups_kind);
  if (result != GRD_SUCCESS) goto done;
  for (size_t g = 0; g < set->group_count; ++g) {
    GeoIndexGroup record;
    memset(&record, 0, sizeof(record));
    memcpy(record.key, set->groups[g].key, PREFIX_TREE_DEPTH_MAX);
    record.start = (uint32_t)set->groups[g].start;
    record.count = (uint32_t)set->groups[g].count;
    result = section_put(file, position, &sections[0], &record, sizeof(record));
    if (result != GRD_SUCCESS) goto done;
  }

  result = section_begin(file, position, &sections[1], offsets_kind);
  if (result != GRD_SUCCESS) goto done;
  result = section_put(file, position, &sections[1], offsets, (set->count + 1) * sizeof(*offsets));
  if (result != GRD_SUCCESS) goto done;

  result = section_begin(file, position, &sections[2], text_kind);
  if (result != GRD_SUCCESS) goto done;
  for (size_t g = 0; g < set->group_count; ++g) {
    const NameGroup *group = &set->groups[g];
    size_t head = key_length(group->key);
    for (size_t i = 0; i < group->count; ++i) {
      const char *rest = set->names[group->start + i];
      result = section_put(file, position, &sections[2], group->key, head);
      if (result != GRD_SUCCESS) goto done;
      result = section_put(file, position, &sections[2], rest, strlen(rest));
      if (result != GRD_SUCCESS) goto done;
    }
  }

done:
  free(offsets);
  return result;
}

/**
 * @brief Write one Roaring bitmap per word and remember where each begins.
 *
 *  The lists arrive sorted and free of doubles, which is exactly what a bitmap
 *  wants; `run_optimize` then folds long stretches of neighbouring documents
 *  into runs, which streets in a town readily form.  Only one bitmap exists at
 *  a time — building seven million of them at once would cost more memory than
 *  the index itself.
 *
 *  The frozen format is written, not the portable one: it is the memory image
 *  a reader may look at directly, and it keeps keys and type codes inside the
 *  buffer instead of rebuilding them beside it.  The price is that every
 *  bitmap must begin on a 32-byte boundary — a few bytes of padding each, in
 *  exchange for a view that touches nothing misaligned.
 *
 *  @param[out] out_offsets  Receives a table of word_count + 1 byte offsets,
 *                           to be freed by the caller.
 */
static grd_result write_postings(
    FILE *file,
    uint64_t *position,
    GeoIndexSection *section,
    const DocSet *documents,
    uint64_t **out_offsets
) {
  size_t word_count = documents->word_count;
  uint64_t *offsets = malloc((word_count + 1) * sizeof(*offsets));
  if (!offsets) return GRD_ERROR_OUT_OF_MEMORY;

  grd_result result = pad_to(file, position, GEO_INDEX_BITMAP_ALIGNMENT);
  if (result == GRD_SUCCESS) {
    result = section_begin(file, position, section, GEO_INDEX_SECTION_POSTINGS);
  }
  if (result != GRD_SUCCESS) {
    free(offsets);
    return result;
  }

  char *buffer = NULL;
  size_t buffer_size = 0;

  for (size_t w = 0; w < word_count; ++w) {
    /* the table holds where the last bitmap ended; the next one begins at the
       following 32-byte mark, which is exactly what the reader computes */
    offsets[w] = *position - section->offset;

    uint32_t first = documents->posting_offsets[w];
    uint32_t last = documents->posting_offsets[w + 1];
    if (last <= first) continue; /* a word nobody used takes no bytes at all */

    roaring_bitmap_t *bitmap = roaring_bitmap_of_ptr(last - first, documents->postings + first);
    if (!bitmap) {
      result = GRD_ERROR_OUT_OF_MEMORY;
      break;
    }
    roaring_bitmap_run_optimize(bitmap);

    result = pad_to(file, position, GEO_INDEX_BITMAP_ALIGNMENT);
    if (result != GRD_SUCCESS) {
      roaring_bitmap_free(bitmap);
      break;
    }
    section->size = *position - section->offset;

    size_t needed = roaring_bitmap_frozen_size_in_bytes(bitmap);
    if (needed > buffer_size) {
      char *grown = realloc(buffer, needed);
      if (!grown) {
        roaring_bitmap_free(bitmap);
        result = GRD_ERROR_OUT_OF_MEMORY;
        break;
      }
      buffer = grown;
      buffer_size = needed;
    }
    roaring_bitmap_frozen_serialize(bitmap, buffer);
    roaring_bitmap_free(bitmap);

    result = section_put(file, position, section, buffer, needed);
    if (result != GRD_SUCCESS) break;
  }
  offsets[word_count] = *position - section->offset;

  free(buffer);
  if (result != GRD_SUCCESS) {
    free(offsets);
    return result;
  }
  *out_offsets = offsets;
  return GRD_SUCCESS;
}

grd_result geo_index_write(
    const char *path,
    const NameSet *words,
    const NameSet *display,
    const DocSet *documents,
    const HouseSet *houses,
    uint64_t total_terms
) {
  if (!path || !words || !display || !documents || !houses) return GRD_ERROR_NULL_POINTER;

  FILE *file = fopen(path, "wb");
  if (!file) return GRD_ERROR_ENCODE_FAILED;
  static char write_buffer[1 << 20];
  setvbuf(file, write_buffer, _IOFBF, sizeof(write_buffer));

  GeoIndexHeader header;
  memset(&header, 0, sizeof(header));
  GeoIndexSection sections[GEO_INDEX_SECTION_COUNT];
  memset(sections, 0, sizeof(sections));

  uint64_t position = sizeof(header) + sizeof(sections);
  uint64_t *posting_offsets = NULL;
  grd_result result = GRD_ERROR_ENCODE_FAILED;
  if (fseek(file, (long)position, SEEK_SET) != 0) goto failed;

  result = write_dictionary(
      file, &position, words, &sections[0], GEO_INDEX_SECTION_WORD_GROUPS,
      GEO_INDEX_SECTION_WORD_OFFSETS, GEO_INDEX_SECTION_WORD_TEXT
  );
  if (result != GRD_SUCCESS) goto failed;

  result = write_dictionary(
      file, &position, display, &sections[3], GEO_INDEX_SECTION_DISPLAY_GROUPS,
      GEO_INDEX_SECTION_DISPLAY_OFFSETS, GEO_INDEX_SECTION_DISPLAY_TEXT
  );
  if (result != GRD_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[6], GEO_INDEX_SECTION_DOCUMENTS);
  if (result != GRD_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[6], documents->documents,
      documents->document_count * sizeof(GeoDocument)
  );
  if (result != GRD_SUCCESS) goto failed;

  /* The bitmaps go down first and tell us their sizes on the way; the table of
     offsets follows behind them, because only then is it known. */
  result = write_postings(file, &position, &sections[8], documents, &posting_offsets);
  if (result != GRD_SUCCESS) goto failed;

  /* the weights travel apart from the records: ranking touches nothing else,
     and a hundred megabytes of them fit into the caches far better than two
     gigabytes of full documents */
  result = section_begin(file, &position, &sections[9], GEO_INDEX_SECTION_IMPORTANCE);
  if (result != GRD_SUCCESS) goto failed;
  for (size_t d = 0; d < documents->document_count; ++d) {
    uint16_t weight = documents->documents[d].importance;
    result = section_put(file, &position, &sections[9], &weight, sizeof(weight));
    if (result != GRD_SUCCESS) goto failed;
  }

  result = section_begin(file, &position, &sections[7], GEO_INDEX_SECTION_POSTING_OFFSETS);
  if (result != GRD_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[7], posting_offsets,
      (documents->word_count + 1) * sizeof(*posting_offsets)
  );
  if (result != GRD_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[10], GEO_INDEX_SECTION_HOUSES);
  if (result != GRD_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[10], houses->houses, houses->house_count * sizeof(GeoHouse)
  );
  if (result != GRD_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[11], GEO_INDEX_SECTION_HOUSE_OFFSETS);
  if (result != GRD_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[11], houses->offsets,
      (documents->document_count + 1) * sizeof(uint32_t)
  );
  if (result != GRD_SUCCESS) goto failed;

  /* --- back to the front, now that every offset is known --- */
  memcpy(header.magic, GEO_INDEX_MAGIC, sizeof(header.magic));
  header.version = GEO_INDEX_VERSION;
  header.byte_order = GEO_INDEX_BYTE_ORDER;
  header.layout_hash = layout_hash();
  header.section_count = GEO_INDEX_SECTION_COUNT;
  header.file_size = position;
  header.word_count = words->count;
  header.word_group_count = words->group_count;
  header.display_count = display->count;
  header.display_group_count = display->group_count;
  header.document_count = documents->document_count;
  header.posting_count = documents->posting_count;
  header.house_count = houses->house_count;
  header.total_terms = total_terms;

  result = GRD_ERROR_ENCODE_FAILED;
  if (fseek(file, 0, SEEK_SET) != 0) goto failed;
  if (fwrite(&header, sizeof(header), 1, file) != 1) goto failed;
  if (fwrite(sections, sizeof(sections), 1, file) != 1) goto failed;
  if (fflush(file)) goto failed;
  result = GRD_SUCCESS;

failed:
  free(posting_offsets);
  if (fclose(file) != 0 && result == GRD_SUCCESS) result = GRD_ERROR_ENCODE_FAILED;
  return result;
}

/* =========================================================================
 *  Opening
 * ========================================================================= */

/** Find a section by kind and check that it lies inside the mapping. */
static const void *section_of(
    const uint8_t *base,
    size_t size,
    const GeoIndexSection *sections,
    uint32_t section_count,
    uint32_t kind,
    uint64_t needed,
    uint64_t *out_size
) {
  for (uint32_t i = 0; i < section_count; ++i) {
    if (sections[i].kind != kind) continue;
    if (sections[i].offset > size || sections[i].size > size - sections[i].offset) return NULL;
    if (sections[i].size < needed) return NULL;
    if (sections[i].offset % GEO_INDEX_ALIGNMENT) return NULL;
    if (out_size) *out_size = sections[i].size;
    return base + sections[i].offset;
  }
  return NULL;
}

/** Bind one dictionary to its three sections and rebuild its tree. */
static grd_result open_dictionary(
    GeoDictionary *dictionary,
    const uint8_t *base,
    size_t size,
    const GeoIndexSection *sections,
    uint32_t section_count,
    uint64_t word_count,
    uint64_t group_count,
    GeoIndexSectionKind groups_kind,
    GeoIndexSectionKind offsets_kind,
    GeoIndexSectionKind text_kind
) {
  memset(dictionary, 0, sizeof(*dictionary));
  uint64_t text_size = 0;
  const GeoIndexGroup *groups = section_of(
      base, size, sections, section_count, groups_kind, group_count * sizeof(GeoIndexGroup), NULL
  );
  const uint32_t *offsets = section_of(
      base, size, sections, section_count, offsets_kind, (word_count + 1) * sizeof(uint32_t), NULL
  );
  const char *text = section_of(base, size, sections, section_count, text_kind, 0, &text_size);
  if (!groups || !offsets || !text) return GRD_ERROR_INVALID_PARAM;
  if (word_count && offsets[word_count] > text_size) return GRD_ERROR_INVALID_PARAM;

  grd_result result = prefix_tree_init(&dictionary->prefixes, NAME_PREFIX_DEPTH);
  if (result != GRD_SUCCESS) return result;
  for (uint64_t g = 0; g < group_count; ++g) {
    size_t assigned = 0;
    result = prefix_tree_intern(&dictionary->prefixes, groups[g].key, &assigned, NULL);
    /* the groups were written in key order; anything else is not our file */
    if (result == GRD_SUCCESS && assigned != g) result = GRD_ERROR_INVALID_PARAM;
    if (result != GRD_SUCCESS) {
      prefix_tree_free(&dictionary->prefixes);
      return result;
    }
  }

  dictionary->groups = groups;
  dictionary->offsets = offsets;
  dictionary->text = text;
  dictionary->group_count = (size_t)group_count;
  dictionary->word_count = (size_t)word_count;
  dictionary->text_size = (size_t)text_size;
  return GRD_SUCCESS;
}

grd_result geo_index_open(GeoIndex *index, const char *path) {
  if (!index || !path) return GRD_ERROR_NULL_POINTER;
  memset(index, 0, sizeof(*index));

  int descriptor = open(path, O_RDONLY);
  if (descriptor < 0) return GRD_ERROR_DECODE_FAILED;

  struct stat status;
  if (fstat(descriptor, &status) != 0 || status.st_size <= (off_t)sizeof(GeoIndexHeader)) {
    close(descriptor);
    return GRD_ERROR_DECODE_FAILED;
  }
  size_t size = (size_t)status.st_size;

  void *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
  close(descriptor); /* the mapping keeps the file alive on its own */
  if (mapping == MAP_FAILED) return GRD_ERROR_DECODE_FAILED;

  const uint8_t *base = mapping;
  const GeoIndexHeader *header = (const GeoIndexHeader *)base;

  /* --- nothing is trusted before the header agrees --- */
  grd_result result = GRD_ERROR_INVALID_PARAM;
  if (memcmp(header->magic, GEO_INDEX_MAGIC, sizeof(header->magic)) != 0) goto refused;
  if (header->version != GEO_INDEX_VERSION) goto refused;
  if (header->byte_order != GEO_INDEX_BYTE_ORDER) goto refused;
  if (header->layout_hash != layout_hash()) goto refused;
  if (header->file_size != size) goto refused;
  if (header->section_count == 0 || header->section_count > 64) goto refused;
  if (sizeof(GeoIndexHeader) + (uint64_t)header->section_count * sizeof(GeoIndexSection) > size) {
    goto refused;
  }
  if (header->word_count > UINT32_MAX || header->display_count > UINT32_MAX) goto refused;
  if (header->document_count > UINT32_MAX || header->posting_count > UINT32_MAX) goto refused;
  if (header->house_count > UINT32_MAX) goto refused;
  if (header->word_group_count > header->word_count) goto refused;
  if (header->display_group_count > header->display_count) goto refused;

  const GeoIndexSection *sections = (const GeoIndexSection *)(base + sizeof(GeoIndexHeader));
  result = open_dictionary(
      &index->words, base, size, sections, header->section_count, header->word_count,
      header->word_group_count, GEO_INDEX_SECTION_WORD_GROUPS, GEO_INDEX_SECTION_WORD_OFFSETS,
      GEO_INDEX_SECTION_WORD_TEXT
  );
  if (result != GRD_SUCCESS) goto refused;
  result = open_dictionary(
      &index->display, base, size, sections, header->section_count, header->display_count,
      header->display_group_count, GEO_INDEX_SECTION_DISPLAY_GROUPS,
      GEO_INDEX_SECTION_DISPLAY_OFFSETS, GEO_INDEX_SECTION_DISPLAY_TEXT
  );
  if (result != GRD_SUCCESS) {
    prefix_tree_free(&index->words.prefixes);
    goto refused;
  }

  result = GRD_ERROR_INVALID_PARAM;
  const GeoDocument *documents = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_DOCUMENTS,
      header->document_count * sizeof(GeoDocument), NULL
  );
  const uint16_t *importance = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_IMPORTANCE,
      header->document_count * sizeof(uint16_t), NULL
  );
  const GeoHouse *houses = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_HOUSES,
      header->house_count * sizeof(GeoHouse), NULL
  );
  const uint32_t *house_offsets = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_HOUSE_OFFSETS,
      (header->document_count + 1) * sizeof(uint32_t), NULL
  );
  const uint64_t *posting_offsets = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_POSTING_OFFSETS,
      (header->word_count + 1) * sizeof(uint64_t), NULL
  );
  uint64_t posting_bytes = 0;
  const char *postings = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_POSTINGS, 0, &posting_bytes
  );
  if (!documents || !importance || !posting_offsets || !postings) goto refused_dictionaries;
  if (!houses || !house_offsets) goto refused_dictionaries;
  if (house_offsets[header->document_count] != header->house_count) goto refused_dictionaries;
  /* The offsets are checked one at a time, when a word is looked up — walking
     seven million of them here would turn an instant open into a wait. */
  if (posting_offsets[header->word_count] > posting_bytes) goto refused_dictionaries;

  index->base = base;
  index->size = size;
  index->documents = documents;
  index->importance = importance;
  index->houses = houses;
  index->house_offsets = house_offsets;
  index->house_count = (size_t)header->house_count;
  index->document_count = (size_t)header->document_count;
  index->posting_offsets = posting_offsets;
  index->postings = postings;
  index->posting_bytes = (size_t)posting_bytes;
  index->posting_count = (size_t)header->posting_count;
  index->total_terms = header->total_terms;
  return GRD_SUCCESS;

refused_dictionaries:
  prefix_tree_free(&index->words.prefixes);
  prefix_tree_free(&index->display.prefixes);
refused:
  munmap(mapping, size);
  memset(index, 0, sizeof(*index));
  return result;
}

void geo_index_close(GeoIndex *index) {
  if (!index) return;
  if (index->base) munmap((void *)index->base, index->size);
  prefix_tree_free(&index->words.prefixes);
  prefix_tree_free(&index->display.prefixes);
  memset(index, 0, sizeof(*index));
}

/* =========================================================================
 *  Reading
 * ========================================================================= */

const char *geo_dictionary_word(const GeoDictionary *dictionary, size_t rank, size_t *out_size) {
  if (out_size) *out_size = 0;
  if (!dictionary || !dictionary->text || rank >= dictionary->word_count) return NULL;
  uint32_t start = dictionary->offsets[rank];
  uint32_t end = dictionary->offsets[rank + 1];
  if (out_size) *out_size = end - start;
  return dictionary->text + start;
}

bool geo_dictionary_find(
    const GeoDictionary *dictionary, const char *word, size_t size, size_t *out_rank
) {
  if (!dictionary || !dictionary->text || !word || !size) return false;

  PrefixKey key;
  prefix_tree_key(word, size, NAME_PREFIX_DEPTH, key);
  size_t group_index = 0;
  if (!prefix_tree_find(&dictionary->prefixes, key, &group_index)) return false;
  if (group_index >= dictionary->group_count) return false;

  /* --- inside the group the words lie in byte order --- */
  const GeoIndexGroup *group = &dictionary->groups[group_index];
  size_t low = group->start;
  size_t high = group->start + group->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    size_t candidate_size = 0;
    const char *candidate = geo_dictionary_word(dictionary, middle, &candidate_size);
    size_t shared = candidate_size < size ? candidate_size : size;
    int order = shared ? memcmp(candidate, word, shared) : 0;
    if (order == 0 && candidate_size != size) order = candidate_size < size ? -1 : 1;
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

const roaring_bitmap_t *geo_index_word_documents(const GeoIndex *index, size_t rank) {
  if (!index || !index->postings || rank >= index->words.word_count) return NULL;
  /* the table records where the previous bitmap ended; this one starts at the
     next 32-byte mark and reaches exactly to the entry behind it */
  uint64_t previous = index->posting_offsets[rank];
  uint64_t end = index->posting_offsets[rank + 1];
  uint64_t start =
      (previous + GEO_INDEX_BITMAP_ALIGNMENT - 1) & ~(uint64_t)(GEO_INDEX_BITMAP_ALIGNMENT - 1);
  if (end <= start || end > index->posting_bytes) return NULL; /* empty, or not ours */

  const char *buffer = index->postings + start;
  if ((uintptr_t)buffer % GEO_INDEX_BITMAP_ALIGNMENT) return NULL; /* not written by us */
  /* the length must be exact — a frozen bitmap reads its header from the end */
  return roaring_bitmap_frozen_view(buffer, (size_t)(end - start));
}

const GeoHouse *geo_index_houses(const GeoIndex *index, size_t document, size_t *out_count) {
  if (out_count) *out_count = 0;
  if (!index || !index->houses || document >= index->document_count) return NULL;
  uint32_t start = index->house_offsets[document];
  uint32_t end = index->house_offsets[document + 1];
  if (end <= start || end > index->house_count) return NULL;
  if (out_count) *out_count = end - start;
  return index->houses + start;
}

/* =========================================================================
 *  Querying
 * ========================================================================= */

/** Keep the heaviest hits in order, without ever growing beyond @p limit. */
static size_t hit_insert(GeoHit *hits, size_t count, size_t limit, GeoHit candidate) {
  if (count == limit && hits[count - 1].importance >= candidate.importance) return count;
  size_t position = count < limit ? count : limit - 1;
  while (position > 0 && hits[position - 1].importance < candidate.importance) {
    hits[position] = hits[position - 1];
    --position;
  }
  hits[position] = candidate;
  return count < limit ? count + 1 : count;
}

/** The readings of one query word — alternatives, never demands of their own. */
typedef struct QueryGroup {
  const roaring_bitmap_t *readings[4];
  size_t reading_count;
  uint64_t weight; /**< Documents the widest reading covers; decides the order. */
  uint16_t source; /**< The word of the query these readings came from. */
} QueryGroup;

/** Longest a query may be, in words; further words are ignored. */
#define GEO_QUERY_GROUP_MAX 16

/**
 * @brief Narrow @p carried down to the documents that also answer to @p group.
 *
 *  The readings are alternatives, so each is intersected with what is already
 *  carried and the results joined.  Intersecting first and joining after keeps
 *  the work inside the small set: uniting *muenchen* and *munchen* over the
 *  whole planet would build a bitmap of millions, only to throw all but a
 *  handful away.
 *
 *  @return The narrowed set, or NULL when nothing is left.
 */
static roaring_bitmap_t *narrow_by(const roaring_bitmap_t *carried, const QueryGroup *group) {
  roaring_bitmap_t *joined = NULL;
  for (size_t r = 0; r < group->reading_count; ++r) {
    roaring_bitmap_t *part = roaring_bitmap_and(carried, group->readings[r]);
    if (!part) continue;
    if (!joined) {
      joined = part;
    } else {
      roaring_bitmap_or_inplace(joined, part);
      roaring_bitmap_free(part);
    }
  }
  return joined;
}

/** Does this token carry a digit? Then it may be a house number. */
static bool token_has_digit(const TextToken *token) {
  for (size_t i = 0; i < token->size; ++i) {
    if (token->data[i] >= '0' && token->data[i] <= '9') return true;
  }
  return false;
}

/** Compare a written house number with a folded one, letters case aside. */
static bool number_equal(const char *written, size_t written_size, const TextToken *token) {
  if (written_size != token->size) return false;
  for (size_t i = 0; i < written_size; ++i) {
    char a = written[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (a != token->data[i]) return false;
  }
  return true;
}

/**
 * @brief Look for one number among the houses of a street.
 *
 *  The numbers of a street lie ordered by the rank of their spelling, not by
 *  their text, so this walks them.  A street carries tens of houses, rarely
 *  more than a few hundred — the walk costs less than the search would.
 *
 *  @return Index into the index's houses, or GEO_RANK_NONE.
 */
static uint32_t find_house(const GeoIndex *index, uint32_t document, const TextToken *number) {
  size_t count = 0;
  const GeoHouse *houses = geo_index_houses(index, document, &count);
  if (!houses) return GEO_RANK_NONE;

  for (size_t i = 0; i < count; ++i) {
    size_t written_size = 0;
    const char *written =
        geo_dictionary_word(&index->display, houses[i].number_rank, &written_size);
    if (written && number_equal(written, written_size, number)) {
      return (uint32_t)((houses - index->houses) + i);
    }
  }
  return GEO_RANK_NONE;
}

/**
 * @brief Answer the query's words, optionally leaving the numbers out.
 *
 *  @return Number of results written into @p hits.
 */
static size_t query_words(
    const GeoIndex *index,
    const TextTokenizer *tokenizer,
    bool without_numbers,
    GeoHit *hits,
    size_t limit
) {
  QueryGroup groups[GEO_QUERY_GROUP_MAX];
  size_t group_count = 0;
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    const TextToken *token = &tokenizer->tokens[t];
    if (token->part) continue; /* pieces of a compound serve the index, not the query */
    if (without_numbers && token_has_digit(token)) continue;

    size_t rank = 0;
    if (!geo_dictionary_find(&index->words, token->data, token->size, &rank)) {
      continue; /* a word nobody ever wrote cannot narrow anything down */
    }
    const roaring_bitmap_t *documents = geo_index_word_documents(index, rank);
    if (!documents) continue;

    /* readings of the same word join the same group */
    QueryGroup *group = NULL;
    for (size_t g = 0; g < group_count; ++g) {
      if (groups[g].source == token->group) { group = &groups[g]; }
    }
    if (!group) {
      if (group_count >= GEO_QUERY_GROUP_MAX) {
        roaring_bitmap_free(documents);
        break;
      }
      group = &groups[group_count++];
      group->reading_count = 0;
      group->weight = 0;
      group->source = token->group;
    }
    if (group->reading_count >= sizeof(group->readings) / sizeof(group->readings[0])) {
      roaring_bitmap_free(documents);
      continue;
    }
    uint64_t weight = roaring_bitmap_get_cardinality(documents);
    group->readings[group->reading_count++] = documents;
    if (weight > group->weight) group->weight = weight;
  }
  if (!group_count) return 0;

  /* --- narrowest word first, so the carried set shrinks as early as it can --- */
  for (size_t g = 1; g < group_count; ++g) {
    QueryGroup group = groups[g];
    size_t place = g;
    while (place > 0 && groups[place - 1].weight > group.weight) {
      groups[place] = groups[place - 1];
      --place;
    }
    groups[place] = group;
  }

  /* --- the first word is carried as it is, the rest narrow it --- */
  roaring_bitmap_t *carried = roaring_bitmap_copy(groups[0].readings[0]);
  for (size_t r = 1; carried && r < groups[0].reading_count; ++r) {
    roaring_bitmap_or_inplace(carried, groups[0].readings[r]);
  }
  for (size_t g = 1; carried && g < group_count; ++g) {
    roaring_bitmap_t *narrowed = narrow_by(carried, &groups[g]);
    roaring_bitmap_free(carried);
    carried = narrowed;
  }

  size_t count = 0;
  if (carried) {
    uint32_t batch[256];
    roaring_uint32_iterator_t walk;
    roaring_iterator_init(carried, &walk);
    for (;;) {
      uint32_t read =
          roaring_uint32_iterator_read(&walk, batch, (uint32_t)(sizeof(batch) / sizeof(batch[0])));
      if (!read) break;
      for (uint32_t i = 0; i < read; ++i) {
        uint32_t document = batch[i];
        if (document >= index->document_count) continue;
        GeoHit hit = {
            .document = document,
            .matched = (uint32_t)group_count,
            .house = GEO_RANK_NONE,
            .importance = index->importance[document],
        };
        count = hit_insert(hits, count, limit, hit);
      }
    }
    roaring_bitmap_free(carried);
  }

  for (size_t g = 0; g < group_count; ++g) {
    for (size_t r = 0; r < groups[g].reading_count; ++r) {
      roaring_bitmap_free(groups[g].readings[r]);
    }
  }
  return count;
}

size_t geo_index_query(
    const GeoIndex *index,
    TextTokenizer *tokenizer,
    const char *query,
    size_t size,
    GeoHit *hits,
    size_t limit
) {
  if (!index || !tokenizer || !query || !size || !hits || !limit) return 0;

  /* a query is never a repetition of the one before it */
  text_tokenizer_init(tokenizer);
  if (!text_tokenize(tokenizer, query, size)) return 0;

  /* --- a number in an address is a house before it is a word.  So the words
         are asked first without it; only if they answer with nothing does the
         number get its turn as a word of its own, the way *Straße des 17. Juni*
         needs it. --- */
  bool numbered = false;
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    if (!tokenizer->tokens[t].part && token_has_digit(&tokenizer->tokens[t])) { numbered = true; }
  }

  size_t count = numbered ? query_words(index, tokenizer, true, hits, limit) : 0;
  if (!count) return query_words(index, tokenizer, false, hits, limit);

  /* --- and now the number finds its door --- */
  for (size_t h = 0; h < count; ++h) {
    for (size_t t = 0; t < tokenizer->token_count; ++t) {
      const TextToken *token = &tokenizer->tokens[t];
      if (token->part || !token_has_digit(token)) continue;
      uint32_t house = find_house(index, hits[h].document, token);
      if (house != GEO_RANK_NONE) {
        hits[h].house = house;
        break;
      }
    }
  }

  /* --- whoever asked for a number means the street that has it.  The order
         among them stays as it was, so weight still decides within. --- */
  size_t front = 0;
  for (size_t h = 0; h < count; ++h) {
    if (hits[h].house == GEO_RANK_NONE) continue;
    GeoHit found = hits[h];
    for (size_t back = h; back > front; --back) { hits[back] = hits[back - 1]; }
    hits[front++] = found;
  }
  return count;
}

/** @endcond */

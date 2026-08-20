/** @cond INTERNAL */

#include "search/geo_index.h"

#include "search/geo_cell.h"

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
static hostmem_result pad_to(FILE *file, uint64_t *position, uint64_t alignment) {
  static const char zeros[GEO_INDEX_BITMAP_ALIGNMENT] = {0};
  uint64_t misaligned = *position % alignment;
  if (!misaligned) return HOSTMEM_SUCCESS;
  size_t padding = (size_t)(alignment - misaligned);
  if (fwrite(zeros, 1, padding, file) != padding) return HOSTMEM_ERROR_ENCODE_FAILED;
  *position += padding;
  return HOSTMEM_SUCCESS;
}

/** Pad the file to the next section boundary. */
static hostmem_result pad_to_alignment(FILE *file, uint64_t *position) {
  return pad_to(file, position, GEO_INDEX_ALIGNMENT);
}

/** Open one section at the current position. */
static hostmem_result section_begin(
    FILE *file, uint64_t *position, GeoIndexSection *section, GeoIndexSectionKind kind
) {
  hostmem_result result = pad_to_alignment(file, position);
  if (result != HOSTMEM_SUCCESS) return result;
  section->kind = (uint32_t)kind;
  section->flags = 0;
  section->offset = *position;
  section->size = 0;
  return HOSTMEM_SUCCESS;
}

/** Append bytes to the open section. */
static hostmem_result section_put(
    FILE *file, uint64_t *position, GeoIndexSection *section, const void *data, size_t size
) {
  if (size && fwrite(data, 1, size, file) != size) return HOSTMEM_ERROR_ENCODE_FAILED;
  *position += size;
  section->size = *position - section->offset;
  return HOSTMEM_SUCCESS;
}

/**
 * @brief Write one dictionary as its three sections.
 *
 *  Words go in whole: the leading bytes their group carries, then the
 *  remainder that was stored.  What was split for order is joined again for
 *  reading.
 */
static hostmem_result write_dictionary(
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
  if (!offsets) return HOSTMEM_ERROR_OUT_OF_MEMORY;

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
        return HOSTMEM_ERROR_ARITHMETIC_OVERFLOW;
      }
    }
  }
  offsets[counted] = (uint32_t)text_size;

  hostmem_result result = section_begin(file, position, &sections[0], groups_kind);
  if (result != HOSTMEM_SUCCESS) goto done;
  for (size_t g = 0; g < set->group_count; ++g) {
    GeoIndexGroup record;
    memset(&record, 0, sizeof(record));
    memcpy(record.key, set->groups[g].key, PREFIX_TREE_DEPTH_MAX);
    record.start = (uint32_t)set->groups[g].start;
    record.count = (uint32_t)set->groups[g].count;
    result = section_put(file, position, &sections[0], &record, sizeof(record));
    if (result != HOSTMEM_SUCCESS) goto done;
  }

  result = section_begin(file, position, &sections[1], offsets_kind);
  if (result != HOSTMEM_SUCCESS) goto done;
  result = section_put(file, position, &sections[1], offsets, (set->count + 1) * sizeof(*offsets));
  if (result != HOSTMEM_SUCCESS) goto done;

  result = section_begin(file, position, &sections[2], text_kind);
  if (result != HOSTMEM_SUCCESS) goto done;
  for (size_t g = 0; g < set->group_count; ++g) {
    const NameGroup *group = &set->groups[g];
    size_t head = key_length(group->key);
    for (size_t i = 0; i < group->count; ++i) {
      const char *rest = set->names[group->start + i];
      result = section_put(file, position, &sections[2], group->key, head);
      if (result != HOSTMEM_SUCCESS) goto done;
      result = section_put(file, position, &sections[2], rest, strlen(rest));
      if (result != HOSTMEM_SUCCESS) goto done;
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
static hostmem_result write_postings(
    FILE *file,
    uint64_t *position,
    GeoIndexSection *section,
    const DocSet *documents,
    uint64_t **out_offsets
) {
  size_t word_count = documents->word_count;
  uint64_t *offsets = malloc((word_count + 1) * sizeof(*offsets));
  if (!offsets) return HOSTMEM_ERROR_OUT_OF_MEMORY;

  hostmem_result result = pad_to(file, position, GEO_INDEX_BITMAP_ALIGNMENT);
  if (result == HOSTMEM_SUCCESS) {
    result = section_begin(file, position, section, GEO_INDEX_SECTION_POSTINGS);
  }
  if (result != HOSTMEM_SUCCESS) {
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
      result = HOSTMEM_ERROR_OUT_OF_MEMORY;
      break;
    }
    roaring_bitmap_run_optimize(bitmap);

    result = pad_to(file, position, GEO_INDEX_BITMAP_ALIGNMENT);
    if (result != HOSTMEM_SUCCESS) {
      roaring_bitmap_free(bitmap);
      break;
    }
    section->size = *position - section->offset;

    size_t needed = roaring_bitmap_frozen_size_in_bytes(bitmap);
    if (needed > buffer_size) {
      char *grown = realloc(buffer, needed);
      if (!grown) {
        roaring_bitmap_free(bitmap);
        result = HOSTMEM_ERROR_OUT_OF_MEMORY;
        break;
      }
      buffer = grown;
      buffer_size = needed;
    }
    roaring_bitmap_frozen_serialize(bitmap, buffer);
    roaring_bitmap_free(bitmap);

    result = section_put(file, position, section, buffer, needed);
    if (result != HOSTMEM_SUCCESS) break;
  }
  offsets[word_count] = *position - section->offset;

  free(buffer);
  if (result != HOSTMEM_SUCCESS) {
    free(offsets);
    return result;
  }
  *out_offsets = offsets;
  return HOSTMEM_SUCCESS;
}

hostmem_result geo_index_write(
    const char *path,
    const NameSet *words,
    const NameSet *display,
    const DocSet *documents,
    const HouseSet *houses,
    uint64_t total_terms
) {
  if (!path || !words || !display || !documents || !houses) return HOSTMEM_ERROR_NULL_POINTER;

  FILE *file = fopen(path, "wb");
  if (!file) return HOSTMEM_ERROR_ENCODE_FAILED;
  static char write_buffer[1 << 20];
  setvbuf(file, write_buffer, _IOFBF, sizeof(write_buffer));

  GeoIndexHeader header;
  memset(&header, 0, sizeof(header));
  GeoIndexSection sections[GEO_INDEX_SECTION_COUNT];
  memset(sections, 0, sizeof(sections));

  uint64_t position = sizeof(header) + sizeof(sections);
  uint64_t *posting_offsets = NULL;
  hostmem_result result = HOSTMEM_ERROR_ENCODE_FAILED;
  if (fseek(file, (long)position, SEEK_SET) != 0) goto failed;

  result = write_dictionary(
      file, &position, words, &sections[0], GEO_INDEX_SECTION_WORD_GROUPS,
      GEO_INDEX_SECTION_WORD_OFFSETS, GEO_INDEX_SECTION_WORD_TEXT
  );
  if (result != HOSTMEM_SUCCESS) goto failed;

  result = write_dictionary(
      file, &position, display, &sections[3], GEO_INDEX_SECTION_DISPLAY_GROUPS,
      GEO_INDEX_SECTION_DISPLAY_OFFSETS, GEO_INDEX_SECTION_DISPLAY_TEXT
  );
  if (result != HOSTMEM_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[6], GEO_INDEX_SECTION_DOCUMENTS);
  if (result != HOSTMEM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[6], documents->documents,
      documents->document_count * sizeof(GeoDocument)
  );
  if (result != HOSTMEM_SUCCESS) goto failed;

  /* The bitmaps go down first and tell us their sizes on the way; the table of
     offsets follows behind them, because only then is it known. */
  result = write_postings(file, &position, &sections[8], documents, &posting_offsets);
  if (result != HOSTMEM_SUCCESS) goto failed;

  /* the weights travel apart from the records: ranking touches nothing else,
     and a hundred megabytes of them fit into the caches far better than two
     gigabytes of full documents */
  result = section_begin(file, &position, &sections[9], GEO_INDEX_SECTION_IMPORTANCE);
  if (result != HOSTMEM_SUCCESS) goto failed;
  for (size_t d = 0; d < documents->document_count; ++d) {
    uint16_t weight = documents->documents[d].importance;
    result = section_put(file, &position, &sections[9], &weight, sizeof(weight));
    if (result != HOSTMEM_SUCCESS) goto failed;
  }

  result = section_begin(file, &position, &sections[7], GEO_INDEX_SECTION_POSTING_OFFSETS);
  if (result != HOSTMEM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[7], posting_offsets,
      (documents->word_count + 1) * sizeof(*posting_offsets)
  );
  if (result != HOSTMEM_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[10], GEO_INDEX_SECTION_HOUSES);
  if (result != HOSTMEM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[10], houses->houses, houses->house_count * sizeof(GeoHouse)
  );
  if (result != HOSTMEM_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[11], GEO_INDEX_SECTION_HOUSE_OFFSETS);
  if (result != HOSTMEM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[11], houses->offsets,
      (documents->document_count + 1) * sizeof(uint32_t)
  );
  if (result != HOSTMEM_SUCCESS) goto failed;

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

  result = HOSTMEM_ERROR_ENCODE_FAILED;
  if (fseek(file, 0, SEEK_SET) != 0) goto failed;
  if (fwrite(&header, sizeof(header), 1, file) != 1) goto failed;
  if (fwrite(sections, sizeof(sections), 1, file) != 1) goto failed;
  if (fflush(file)) goto failed;
  result = HOSTMEM_SUCCESS;

failed:
  free(posting_offsets);
  if (fclose(file) != 0 && result == HOSTMEM_SUCCESS) result = HOSTMEM_ERROR_ENCODE_FAILED;
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
static hostmem_result open_dictionary(
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
  if (!groups || !offsets || !text) return HOSTMEM_ERROR_INVALID_PARAM;
  if (word_count && offsets[word_count] > text_size) return HOSTMEM_ERROR_INVALID_PARAM;

  hostmem_result result = prefix_tree_init(&dictionary->prefixes, NAME_PREFIX_DEPTH);
  if (result != HOSTMEM_SUCCESS) return result;
  for (uint64_t g = 0; g < group_count; ++g) {
    size_t assigned = 0;
    result = prefix_tree_intern(&dictionary->prefixes, groups[g].key, &assigned, NULL);
    /* the groups were written in key order; anything else is not our file */
    if (result == HOSTMEM_SUCCESS && assigned != g) result = HOSTMEM_ERROR_INVALID_PARAM;
    if (result != HOSTMEM_SUCCESS) {
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
  return HOSTMEM_SUCCESS;
}

hostmem_result geo_index_open(GeoIndex *index, const char *path) {
  if (!index || !path) return HOSTMEM_ERROR_NULL_POINTER;
  memset(index, 0, sizeof(*index));

  int descriptor = open(path, O_RDONLY);
  if (descriptor < 0) return HOSTMEM_ERROR_DECODE_FAILED;

  struct stat status;
  if (fstat(descriptor, &status) != 0 || status.st_size <= (off_t)sizeof(GeoIndexHeader)) {
    close(descriptor);
    return HOSTMEM_ERROR_DECODE_FAILED;
  }
  size_t size = (size_t)status.st_size;

  void *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
  close(descriptor); /* the mapping keeps the file alive on its own */
  if (mapping == MAP_FAILED) return HOSTMEM_ERROR_DECODE_FAILED;

  const uint8_t *base = mapping;
  const GeoIndexHeader *header = (const GeoIndexHeader *)base;

  /* --- nothing is trusted before the header agrees --- */
  hostmem_result result = HOSTMEM_ERROR_INVALID_PARAM;
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
  if (result != HOSTMEM_SUCCESS) goto refused;
  result = open_dictionary(
      &index->display, base, size, sections, header->section_count, header->display_count,
      header->display_group_count, GEO_INDEX_SECTION_DISPLAY_GROUPS,
      GEO_INDEX_SECTION_DISPLAY_OFFSETS, GEO_INDEX_SECTION_DISPLAY_TEXT
  );
  if (result != HOSTMEM_SUCCESS) {
    prefix_tree_free(&index->words.prefixes);
    goto refused;
  }

  result = HOSTMEM_ERROR_INVALID_PARAM;
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
  return HOSTMEM_SUCCESS;

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
  bool borrowed;   /**< The readings belong to someone else and are not freed here. */
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

/** Shortest prefix that is expanded; below that the range is the whole alphabet. */
#define GEO_QUERY_PREFIX_MIN 3

/** Words one prefix may pull in; beyond that it is no longer a hint but a shrug. */
#define GEO_QUERY_PREFIX_TERMS 4096

/**
 * @brief Where a word stands relative to a prefix.
 *
 *  @return <0 before it, 0 when the word begins with it, >0 after it.  A word
 *          shorter than the prefix sorts before it, which is what makes the
 *          two binary searches below delimit exactly the words that start
 *          with it.
 */
static int compare_prefix(const char *word, size_t word_size, const char *prefix, size_t size) {
  size_t shared = word_size < size ? word_size : size;
  int order = shared ? memcmp(word, prefix, shared) : 0;
  if (order) return order;
  return word_size < size ? -1 : 0;
}

/** First rank whose word is not before @p prefix, or past it when @p after is set. */
static size_t prefix_bound(
    const GeoDictionary *dictionary, const char *prefix, size_t size, bool after
) {
  size_t low = 0, high = dictionary->word_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    size_t word_size = 0;
    const char *word = geo_dictionary_word(dictionary, middle, &word_size);
    int order = compare_prefix(word, word_size, prefix, size);
    if (order < 0 || (after && order == 0)) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

/**
 * @brief Every document named by a word that begins with @p prefix.
 *
 *  The dictionary is sorted, so the words sharing a beginning stand together
 *  and two binary searches delimit them.  Their document sets are then joined
 *  — which is what a bitmap does best, and what a search typing one letter at
 *  a time needs on every keystroke.
 *
 *  A prefix matching more than @ref GEO_QUERY_PREFIX_TERMS words is refused
 *  rather than cut off.  Cutting would take the first few thousand words in
 *  alphabetical order and quietly drop the rest — *mar* would then find
 *  *marabu* and never *marienplatz*, and nothing in the answer would say so.
 *  A refused word simply narrows nothing, which the other words of the query
 *  survive.
 *
 *  @param[in,out] stats  Counts of this query, or NULL.  The words the prefix
 *                        covered are added even when it is refused — that count
 *                        is the reason for the refusal.
 *  @return The joined set, or NULL when no word begins with @p prefix or too
 *          many do.
 */
static roaring_bitmap_t *prefix_documents(
    const GeoIndex *index, const char *prefix, size_t size, GeoQueryStats *stats
) {
  if (size < GEO_QUERY_PREFIX_MIN) return NULL;
  size_t first = prefix_bound(&index->words, prefix, size, false);
  size_t last = prefix_bound(&index->words, prefix, size, true);
  if (stats) stats->prefix_terms += last - first;
  if (last - first > GEO_QUERY_PREFIX_TERMS) {
    if (stats) ++stats->prefix_refused;
    return NULL;
  }

  roaring_bitmap_t *joined = NULL;
  for (size_t rank = first; rank < last; ++rank) {
    const roaring_bitmap_t *documents = geo_index_word_documents(index, rank);
    if (!documents) continue;
    if (stats) {
      ++stats->posting_lists;
      stats->posting_documents += roaring_bitmap_get_cardinality(documents);
    }
    if (!joined) {
      joined = roaring_bitmap_copy(documents);
    } else {
      roaring_bitmap_or_inplace(joined, documents);
    }
    roaring_bitmap_free(documents);
  }
  return joined;
}

/* =========================================================================
 *  Where the searcher stands
 * ========================================================================= */

/** Cells to either side of the searcher that are asked for; 1 makes a 3 × 3 block. */
#define GEO_QUERY_NEAR_RADIUS 1

/** Cells one ring may hold. */
#define GEO_QUERY_NEAR_CELLS ((2 * GEO_QUERY_NEAR_RADIUS + 1) * (2 * GEO_QUERY_NEAR_RADIUS + 1))

/**
 * @brief Every document standing in the cells around @p options.
 *
 *  The cells are ordinary words, so this is an ordinary lookup — nine of them,
 *  joined into one set.  What comes back narrows the query like any other word
 *  and, unlike any other word, it narrows by where a place is rather than by
 *  what it is called.
 *
 *  An index built before the cells existed simply has none of these words, and
 *  the ring comes back empty; the caller drops the position and asks again.
 *
 *  @param[in]     index    Opened index.
 *  @param[in]     options  Query options; a position must be set.
 *  @param[in,out] stats    Counts of this query, or NULL.
 *  @return The joined set, to be freed by the caller, or NULL when no place
 *          around the searcher is in the index.
 *
 *  @whisper The ground underfoot answers before any name is spoken
 */
static roaring_bitmap_t *near_documents(
    const GeoIndex *index, const GeoQueryOptions *options, GeoQueryStats *stats
) {
  uint32_t cells[GEO_QUERY_NEAR_CELLS];
  size_t count = geo_cell_ring(
      cells, GEO_QUERY_NEAR_CELLS, options->latitude_e7, options->longitude_e7,
      GEO_QUERY_NEAR_RADIUS
  );

  roaring_bitmap_t *joined = NULL;
  for (size_t c = 0; c < count; ++c) {
    char token[GEO_CELL_TOKEN_SIZE];
    size_t size = geo_cell_token(token, cells[c]);

    size_t rank = 0;
    if (!geo_dictionary_find(&index->words, token, size, &rank)) continue;
    const roaring_bitmap_t *documents = geo_index_word_documents(index, rank);
    if (!documents) continue;

    if (stats) {
      ++stats->near_cells;
      ++stats->posting_lists;
      stats->posting_documents += roaring_bitmap_get_cardinality(documents);
    }
    if (!joined) {
      joined = roaring_bitmap_copy(documents);
    } else {
      roaring_bitmap_or_inplace(joined, documents);
    }
    roaring_bitmap_free(documents);
  }
  if (joined && stats) stats->near_documents = roaring_bitmap_get_cardinality(joined);
  return joined;
}

/** How far a candidate may stand and still count as near, in degrees × 10⁷. */
static const int32_t GEO_NEAR_BANDS_E7[] = {
    180000,  /**< ≈ 2 km — the same quarter. */
    900000,  /**< ≈ 10 km — the same town. */
    4500000, /**< ≈ 50 km — the same region. */
};

/** Bands a candidate may fall into; the last one is everything beyond. */
#define GEO_NEAR_BAND_COUNT (sizeof(GEO_NEAR_BANDS_E7) / sizeof(GEO_NEAR_BANDS_E7[0]) + 1)

/**
 * @brief Longitude shrinks towards the poles; by how much, in sixteenths.
 *
 *  A degree of longitude is a degree of latitude times the cosine of where one
 *  stands.  The table holds that cosine per ten degrees, rounded to sixteenths
 *  — enough for a comparison that ends in four steps, and it keeps the ranking
 *  free of a maths library it needs for nothing else.
 */
static int32_t longitude_shrink(int32_t lat_e7) {
  static const uint8_t COSINE[10] = {16, 16, 15, 14, 12, 10, 8, 5, 3, 1};
  int32_t degrees = lat_e7 / 10000000;
  if (degrees < 0) degrees = -degrees;
  size_t step = (size_t)(degrees / 10);
  if (step > 9) step = 9;
  return COSINE[step];
}

/**
 * @brief Which band a document falls into, seen from where the searcher stands.
 *
 *  Coarse on purpose.  A sharp distance ordering would put a nameless field
 *  path in front of the cathedral three streets further on, and that is not
 *  what someone typing *Dom* in Cologne means.  Inside a band the keys that
 *  know what a place *is* — the house number, the weight the dump gave it —
 *  decide as they did before.
 *
 *  @return 0 for the nearest band … GEO_NEAR_BAND_COUNT - 1 for everything else,
 *          and the last band as well for a document that carries no coordinate.
 */
static uint8_t near_band_of(
    const GeoIndex *index, uint32_t document, const GeoQueryOptions *options
) {
  const GeoDocument *record = &index->documents[document];
  if (!(record->flags & GEO_DOCUMENT_HAS_POINT)) return GEO_NEAR_BAND_COUNT - 1;

  int64_t north = (int64_t)record->lat_e7 - options->latitude_e7;
  int64_t east = (int64_t)record->lon_e7 - options->longitude_e7;
  /* the shorter way round the world, for a searcher near the dateline */
  if (east > 1800000000) east -= 3600000000LL;
  if (east < -1800000000) east += 3600000000LL;
  east = east * longitude_shrink(options->latitude_e7) / 16;

  int64_t squared = north * north + east * east;
  for (size_t band = 0; band + 1 < GEO_NEAR_BAND_COUNT; ++band) {
    int64_t edge = GEO_NEAR_BANDS_E7[band];
    if (squared <= edge * edge) return (uint8_t)band;
  }
  return GEO_NEAR_BAND_COUNT - 1;
}

/** Does this token carry a digit? Then it may be a house number. */
static bool token_has_digit(const TextToken *token) {
  for (size_t i = 0; i < token->size; ++i) {
    if (token->data[i] >= '0' && token->data[i] <= '9') return true;
  }
  return false;
}

/** Fewest digits a bare number must have before it is read as a postal code. */
#define GEO_QUERY_CODE_DIGITS 4

/**
 * @brief Does this token read as a postal code rather than a house number?
 *
 *  Digits only, and at least @ref GEO_QUERY_CODE_DIGITS of them.  A house number
 *  carrying a letter — *12a*, *17-19* — is never one, and neither is a short
 *  run: the four-digit floor is what separates the codes of Germany, Austria
 *  and Switzerland from the numbers on their doors.  The rule guesses, and it
 *  guesses for one part of the world; a wrong guess costs nothing, because a
 *  code that narrows the answer to nothing is asked again without it.
 */
static bool token_is_code(const TextToken *token) {
  if (token->size < GEO_QUERY_CODE_DIGITS) return false;
  for (size_t i = 0; i < token->size; ++i) {
    if (token->data[i] < '0' || token->data[i] > '9') return false;
  }
  return true;
}

/** How the numbers of a query are read while the candidates are gathered. */
typedef enum NumberReading {
  /** Every number narrows, the way *Straße des 17. Juni* needs it. */
  NUMBERS_AS_WORDS,
  /** No number narrows; each is held back to be looked for as a house number. */
  NUMBERS_AS_HOUSES,
  /** Only a postal code narrows; shorter numbers stay house numbers. */
  NUMBERS_BUT_CODES
} NumberReading;

/** May this token take part in narrowing the answer down, under @p reading? */
static bool token_narrows(const TextToken *token, NumberReading reading) {
  if (reading == NUMBERS_AS_WORDS) return true;
  if (!token_has_digit(token)) return true;
  return reading == NUMBERS_BUT_CODES && token_is_code(token);
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
 * @brief Answer the query's words, reading its numbers as @p reading says.
 *
 *  @param[in]     near   Documents around the searcher, or NULL.  Narrows like
 *                        a word of the query and is borrowed, not consumed —
 *                        the same ring serves every reading.
 *  @param[in,out] stats  Counts of this query, or NULL.  The sums grow with
 *                        every reading; what describes one pass alone is
 *                        overwritten, so the pass that answers is the one
 *                        described.
 *  @return Number of results written into @p hits.
 */
static size_t query_words(
    const GeoIndex *index,
    const TextTokenizer *tokenizer,
    NumberReading reading,
    bool prefix_last,
    const roaring_bitmap_t *near,
    GeoHit *hits,
    size_t limit,
    GeoQueryStats *stats
) {
  if (stats) {
    ++stats->passes;
    /* what describes a single pass starts over with it; the sums do not */
    stats->groups = 0;
    stats->narrowed = 0;
  }

  /* --- the word being typed is the last one; it may still grow --- */
  uint16_t typing = 0;
  bool any = false;
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    const TextToken *token = &tokenizer->tokens[t];
    if (token->part) continue;
    if (!token_narrows(token, reading)) continue;
    if (!any || token->group > typing) {
      typing = token->group;
      any = true;
    }
  }

  QueryGroup groups[GEO_QUERY_GROUP_MAX];
  size_t group_count = 0;
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    const TextToken *token = &tokenizer->tokens[t];
    if (token->part) continue; /* pieces of a compound serve the index, not the query */
    if (!token_narrows(token, reading)) continue;

    /* A whole word is read as it stands; the one still being typed is read as
       a beginning as well, so *Marienpl* finds what *Marienplatz* would. */
    const roaring_bitmap_t *readings[2] = {NULL, NULL};
    size_t reading_count = 0;

    size_t rank = 0;
    if (geo_dictionary_find(&index->words, token->data, token->size, &rank)) {
      readings[reading_count] = geo_index_word_documents(index, rank);
      if (readings[reading_count]) {
        if (stats) {
          ++stats->posting_lists;
          stats->posting_documents += roaring_bitmap_get_cardinality(readings[reading_count]);
        }
        ++reading_count;
      }
    }
    if (prefix_last && token->group == typing) {
      readings[reading_count] = prefix_documents(index, token->data, token->size, stats);
      if (readings[reading_count]) ++reading_count;
    }
    if (!reading_count) continue; /* a word nobody ever wrote cannot narrow anything down */

    /* readings of the same word join the same group */
    QueryGroup *group = NULL;
    for (size_t g = 0; g < group_count; ++g) {
      if (groups[g].source == token->group) { group = &groups[g]; }
    }
    if (!group) {
      /* one slot is kept free, so the ring around the searcher always fits */
      if (group_count + 1 >= GEO_QUERY_GROUP_MAX) {
        for (size_t r = 0; r < reading_count; ++r) { roaring_bitmap_free(readings[r]); }
        break;
      }
      group = &groups[group_count++];
      group->reading_count = 0;
      group->weight = 0;
      group->source = token->group;
      group->borrowed = false;
    }
    for (size_t r = 0; r < reading_count; ++r) {
      if (group->reading_count >= sizeof(group->readings) / sizeof(group->readings[0])) {
        roaring_bitmap_free(readings[r]);
        continue;
      }
      uint64_t weight = roaring_bitmap_get_cardinality(readings[r]);
      group->readings[group->reading_count++] = readings[r];
      if (weight > group->weight) group->weight = weight;
    }
  }
  if (stats) stats->groups = (uint32_t)group_count;
  /* The ring is not a word and cannot stand for one: a query whose words the
     dictionary does not know would otherwise be answered with everything the
     searcher is standing next to, which is not what they typed. */
  if (!group_count) return 0;

  if (near) {
    QueryGroup *group = &groups[group_count++];
    group->readings[0] = near;
    group->reading_count = 1;
    group->weight = roaring_bitmap_get_cardinality(near);
    group->source = UINT16_MAX;
    group->borrowed = true;
  }

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
    if (stats) stats->narrowed = roaring_bitmap_get_cardinality(carried);
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
    if (groups[g].borrowed) continue; /* the ring outlives this reading */
    for (size_t r = 0; r < groups[g].reading_count; ++r) {
      roaring_bitmap_free(groups[g].readings[r]);
    }
  }
  return count;
}

/* =========================================================================
 *  Ranking — what the query described, ahead of what the world finds heavy
 * ========================================================================= */

/** Candidates weighed for every result asked for, before the ranking trims. */
#define GEO_QUERY_OVERSAMPLE 4

/**
 * Fewest candidates weighed, however few results were asked for.
 *
 * The sample is filled by weight alone, and what it does not hold can no longer
 * be lifted.  A query naming a postcode may well mean the fortieth-heaviest of
 * the streets that carry its words — asking for two results must not mean that
 * only the two heaviest were ever considered.
 */
#define GEO_QUERY_SAMPLE_MIN 64

/* The sample is what @ref GEO_QUERY_LIMIT_MAX measures — 256 hits are four
   kilobytes of stack — so the two are one number, kept in the header where a
   caller can read it. */
static_assert(
    GEO_QUERY_SAMPLE_MIN <= GEO_QUERY_LIMIT_MAX, "the smallest sample outgrew the largest"
);

/** A postcode the query named — the narrowest thing an address can say. */
#define GEO_AGREEMENT_POSTCODE 2u
/** A town the query named. Towns repeat, postcodes far less. */
#define GEO_AGREEMENT_CITY 1u

/**
 * @brief The query's own words, kept while the tokenizer turns to other texts.
 *
 *  The tokenizer holds one input at a time, and the ranking has to fold the
 *  names of the candidates through the same door the query came through.  So
 *  the query's whole words are copied aside first — pieces of compounds stay
 *  behind, and at most @ref TEXT_BUFFER_MAX bytes are taken, the ceiling the
 *  tokenizer itself keeps.
 *
 *  @whisper What was asked is set down, so the asking survives the answering
 */
typedef struct QueryWords {
  char bytes[TEXT_BUFFER_MAX]; /**< Folded words, laid end to end. */
  uint16_t start[TEXT_TOKEN_MAX];
  uint16_t size[TEXT_TOKEN_MAX];
  size_t count; /**< Words held. */
  size_t used;  /**< Bytes taken from @c bytes. */
} QueryWords;

/** Copy the whole words of @p tokenizer aside, in the order they were typed. */
static void query_words_keep(QueryWords *kept, const TextTokenizer *tokenizer) {
  kept->count = 0;
  kept->used = 0;
  for (size_t t = 0; t < tokenizer->token_count && kept->count < TEXT_TOKEN_MAX; ++t) {
    const TextToken *token = &tokenizer->tokens[t];
    if (token->part) continue; /* a piece of a compound is not a word someone typed */
    if (kept->used + token->size > sizeof(kept->bytes)) break;
    memcpy(&kept->bytes[kept->used], token->data, token->size);
    kept->start[kept->count] = (uint16_t)kept->used;
    kept->size[kept->count] = (uint16_t)token->size;
    ++kept->count;
    kept->used += token->size;
  }
}

/** Was this word among the ones the query brought? */
static bool query_words_have(const QueryWords *kept, const char *word, size_t size) {
  for (size_t w = 0; w < kept->count; ++w) {
    if (kept->size[w] == size && memcmp(&kept->bytes[kept->start[w]], word, size) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief How many of the query's words stand in the spelling behind @p rank.
 *
 *  The text is folded by the same tokenizer the query passed through, so
 *  *Straße* and *strasse* meet, and the pieces of a compound count as well —
 *  someone asking for *Leopold* should be recognised by *Leopoldstraße*.
 *
 *  @param[in]     index    Opened index; the spelling is borrowed from it.
 *  @param[in]     rank     Display rank, or GEO_RANK_NONE for a field the
 *                          document never carried.
 *  @param[in]     kept     Words of the query.
 *  @param[in,out] scratch  Tokenizer, overwritten by this call.
 *  @return Words of the query found in that spelling; 0 when there is none.
 */
static unsigned words_in_display(
    const GeoIndex *index, uint32_t rank, const QueryWords *kept, TextTokenizer *scratch
) {
  if (rank == GEO_RANK_NONE) return 0;
  size_t size = 0;
  const char *text = geo_dictionary_word(&index->display, rank, &size);
  if (!text || !size) return 0;

  size_t tokens = text_tokenize(scratch, text, size);
  unsigned found = 0;
  for (size_t t = 0; t < tokens; ++t) {
    if (query_words_have(kept, scratch->tokens[t].data, scratch->tokens[t].size)) ++found;
  }
  return found;
}

/**
 * @brief How far a document lies where the query said it should.
 *
 *  Two questions, and only two: does the query name this document's postcode,
 *  and does it name its town.  Both are unambiguous — a place either carries
 *  that postcode or it does not — and neither can be earned by a document that
 *  merely mentions another place in passing.
 *
 *  The name is deliberately left out of this.  A word reaches a document
 *  through everything its entry carried, its own name as much as the street its
 *  address block named, and the index cannot tell the two apart: *Domplatte*
 *  answers to *Dom* without showing it, and so does the *Leopoldstraße* whose
 *  address block names the Berliner Straße it crosses.  Rewarding a word seen
 *  in the display name lifts the first case and the second alike, and demotes
 *  every place whose word came from elsewhere — measured, it costs more than it
 *  wins.  Telling a name from a mention needs a mark set where the posting is
 *  made, in the builder, not here.
 *
 *  @param[in]     index     Opened index; must not be NULL.
 *  @param[in]     document  Document number, below @c index->document_count.
 *  @param[in]     kept      Words of the query.
 *  @param[in,out] scratch   Tokenizer, overwritten by this call.
 *  @return 0 … GEO_AGREEMENT_POSTCODE + GEO_AGREEMENT_CITY.  0 when the query
 *          named no place at all, and then the order the weights gave stands
 *          untouched.
 *
 *  @whisper The words of the asking settle onto the place that was meant
 */
static unsigned agreement_of(
    const GeoIndex *index, uint32_t document, const QueryWords *kept, TextTokenizer *scratch
) {
  const GeoDocument *record = &index->documents[document];
  unsigned score = 0;

  if (words_in_display(index, record->postcode_rank, kept, scratch)) {
    score += GEO_AGREEMENT_POSTCODE;
  }
  if (words_in_display(index, record->city_rank, kept, scratch)) { score += GEO_AGREEMENT_CITY; }
  return score;
}

/** One candidate as the ranking sees it — the hit itself says nothing of this. */
typedef struct HitRank {
  uint8_t agreement; /**< What the query said about *where*, 0 … 3. */
  uint8_t named;     /**< The place still goes by what was typed; 0 for everyone
                          when no position was given. */
  uint8_t band;      /**< How near the searcher stands, 0 = nearest; 0 for everyone
                          when no position was given. */
} HitRank;

/**
 * @brief Does @p left stand before @p right?
 *
 *  Five keys, in this order: the place the query named; the house number it
 *  asked for; whether the place still goes by what was typed; how near it lies
 *  to the searcher; the weight the dump gave it.
 *
 *  What was typed comes before where it was typed from.  A town or a postcode
 *  says outright which place is meant, and no coordinate may argue with that:
 *  whoever types *Berlin* from Potsdam means Berlin.  A house number likewise —
 *  it was asked for, while a position is only the ground someone happened to be
 *  standing on.  Measured against real data the other order reads badly: *Haupt-
 *  straße 5* answered from Bonn put a street a kilometre nearer, carrying no
 *  such number, ahead of the Hauptstraße that had one.
 *
 *  ### Why the name is weighed at all, and only here
 *
 *  A place answers to more than it is called.  The dump gives every name a
 *  street ever had, and rightly so — whoever types the old one should find the
 *  street.  But an old name is a weaker answer than the current one, and the
 *  band is the one key that can lift a place for a reason the query never
 *  mentioned.  Left unguarded it does: *Hauptstraße* asked from Bonn put the
 *  Friedrich-Breuer-Straße first, which carried *Hauptstraße* among its former
 *  names and lay a kilometre nearer than the street that is called that today.
 *
 *  So the name is weighed where a position was given and nowhere else.  Without
 *  one, this key is 0 for every candidate and the order is the one this index
 *  has always answered with — deliberately, because rewarding a name outright
 *  was measured there and cost more than it won: a word reaches a place through
 *  everything its entry carried, and demanding it in the name demotes every
 *  place that answers legitimately without showing the word.  With a position
 *  the candidates are already the ones standing nearby, and among those the
 *  question "is this still its name" is worth asking.
 */
static bool ranks_before(
    const GeoHit *left, HitRank left_rank, const GeoHit *right, HitRank right_rank
) {
  if (left_rank.agreement != right_rank.agreement) {
    return left_rank.agreement > right_rank.agreement;
  }
  bool left_house = left->house != GEO_RANK_NONE;
  bool right_house = right->house != GEO_RANK_NONE;
  if (left_house != right_house) return left_house;
  if (left_rank.named != right_rank.named) return left_rank.named > right_rank.named;
  if (left_rank.band != right_rank.band) return left_rank.band < right_rank.band;
  return left->importance > right->importance;
}

/**
 * @brief Order @p hits by agreement, house, nearness and weight, keeping equals
 *        as they lie.
 *
 *  An insertion sort: the sample is at most @ref GEO_QUERY_LIMIT_MAX long and
 *  already nearly in order, which is the case this sort is quickest at, and it
 *  moves equal hits past nothing — so the order weight gave them survives.
 */
static void rank_hits(GeoHit *hits, HitRank *ranks, size_t count) {
  for (size_t h = 1; h < count; ++h) {
    GeoHit hit = hits[h];
    HitRank rank = ranks[h];
    size_t place = h;
    while (place > 0 && ranks_before(&hit, rank, &hits[place - 1], ranks[place - 1])) {
      hits[place] = hits[place - 1];
      ranks[place] = ranks[place - 1];
      --place;
    }
    hits[place] = hit;
    ranks[place] = rank;
  }
}

size_t geo_index_query(
    const GeoIndex *index,
    TextTokenizer *tokenizer,
    const char *query,
    size_t size,
    bool prefix_last,
    GeoHit *hits,
    size_t limit
) {
  GeoQueryOptions options = {.prefix_last = prefix_last};
  return geo_index_query_options(index, tokenizer, query, size, &options, hits, limit, NULL);
}

size_t geo_index_query_options(
    const GeoIndex *index,
    TextTokenizer *tokenizer,
    const char *query,
    size_t size,
    const GeoQueryOptions *options,
    GeoHit *hits,
    size_t limit,
    GeoQueryStats *stats
) {
  /* zeroed before anything may fail, so a caller reads counts and not leftovers */
  if (stats) memset(stats, 0, sizeof(*stats));
  if (!index || !tokenizer || !query || !size || !options || !hits || !limit) return 0;
  bool prefix_last = options->prefix_last;
  /* Beyond the ceiling a search stops being a search and becomes a listing, and
     the ranking could no longer hold every candidate at once.  Answering with
     fewer results is the honest reading of too large a limit — quietly dropping
     the ranking instead would return the full count in the wrong order. */
  if (limit > GEO_QUERY_LIMIT_MAX) limit = GEO_QUERY_LIMIT_MAX;

  /* a query is never a repetition of the one before it */
  text_tokenizer_init(tokenizer);
  if (!text_tokenize(tokenizer, query, size)) return 0;

  /* --- a number in an address is a house before it is a word.  So the words
         are asked first without it; only if they answer with nothing does the
         number get its turn as a word of its own, the way *Straße des 17. Juni*
         needs it. --- */
  bool numbers_present = false;
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    if (!tokenizer->tokens[t].part && token_has_digit(&tokenizer->tokens[t])) {
      numbers_present = true;
    }
  }
  bool numbered = numbers_present;

  /* --- more candidates than were asked for, so the ranking has something to
         choose from.  The place someone means is not always among the heaviest
         that carry the words, and what is cut here can never be lifted later.
         A caller who wants more at once than the sample holds is served
         straight into its own array, as before.

         Both arrays are bounded by the ceiling the limit was clamped to, so
         however many candidates survive, the ranking below can hold every one
         of them.  That is the whole point of clamping: a query that answers
         more places than the sample fits would otherwise have to give the
         ranking up, and would give it up in silence. --- */
  GeoHit sample[GEO_QUERY_LIMIT_MAX];
  GeoHit *pool = hits;
  size_t pool_limit = limit;
  if (limit <= GEO_QUERY_LIMIT_MAX / GEO_QUERY_OVERSAMPLE) {
    pool = sample;
    pool_limit = limit * GEO_QUERY_OVERSAMPLE;
    if (pool_limit < GEO_QUERY_SAMPLE_MIN) pool_limit = GEO_QUERY_SAMPLE_MIN;
  }
  /* Neither array may be outgrown, whatever the constants above are set to
     later — this is the one line that makes the ranking's count fit its scores. */
  if (pool_limit > GEO_QUERY_LIMIT_MAX) pool_limit = GEO_QUERY_LIMIT_MAX;

  /* --- Three readings, each asked only when the one before found nothing.
         A postal code is the narrowest thing an address carries, so it is let
         through first: it shrinks the candidates before weight ever cuts them,
         which is the only way a light place can survive to be ranked at all.
         Should the code narrow the answer to nothing — the street filed under
         the neighbouring code, a digit mistyped, a four-digit house number
         mistaken for a code — it is dropped and the words are asked alone.
         Only if they too find nothing does every number take its turn as a
         word, the way *Straße des 17. Juni* needs it. --- */
  /* --- and where a position was given, the ring around it is asked for once
         and carried through every reading below.  It narrows before weight
         cuts, which is the whole reason it exists: the nearest Hauptstraße is
         never among the sixty-four heaviest of the nine thousand that carry the
         word, so a position applied afterwards would arrive to find it gone. --- */
  roaring_bitmap_t *near = NULL;
  if (options->has_position) {
    near = near_documents(index, options, stats);
    /* Nothing at all around the searcher — an ocean, or an index built before
       the cells existed.  There is no ring to let go of later, so it is let go
       of here, and the counts say so rather than showing a position that was
       never used. */
    if (!near && stats) stats->position_dropped = 1;
  }

  size_t count = 0;
  bool near_used = near != NULL;
  for (int attempt = 0; attempt < 2 && !count; ++attempt) {
    /* The position is the first thing let go of.  A search that finds nothing
       nearby was asking about somewhere else — that is a plain reading of the
       words, while returning nothing at all is not.  Whoever named a town or a
       postcode said so outright, and those readings come after. */
    if (attempt) {
      if (!near) break; /* there was nothing to let go of; the chain already ran */
      near_used = false;
      if (stats) stats->position_dropped = 1;
    }
    const roaring_bitmap_t *carried_near = near_used ? near : NULL;
    numbered = numbers_present;

    if (numbered) {
      count = query_words(
          index, tokenizer, NUMBERS_BUT_CODES, prefix_last, carried_near, pool, pool_limit, stats
      );
      if (!count) {
        count = query_words(
            index, tokenizer, NUMBERS_AS_HOUSES, prefix_last, carried_near, pool, pool_limit, stats
        );
      }
    }
    if (!count) {
      numbered = false;
      count = query_words(
          index, tokenizer, NUMBERS_AS_WORDS, prefix_last, carried_near, pool, pool_limit, stats
      );
    }
  }
  if (near) roaring_bitmap_free(near);
  if (!count) return 0;

  /* --- and now the number finds its door --- */
  if (numbered) {
    for (size_t h = 0; h < count; ++h) {
      for (size_t t = 0; t < tokenizer->token_count; ++t) {
        const TextToken *token = &tokenizer->tokens[t];
        if (token->part || !token_has_digit(token)) continue;
        uint32_t house = find_house(index, pool[h].document, token);
        if (house != GEO_RANK_NONE) {
          pool[h].house = house;
          break;
        }
      }
    }
  }

  /* --- whoever named a town or a postcode means the place that lies there;
         among the places that answer equally, whoever asked for a number means
         the street that has it.  Weight decides only where neither says
         anything, which is where it always did. --- */
  QueryWords kept;
  query_words_keep(&kept, tokenizer);
  /* From here the tokenizer folds the candidates' names, and two of them may
     well be named alike — the filter that skips a repeated input would let
     the second one score nothing. */
  tokenizer->repetition_filter = 0;

  /* pool_limit is bounded above, and hit_insert never returns more than it was
     given — so count fits, always, and the ranking runs for every query rather
     than for most of them. */
  HitRank ranks[GEO_QUERY_LIMIT_MAX];
  for (size_t h = 0; h < count; ++h) {
    ranks[h].agreement = (uint8_t)agreement_of(index, pool[h].document, &kept, tokenizer);
    /* Nearness is weighed even where the ring found nothing and was dropped:
       the question "which of these is closest" still has an answer, and the
       band is the only key that can give it.  Its guard travels with it — see
       ranks_before() for why the name is asked about here and nowhere else. */
    if (options->has_position) {
      uint32_t name_rank = index->documents[pool[h].document].name_rank;
      ranks[h].named = words_in_display(index, name_rank, &kept, tokenizer) ? 1u : 0u;
      ranks[h].band = near_band_of(index, pool[h].document, options);
    } else {
      ranks[h].named = 0;
      ranks[h].band = 0;
    }
  }
  rank_hits(pool, ranks, count);

  if (stats) stats->weighed = count; /* what the ranking held, before the limit trims */
  if (count > limit) count = limit;
  if (pool != hits) { memcpy(hits, pool, count * sizeof(*hits)); }
  if (stats) stats->results = count;
  return count;
}

/** @endcond */

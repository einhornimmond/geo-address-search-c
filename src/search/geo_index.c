/** @cond INTERNAL */

#include "search/geo_index.h"

#include "search/geo_cell.h"
#include "search/geo_country.h"
#include "types/photon_place_type.h"

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
      (uint32_t)GEO_INDEX_SECTION_COUNT, (uint32_t)sizeof(GeoIndexLanguage),
      (uint32_t)sizeof(GeoVariant),
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
static arnm_result pad_to(FILE *file, uint64_t *position, uint64_t alignment) {
  static const char zeros[GEO_INDEX_BITMAP_ALIGNMENT] = {0};
  uint64_t misaligned = *position % alignment;
  if (!misaligned) return ARNM_SUCCESS;
  size_t padding = (size_t)(alignment - misaligned);
  if (fwrite(zeros, 1, padding, file) != padding) return ARNM_ERROR_ENCODE_FAILED;
  *position += padding;
  return ARNM_SUCCESS;
}

/** Pad the file to the next section boundary. */
static arnm_result pad_to_alignment(FILE *file, uint64_t *position) {
  return pad_to(file, position, GEO_INDEX_ALIGNMENT);
}

/** Open one section at the current position. */
static arnm_result section_begin(
    FILE *file, uint64_t *position, GeoIndexSection *section, GeoIndexSectionKind kind
) {
  arnm_result result = pad_to_alignment(file, position);
  if (result != ARNM_SUCCESS) return result;
  section->kind = (uint32_t)kind;
  section->flags = 0;
  section->offset = *position;
  section->size = 0;
  return ARNM_SUCCESS;
}

/** Append bytes to the open section. */
static arnm_result section_put(
    FILE *file, uint64_t *position, GeoIndexSection *section, const void *data, size_t size
) {
  if (size && fwrite(data, 1, size, file) != size) return ARNM_ERROR_ENCODE_FAILED;
  *position += size;
  section->size = *position - section->offset;
  return ARNM_SUCCESS;
}

/**
 * @brief Write one dictionary as its three sections.
 *
 *  Words go in whole: the leading bytes their group carries, then the
 *  remainder that was stored.  What was split for order is joined again for
 *  reading.
 */
static arnm_result write_dictionary(
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
  if (!offsets) return ARNM_ERROR_OUT_OF_MEMORY;

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
        return ARNM_ERROR_ARITHMETIC_OVERFLOW;
      }
    }
  }
  offsets[counted] = (uint32_t)text_size;

  arnm_result result = section_begin(file, position, &sections[0], groups_kind);
  if (result != ARNM_SUCCESS) goto done;
  for (size_t g = 0; g < set->group_count; ++g) {
    GeoIndexGroup record;
    memset(&record, 0, sizeof(record));
    memcpy(record.key, set->groups[g].key, PREFIX_TREE_DEPTH_MAX);
    record.start = (uint32_t)set->groups[g].start;
    record.count = (uint32_t)set->groups[g].count;
    result = section_put(file, position, &sections[0], &record, sizeof(record));
    if (result != ARNM_SUCCESS) goto done;
  }

  result = section_begin(file, position, &sections[1], offsets_kind);
  if (result != ARNM_SUCCESS) goto done;
  result = section_put(file, position, &sections[1], offsets, (set->count + 1) * sizeof(*offsets));
  if (result != ARNM_SUCCESS) goto done;

  result = section_begin(file, position, &sections[2], text_kind);
  if (result != ARNM_SUCCESS) goto done;
  for (size_t g = 0; g < set->group_count; ++g) {
    const NameGroup *group = &set->groups[g];
    size_t head = key_length(group->key);
    for (size_t i = 0; i < group->count; ++i) {
      const char *rest = set->names[group->start + i];
      result = section_put(file, position, &sections[2], group->key, head);
      if (result != ARNM_SUCCESS) goto done;
      result = section_put(file, position, &sections[2], rest, strlen(rest));
      if (result != ARNM_SUCCESS) goto done;
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
static arnm_result write_postings(
    FILE *file,
    uint64_t *position,
    GeoIndexSection *section,
    const DocSet *documents,
    uint64_t **out_offsets
) {
  size_t word_count = documents->word_count;
  uint64_t *offsets = malloc((word_count + 1) * sizeof(*offsets));
  if (!offsets) return ARNM_ERROR_OUT_OF_MEMORY;

  arnm_result result = pad_to(file, position, GEO_INDEX_BITMAP_ALIGNMENT);
  if (result == ARNM_SUCCESS) {
    result = section_begin(file, position, section, GEO_INDEX_SECTION_POSTINGS);
  }
  if (result != ARNM_SUCCESS) {
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
      result = ARNM_ERROR_OUT_OF_MEMORY;
      break;
    }
    roaring_bitmap_run_optimize(bitmap);

    result = pad_to(file, position, GEO_INDEX_BITMAP_ALIGNMENT);
    if (result != ARNM_SUCCESS) {
      roaring_bitmap_free(bitmap);
      break;
    }
    section->size = *position - section->offset;

    size_t needed = roaring_bitmap_frozen_size_in_bytes(bitmap);
    if (needed > buffer_size) {
      char *grown = realloc(buffer, needed);
      if (!grown) {
        roaring_bitmap_free(bitmap);
        result = ARNM_ERROR_OUT_OF_MEMORY;
        break;
      }
      buffer = grown;
      buffer_size = needed;
    }
    roaring_bitmap_frozen_serialize(bitmap, buffer);
    roaring_bitmap_free(bitmap);

    result = section_put(file, position, section, buffer, needed);
    if (result != ARNM_SUCCESS) break;
  }
  offsets[word_count] = *position - section->offset;

  free(buffer);
  if (result != ARNM_SUCCESS) {
    free(offsets);
    return result;
  }
  *out_offsets = offsets;
  return ARNM_SUCCESS;
}

arnm_result geo_index_write(
    const char *path,
    const NameSet *words,
    const NameSet *display,
    const DocSet *documents,
    const HouseSet *houses,
    const char (*language_tags)[GEO_LANGUAGE_TAG_MAX],
    uint64_t total_terms
) {
  if (!path || !words || !display || !documents || !houses) return ARNM_ERROR_NULL_POINTER;

  FILE *file = fopen(path, "wb");
  if (!file) return ARNM_ERROR_ENCODE_FAILED;
  static char write_buffer[1 << 20];
  setvbuf(file, write_buffer, _IOFBF, sizeof(write_buffer));

  GeoIndexHeader header;
  memset(&header, 0, sizeof(header));
  GeoIndexSection sections[GEO_INDEX_SECTION_COUNT];
  memset(sections, 0, sizeof(sections));

  uint64_t position = sizeof(header) + sizeof(sections);
  uint64_t *posting_offsets = NULL;
  arnm_result result = ARNM_ERROR_ENCODE_FAILED;
  if (fseek(file, (long)position, SEEK_SET) != 0) goto failed;

  result = write_dictionary(
      file, &position, words, &sections[0], GEO_INDEX_SECTION_WORD_GROUPS,
      GEO_INDEX_SECTION_WORD_OFFSETS, GEO_INDEX_SECTION_WORD_TEXT
  );
  if (result != ARNM_SUCCESS) goto failed;

  result = write_dictionary(
      file, &position, display, &sections[3], GEO_INDEX_SECTION_DISPLAY_GROUPS,
      GEO_INDEX_SECTION_DISPLAY_OFFSETS, GEO_INDEX_SECTION_DISPLAY_TEXT
  );
  if (result != ARNM_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[6], GEO_INDEX_SECTION_DOCUMENTS);
  if (result != ARNM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[6], documents->documents,
      documents->document_count * sizeof(GeoDocument)
  );
  if (result != ARNM_SUCCESS) goto failed;

  /* The bitmaps go down first and tell us their sizes on the way; the table of
     offsets follows behind them, because only then is it known. */
  result = write_postings(file, &position, &sections[8], documents, &posting_offsets);
  if (result != ARNM_SUCCESS) goto failed;

  /* the weights travel apart from the records: ranking touches nothing else,
     and a hundred megabytes of them fit into the caches far better than two
     gigabytes of full documents */
  result = section_begin(file, &position, &sections[9], GEO_INDEX_SECTION_IMPORTANCE);
  if (result != ARNM_SUCCESS) goto failed;
  for (size_t d = 0; d < documents->document_count; ++d) {
    uint16_t weight = documents->documents[d].importance;
    result = section_put(file, &position, &sections[9], &weight, sizeof(weight));
    if (result != ARNM_SUCCESS) goto failed;
  }

  result = section_begin(file, &position, &sections[7], GEO_INDEX_SECTION_POSTING_OFFSETS);
  if (result != ARNM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[7], posting_offsets,
      (documents->word_count + 1) * sizeof(*posting_offsets)
  );
  if (result != ARNM_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[10], GEO_INDEX_SECTION_HOUSES);
  if (result != ARNM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[10], houses->houses, houses->house_count * sizeof(GeoHouse)
  );
  if (result != ARNM_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[11], GEO_INDEX_SECTION_HOUSE_OFFSETS);
  if (result != ARNM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[11], houses->offsets,
      (documents->document_count + 1) * sizeof(uint32_t)
  );
  if (result != ARNM_SUCCESS) goto failed;

  /* The languages and their readings go down last, where a later format may
     grow without moving anything in front of them.
     A run is addressed by two uint32; a table beyond that would be written
     truncated and read as something else entirely. */
  if (documents->variant_count > UINT32_MAX || documents->language_count > GEO_LANGUAGE_MAX) {
    result = ARNM_ERROR_ARITHMETIC_OVERFLOW;
    goto failed;
  }
  result = section_begin(file, &position, &sections[12], GEO_INDEX_SECTION_LANGUAGES);
  if (result != ARNM_SUCCESS) goto failed;
  for (size_t l = 0; l < documents->language_count; ++l) {
    GeoIndexLanguage entry;
    memset(&entry, 0, sizeof(entry));
    if (language_tags) {
      memcpy(entry.tag, language_tags[l], GEO_LANGUAGE_TAG_MAX);
      entry.tag[GEO_LANGUAGE_TAG_MAX - 1] = '\0';
    }
    entry.start = documents->language_offsets ? documents->language_offsets[l] : 0;
    entry.count =
        documents->language_offsets ? documents->language_offsets[l + 1] - entry.start : 0;
    result = section_put(file, &position, &sections[12], &entry, sizeof(entry));
    if (result != ARNM_SUCCESS) goto failed;
  }

  result = section_begin(file, &position, &sections[13], GEO_INDEX_SECTION_VARIANTS);
  if (result != ARNM_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[13], documents->variants,
      documents->variant_count * sizeof(GeoVariant)
  );
  if (result != ARNM_SUCCESS) goto failed;

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
  header.language_count = documents->language_count;
  header.variant_count = documents->variant_count;

  result = ARNM_ERROR_ENCODE_FAILED;
  if (fseek(file, 0, SEEK_SET) != 0) goto failed;
  if (fwrite(&header, sizeof(header), 1, file) != 1) goto failed;
  if (fwrite(sections, sizeof(sections), 1, file) != 1) goto failed;
  if (fflush(file)) goto failed;
  result = ARNM_SUCCESS;

failed:
  free(posting_offsets);
  if (fclose(file) != 0 && result == ARNM_SUCCESS) result = ARNM_ERROR_ENCODE_FAILED;
  return result;
}

/* =========================================================================
 *  Languages
 * ========================================================================= */

int geo_index_language(const GeoIndex *index, const char *tag) {
  if (!index || !tag || !*tag || !index->languages) return -1;
  for (size_t l = 0; l < index->language_count; ++l) {
    if (strncmp(index->languages[l].tag, tag, GEO_LANGUAGE_TAG_MAX) == 0) return (int)l;
  }
  return -1;
}

const GeoVariant *geo_index_variant(const GeoIndex *index, int language, uint32_t document) {
  if (!index || language < 0 || (size_t)language >= index->language_count) return NULL;
  const GeoIndexLanguage *entry = &index->languages[language];
  size_t low = entry->start;
  size_t high = low + entry->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    uint32_t held = index->variants[middle].document;
    if (held == document) return &index->variants[middle];
    if (held < document) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return NULL;
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
static arnm_result open_dictionary(
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
  if (!groups || !offsets || !text) return ARNM_ERROR_INVALID_PARAM;
  if (word_count && offsets[word_count] > text_size) return ARNM_ERROR_INVALID_PARAM;

  arnm_result result = prefix_tree_init(&dictionary->prefixes, NAME_PREFIX_DEPTH);
  if (result != ARNM_SUCCESS) return result;
  for (uint64_t g = 0; g < group_count; ++g) {
    size_t assigned = 0;
    result = prefix_tree_intern(&dictionary->prefixes, groups[g].key, &assigned, NULL);
    /* the groups were written in key order; anything else is not our file */
    if (result == ARNM_SUCCESS && assigned != g) result = ARNM_ERROR_INVALID_PARAM;
    if (result != ARNM_SUCCESS) {
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
  return ARNM_SUCCESS;
}

arnm_result geo_index_open(GeoIndex *index, const char *path) {
  if (!index || !path) return ARNM_ERROR_NULL_POINTER;
  memset(index, 0, sizeof(*index));

  int descriptor = open(path, O_RDONLY);
  if (descriptor < 0) return ARNM_ERROR_DECODE_FAILED;

  struct stat status;
  if (fstat(descriptor, &status) != 0 || status.st_size <= (off_t)sizeof(GeoIndexHeader)) {
    close(descriptor);
    return ARNM_ERROR_DECODE_FAILED;
  }
  size_t size = (size_t)status.st_size;

  void *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
  close(descriptor); /* the mapping keeps the file alive on its own */
  if (mapping == MAP_FAILED) return ARNM_ERROR_DECODE_FAILED;

  const uint8_t *base = mapping;
  const GeoIndexHeader *header = (const GeoIndexHeader *)base;

  /* --- nothing is trusted before the header agrees --- */
  arnm_result result = ARNM_ERROR_INVALID_PARAM;
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
  if (header->language_count > GEO_LANGUAGE_MAX) goto refused;
  if (header->variant_count > UINT32_MAX) goto refused;
  if (!header->language_count && header->variant_count) goto refused;
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
  if (result != ARNM_SUCCESS) goto refused;
  result = open_dictionary(
      &index->display, base, size, sections, header->section_count, header->display_count,
      header->display_group_count, GEO_INDEX_SECTION_DISPLAY_GROUPS,
      GEO_INDEX_SECTION_DISPLAY_OFFSETS, GEO_INDEX_SECTION_DISPLAY_TEXT
  );
  if (result != ARNM_SUCCESS) {
    prefix_tree_free(&index->words.prefixes);
    goto refused;
  }

  result = ARNM_ERROR_INVALID_PARAM;
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
  const GeoIndexLanguage *languages = NULL;
  const GeoVariant *variants = NULL;
  if (header->language_count) {
    languages = section_of(
        base, size, sections, header->section_count, GEO_INDEX_SECTION_LANGUAGES,
        header->language_count * sizeof(GeoIndexLanguage), NULL
    );
    variants = section_of(
        base, size, sections, header->section_count, GEO_INDEX_SECTION_VARIANTS,
        header->variant_count * sizeof(GeoVariant), NULL
    );
    if (!languages || !variants) goto refused_dictionaries;
    /* Every run must lie inside the variant table; that is checked here, once,
       rather than on every answer.  The ranks a variant carries are not looked
       at — geo_dictionary_word() refuses one past the end when the answer asks
       for it. */
    for (uint64_t l = 0; l < header->language_count; ++l) {
      uint64_t start = languages[l].start;
      uint64_t count = languages[l].count;
      if (start > header->variant_count || count > header->variant_count - start) {
        goto refused_dictionaries;
      }
    }
  }
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
  index->languages = languages;
  index->language_count = (size_t)header->language_count;
  index->variants = variants;
  index->variant_count = (size_t)header->variant_count;
  return ARNM_SUCCESS;

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

/**
 * The beginnings one query has joined, so that each is joined only once.
 *
 * A beginning is asked for by every reading, with the ring and without it, with
 * the country and without it — the same union of up to
 * @ref GEO_QUERY_PREFIX_TERMS posting lists, a dozen times over for a query
 * that finds nothing.  Keyed by token, which stays where it is until the query
 * is done.
 */
typedef struct PrefixCache {
  roaring_bitmap_t *documents[TEXT_TOKEN_MAX]; /**< By token; NULL where nothing began. */
  uint64_t joined; /**< Tokens whose beginning was joined, whatever it found. */
} PrefixCache;

/**
 * @brief The documents whose words begin with token @p t, joined once per query.
 *
 *  @param[in]     prefixes  What this query has joined so far; grows here.
 *  @param[in,out] stats     Counts of this query, or NULL; they count the
 *                           posting lists read, so a beginning taken from
 *                           @p prefixes adds nothing.
 *  @return A copy the caller frees, or NULL — see prefix_documents().
 */
static roaring_bitmap_t *prefix_reading(
    const GeoIndex *index,
    PrefixCache *prefixes,
    const TextTokenizer *tokenizer,
    size_t t,
    GeoQueryStats *stats
) {
  const TextToken *token = &tokenizer->tokens[t];
  uint64_t bit = UINT64_C(1) << t;
  if (!(prefixes->joined & bit)) {
    prefixes->documents[t] = prefix_documents(index, token->data, token->size, stats);
    prefixes->joined |= bit;
  }
  return prefixes->documents[t] ? roaring_bitmap_copy(prefixes->documents[t]) : NULL;
}

/** Let go of everything @p prefixes joined. */
static void prefix_cache_free(PrefixCache *prefixes) {
  for (size_t t = 0; t < TEXT_TOKEN_MAX; ++t) {
    if (prefixes->documents[t]) roaring_bitmap_free(prefixes->documents[t]);
    prefixes->documents[t] = NULL;
  }
  prefixes->joined = 0;
}

/** Candidates a beginning is held against by name; beyond this it is let go of. */
#define GEO_QUERY_NAME_CHECKED 4096

/**
 * @brief Does @p document carry a name or a town whose words begin with every
 *        word of @p by_name?
 *
 *  The name is folded by the same tokenizer the query passed through, so
 *  *Straße* and *strasse* meet here as everywhere.  A word folding expanded
 *  from an abbreviation is looked for as it was written as well — *Cottbusser
 *  St* was broken off inside *Straße*, not typed as *Sankt*.  A document the
 *  dump left nameless answers nothing: there is no name for a beginning to
 *  stand in.  The town it lies in answers as well: *A Coruña* is a town whose
 *  name is two letters long, and the street there carries no such word.
 *
 *  @param[in]     index      Opened index.
 *  @param[in]     document   Document number, below @c index->document_count.
 *  @param[in]     tokenizer  Holding the query; left as it is.
 *  @param[in]     by_name    The words to look for, by token group.
 *  @param[in,out] scratch    Tokenizer, overwritten by this call.
 *  @return Whether the name or the town begins a word with each of them, any
 *          one reading of a word answering for it.
 */
static bool name_begins_with(
    const GeoIndex *index,
    uint32_t document,
    const TextTokenizer *tokenizer,
    uint64_t by_name,
    TextTokenizer *scratch
) {
  const GeoDocument *record = &index->documents[document];
  size_t size = 0;
  const char *text = geo_dictionary_word(&index->display, record->name_rank, &size);
  size_t names = text && size ? text_tokenize(scratch, text, size) : 0;

  /* the name is folded first and set aside, so the town can follow it through
     the same tokenizer — a word of the query may stand in either */
  if (names > TEXT_TOKEN_MAX) names = TEXT_TOKEN_MAX;
  TextToken name_words[TEXT_TOKEN_MAX];
  char name_bytes[TEXT_BUFFER_MAX];
  memcpy(name_words, scratch->tokens, names * sizeof(name_words[0]));
  memcpy(name_bytes, scratch->buffer, scratch->used);
  for (size_t n = 0; n < names; ++n) {
    name_words[n].data = name_bytes + (size_t)(name_words[n].data - scratch->buffer);
  }
  text = geo_dictionary_word(&index->display, record->city_rank, &size);
  size_t towns = text && size ? text_tokenize(scratch, text, size) : 0;
  if (!names && !towns) return false;

  /* A word may arrive in more than one reading — *München* as `muenchen` and as
     `munchen` — and either of them standing in the name answers for the word. */
  uint64_t found = 0;
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    const TextToken *word = &tokenizer->tokens[t];
    if (word->part || word->group >= 64 || !((by_name >> word->group) & 1u)) continue;
    if ((found >> word->group) & 1u) continue;
    const char *written = NULL;
    size_t written_size = text_written_form(word->data, word->size, &written);
    bool begins = false;
    for (size_t n = 0; n < names + towns && !begins; ++n) {
      const TextToken *name = n < names ? &name_words[n] : &scratch->tokens[n - names];
      begins = name->size >= word->size && memcmp(name->data, word->data, word->size) == 0;
      if (!begins && written_size) {
        begins = name->size >= written_size && memcmp(name->data, written, written_size) == 0;
      }
    }
    if (begins) found |= UINT64_C(1) << word->group;
  }
  return (by_name & ~found) == 0;
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

/** How far a document lies from the searcher, squared, in degrees × 10⁷ — and
 *  the width of a degree of longitude taken where the searcher stands. */
static int64_t distance_squared(
    const GeoIndex *index, uint32_t document, const GeoQueryOptions *options
) {
  const GeoDocument *record = &index->documents[document];
  if (!(record->flags & GEO_DOCUMENT_HAS_POINT)) return INT64_MAX;
  int64_t north = (int64_t)record->lat_e7 - options->latitude_e7;
  int64_t east = (int64_t)record->lon_e7 - options->longitude_e7;
  if (east > 1800000000) east -= 3600000000LL;
  if (east < -1800000000) east += 3600000000LL;
  east = east * longitude_shrink(options->latitude_e7) / 16;
  return north * north + east * east;
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

/**
 * @brief Could this word finish a house number written apart from it — the *A*
 *        of *42 A*, the *bis* of *12 bis*?
 *
 *  A single letter, or one of the French suffixes *bis*, *ter* and *quater*.
 *  Whether it does depends on what stands before it; see house_suffix_of().
 */
static bool token_is_suffix_shape(const TextToken *token) {
  if (token->part) return false;
  if (token->size == 1) return token->data[0] >= 'a' && token->data[0] <= 'z';
  return (token->size == 3 && memcmp(token->data, "bis", 3) == 0) ||
         (token->size == 3 && memcmp(token->data, "ter", 3) == 0) ||
         (token->size == 6 && memcmp(token->data, "quater", 6) == 0);
}

/** The whole word right behind the one at @p t in the input, or NULL. */
static const TextToken *word_after(const TextTokenizer *tokenizer, size_t t) {
  for (size_t n = 0; n < tokenizer->token_count; ++n) {
    const TextToken *next = &tokenizer->tokens[n];
    if (!next->part && next->group == tokenizer->tokens[t].group + 1) return next;
  }
  return NULL;
}

/**
 * @brief The word that finishes the house number at @p t, or NULL.
 *
 *  The very next word of the input, when it has the shape of a suffix — see
 *  token_is_suffix_shape() — and nothing but a space or a dash stands between.
 *  *Osterstraße 42 A* writes the number of one door as two words, and the
 *  tokenizer, which ends a word at every space, cannot know that.
 */
static const TextToken *house_suffix_of(const TextTokenizer *tokenizer, size_t t) {
  const TextToken *number = &tokenizer->tokens[t];
  if (number->part || !token_has_digit(number)) return NULL;
  const TextToken *next = word_after(tokenizer, t);
  if (!next || token_has_digit(next) || !token_is_suffix_shape(next)) return NULL;
  return next->joint == TEXT_JOINT_NONE || next->joint == TEXT_JOINT_DASH ? next : NULL;
}

/**
 * @brief The number that closes the house number at @p t, or NULL.
 *
 *  The very next word, when it carries a digit and a single dash or slash
 *  stands between: *Anderter Straße 1-3*, *Hauptstraße 12/1*.  A space alone
 *  does not join — *Hauptstraße 5 53111* is a door and a postal code.
 */
static const TextToken *house_second_of(const TextTokenizer *tokenizer, size_t t) {
  const TextToken *number = &tokenizer->tokens[t];
  if (number->part || !token_has_digit(number)) return NULL;
  const TextToken *next = word_after(tokenizer, t);
  if (!next || !token_has_digit(next)) return NULL;
  return next->joint == TEXT_JOINT_DASH || next->joint == TEXT_JOINT_SLASH ? next : NULL;
}

/** Is the word at @p t the suffix, or the closing number, of the house number before it? */
static bool token_ends_a_house(const TextTokenizer *tokenizer, size_t t) {
  const TextToken *token = &tokenizer->tokens[t];
  if (token->part || token->joint == TEXT_JOINT_OTHER) return false;
  for (size_t n = 0; n < tokenizer->token_count; ++n) {
    if (house_suffix_of(tokenizer, n) == token || house_second_of(tokenizer, n) == token) {
      return true;
    }
  }
  return false;
}

/**
 * @brief May the word at @p t take part in narrowing the answer down, under
 *        @p reading?
 *
 *  Wherever a number is held back as a house number, so is the suffix written
 *  apart from it: *A* in *Osterstraße 42 A* names no place, and asked as a word
 *  it narrows the street away.
 */
static bool token_narrows(const TextTokenizer *tokenizer, size_t t, NumberReading reading) {
  const TextToken *token = &tokenizer->tokens[t];
  if (reading == NUMBERS_AS_WORDS) return true;
  if (!token_has_digit(token)) return !token_ends_a_house(tokenizer, t);
  return reading == NUMBERS_BUT_CODES && token_is_code(token);
}

/** Longest house number a query asks for, in folded bytes; longer ones are never found. */
#define GEO_HOUSE_ASKED_MAX 32

/** Widest range a written house number may span and still hold a number asked for. */
#define GEO_HOUSE_RANGE_MAX 100

/**
 * @brief One house number as the query asks for it.
 *
 *  Folded, without spaces, and with `-` for every dash or slash inside it:
 *  *42a* whether it was typed *42a*, *42A* or *42 A*; *1-3* for *1 - 3*, and
 *  *12-1* for *12/1*.  @c digits is the length of the plain number in front of
 *  whatever follows it — 2 for *29d* and for *12-1*, 0 for a plain *29* — and is
 *  what is looked for once the street turns out to have no such door.
 */
typedef struct HouseAsked {
  char text[GEO_HOUSE_ASKED_MAX];
  size_t size;
  size_t digits;
} HouseAsked;

/**
 * @brief Spell the house number at @p t, its suffix or closing number joined on.
 *
 *  @return false when the word is no house number, or only closes the one
 *          before it — the *3* of *1-3* is not a door of its own.
 */
static bool house_asked_of(const TextTokenizer *tokenizer, size_t t, HouseAsked *out) {
  const TextToken *number = &tokenizer->tokens[t];
  if (number->part || !token_has_digit(number)) return false;
  if (token_ends_a_house(tokenizer, t)) return false;
  const TextToken *suffix = house_suffix_of(tokenizer, t);
  const TextToken *second = suffix ? NULL : house_second_of(tokenizer, t);

  size_t size = number->size;
  if (suffix) size += suffix->size;
  if (second) size += 1 + second->size;
  if (size > sizeof(out->text)) return false;
  memcpy(out->text, number->data, number->size);
  if (suffix) memcpy(out->text + number->size, suffix->data, suffix->size);
  if (second) {
    out->text[number->size] = '-';
    memcpy(out->text + number->size + 1, second->data, second->size);
  }
  out->size = size;

  size_t digits = 0;
  while (digits < size && out->text[digits] >= '0' && out->text[digits] <= '9') ++digits;
  out->digits = digits < size ? digits : 0;
  return true;
}

/**
 * @brief The next byte of a written house number as it is compared.
 *
 *  Spaces are passed over, upper case is lowered, and every dash or slash —
 *  the en dash `–` as well — reads as `-`.
 *
 *  @param[in,out] at  Position in @p written, moved past what was read.
 *  @return The byte, or 0 at the end.
 */
static char number_next(const char *written, size_t size, size_t *at) {
  while (*at < size) {
    unsigned char c = (unsigned char)written[*at];
    if (c == ' ') {
      ++*at;
      continue;
    }
    /* U+2010 … U+2014, the hyphens and dashes, are E2 80 90 … E2 80 94 */
    if (c == 0xE2 && *at + 2 < size && (unsigned char)written[*at + 1] == 0x80 &&
        (unsigned char)written[*at + 2] >= 0x90 && (unsigned char)written[*at + 2] <= 0x94) {
      *at += 3;
      return '-';
    }
    ++*at;
    if (c == '/') return '-';
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return (char)c;
  }
  return 0;
}

/**
 * @brief Compare a written house number with an asked one, the way people
 *        write it aside.
 *
 *  The dump writes one door as *42A*, *42a* or *42 A*, and one range as *1-3*,
 *  *1 - 3* or *1/3*, whichever the mapper chose, and a searcher cannot know
 *  which.  See number_next() for what does not count.
 */
static bool number_equal(
    const char *written, size_t written_size, const char *asked, size_t asked_size
) {
  size_t at = 0;
  for (size_t a = 0; a < asked_size; ++a) {
    if (number_next(written, written_size, &at) != asked[a]) return false;
  }
  return number_next(written, written_size, &at) == 0;
}

/** Read a run of digits at @p at as a number; false when there is none or it is too long. */
static bool number_value(const char *written, size_t size, size_t *at, char *next, uint32_t *out) {
  uint32_t value = 0;
  size_t digits = 0;
  char c = number_next(written, size, at);
  while (c >= '0' && c <= '9') {
    if (++digits > 6) return false;
    value = value * 10u + (uint32_t)(c - '0');
    c = number_next(written, size, at);
  }
  *next = c;
  *out = value;
  return digits > 0;
}

/**
 * @brief Does the range a house number is written as hold @p value?
 *
 *  *1-4* holds 1 to 4 — but a range whose ends lie on the same side of the
 *  street holds only the numbers of that side, so *23-25* holds 23 and 25 and
 *  not the 24 across the road.  A letter behind either end is passed over.
 *
 *  A slash makes no range.  In the south-west it numbers the houses behind a
 *  house — *12/1* is the first behind the 12 — and one street of the German
 *  dump carries *2/3*, *2/4*, *2/6* and *2/12* side by side, so even *2/4* is a
 *  door of its own and not the 2, 3 and 4.
 */
static bool number_range_holds(const char *written, size_t size, uint32_t value) {
  if (memchr(written, '/', size)) return false;
  size_t at = 0;
  char next = 0;
  uint32_t from = 0, to = 0;
  if (!number_value(written, size, &at, &next, &from)) return false;
  while (next >= 'a' && next <= 'z') next = number_next(written, size, &at);
  if (next != '-') return false;
  if (!number_value(written, size, &at, &next, &to)) return false;
  while (next >= 'a' && next <= 'z') next = number_next(written, size, &at);
  if (next != 0 || to <= from || to - from > GEO_HOUSE_RANGE_MAX) return false;
  if (value < from || value > to) return false;
  return (from % 2u) != (to % 2u) || (value % 2u) == (from % 2u);
}

/** How a house number is looked for on a street. */
typedef enum HouseMatch {
  HOUSE_AS_ASKED,    /**< The number exactly as it was asked for. */
  HOUSE_IN_RANGE,    /**< A range that holds the plain number asked for. */
  HOUSE_PLAIN_NUMBER /**< The plain number in front of the suffix asked for. */
} HouseMatch;

/** Does the written house number answer @p asked, looked for as @p how says? */
static bool house_answers(
    const char *written, size_t written_size, const HouseAsked *asked, HouseMatch how
) {
  if (how == HOUSE_AS_ASKED) return number_equal(written, written_size, asked->text, asked->size);
  if (how == HOUSE_PLAIN_NUMBER) {
    return asked->digits && number_equal(written, written_size, asked->text, asked->digits);
  }
  if (asked->digits || asked->size > 6) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < asked->size; ++i) {
    if (asked->text[i] < '0' || asked->text[i] > '9') return false;
    value = value * 10u + (uint32_t)(asked->text[i] - '0');
  }
  return number_range_holds(written, written_size, value);
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
static uint32_t find_house(
    const GeoIndex *index, uint32_t document, const HouseAsked *asked, HouseMatch how
) {
  size_t count = 0;
  const GeoHouse *houses = geo_index_houses(index, document, &count);
  if (!houses) return GEO_RANK_NONE;

  for (size_t i = 0; i < count; ++i) {
    size_t written_size = 0;
    const char *written =
        geo_dictionary_word(&index->display, houses[i].number_rank, &written_size);
    if (written && house_answers(written, written_size, asked, how)) {
      return (uint32_t)((houses - index->houses) + i);
    }
  }
  return GEO_RANK_NONE;
}

/**
 * @brief Does the house at @p house carry a number of @p asked — as it was
 *        asked for, or inside its range — rather than only its plain number?
 */
static bool house_is_asked(
    const GeoIndex *index, uint32_t house, const HouseAsked *asked, size_t asked_count
) {
  size_t written_size = 0;
  const char *written =
      geo_dictionary_word(&index->display, index->houses[house].number_rank, &written_size);
  if (!written) return false;
  for (size_t a = 0; a < asked_count; ++a) {
    if (house_answers(written, written_size, &asked[a], HOUSE_AS_ASKED) ||
        house_answers(written, written_size, &asked[a], HOUSE_IN_RANGE)) {
      return true;
    }
  }
  return false;
}

bool geo_index_house_estimate(
    const GeoIndex *index,
    size_t document,
    uint32_t number,
    uint32_t passed_over,
    int32_t *lat_e7,
    int32_t *lon_e7
) {
  if (!number || !lat_e7 || !lon_e7) return false;
  size_t count = 0;
  const GeoHouse *houses = geo_index_houses(index, document, &count);
  if (!houses) return false;

  const GeoHouse *below = NULL, *above = NULL;
  uint32_t low = 0, high = 0;
  for (size_t i = 0; i < count; ++i) {
    if ((size_t)(houses - index->houses) + i == passed_over) continue;
    size_t size = 0;
    const char *written = geo_dictionary_word(&index->display, houses[i].number_rank, &size);
    size_t at = 0;
    char next = 0;
    uint32_t value = 0;
    if (!written || !number_value(written, size, &at, &next, &value)) continue;
    if (value % 2u != number % 2u) continue; /* the other side of the street */
    if (value <= number && (!below || value > low)) {
      below = &houses[i];
      low = value;
    }
    if (value >= number && (!above || value < high)) {
      above = &houses[i];
      high = value;
    }
  }
  if (!below || !above || high - low > GEO_HOUSE_ESTIMATE_GAP) return false;

  int64_t north = (int64_t)above->lat_e7 - below->lat_e7;
  int64_t east = ((int64_t)above->lon_e7 - below->lon_e7) * longitude_shrink(below->lat_e7) / 16;
  int64_t span = GEO_HOUSE_ESTIMATE_SPAN_E7;
  if (north * north + east * east > span * span) return false;

  if (high == low) {
    *lat_e7 = below->lat_e7;
    *lon_e7 = below->lon_e7;
    return true;
  }
  int64_t step = number - low, steps = high - low;
  *lat_e7 = (int32_t)(below->lat_e7 + ((int64_t)above->lat_e7 - below->lat_e7) * step / steps);
  *lon_e7 = (int32_t)(below->lon_e7 + ((int64_t)above->lon_e7 - below->lon_e7) * step / steps);
  return true;
}

/**
 * @brief Answer the query's words, reading its numbers as @p reading says.
 *
 *  @param[in]     near   Documents around the searcher, or NULL.  Narrows like
 *                        a word of the query and is borrowed, not consumed —
 *                        the same ring serves every reading.
 *  @param[in]     country  Documents of the country the query names, or NULL.
 *                        Narrows like @p near, and is borrowed like it.
 *  @param[in]     unfinished  Words of the query, by token group, read as
 *                        beginnings rather than passed over — see
 *                        unfinished_words().  0 for none.
 *  @param[in,out] prefixes  The beginnings this query has joined.
 *  @param[in]     by_name  Words that narrow nothing and are held against the
 *                        name of every candidate instead — see
 *                        name_begins_with().  0 for none.
 *  @param[in,out] names    Tokenizer the candidates' names are folded with,
 *                        overwritten here; NULL where @p by_name is 0.
 *  @param[in]     skipped  Words of the query, by token group (bit @c group, for
 *                        groups below 64), that narrow nothing — the words that
 *                        named @p country.  0 for none.
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
    uint64_t unfinished,
    PrefixCache *prefixes,
    uint64_t by_name,
    TextTokenizer *names,
    const roaring_bitmap_t *near,
    const roaring_bitmap_t *country,
    uint64_t skipped,
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
    if (!token_narrows(tokenizer, t, reading)) continue;
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
    if (!token_narrows(tokenizer, t, reading)) continue;
    /* a word that named the country narrows through the country, not through itself */
    if (token->group < 64 && (skipped >> token->group) & 1u) continue;

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
      readings[reading_count] = prefix_reading(index, prefixes, tokenizer, t, stats);
      if (readings[reading_count]) ++reading_count;
    } else if (token->group < 64 && (unfinished >> token->group) & 1u) {
      /* a word left unfinished before the next one was begun: *Kurpfa 57 Bammental* */
      readings[reading_count] = prefix_reading(index, prefixes, tokenizer, t, stats);
      if (readings[reading_count]) ++reading_count;
    }
    if (!reading_count) continue; /* a word nobody ever wrote cannot narrow anything down */

    /* readings of the same word join the same group */
    QueryGroup *group = NULL;
    for (size_t g = 0; g < group_count; ++g) {
      if (groups[g].source == token->group) { group = &groups[g]; }
    }
    if (!group) {
      /* one slot is kept free, so the ring around the searcher always fits, and
         a second only where a country was named — a query without one keeps
         every word it always kept */
      size_t reserved = country ? 2u : 1u;
      if (group_count + reserved >= GEO_QUERY_GROUP_MAX) {
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
  if (country) {
    QueryGroup *group = &groups[group_count++];
    group->readings[0] = country;
    group->reading_count = 1;
    group->weight = roaring_bitmap_get_cardinality(country);
    group->source = UINT16_MAX - 1;
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
  /* Holding a beginning against this many names costs more than the answer is
     worth, and answering without it would answer a query nobody typed: the
     word is one the query brought, and the check is the only thing that still
     honours it.  So the round ends here with nothing, as it would have without
     ever being asked. */
  if (carried && by_name && roaring_bitmap_get_cardinality(carried) > GEO_QUERY_NAME_CHECKED) {
    roaring_bitmap_free(carried);
    carried = NULL;
  }
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
        if (by_name && !name_begins_with(index, document, tokenizer, by_name, names)) continue;
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

/**
 * Places from beyond the ring the ranking may take in — see far_named_places().
 *
 * Few by nature: they have to be named by the query *and* weigh at least
 * @ref GEO_QUERY_FAR_WEIGHT_MIN, which on the planet is a handful of cities per
 * name.  The bound is what keeps the sample and its scores on the stack.
 */
#define GEO_QUERY_FAR_MAX 16

/**
 * Weight a place beyond the ring needs before a name may lift it over what is
 * near, on the 0 … 65535 scale of GeoDocument::importance.
 *
 * Measured on the planet index against queries asked from Berlin, Munich,
 * Cologne and Vienna.  Everything that has to be lifted weighs more — Frankfurt
 * (Oder) 40527, Ulm 42393, Halle (Saale) 43648, Würzburg 43929, Wien 54450 —
 * and everything that must not be lifted weighs less: Charlottenburg 39243 for
 * *Berlin* asked from Munich, Neustadt an der Weinstraße 36110 over Cologne's
 * own Neustadt/Nord, Gmünd-Bahnhof 33409 over the stations around Berlin, the
 * Burkinabé region Mitte-Ost 31526 over Berlin-Mitte.  At 36000 the last two
 * stay down but Neustadt and the districts come up; at 44000 Halle and Würzburg
 * stay hidden.
 */
#define GEO_QUERY_FAR_WEIGHT_MIN 40000u

/**
 * The same, for a town whose own name the query begins, while nothing inside
 * the ring carries that word as the first of its own name.
 *
 * Below @ref GEO_QUERY_FAR_WEIGHT_MIN weigh the middle-sized towns — Gera
 * 38608, Brandenburg an der Havel 38396, Cottbus 39915 — and *Gera* asked from
 * Munich found nothing but the Gerastraße there, *Brandenburg* nothing but the
 * state and the Brandenburger Straße.  What the weight kept down is still kept
 * down by the two conditions: *Gmünd-Bahnhof* and *Mitte-Ost* do not begin with
 * *Bahnhof* and *Mitte*, and Neustadt an der Weinstraße or Neustadt in Holstein
 * do not come up over Cologne's Neustadt/Süd, which carries the word first.
 *
 * Measured on the planet index with the 1 795 towns of Germany whose name no
 * other settlement there bears, and 415 whose first word leads to them alone,
 * each asked from Berlin, Munich, Cologne and Hamburg, and with 635 quarters and
 * villages near those cities whose name a place far off bears or begins with:
 * from 40000 down to this, 563 far towns come first that did not, the towns
 * asked by their first word stand among the first three in 91.0 % instead of
 * 76.3 %, and nothing near falls but one village of 635.  Below it nothing
 * changes any more.
 */
#define GEO_QUERY_FAR_NAMED_WEIGHT_MIN 25000u

/**
 * How far from the searcher a lighter town may lie in another country than
 * theirs, in degrees × 10⁷ of latitude — about 100 km.
 *
 * *Halle* asked in Berlin found Halle in Belgium and the Belgian district
 * Halle-Vilvoorde before Halle (Westf.), where a map zoomed onto Germany shows
 * the two German Halles and nothing else.  A lighter town abroad therefore
 * comes in only near the searcher — the border towns a map would show: Venlo
 * from Mönchengladbach, Kufstein from Rosenheim, Enschede from Münster at
 * 58 km.  Measured with 23 of them, each asked from the German town nearest:
 * without the exception 6 fell out of the first ten, from 75 km on none does.
 * A town of weight — @ref GEO_QUERY_FAR_WEIGHT_MIN — comes in from anywhere.
 */
#define GEO_QUERY_FAR_ABROAD_E7 9000000

/* The sample is what @ref GEO_QUERY_LIMIT_MAX measures — 256 hits are four
   kilobytes of stack — so the two are one number, kept in the header where a
   caller can read it. */
static_assert(
    GEO_QUERY_SAMPLE_MIN <= GEO_QUERY_LIMIT_MAX, "the smallest sample outgrew the largest"
);

/** A postcode the query named — the narrowest thing an address can say. */
#define GEO_AGREEMENT_POSTCODE 4u
/** A town the query named by its own name. Towns repeat, postcodes far less. */
#define GEO_AGREEMENT_CITY 2u
/** A town whose name holds the query's word only as the place it lies *beside* —
 *  *Neubrunn bei Würzburg* to someone asking for Würzburg. */
#define GEO_AGREEMENT_CITY_BESIDE 1u

/* A postcode outweighs any town, however it was named. */
static_assert(
    GEO_AGREEMENT_POSTCODE > GEO_AGREEMENT_CITY && GEO_AGREEMENT_CITY > GEO_AGREEMENT_CITY_BESIDE,
    "the agreements lost their order"
);

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
 * @brief Is a place of this kind an area in its own right — something a query
 *        names as *where*, not *what*?
 *
 *  True for @c PHOTON_PLACE_TYPE_COUNTRY, @c _STATE, @c _COUNTY, @c _CITY,
 *  @c _STATE_COUNTY_CITY and @c _INDEPENDENT_CITY; false for every other value,
 *  unknown ones included.  Streets, houses, districts and localities lie
 *  *inside* a town and are answered by the town they carry.
 *
 *  @param[in] type  A @c PhotonPlaceType as the document record stores it.
 *  @return Whether the place's own name stands for its town.
 */
static bool place_is_area(uint8_t type) {
  switch (type) {
  case PHOTON_PLACE_TYPE_COUNTRY:
  case PHOTON_PLACE_TYPE_STATE:
  case PHOTON_PLACE_TYPE_COUNTY:
  case PHOTON_PLACE_TYPE_CITY:
  case PHOTON_PLACE_TYPE_STATE_COUNTY_CITY:
  case PHOTON_PLACE_TYPE_INDEPENDENT_CITY:
    return true;
  default:
    return false;
  }
}

/**
 * @brief How plainly the spelling behind @p rank names a town the query named.
 *
 *  The spelling is cut into words by the same tokenizer the query passed
 *  through; its first word is the one with @c group 0.  A query word found in
 *  it counts in one of two ways:
 *
 *  - **by name** — the first word was typed, or all words but at most one
 *    were: *Halle (Saale)*, *Frankfurt am Main*, *Den Haag*, *Bad Tölz*,
 *    *Landkreis Würzburg* for someone asking for Halle, Frankfurt, Haag, Tölz,
 *    Würzburg;
 *  - **beside** — the word stands later and two words or more were not typed:
 *    *Neubrunn bei Würzburg*, *Garching bei München*, *Le Touquet-Paris-Plage*.
 *
 *  The line runs between one word and two because that is what separates a
 *  prefix from a locator.  A prefix — *Bad*, *Den*, *Wiener*, *Landkreis* — is
 *  one word in front of the name.  A locator always brings a preposition and a
 *  place of its own, so the town it names is someone else's.  Demanding the
 *  whole spelling instead was measured and reads worse: *Halle* then answers
 *  with a village of that name before Halle (Saale), and *Haag* with Haag in
 *  Oberbayern before Den Haag.
 *
 *  Only the first 64 words of a spelling take part; a word beyond them is
 *  neither demanded nor counted.
 *
 *  @param[in]     index    Opened index; the spelling is borrowed from it.
 *  @param[in]     rank     Display rank, or GEO_RANK_NONE for a field the
 *                          document never carried.
 *  @param[in]     kept     Words of the query.
 *  @param[in,out] scratch  Tokenizer, overwritten by this call.
 *  @return GEO_AGREEMENT_CITY, GEO_AGREEMENT_CITY_BESIDE, or 0 when no word of
 *          the query stands in the spelling or there is none.
 *
 *  @whisper A town named in passing is still a town, only not the one that was asked for
 */
static unsigned town_agreement(
    const GeoIndex *index, uint32_t rank, const QueryWords *kept, TextTokenizer *scratch
) {
  if (rank == GEO_RANK_NONE) return 0;
  size_t size = 0;
  const char *text = geo_dictionary_word(&index->display, rank, &size);
  if (!text || !size) return 0;

  /* one bit per word of the spelling: which it has, and which the query typed.
     Both readings of an umlaut and the pieces of a compound share their word's
     bit, so any of them meets it. */
  size_t tokens = text_tokenize(scratch, text, size);
  uint64_t words = 0;
  uint64_t typed = 0;
  bool found = false;
  for (size_t t = 0; t < tokens; ++t) {
    const TextToken *token = &scratch->tokens[t];
    if (token->group >= 64) continue; /* beyond the bits: neither demanded nor counted */
    uint64_t bit = UINT64_C(1) << token->group;
    if (!token->part) words |= bit;
    if (query_words_have(kept, token->data, token->size)) {
      found = true;
      typed |= bit;
    }
  }
  if (!found) return 0;
  if (typed & UINT64_C(1)) return GEO_AGREEMENT_CITY;

  uint64_t missing = words & ~typed;
  bool at_most_one = (missing & (missing - 1)) == 0;
  return at_most_one ? GEO_AGREEMENT_CITY : GEO_AGREEMENT_CITY_BESIDE;
}

/**
 * @brief How far a document lies where the query said it should.
 *
 *  Two questions, and only two: does the query name this document's postcode,
 *  and does it name its town.  Both are unambiguous — a place either carries
 *  that postcode or it does not — and neither can be earned by a document that
 *  merely mentions another place in passing.
 *
 *  An area — see place_is_area() — *is* a where, and its own name answers the
 *  town question alongside the town it carries.  Without that the dump's own
 *  filing decides the order: *Würzburg* is filed as a county and carries no
 *  town at all, so it scored nothing, while *Neubrunn bei Würzburg* scored
 *  through its town field and *Residenzplatz* through Würzburg's — every one of
 *  them ranked before the city that was typed, whatever its weight.  Where both
 *  fields agree, the plainer of the two counts; they are never added.
 *
 *  A town counts fully only where it is named by its own name, and half where
 *  its name holds the query's word as a place it lies beside — see
 *  town_agreement().  Otherwise *Schulstraße Würzburg* weighs the street in
 *  Hausen bei Würzburg exactly like the one in Würzburg, and only a single
 *  point of weight, which the dump happens to give the city's street, keeps
 *  them apart.
 *
 *  Every other name is deliberately left out of this.  A word reaches a document
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
  unsigned town = town_agreement(index, record->city_rank, kept, scratch);
  if (town < GEO_AGREEMENT_CITY && place_is_area(record->type)) {
    unsigned own = town_agreement(index, record->name_rank, kept, scratch);
    if (own > town) town = own;
  }
  return score + town;
}

/** One candidate as the ranking sees it — the hit itself says nothing of this. */
typedef struct HitRank {
  uint8_t agreement; /**< What the query said about *where*, 0 …
                          GEO_AGREEMENT_POSTCODE + GEO_AGREEMENT_CITY. */
  uint8_t door;      /**< The house found: 2 the number as asked or a range holding
                          it, 1 only the plain number in front of its suffix, 0 none. */
  uint8_t has_name;  /**< 1 where the place carries a name of its own, 0 where the
                          dump left it nameless and an answer shows an empty line. */
  uint8_t named;     /**< The place still goes by what was typed; 0 for everyone
                          when no position was given. */
  uint8_t band;      /**< How near the searcher stands, 0 = nearest; 0 for everyone
                          when no position was given. */
} HitRank;

/**
 * @brief Does @p left stand before @p right?
 *
 *  Six keys, in this order: the place the query named; the house number it
 *  asked for; whether it is a place with a name at all; whether it still goes
 *  by what was typed; how near it lies to the searcher; the weight the dump
 *  gave it.
 *
 *  ### Why a name at all is weighed
 *
 *  The dump files a nameless address block as a place of its own — a street
 *  without a name, standing on a postal code and a town.  Four of them answered
 *  *Kirchheim bei München* before the town itself, each shown as an empty line
 *  with a postcode behind it, because they weighed more than the town did.  A
 *  place nobody can read is the weakest answer there is, whatever its weight,
 *  so it follows every place that has a name.
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
  if (left_rank.door != right_rank.door) return left_rank.door > right_rank.door;
  if (left_rank.has_name != right_rank.has_name) return left_rank.has_name > right_rank.has_name;
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

/**
 * @brief The places of the country @p document lies in, or NULL.
 *
 *  The country words — `#de`, `#at` — stand side by side in the dictionary, a
 *  few hundred of them, and the one whose places hold @p document is its
 *  country; see @ref geo_country.  An index built before the country words
 *  holds none, and every document there lies in no country.
 *
 *  @return A view to release with roaring_bitmap_free(), or NULL.
 */
static const roaring_bitmap_t *country_of(const GeoIndex *index, uint32_t document) {
  const char mark[1] = {GEO_COUNTRY_MARK};
  size_t first = prefix_bound(&index->words, mark, 1, false);
  size_t last = prefix_bound(&index->words, mark, 1, true);
  for (size_t rank = first; rank < last; ++rank) {
    size_t size = 0;
    geo_dictionary_word(&index->words, rank, &size);
    if (size != GEO_COUNTRY_TOKEN_SIZE) continue;
    const roaring_bitmap_t *documents = geo_index_word_documents(index, rank);
    if (!documents) continue;
    if (roaring_bitmap_contains(documents, document)) return documents;
    roaring_bitmap_free((roaring_bitmap_t *)documents);
  }
  return NULL;
}

/**
 * @brief The country the searcher stands in, as the places nearest them tell it.
 *
 *  The nearest of @p near that lies in a country decides — not the heaviest,
 *  which is the order @p near is kept in.  At a border the heaviest place of the
 *  ring is as likely to stand across it as not: someone asking in Aachen may
 *  well have a district of Vaals as the weightiest candidate around them, and
 *  counted from it every German town beyond 100 km would lie abroad.  Where the
 *  nearest place lies in no country, the next nearest is asked, and so on.
 *
 *  @param[in] index       Opened index.
 *  @param[in] near        The candidates found inside the ring.
 *  @param[in] near_count  Entries in @p near, at most @ref GEO_QUERY_LIMIT_MAX.
 *  @param[in] options     Query options; a position must be set.
 *  @return The places of that country, a view to release with
 *          roaring_bitmap_free(), or NULL where no candidate with a point lies
 *          in a country — in an index built without country words, never.
 */
static const roaring_bitmap_t *searcher_country(
    const GeoIndex *index, const GeoHit *near, size_t near_count, const GeoQueryOptions *options
) {
  bool asked[GEO_QUERY_LIMIT_MAX] = {false};
  if (near_count > GEO_QUERY_LIMIT_MAX) near_count = GEO_QUERY_LIMIT_MAX;
  for (size_t round = 0; round < near_count; ++round) {
    size_t nearest = SIZE_MAX;
    int64_t nearest_gap = INT64_MAX;
    for (size_t h = 0; h < near_count; ++h) {
      if (asked[h]) continue;
      int64_t gap = distance_squared(index, near[h].document, options);
      if (gap < nearest_gap) {
        nearest_gap = gap;
        nearest = h;
      }
    }
    if (nearest == SIZE_MAX) return NULL; /* nothing left that stands anywhere */
    asked[nearest] = true;
    const roaring_bitmap_t *country = country_of(index, near[nearest].document);
    if (country) return country;
  }
  return NULL;
}

/**
 * @brief Does the query name the spelling behind @p rank?
 *
 *  Only whole words count: the pieces a compound falls into are passed over, so
 *  *Gerastraße* is not named by *Gera*, and neither is its first word.
 *
 *  @param[in]     index       Opened index.
 *  @param[in]     rank        Display rank, or GEO_RANK_NONE.
 *  @param[in]     kept        Words of the query.
 *  @param[in,out] scratch     Tokenizer, overwritten.
 *  @param[in]     first_only  Ask for the first word of the spelling alone
 *                             rather than for every one of them.
 *  @return Whether the query holds them; false for a spelling of more than 64
 *          words, or none.
 */
static bool name_typed(
    const GeoIndex *index,
    uint32_t rank,
    const QueryWords *kept,
    TextTokenizer *scratch,
    bool first_only
) {
  if (rank == GEO_RANK_NONE) return false;
  size_t size = 0;
  const char *text = geo_dictionary_word(&index->display, rank, &size);
  if (!text || !size) return false;
  size_t tokens = text_tokenize(scratch, text, size);
  uint64_t words = 0, typed = 0;
  for (size_t t = 0; t < tokens; ++t) {
    const TextToken *token = &scratch->tokens[t];
    if (token->group >= 64) return false;
    uint64_t bit = UINT64_C(1) << token->group;
    if (token->part) continue; /* the halves of Gerastraße do not name Gera */
    words |= bit;
    if (query_words_have(kept, token->data, token->size)) typed |= bit;
  }
  if (first_only) return (typed & UINT64_C(1)) != 0;
  return words && (words & ~typed) == 0;
}

/**
 * @brief Take in the places beyond the ring that the query names outright.
 *
 *  A candidate of @p far joins the sample when all of these hold:
 *
 *  - it weighs at least @ref GEO_QUERY_FAR_WEIGHT_MIN — or at least
 *    @ref GEO_QUERY_FAR_NAMED_WEIGHT_MIN where it is a town (place_is_area()),
 *    the query typed the first word of its own name, no candidate inside the
 *    ring has a first word the query typed, and it lies in the searcher's
 *    country or within @ref GEO_QUERY_FAR_ABROAD_E7 of them;
 *  - it is not already among the first @p count of @p pool;
 *  - agreement_of() reaches GEO_AGREEMENT_CITY: the query names its postcode,
 *    or names its town by name as town_agreement() reads it — the first word of
 *    the town typed, or all but at most one of its words.  So *Halle* takes in
 *    Halle (Saale) and *Frankfurt* Frankfurt am Main; only a town the query
 *    names as the place it lies beside, *Garching bei München* for *München*,
 *    stays out.
 *
 *  At most @ref GEO_QUERY_FAR_MAX join, in the order @p far holds them, which is
 *  heaviest first.  Each arrives with its agreement written into @p ranks; the
 *  other keys are the caller's to fill, as for every candidate.
 *
 *  Once in, nothing is settled.  The ranking weighs a far place against the
 *  near ones by the same keys as always, so a named city stands before a street
 *  that only carries its name, and a named place inside the ring still stands
 *  before a named one outside it, since its band is nearer.
 *
 *  The weight is the guard.  A name alone would lift every village called
 *  after a common word — *Gmünd-Bahnhof* over the stations around the searcher,
 *  *Mitte-Ost* over the Mitte they stand in — because such a village carries
 *  the word as its own name just as a city does.  What tells the two apart is
 *  not the name but how much the place weighs.
 *
 *  Weight alone draws the line too high for towns, though: it kept *Gera* from
 *  anyone asking in Munich, where a Gerastraße stands, and *Brandenburg* found
 *  the state but not Brandenburg an der Havel.  A lighter town comes in where
 *  the query begins its own name and nothing near begins with what was typed —
 *  so *Mitte* does not reach *Mitte-Ost*, whose name begins otherwise, and a
 *  quarter nearby that begins with the word, *Neustadt/Süd* in Cologne, keeps
 *  *Neustadt* for itself.  Among the towns that come in, weight orders as it
 *  does everywhere: *Kamen* from Berlin answers with Kamen am Ob before Kamen
 *  in Westphalia, as *Halle* answers with Halle (Saale) before a village called
 *  Halle.  And a lighter town abroad comes in only near the border: *Halle* in
 *  Berlin answers with Halle (Saale) and Halle (Westf.), not with Halle in
 *  Belgium, while *Venlo* in Mönchengladbach still finds Venlo.
 *
 *  @param[in]     index     Opened index.
 *  @param[in]     kept      Words of the query.
 *  @param[in,out] scratch   Tokenizer, overwritten.
 *  @param[in]     far       Candidates found without the ring.
 *  @param[in]     far_count Entries in @p far.
 *  @param[in,out] pool      Sample; room for @p count + @ref GEO_QUERY_FAR_MAX.
 *  @param[out]    ranks     Receives the agreement of every place taken in.
 *  @param[in]     count     Candidates already in @p pool.
 *  @return Candidates in @p pool afterwards, at most @p count + GEO_QUERY_FAR_MAX.
 *
 *  @whisper A city heard from afar still answers to its name
 */
static size_t far_named_places(
    const GeoIndex *index,
    const QueryWords *kept,
    TextTokenizer *scratch,
    const GeoQueryOptions *options,
    const GeoHit *far,
    size_t far_count,
    GeoHit *pool,
    HitRank *ranks,
    size_t count
) {
  const size_t near_count = count;
  int near_named = -1;                 /* asked for once, where a light town first needs it */
  const roaring_bitmap_t *home = NULL; /* the searcher's country, likewise */
  bool home_asked = false;
  for (size_t f = 0; f < far_count && count < near_count + GEO_QUERY_FAR_MAX; ++f) {
    /* cheapest first: the weight is a field, the agreement folds two texts */
    if (far[f].importance < GEO_QUERY_FAR_NAMED_WEIGHT_MIN) continue;

    bool held = false;
    for (size_t h = 0; h < near_count && !held; ++h) held = pool[h].document == far[f].document;
    if (held) continue;

    unsigned agreement = agreement_of(index, far[f].document, kept, scratch);
    if (agreement < GEO_AGREEMENT_CITY) continue;

    if (far[f].importance < GEO_QUERY_FAR_WEIGHT_MIN) {
      const GeoDocument *record = &index->documents[far[f].document];
      if (!place_is_area(record->type) ||
          !name_typed(index, record->name_rank, kept, scratch, true)) {
        continue;
      }
      if (near_named < 0) {
        near_named = 0;
        for (size_t h = 0; h < near_count && !near_named; ++h) {
          uint32_t name = index->documents[pool[h].document].name_rank;
          near_named = name_typed(index, name, kept, scratch, true) ? 1 : 0;
        }
      }
      if (near_named) continue;
      if (!home_asked) {
        home_asked = true;
        home = searcher_country(index, pool, near_count, options);
      }
      if (home && !roaring_bitmap_contains(home, far[f].document)) {
        int64_t reach = GEO_QUERY_FAR_ABROAD_E7;
        if (distance_squared(index, far[f].document, options) > reach * reach) continue;
      }
    }

    pool[count] = far[f];
    ranks[count].agreement = (uint8_t)agreement;
    ++count;
  }
  if (home) roaring_bitmap_free((roaring_bitmap_t *)home);
  return count;
}

/* =========================================================================
 *  A country named in the query
 * ========================================================================= */

/** A country the query names, and the words that name it. */
typedef struct QueryCountry {
  roaring_bitmap_t *documents; /**< Every place in the country; owned, NULL for none. */
  uint64_t groups;             /**< The naming words, by token group: bit @c group. */
} QueryCountry;

/**
 * @brief Which query words does the spelling behind @p rank name in full?
 *
 *  The spelling is folded by the same tokenizer the query passed through.  It is
 *  named in full when every word of it — every token group, the pieces of a
 *  compound aside — stands among the query's words; a spelling of more than 64
 *  words is never named.
 *
 *  @param[in]     index     Opened index.
 *  @param[in]     rank      Display rank of the spelling, or GEO_RANK_NONE.
 *  @param[in]     query     Tokenizer still holding the query.
 *  @param[in,out] scratch   Tokenizer for the spelling, overwritten.
 *  @return The query words that name it, by token group; 0 when it is not named
 *          in full.
 */
static uint64_t groups_naming(
    const GeoIndex *index, uint32_t rank, const TextTokenizer *query, TextTokenizer *scratch
) {
  if (rank == GEO_RANK_NONE) return 0;
  size_t size = 0;
  const char *text = geo_dictionary_word(&index->display, rank, &size);
  if (!text || !size) return 0;

  size_t tokens = text_tokenize(scratch, text, size);
  uint64_t words = 0, met = 0, naming = 0;
  for (size_t t = 0; t < tokens; ++t) {
    const TextToken *token = &scratch->tokens[t];
    if (token->group >= 64) return 0;
    uint64_t bit = UINT64_C(1) << token->group;
    if (!token->part) words |= bit;
    for (size_t q = 0; q < query->token_count; ++q) {
      const TextToken *asked = &query->tokens[q];
      if (asked->part || asked->group >= 64) continue;
      if (asked->size == token->size && memcmp(asked->data, token->data, token->size) == 0) {
        met |= bit;
        naming |= UINT64_C(1) << asked->group;
      }
    }
  }
  return words && (words & ~met) == 0 ? naming : 0;
}

/**
 * @brief Does the query name a country beside other words — `Marienplatz München
 *        Deutschland`?
 *
 *  A word names a country when a country document — one carrying
 *  @ref GEO_COUNTRY_PLACE_TOKEN — answers to it, and one of that document's
 *  spellings, in the default reading or in any language of the index, stands in
 *  the query in full: *Deutschland*, *Germany*, *Vereinigte Staaten*.  Of several
 *  countries named, the one named with the most words is taken.
 *
 *  A query that is nothing but the country's name does not count — it asks for
 *  the country, and the country is found by its name like any place.  Nor does
 *  an index built before the country words existed: it has no
 *  @ref GEO_COUNTRY_PLACE_TOKEN, and nothing is named.
 *
 *  @param[in]  index      Opened index.
 *  @param[in]  tokenizer  Holding the query; left as it is.
 *  @param[out] out        Receives the country's places and its words; zeroed
 *                         when none is named.
 *  @return Whether a country is named beside other words.
 *
 *  @whisper A border named in passing narrows the map without being a place itself
 */
static bool query_country(
    const GeoIndex *index, const TextTokenizer *tokenizer, QueryCountry *out
) {
  memset(out, 0, sizeof(*out));
  size_t marker = 0;
  if (!geo_dictionary_find(
          &index->words, GEO_COUNTRY_PLACE_TOKEN, GEO_COUNTRY_PLACE_TOKEN_SIZE, &marker
      )) {
    return false;
  }
  const roaring_bitmap_t *countries = geo_index_word_documents(index, marker);
  if (!countries) return false;

  uint64_t asked = 0;
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    if (!tokenizer->tokens[t].part && tokenizer->tokens[t].group < 64) {
      asked |= UINT64_C(1) << tokenizer->tokens[t].group;
    }
  }

  TextTokenizer scratch;
  text_tokenizer_init(&scratch);
  scratch.repetition_filter = 0;
  uint32_t named = GEO_RANK_NONE;
  uint64_t naming = 0;

  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    const TextToken *token = &tokenizer->tokens[t];
    if (token->part || token_has_digit(token)) continue;
    size_t rank = 0;
    if (!geo_dictionary_find(&index->words, token->data, token->size, &rank)) continue;
    const roaring_bitmap_t *documents = geo_index_word_documents(index, rank);
    if (!documents) continue;
    roaring_bitmap_t *candidates = roaring_bitmap_and(documents, countries);
    roaring_bitmap_free(documents);
    if (!candidates) continue;

    roaring_uint32_iterator_t walk;
    roaring_iterator_init(candidates, &walk);
    for (; walk.has_value; roaring_uint32_iterator_advance(&walk)) {
      uint32_t document = walk.current_value;
      if (document >= index->document_count) continue;
      uint64_t best =
          groups_naming(index, index->documents[document].name_rank, tokenizer, &scratch);
      for (size_t l = 0; l < index->language_count; ++l) {
        const GeoVariant *variant = geo_index_variant(index, (int)l, document);
        if (!variant) continue;
        uint64_t groups = groups_naming(index, variant->name_rank, tokenizer, &scratch);
        if (__builtin_popcountll(groups) > __builtin_popcountll(best)) best = groups;
      }
      if (__builtin_popcountll(best) > __builtin_popcountll(naming)) {
        naming = best;
        named = document;
      }
    }
    roaring_bitmap_free(candidates);
  }
  roaring_bitmap_free(countries);

  /* nothing but the country's name: that asks for the country itself */
  if (named == GEO_RANK_NONE || (asked & ~naming) == 0) return false;

  /* the country's own code word is the one of the `#xx` words its document
     carries, walked only for a query that named a country */
  const roaring_bitmap_t *documents = country_of(index, named);
  if (!documents) return false;
  out->documents = roaring_bitmap_copy(documents);
  roaring_bitmap_free((roaring_bitmap_t *)documents);
  if (!out->documents) return false;
  out->groups = naming;
  return true;
}

/** What the readings answered with, and how they were asked when they did. */
typedef struct ReadingsAnswer {
  NumberReading answered; /**< The reading that answered, or the last one asked. */
  bool numbered;          /**< Its numbers were held back as house numbers. */
  bool near_used;         /**< The ring still narrowed when it answered. */
  uint64_t unfinished;    /**< The words read as beginnings, by token group; 0 for none. */
  uint64_t by_name;       /**< The words held against the candidates' names instead. */
} ReadingsAnswer;

/** The words a query broke off, told apart by what can be done with them. */
typedef struct BrokenWords {
  uint64_t unknown;  /**< Broken off and unknown to the dictionary. */
  uint64_t any;      /**< Every word broken off, the known ones too. */
  uint64_t tiny;     /**< One or two letters: never asked as a word at all. */
  uint64_t shrugged; /**< Those whose beginning matches too much to look up. */
} BrokenWords;

/**
 * @brief Which words of the query were left unfinished before the next one
 *        was begun?
 *
 *  A word the dictionary does not hold is passed over, so that a typo does not
 *  silence an otherwise clear address.  But just as often it is a street left
 *  unfinished while the town was typed behind it — *Kurpfa 57 Bammental*,
 *  *Hafenga Ulm* — and then passing over it answers with any house 57 in
 *  Bammental.  So the readings are asked with such a word read as a beginning
 *  first, and passed over only where that finds nothing: *Würzbrug* begins no
 *  word at all.
 *
 *  Not every unknown word counts.  One with a digit is a number, not a name.
 *  The last word is either still being typed, and read as a beginning already,
 *  or closed by a space, and asked exactly as it stands.  And a word joined to
 *  the next by a dash or a slash was written through, not broken off:
 *  *Badne-Baden* is a typo of Baden-Baden, and read as a beginning it answers
 *  with the Badner Weg.
 *
 *  Where even that finds nothing, a word the dictionary does hold may have been
 *  broken off too: *Gart 15 Bocholt* stops at a word of its own, and so does
 *  *Rings 27 Borchen*.  Such a query answers nothing as it stands, so reading
 *  those words as beginnings last takes nothing from a query that does.
 *
 *  A beginning that stands in front of more words than
 *  @ref GEO_QUERY_PREFIX_TERMS is no hint but a shrug — *An der Sch 4* — and so
 *  is one that folding expanded from an abbreviation, since what was typed is
 *  two or three letters: *Cottbusser St 26* broke off inside *Straße*.  Such a
 *  word cannot narrow the query, and what it can still do is told where
 *  ask_readings() asks its rounds.
 *
 *  A word of one or two letters is set apart further.  It is no beginning
 *  either, and as a word it is next to nothing: the index holds *s* and *d* and
 *  answers them with whatever happens to be spelled that way, so *Charlotte-S 8
 *  Berlin* found a road in Australia.  Such a word never narrows anything.
 *
 *  @param[in] index      Opened index.
 *  @param[in] tokenizer  Holding the query; left as it is.
 *  @return The words broken off, by token group (bit @c group, for groups below
 *          64), in the three classes of @ref BrokenWords; 0 for none.
 *
 *  @whisper A word broken off mid-breath still points where it was going
 */
static BrokenWords broken_words(const GeoIndex *index, const TextTokenizer *tokenizer) {
  BrokenWords broken = {0, 0, 0};
  for (size_t t = 0; t < tokenizer->token_count; ++t) {
    const TextToken *token = &tokenizer->tokens[t];
    if (token->part || token->group >= 64 || token_has_digit(token)) continue;
    /* the word after it, and what stood between the two */
    const TextToken *next = NULL;
    for (size_t n = t + 1; n < tokenizer->token_count && !next; ++n) {
      const TextToken *candidate = &tokenizer->tokens[n];
      if (!candidate->part && candidate->group > token->group) next = candidate;
    }
    if (!next || next->joint != TEXT_JOINT_NONE) continue;
    /* a letter behind a number is the suffix of a door — *Berliner Straße 12 a* */
    const TextToken *before = NULL;
    for (size_t b = t; b > 0; --b) {
      const TextToken *candidate = &tokenizer->tokens[b - 1];
      if (!candidate->part && candidate->group < token->group) before = candidate;
      if (before) break;
    }
    if (before && token_has_digit(before)) continue;

    uint64_t bit = UINT64_C(1) << token->group;
    broken.any |= bit;
    size_t rank = 0;
    if (!geo_dictionary_find(&index->words, token->data, token->size, &rank)) broken.unknown |= bit;
    const char *written = NULL;
    size_t first = prefix_bound(&index->words, token->data, token->size, false);
    size_t last = prefix_bound(&index->words, token->data, token->size, true);
    if (token->size < GEO_QUERY_PREFIX_MIN) {
      broken.tiny |= bit;
    } else if (
        last - first > GEO_QUERY_PREFIX_TERMS ||
        text_written_form(token->data, token->size, &written)) {
      broken.shrugged |= bit;
    }
  }
  return broken;
}

/**
 * @brief Ask the three readings in turn, first inside the ring and then without it.
 *
 *  Each reading is asked only when the one before found nothing; why they come
 *  in this order is told where geo_index_query_options() asks for them.
 *
 *  @param[in]     numbers_present  The query holds a word with a digit.
 *  @param[in]     unfinished  The unknown words unfinished_words() found, by
 *                          token group; everything is asked with them read as
 *                          beginnings first, and passed over only where that
 *                          found nothing.
 *  @param[in]     broken   The words broken off, in their three classes.
 *  @param[in,out] prefixes The beginnings this query has joined.
 *  @param[in,out] names    Tokenizer for the last round's name check.
 *  @param[in]     near     Documents around the searcher, or NULL; borrowed.
 *  @param[in]     country  Documents of the country named, or NULL; borrowed.
 *  @param[in]     skipped  The words that named @p country, by token group.
 *  @param[out]    pool     Receives the answer.
 *  @param[in,out] stats    Counts of this query, or NULL.
 *  @param[out]    out      How the answer was found.
 *  @return Number of results written into @p pool; 0 when every reading failed.
 */
static size_t ask_readings(
    const GeoIndex *index,
    const TextTokenizer *tokenizer,
    bool prefix_last,
    bool numbers_present,
    BrokenWords broken,
    PrefixCache *prefixes,
    TextTokenizer *names,
    const roaring_bitmap_t *near,
    const roaring_bitmap_t *country,
    uint64_t skipped,
    GeoHit *pool,
    size_t pool_limit,
    GeoQueryStats *stats,
    ReadingsAnswer *out
) {
  size_t count = 0;
  out->answered = NUMBERS_AS_WORDS;
  out->numbered = numbers_present;
  /* Unknown words as beginnings, then passed over, then every broken word as
     one, and last — where none of that answered — the words that cannot be
     looked up at all, let go of and held against the names and towns of what
     the rest of the query leaves standing.  A word of one or two letters is
     let go of there as well: the index answers *s* and *d* with whatever
     happens to be spelled that way.

     The check belongs to that last round and to no other.  A place answers
     through every name its entry carried, and only the current one is written
     down to show: *Rue de la Paix* is filed under *Friedenstraße* and *Via IV
     Novembre* under *Via Quattro Novembre*, and both would fail a check they
     never had to pass. */
  uint64_t rounds[4];
  int round_count = 0;
  if (broken.unknown) rounds[round_count++] = broken.unknown;
  rounds[round_count++] = 0;
  if (broken.any != broken.unknown) rounds[round_count++] = broken.any;
  int checked = -1;
  if (broken.tiny | broken.shrugged) {
    checked = round_count;
    rounds[round_count++] = 0;
  }
  for (int round = 0; round < round_count && !count; ++round) {
    uint64_t let_go = round == checked ? broken.tiny | broken.shrugged : 0;
    out->by_name = let_go;
    out->unfinished = rounds[round];
    uint64_t asked = skipped | let_go;
    out->near_used = near != NULL;
    /* a position let go of by an earlier call is taken up again by this one */
    if (stats && near) stats->position_dropped = 0;
    for (int attempt = 0; attempt < 2 && !count; ++attempt) {
      /* The position is the first thing let go of.  A search that finds nothing
         nearby was asking about somewhere else — that is a plain reading of the
         words, while returning nothing at all is not.  Whoever named a town or a
         postcode said so outright, and those readings come after. */
      if (attempt) {
        if (!near) break; /* there was nothing to let go of; the chain already ran */
        out->near_used = false;
        if (stats) stats->position_dropped = 1;
      }
      const roaring_bitmap_t *carried_near = out->near_used ? near : NULL;
      out->numbered = numbers_present;

      if (out->numbered) {
        out->answered = NUMBERS_BUT_CODES;
        count = query_words(
            index, tokenizer, NUMBERS_BUT_CODES, prefix_last, out->unfinished, prefixes,
            out->by_name, names, carried_near, country, asked, pool, pool_limit, stats
        );
        if (!count) {
          out->answered = NUMBERS_AS_HOUSES;
          count = query_words(
              index, tokenizer, NUMBERS_AS_HOUSES, prefix_last, out->unfinished, prefixes,
              out->by_name, names, carried_near, country, asked, pool, pool_limit, stats
          );
        }
      }
      if (!count) {
        out->numbered = false;
        out->answered = NUMBERS_AS_WORDS;
        count = query_words(
            index, tokenizer, NUMBERS_AS_WORDS, prefix_last, out->unfinished, prefixes,
            out->by_name, names, carried_near, country, asked, pool, pool_limit, stats
        );
      }
    }
  }
  return count;
}

/**
 * @brief Do the words that named a country stand inside a longer name one of
 *        @p hits carries — its own, or its town's?
 *
 *  *28 Rue de Madagascar* names Madagascar, and *West Jordan* names Jordan, and
 *  neither asks about a country: the word belongs to a name typed in full, with
 *  more words of it beside the country's.  A name that is nothing but the
 *  country's words does not count — *CEMEX Deutschland AG* is not typed in
 *  full by *Berlin Deutschland*, and a café called *Deutschland* is no reason
 *  to forget the country.
 *
 *  @param[in] index      Opened index.
 *  @param[in] hits       Places the query found without the country.
 *  @param[in] count      Number of @p hits.
 *  @param[in] tokenizer  Holding the query; left as it is.
 *  @param[in] groups     The words that named the country, by token group.
 *  @return Whether one of the places carries them inside a name of its own.
 *
 *  @whisper A border can lend its name to a street far away from it
 */
static bool country_in_a_name(
    const GeoIndex *index,
    const GeoHit *hits,
    size_t count,
    const TextTokenizer *tokenizer,
    uint64_t groups
) {
  if (!count || !groups) return false;
  TextTokenizer scratch;
  text_tokenizer_init(&scratch);
  scratch.repetition_filter = 0;
  for (size_t h = 0; h < count; ++h) {
    if (hits[h].document >= index->document_count) continue;
    const GeoDocument *record = &index->documents[hits[h].document];
    const uint32_t ranks[] = {record->name_rank, record->city_rank};
    for (size_t r = 0; r < sizeof(ranks) / sizeof(ranks[0]); ++r) {
      uint64_t naming = groups_naming(index, ranks[r], tokenizer, &scratch);
      if ((naming & groups) == groups && (naming & ~groups) != 0) return true;
    }
  }
  return false;
}

/** Farthest two nameless places of one town may stand apart and still be one line. */
#define GEO_QUERY_SAME_BLOCK_E7 180000

/** Farthest two named places may stand apart and still be one place, with a
 *  position to tell which of them is meant. */
#define GEO_QUERY_SAME_PLACE_NEAR_E7 180000

/**
 * @brief Are these two places the same one, said twice?
 *
 *  The dump files a place once per kind, and a merge keeps apart what carries
 *  different names — so *Halle (Westf.)* arrives as a town and as a district,
 *  *Den Haag* twice with two weights, and a town's nameless address blocks as
 *  one place each.  An answer shows a name, a town and a postal code, and where
 *  two places agree in all three and stand near enough to each other — within
 *  @ref GEO_QUERY_SAME_BLOCK_E7 while both are nameless, within
 *  @ref GEO_QUERY_SAME_PLACE_NEAR_E7 while @p positioned says the searcher can
 *  tell them apart — they are one line repeated.
 *
 *  A missing field counts as a field: two nameless places agree in their
 *  namelessness, which is exactly what makes them look alike.  A place that
 *  carries no coordinate cannot be told apart by distance, and is taken as the
 *  same as its twin rather than shown beside it.
 */
static bool same_place(const GeoIndex *index, uint32_t left, uint32_t right, bool positioned) {
  const GeoDocument *a = &index->documents[left];
  const GeoDocument *b = &index->documents[right];
  if (a->name_rank != b->name_rank) return false;
  if (a->city_rank != b->city_rank) return false;
  if (a->postcode_rank != b->postcode_rank) return false;
  if (!(a->flags & GEO_DOCUMENT_HAS_POINT) || !(b->flags & GEO_DOCUMENT_HAS_POINT)) return true;

  int64_t north = (int64_t)a->lat_e7 - b->lat_e7;
  int64_t east = (int64_t)a->lon_e7 - b->lon_e7;
  /* the shorter way round the world, for two places either side of the dateline */
  if (east > 1800000000) east -= 3600000000LL;
  if (east < -1800000000) east += 3600000000LL;
  east = east * longitude_shrink(a->lat_e7) / 16;
  /* A nameless line says nothing but its town, so two of them are one line
     wherever in that town they stand.  Two places that *are* named are written
     down twice in two places as often as not — the middle of a town's boundary
     and the point that carries its name lie a kilometre or two apart — and
     which of those is meant only a position can say.  So they are taken
     together where one was given, and shown side by side where none was: the
     build has already joined what stands within 300 m of its twin, and what
     survived that is two answers until something says otherwise. */
  if (a->name_rank != GEO_RANK_NONE && !positioned) return false;
  int64_t reach =
      a->name_rank == GEO_RANK_NONE ? GEO_QUERY_SAME_BLOCK_E7 : GEO_QUERY_SAME_PLACE_NEAR_E7;
  return north * north + east * east <= reach * reach;
}

/**
 * @brief Keep the first of every place that answers twice, in the order the
 *        ranking left them.
 *
 *  The ranking has spoken by the time this runs, so the one kept is the one it
 *  put first — the heaviest of them, or the one carrying the house number.
 *  What follows only repeats it, and a list that repeats itself reads as
 *  broken however right each line is.
 *
 *  @return How many hits remain at the front of @p hits.
 *
 *  @whisper One place says its name once, however many times it was written down
 */
static size_t drop_repeats(
    const GeoIndex *index,
    GeoHit *hits,
    HitRank *ranks,
    size_t count,
    const GeoQueryOptions *options
) {
  bool positioned = options->has_position;
  size_t kept = 0;
  for (size_t h = 0; h < count; ++h) {
    bool repeated = false;
    for (size_t k = 0; k < kept && !repeated; ++k) {
      repeated = same_place(index, hits[k].document, hits[h].document, positioned);
      /* Of two ways of writing one place down, the one nearer the searcher is
         the one they mean: a town's own point stands in the town, the middle of
         its boundary a kilometre outside it.  The place keeps the rank the
         ranking gave it and answers with the nearer of its two records.

         Only where the two say the same about the house number, though.  One
         record of a street may carry the number that was asked for while its
         twin does not, and the door is worth more than the few hundred metres:
         whoever asked for it means the record that has it. */
      if (repeated && positioned && ranks[h].door == ranks[k].door &&
          distance_squared(index, hits[h].document, options) <
              distance_squared(index, hits[k].document, options)) {
        GeoHit nearer = hits[h];
        nearer.matched = hits[k].matched;
        hits[k] = nearer;
      }
    }
    if (!repeated) {
      hits[kept] = hits[h];
      ranks[kept] = ranks[h];
      ++kept;
    }
  }
  return kept;
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

  /* --- a country named beside other words narrows by where a place lies, not by
         what it is called: no place in the dump carries its country's name, so
         *Marienplatz München Deutschland* would otherwise meet nothing at all. --- */
  QueryCountry country;
  bool has_country = query_country(index, tokenizer, &country);

  /* --- more candidates than were asked for, so the ranking has something to
         choose from.  The place someone means is not always among the heaviest
         that carry the words, and what is cut here can never be lifted later.
         A caller who asks for more than a sample of four per result holds is
         weighed at exactly its limit.

         The sample is bounded by the ceiling the limit was clamped to, plus the
         few places far_named_places() may add, so however many candidates
         survive, the ranking below can hold every one of them.  That is the
         whole point of clamping: a query that answers more places than the
         sample fits would otherwise have to give the ranking up, and would give
         it up in silence.  The answer is copied out at the end — at most 256
         hits, which is nothing beside the search that found them. --- */
  GeoHit sample[GEO_QUERY_LIMIT_MAX + GEO_QUERY_FAR_MAX];
  GeoHit *pool = sample;
  size_t pool_limit = limit;
  if (limit <= GEO_QUERY_LIMIT_MAX / GEO_QUERY_OVERSAMPLE) {
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

  /* --- A word that names a country may just as well be part of a longer name
         typed in full: *Rue de Madagascar*, *West Jordan*, *Avenue Albert 1er
         de Belgique*.  So the words are asked plainly first, and the country
         narrows only where no place found that way carries the naming words
         inside a name of its own or of its town.  Should the country then
         narrow the answer to nothing — *Atlanta Georgia* names a state, not the
         country — the plain answer is asked for again, since the pass that
         answers is the one the counts describe. --- */
  BrokenWords broken = broken_words(index, tokenizer);
  PrefixCache prefixes = {0};
  TextTokenizer names;
  text_tokenizer_init(&names);
  /* two candidates may well be named alike, and the filter that skips a
     repeated input would let the second one fail the check */
  names.repetition_filter = 0;
  ReadingsAnswer answer;
  size_t count = ask_readings(
      index, tokenizer, prefix_last, numbers_present, broken, &prefixes, &names, near, NULL, 0,
      pool, pool_limit, stats, &answer
  );
  bool country_used = false;
  if (has_country && !country_in_a_name(index, pool, count, tokenizer, country.groups)) {
    ReadingsAnswer narrowed;
    size_t found = ask_readings(
        index, tokenizer, prefix_last, numbers_present, broken, &prefixes, &names, near,
        country.documents, country.groups, pool, pool_limit, stats, &narrowed
    );
    if (found) {
      count = found;
      answer = narrowed;
      country_used = true;
    } else if (count) {
      count = ask_readings(
          index, tokenizer, prefix_last, numbers_present, broken, &prefixes, &names, near, NULL, 0,
          pool, pool_limit, stats, &answer
      );
    }
  }
  bool numbered = answer.numbered;
  bool near_used = answer.near_used;
  NumberReading answered = answer.answered;
  if (near) roaring_bitmap_free(near);
  if (!count) {
    if (has_country) roaring_bitmap_free(country.documents);
    prefix_cache_free(&prefixes);
    return 0;
  }

  /* --- The ring narrowed before weight could cut, and that is also its blind
         spot: a place the query names outright is never a candidate when it
         lies beyond the ring and something inside it carries the same word.
         *Würzburg* asked from Berlin meets the Würzburger Straße there, the
         ring holds, and the city itself is never seen.  So the reading that
         answered is asked once more without the ring, and far_named_places()
         lets the heavy places it names join the ranking.  A position that was
         let go of has already asked without it. --- */
  GeoHit far[GEO_QUERY_LIMIT_MAX];
  size_t far_count = 0;
  if (near_used) {
    /* the counts describe the pass that answered, and this one only looks */
    uint32_t groups = stats ? stats->groups : 0;
    uint64_t narrowed = stats ? stats->narrowed : 0;
    far_count = query_words(
        index, tokenizer, answered, prefix_last, answer.unfinished, &prefixes, answer.by_name,
        &names, NULL, country_used ? country.documents : NULL, country_used ? country.groups : 0,
        far, pool_limit, stats
    );
    if (stats) {
      stats->groups = groups;
      stats->narrowed = narrowed;
    }
  }
  if (has_country) roaring_bitmap_free(country.documents);
  prefix_cache_free(&prefixes);

  /* --- and now the number finds its door.  The number as it was asked for
         first; only where the street has no such door does the plain number
         answer — *Lister Meile 29D* asks for a door that was never mapped,
         and the 29 beside it is the nearest thing there is, while the bare
         street says nothing about where to go.  A plain number asked for
         finds the range that holds it the same way: whoever lives at
         *Anderter Straße 1-3* types *Anderter Straße 1*. --- */
  HouseAsked asked[TEXT_TOKEN_MAX];
  size_t asked_count = 0;
  if (numbered) {
    for (size_t t = 0; t < tokenizer->token_count; ++t) {
      if (house_asked_of(tokenizer, t, &asked[asked_count])) ++asked_count;
    }
    for (size_t h = 0; h < count + far_count; ++h) {
      GeoHit *hit = h < count ? &pool[h] : &far[h - count];
      static const HouseMatch ORDER[] = {HOUSE_AS_ASKED, HOUSE_IN_RANGE, HOUSE_PLAIN_NUMBER};
      for (size_t o = 0; o < 3 && hit->house == GEO_RANK_NONE; ++o) {
        for (size_t a = 0; a < asked_count; ++a) {
          uint32_t house = find_house(index, hit->document, &asked[a], ORDER[o]);
          if (house != GEO_RANK_NONE) {
            hit->house = house;
            break;
          }
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

  /* pool_limit is bounded above, hit_insert never returns more than it was
     given, and far_named_places() adds at most GEO_QUERY_FAR_MAX — so count
     fits, always, and the ranking runs for every query rather than for most. */
  HitRank ranks[GEO_QUERY_LIMIT_MAX + GEO_QUERY_FAR_MAX];
  size_t near_count = count;
  count = far_named_places(index, &kept, tokenizer, options, far, far_count, pool, ranks, count);
  for (size_t h = 0; h < count; ++h) {
    /* a place taken in from beyond the ring arrives with its agreement */
    if (h < near_count) {
      ranks[h].agreement = (uint8_t)agreement_of(index, pool[h].document, &kept, tokenizer);
    }
    /* the door asked for, or the range that holds it, stands before the plain
       number that stood in for a suffix */
    ranks[h].door = 0;
    if (pool[h].house != GEO_RANK_NONE) {
      ranks[h].door = house_is_asked(index, pool[h].house, asked, asked_count) ? 2u : 1u;
    }
    ranks[h].has_name = index->documents[pool[h].document].name_rank != GEO_RANK_NONE ? 1u : 0u;
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
  count = drop_repeats(index, pool, ranks, count, options);

  if (stats) stats->weighed = count; /* what the ranking held, before the limit trims */
  if (count > limit) count = limit;

  /* --- a street that has not the number asked for is still a street with
         houses on it, and where the neighbours of that number stand close
         together the point is laid between them rather than left in the
         middle of the street.  Only for the answers handed out, and only the
         point: the number stays unfound, so the ranking and what an answer
         says are the same as without it. --- */
  for (size_t h = 0; h < count && asked_count; ++h) {
    if (pool[h].house != GEO_RANK_NONE) continue;
    for (size_t a = 0; a < asked_count; ++a) {
      uint32_t number = 0;
      size_t digits = asked[a].digits ? asked[a].digits : asked[a].size;
      if (digits > 6) continue;
      for (size_t i = 0; i < digits && asked[a].text[i] >= '0' && asked[a].text[i] <= '9'; ++i) {
        number = number * 10u + (uint32_t)(asked[a].text[i] - '0');
      }
      if (geo_index_house_estimate(
              index, pool[h].document, number, GEO_RANK_NONE, &pool[h].lat_e7, &pool[h].lon_e7
          )) {
        pool[h].estimated = 1;
        break;
      }
    }
  }
  memcpy(hits, pool, count * sizeof(*hits));
  if (stats) stats->results = count;
  return count;
}

/** @endcond */

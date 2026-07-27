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

/** Pad the file to the next section boundary. */
static grd_result pad_to_alignment(FILE *file, uint64_t *position) {
  static const char zeros[GEO_INDEX_ALIGNMENT] = {0};
  uint64_t misaligned = *position % GEO_INDEX_ALIGNMENT;
  if (!misaligned) return GRD_SUCCESS;
  size_t padding = GEO_INDEX_ALIGNMENT - (size_t)misaligned;
  if (fwrite(zeros, 1, padding, file) != padding) return GRD_ERROR_ENCODE_FAILED;
  *position += padding;
  return GRD_SUCCESS;
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

grd_result geo_index_write(
    const char *path,
    const NameSet *words,
    const NameSet *display,
    const DocSet *documents,
    uint64_t total_terms
) {
  if (!path || !words || !display || !documents) return GRD_ERROR_NULL_POINTER;

  FILE *file = fopen(path, "wb");
  if (!file) return GRD_ERROR_ENCODE_FAILED;
  static char write_buffer[1 << 20];
  setvbuf(file, write_buffer, _IOFBF, sizeof(write_buffer));

  GeoIndexHeader header;
  memset(&header, 0, sizeof(header));
  GeoIndexSection sections[GEO_INDEX_SECTION_COUNT];
  memset(sections, 0, sizeof(sections));

  uint64_t position = sizeof(header) + sizeof(sections);
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

  result = section_begin(file, &position, &sections[7], GEO_INDEX_SECTION_POSTING_OFFSETS);
  if (result != GRD_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[7], documents->posting_offsets,
      (documents->word_count + 1) * sizeof(uint32_t)
  );
  if (result != GRD_SUCCESS) goto failed;

  result = section_begin(file, &position, &sections[8], GEO_INDEX_SECTION_POSTINGS);
  if (result != GRD_SUCCESS) goto failed;
  result = section_put(
      file, &position, &sections[8], documents->postings,
      documents->posting_count * sizeof(uint32_t)
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
  header.total_terms = total_terms;

  result = GRD_ERROR_ENCODE_FAILED;
  if (fseek(file, 0, SEEK_SET) != 0) goto failed;
  if (fwrite(&header, sizeof(header), 1, file) != 1) goto failed;
  if (fwrite(sections, sizeof(sections), 1, file) != 1) goto failed;
  if (fflush(file)) goto failed;
  result = GRD_SUCCESS;

failed:
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
  const uint32_t *posting_offsets = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_POSTING_OFFSETS,
      (header->word_count + 1) * sizeof(uint32_t), NULL
  );
  const uint32_t *postings = section_of(
      base, size, sections, header->section_count, GEO_INDEX_SECTION_POSTINGS,
      header->posting_count * sizeof(uint32_t), NULL
  );
  if (!documents || !posting_offsets || !postings) goto refused_dictionaries;
  if (posting_offsets[header->word_count] != header->posting_count) goto refused_dictionaries;

  index->base = base;
  index->size = size;
  index->documents = documents;
  index->document_count = (size_t)header->document_count;
  index->posting_offsets = posting_offsets;
  index->postings = postings;
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

const uint32_t *geo_index_postings(const GeoIndex *index, size_t rank, size_t *out_count) {
  if (out_count) *out_count = 0;
  if (!index || !index->postings || rank >= index->words.word_count) return NULL;
  uint32_t start = index->posting_offsets[rank];
  uint32_t end = index->posting_offsets[rank + 1];
  if (end <= start) return NULL;
  if (out_count) *out_count = end - start;
  return index->postings + start;
}

/* =========================================================================
 *  Querying
 * ========================================================================= */

/** A word on more than this share of all documents is treated as filler. */
#define GEO_QUERY_COMMON_SHARE 100

/** …but never below this many documents, so small indexes keep every word. */
#define GEO_QUERY_COMMON_FLOOR 1000

/** One word of the query, with the documents that carry it. */
typedef struct QueryTerm {
  const uint32_t *documents;
  size_t count;
} QueryTerm;

/** Is @p document among an ascending list? */
static bool list_contains(const uint32_t *documents, size_t count, uint32_t document) {
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (documents[middle] == document) return true;
    if (documents[middle] < document) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return false;
}

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
  size_t token_count = text_tokenize(tokenizer, query, size);
  if (!token_count) return 0;

  QueryTerm terms[TEXT_TOKEN_MAX];
  size_t term_count = 0;
  for (size_t t = 0; t < token_count && term_count < TEXT_TOKEN_MAX; ++t) {
    size_t rank = 0;
    if (!geo_dictionary_find(
            &index->words, tokenizer->tokens[t].data, tokenizer->tokens[t].size, &rank
        )) {
      continue; /* a word nobody ever wrote cannot narrow anything down */
    }
    size_t count = 0;
    const uint32_t *documents = geo_index_postings(index, rank, &count);
    if (!documents) continue;
    terms[term_count].documents = documents;
    terms[term_count].count = count;
    ++term_count;
  }
  if (!term_count) return 0;

  /* --- shortest list first: it names the candidates, and the ones after it
         are asked in growing order, so a candidate that does not belong is
         usually turned away by a cheap list before an expensive one is
         touched.  A word like *de* sits on millions of documents; every probe
         into its list is a jump through gigabytes. --- */
  for (size_t t = 1; t < term_count; ++t) {
    QueryTerm term = terms[t];
    size_t place = t;
    while (place > 0 && terms[place - 1].count > term.count) {
      terms[place] = terms[place - 1];
      --place;
    }
    terms[place] = term;
  }

  /* --- a word standing on nearly everything narrows nothing down.  It is not
         wrong, only useless, and every probe into its list is a jump through
         gigabytes — so it is let go while narrower words remain.  Which words
         those are needs no list: their own frequency says it.
         Two words are always kept, though: falling back to a single common one
         would turn a search into a list of whatever is most important, and
         *Rio de Janeiro* would answer with everything ever named *Janeiro*. --- */
  size_t common = index->document_count / GEO_QUERY_COMMON_SHARE;
  if (common < GEO_QUERY_COMMON_FLOOR) common = GEO_QUERY_COMMON_FLOOR;
  while (term_count > 2 && terms[term_count - 1].count > common) { --term_count; }

  size_t found = 0;
  for (size_t i = 0; i < terms[0].count; ++i) {
    uint32_t document = terms[0].documents[i];
    if (document >= index->document_count) continue;
    bool everywhere = true;
    for (size_t t = 1; t < term_count && everywhere; ++t) {
      everywhere = list_contains(terms[t].documents, terms[t].count, document);
    }
    if (!everywhere) continue;
    GeoHit hit = {
        .document = document,
        .matched = (uint32_t)term_count,
        .importance = index->documents[document].importance,
    };
    found = hit_insert(hits, found, limit, hit);
  }
  return found;
}

/** @endcond */

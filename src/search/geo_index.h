/** @defgroup geo_index Geo index file
 *  @ingroup search
 *  @brief The built index as it rests on disk — a picture of the memory it
 *         becomes, so that starting up is a mapping and not a reading.
 *
 *  The dump takes minutes to walk; the index built from it must be there in
 *  an instant.  That rules out every format which has to be decoded: the
 *  file is laid out exactly as the search wants to see it, and opening it is
 *  an `mmap`.  Nothing is parsed, nothing is copied, and the operating system
 *  fetches only the pages a query actually touches.
 *
 *  ### Shape
 *
 *  @code
 *  [ header          ] magic, version, layout hash, byte order, counts
 *  [ sections        ] one descriptor per section: kind, offset, size
 *  [ word dictionary ] groups, offsets, text — folded words, what a query meets
 *  [ display dict.   ] groups, offsets, text — original spelling, what an answer shows
 *  [ documents       ] one fixed record per place
 *  [ importance      ] one weight per place, apart from the records
 *  [ posting offsets ] word_count + 1 byte offsets into the postings
 *  [ postings        ] one frozen Roaring bitmap per word, 32-byte aligned
 *  [ houses          ] house numbers with their own coordinate
 *  [ house offsets   ] document_count + 1 entries into the houses
 *  [ languages       ] one tag per language, and where its readings begin
 *  [ variants        ] localized readings, by language then by document
 *  @endcode
 *
 *  A build that asked for one language writes that one language record and no
 *  readings at all: the default reading lives in the document itself, so the
 *  variants section stays empty.  That is why the two sit at the end and cost
 *  such a build nothing but two descriptors and sixteen bytes.
 *
 *  Two dictionaries, because the two jobs disagree: a query types
 *  *muenchen* and an answer must read *München*.  Folded words are shared far
 *  more often than written ones, so the search side stays small.
 *
 *  Three rules keep a mapped file honest:
 *
 *  - **No pointers.** Everything is an index or an offset relative to its
 *    section, so the mapping may land anywhere.
 *  - **Explicit layout.** Fixed widths, written-out padding, `static_assert`
 *    on every record size — a compiler cannot shift the format silently.
 *  - **The header refuses.** Magic, version, byte order and a hash over the
 *    record sizes are checked before a single byte is trusted.
 *
 *  The postings are Roaring bitmaps rather than plain lists of numbers.  A
 *  word standing on millions of places then costs a bit per place instead of
 *  four bytes, and asking whether one of them is *this* place is a bit test
 *  instead of a walk through gigabytes.  Nothing is decoded when the file
 *  opens: a bitmap is viewed where it lies.
 *
 *  Words are stored whole here, not split into prefix and remainder as they
 *  were while being collected: the file is read far more often than written,
 *  and a reader should not have to reassemble anything.  The prefix trees are
 *  rebuilt from the group tables when the file opens, which costs microseconds.
 *
 *  @whisper What was gathered over minutes waits here to be found in an instant
 *  @{
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <roaring/roaring.h>

#include "arnm/result.h"
#include "search/doc_collector.h"
#include "search/house_collector.h"
#include "search/name_collector.h"
#include "search/prefix_tree.h"
#include "search/text_tokenize.h"
#include "types/geo_index_section_kind.h"
#include "types/geo_query_stats.h"

/** Eight bytes opening every index file. */
#define GEO_INDEX_MAGIC "GRDGEOIX"

/** Format version; a reader refuses anything it does not know.
 *
 *  Version 7 added the cell words that carry a place's position into the
 *  dictionary.  The layout did not change by a byte — an older file would be
 *  read without complaint and would answer every query about *near me* with
 *  nothing, silently, because the words it looks for were never written.  A
 *  refusal that names the reason is the better failure.
 *
 *  Version 8 widened the folding to the whole Latin script.  The layout stayed
 *  as it was again, and again an older file would be read without complaint:
 *  its dictionary holds *bucurești* and *nội* as the dump wrote them, while a
 *  query now asks for *bucuresti* and *noi*.  The two would never meet, and
 *  nothing would say why.  The index must be built again.
 *
 *  Version 9 gave the header two counters and the file two sections, for the
 *  readings a build keeps beside the default one.  Here the layout did move,
 *  and the layout hash would have caught it — the version says the same thing
 *  earlier and in words. */
#define GEO_INDEX_VERSION 9u

/** Written as 0x01020304 — a reader on the other byte order sees it reversed. */
#define GEO_INDEX_BYTE_ORDER 0x01020304u

/** Customary file extension, and what the command line dispatches on. */
#define GEO_INDEX_EXTENSION ".gdx"

/** Where one section sits in the file. */
typedef struct GeoIndexSection {
  uint32_t kind;   /**< A @ref GeoIndexSectionKind. */
  uint32_t flags;  /**< Reserved, written as 0. */
  uint64_t offset; /**< Byte offset from the start of the file, multiple of 8. */
  uint64_t size;   /**< Byte length. */
} GeoIndexSection;

/** Room for a language tag and its terminator, as the file keeps it. */
#define GEO_LANGUAGE_TAG_MAX 8

/** Most languages one index may hold readings for. */
#define GEO_LANGUAGE_MAX 64

/**
 * @brief One language the index holds readings for.
 *
 *  The tags stand in the order the build named them, and that order is what a
 *  variant record's language number means.  A caller asks by tag and is given
 *  the number once, before the results are walked.
 */
typedef struct GeoIndexLanguage {
  char tag[GEO_LANGUAGE_TAG_MAX]; /**< Lowercase, NUL-terminated, zero-padded. */
  uint32_t start;                 /**< First variant record of this language. */
  uint32_t count;                 /**< How many it has; 0 for a language nothing named. */
} GeoIndexLanguage;

/** One prefix group: the words sharing their leading bytes. */
typedef struct GeoIndexGroup {
  uint8_t key[PREFIX_TREE_DEPTH_MAX]; /**< Leading bytes, zero-padded. */
  uint32_t start;                     /**< Rank of the group's first word. */
  uint32_t count;                     /**< Words in the group; never 0. */
} GeoIndexGroup;

/** The header, first bytes of the file. */
typedef struct GeoIndexHeader {
  char magic[8];                /**< @ref GEO_INDEX_MAGIC, not NUL-terminated. */
  uint32_t version;             /**< @ref GEO_INDEX_VERSION. */
  uint32_t byte_order;          /**< @ref GEO_INDEX_BYTE_ORDER. */
  uint32_t layout_hash;         /**< Over the record sizes — catches a shifted layout. */
  uint32_t section_count;       /**< Descriptors following this header. */
  uint64_t file_size;           /**< Total bytes, checked against the real file. */
  uint64_t word_count;          /**< Folded words. */
  uint64_t word_group_count;    /**< Prefix groups of the word dictionary. */
  uint64_t display_count;       /**< Written spellings. */
  uint64_t display_group_count; /**< Prefix groups of the display dictionary. */
  uint64_t document_count;      /**< Places one can find. */
  uint64_t posting_count;       /**< Word-to-document connections. */
  uint64_t house_count;         /**< House numbers hanging on the streets. */
  uint64_t total_terms;         /**< Terms seen while building — reporting only. */
  uint64_t language_count;      /**< Languages of the build, the default one at place 0
                                     included; also the records in the language section. */
  uint64_t variant_count;       /**< Localized readings held for them. */
} GeoIndexHeader;

static_assert(sizeof(GeoIndexHeader) == 112, "GeoIndexHeader layout changed");
static_assert(sizeof(GeoIndexLanguage) == 16, "GeoIndexLanguage layout changed");
static_assert(sizeof(GeoVariant) == 12, "GeoVariant layout changed");
static_assert(sizeof(GeoIndexSection) == 24, "GeoIndexSection layout changed");
static_assert(sizeof(GeoIndexGroup) == 12, "GeoIndexGroup layout changed");
static_assert(sizeof(GeoDocument) == 24, "GeoDocument layout changed");
static_assert(sizeof(GeoHouse) == 12, "GeoHouse layout changed");

/**
 * @brief One dictionary inside the mapping: sorted words, reachable by rank or by text.
 */
typedef struct GeoDictionary {
  const GeoIndexGroup *groups; /**< Group table inside the mapping. */
  const uint32_t *offsets;     /**< Offset table inside the mapping. */
  const char *text;            /**< Word bytes inside the mapping. */
  size_t group_count;          /**< Entries in @c groups. */
  size_t word_count;           /**< Words the dictionary holds; @c offsets has one more. */
  size_t text_size;            /**< Bytes in @c text. */
  PrefixTree prefixes; /**< Leading bytes → group index, rebuilt on open. */
} GeoDictionary;

/**
 * @brief An opened index — a window onto the mapped file.
 *
 *  Every pointer refers into the mapping and stays valid until
 *  geo_index_close().  The trees are the only part built in memory.
 */
typedef struct GeoIndex {
  const uint8_t *base;   /**< Start of the mapping. */
  size_t size;           /**< Mapped bytes. */
  GeoDictionary words;   /**< Folded words — the search side. */
  GeoDictionary display; /**< Written spellings — the answer side. */
  const GeoDocument *documents; /**< One record per place, inside the mapping. */
  const uint16_t *importance;   /**< One weight per document, read while ranking. */
  size_t document_count;        /**< Entries in @c documents. */
  const uint64_t *posting_offsets; /**< words.word_count + 1 byte offsets into @c postings. */
  const char *postings;            /**< Serialized bitmaps, one per word. */
  size_t posting_bytes;            /**< Length of the bitmap blob. */
  size_t posting_count;            /**< Word-to-document connections held in it. */
  const GeoHouse *houses;          /**< House numbers, ordered by street. */
  const uint32_t *house_offsets;   /**< document_count + 1 entries into @c houses. */
  size_t house_count;              /**< Entries in @c houses. */
  const GeoIndexLanguage *languages; /**< Language table inside the mapping, or NULL. */
  size_t language_count;             /**< Entries in @c languages. */
  const GeoVariant *variants;        /**< Localized readings, or NULL. */
  size_t variant_count;              /**< Entries in @c variants. */
  uint64_t total_terms;            /**< Terms seen while building; reporting only. */
} GeoIndex;

/**
 * @brief Write dictionaries, documents and postings to @p path.
 *
 *  Lays out header, section table and sections in one sequential pass, then
 *  returns to the header to record the offsets it now knows.  An existing
 *  file is replaced.
 *
 *  @param[in] path         Destination file; the caller chooses the extension.
 *  @param[in] words        Folded words, merged and sorted.
 *  @param[in] display      Written spellings, merged and sorted.
 *  @param[in] documents    Joined documents and postings.
 *  @param[in] houses       Joined house numbers, ordered by street.
 *  @param[in] language_tags One tag per language @p documents counted; may be
 *                          NULL where there are none.
 *  @param[in] total_terms  Terms seen while building, kept for the report.
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER on a NULL argument,
 *          ARNM_ERROR_ARITHMETIC_OVERFLOW when a text exceeds 4 GiB, the
 *          localized readings outgrow what a uint32 addresses or @p documents
 *          counts more than @ref GEO_LANGUAGE_MAX languages,
 *          ARNM_ERROR_OUT_OF_MEMORY when an offset table did not fit, or
 *          ARNM_ERROR_ENCODE_FAILED when the file could not be written.
 *
 *  @whisper The work of minutes lies down in the order it will be read
 */
arnm_result geo_index_write(
    const char *path,
    const NameSet *words,
    const NameSet *display,
    const DocSet *documents,
    const HouseSet *houses,
    const char (*language_tags)[GEO_LANGUAGE_TAG_MAX],
    uint64_t total_terms
);

/**
 * @brief Map an index file and check that it may be trusted.
 *
 *  Verifies magic, version, byte order, layout hash, the recorded file size
 *  and every section's bounds before exposing anything.  Rebuilds both prefix
 *  trees from their group tables.
 *
 *  @param[out] index  Receives the opened index; zeroed on failure.
 *  @param[in]  path   File written by geo_index_write().
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER on a NULL argument,
 *          ARNM_ERROR_DECODE_FAILED when the file cannot be opened or mapped,
 *          ARNM_ERROR_INVALID_PARAM when the header does not fit this build, or
 *          ARNM_ERROR_OUT_OF_MEMORY when a tree could not be rebuilt.
 */
arnm_result geo_index_open(GeoIndex *index, const char *path);

/**
 * @brief Release the mapping and the trees.
 *
 *  Every pointer handed out before becomes invalid.  Safe to call with NULL.
 *
 *  @param[in,out] index  Index to close (NULL is a no-op).
 */
void geo_index_close(GeoIndex *index);

/**
 * @brief Borrow one word of a dictionary by its rank.
 *
 *  @param[in]  dictionary  Dictionary of an opened index; must not be NULL.
 *  @param[in]  rank        Position in [0, word_count) — words are in byte order.
 *  @param[out] out_size    Receives the word's byte length.
 *  @return Pointer into the mapping, not NUL-terminated; NULL if @p rank is
 *          out of range.
 */
const char *geo_dictionary_word(const GeoDictionary *dictionary, size_t rank, size_t *out_size);

/**
 * @brief The number of the language written @p tag.
 *
 *  Asked once per query rather than once per result: the tag is text and the
 *  number is what the variant table is ordered by.
 *
 *  @param[in] index  Opened index; must not be NULL.
 *  @param[in] tag    Lowercase tag, NUL-terminated — `en`, `fr`.  NULL or empty
 *                    yields -1, which is what a caller that named no language
 *                    passes on unchanged.
 *  @return The language's number, or -1 when this index holds no readings for it.
 */
int geo_index_language(const GeoIndex *index, const char *tag);

/**
 * @brief The reading @p language holds for @p document, or NULL where it holds none.
 *
 *  A binary search over the run of one language.  Most documents are not in it
 *  — a village names itself the same in every language — and NULL is the
 *  ordinary answer, not a failure.
 *
 *  @param[in] index     Opened index; must not be NULL.
 *  @param[in] language  Number from geo_index_language(); a negative one yields NULL.
 *  @param[in] document  Document number.
 *  @return The reading, valid as long as the mapping is, or NULL.
 *
 *  @whisper A place answers in the tongue it was asked in, where it knows one
 */
const GeoVariant *geo_index_variant(const GeoIndex *index, int language, uint32_t document);

/**
 * @brief Look a word up and learn its rank.
 *
 *  Two steps: the leading bytes choose a group through the tree, a binary
 *  search finds the word inside it.
 *
 *  @param[in]  dictionary  Dictionary of an opened index; must not be NULL.
 *  @param[in]  word        Text exactly as it was stored.
 *  @param[in]  size        Byte length of @p word.
 *  @param[out] out_rank    Receives the rank if the word exists; may be NULL.
 *  @return true if the word is present.
 *
 *  @whisper A word asks for its place, and the index answers without searching far
 */
bool geo_dictionary_find(
    const GeoDictionary *dictionary, const char *word, size_t size, size_t *out_rank
);

/**
 * @brief Open the set of documents a word names.
 *
 *  The bitmap is a view onto the mapping — nothing is copied, and it stays
 *  valid as long as the index is open.  Release it with roaring_bitmap_free();
 *  that frees the small header the view needs, never the mapped bytes.
 *
 *  @param[in] index  Opened index; must not be NULL.
 *  @param[in] rank   Word rank in the word dictionary.
 *  @return A read-only bitmap, or NULL when nothing carries the word or the
 *          stored bitmap does not fit its slice.
 *
 *  @whisper A word opens its hand and shows every place that answers to it
 */
const roaring_bitmap_t *geo_index_word_documents(const GeoIndex *index, size_t rank);

/** One place a query found. */
typedef struct GeoHit {
  uint32_t document;   /**< Index into the documents section. */
  uint32_t matched;    /**< Query words this document carries. */
  uint32_t house;      /**< The house number found on it, or GEO_RANK_NONE. */
  uint16_t importance; /**< The document's weight, copied for sorting. */
} GeoHit;

/**
 * Most results one query may ask for; a larger limit is answered with this many.
 *
 * The ranking weighs every candidate it carries at once, in one array on the
 * stack, so the ceiling is that array's size and not a matter of taste.  It is
 * named here rather than kept inside the query for two reasons: a caller has to
 * size its own array to it, and a second copy of the number living somewhere
 * else would be free to drift away from this one.
 */
enum {
  GEO_QUERY_LIMIT_MAX = 256 /**< Most results one query may be asked for. */
};

/**
 * @brief Borrow the house numbers standing on one document.
 *
 *  @param[in]  index      Opened index; must not be NULL.
 *  @param[in]  document   Document number.
 *  @param[out] out_count  Receives how many houses stand there.
 *  @return The first house, ordered by the rank of its number, or NULL.
 */
const GeoHouse *geo_index_houses(const GeoIndex *index, size_t document, size_t *out_count);

/**
 * @brief Answer a query with the places that carry all of its words.
 *
 *  The query walks the same folding as the index did, so *Superstr.* and
 *  *superstrasse* arrive as the same word.  Order is irrelevant: the words
 *  are sets, and the answer is where the sets meet — an intersection of
 *  bitmaps, which is cheap even when one of the words stands on millions of
 *  places.  Words the dictionary does not know are passed over rather than
 *  made to fail the whole query — a country name or a filler word should not
 *  silence an otherwise clear address.  If no word is known at all, nothing
 *  is found.
 *
 *  Three keys order the results.  First how far a place answers what the query
 *  said about *where*: a postcode it named weighs more than a town, and a place
 *  that answers neither scores nothing.  Then, among places that agree equally,
 *  the one carrying the house number that was asked for.  Weight decides only
 *  where the query described no place at all — as it always did.
 *
 *  A bare number of four digits or more is read as a postal code and narrows
 *  the answer like any other word.  A shorter one, or one carrying a letter, is
 *  held back as a house number and narrows nothing.  Where the code leaves
 *  nothing standing — a street filed under the neighbouring one, a digit
 *  mistyped, a long house number mistaken for a code — it is dropped and the
 *  words are asked alone; only if those find nothing either does every number
 *  take its turn as a word.
 *
 *  The candidates are gathered by weight before they are ordered, and only a
 *  bounded sample of them — a place too light to reach that sample cannot be
 *  lifted by anything the query says about it afterwards.  That is why the code
 *  narrows rather than merely ranks: it must shrink the field before weight
 *  cuts it, or the smallest street in the right town never arrives.
 *
 *  @param[in]     index      Opened index; must not be NULL.
 *  @param[in,out] tokenizer  Scratch space; reset by this call.
 *  @param[in]     query      Free text, words in any order.
 *  @param[in]     size       Byte length of @p query.
 *  @param[in]     prefix_last  Read the last word as a beginning as well, so
 *                            *Marienpl* finds *Marienplatz*.  What someone is
 *                            still typing wants this; a query they finished
 *                            does not, because a beginning always matches more
 *                            than the word itself.
 *  @param[out]    hits       Receives up to @p limit results.
 *  @param[in]     limit      Capacity of @p hits.  Anything above
 *                            @ref GEO_QUERY_LIMIT_MAX is answered with that
 *                            many — the ranking holds every candidate at once
 *                            and cannot be asked for more.
 *  @return Number of results written.
 *
 *  @whisper Words that were never spoken together find the one place where they belong
 */
size_t geo_index_query(
    const GeoIndex *index,
    TextTokenizer *tokenizer,
    const char *query,
    size_t size,
    bool prefix_last,
    GeoHit *hits,
    size_t limit
);

/** Everything a query carries beyond its words. */
typedef struct GeoQueryOptions {
  /** Read the last word as a beginning as well, for a query still being typed. */
  bool prefix_last;
  /** When set, @c latitude_e7 and @c longitude_e7 say where the searcher stands. */
  bool has_position;
  int32_t latitude_e7;  /**< Latitude × 10⁷. */
  int32_t longitude_e7; /**< Longitude × 10⁷. */
} GeoQueryOptions;

/**
 * @brief Answer a query from where the searcher stands, and say what it cost.
 *
 *  The plain search is geo_index_query(); this is the same call with room for a
 *  position and a place to write its counts.  The counts are gathered only
 *  where @p stats is not NULL, and the cardinalities that fill their sums are
 *  asked for nowhere else — a query passing NULL runs exactly as it did before.
 *
 *  ### What a position does
 *
 *  It narrows before it sorts.  The cells around the searcher are words like
 *  any other — see @ref geo_cell — so the ring of them intersects the query's
 *  own words while the candidates are still being gathered.  That is the only
 *  moment at which a light place standing nearby can survive: applied
 *  afterwards, a position would arrive to find the sample already filled by
 *  weight, and the nearest of nine thousand streets of one name is never among
 *  the heaviest of them.
 *
 *  Should nothing stand near the searcher at all — an ocean, a position in a
 *  country the dump does not cover — the position is let go of and the query
 *  asked again without it, before any other reading is tried.  A wrong position
 *  costs a place its order, never its presence.
 *
 *  In the ordering, nearness comes after what the query said outright: whoever
 *  names a town means that town, however far away it lies.  Beyond that,
 *  distance counts in coarse bands only, so that three streets closer never
 *  outweighs what a place is.
 *
 *  @param[in]     index      Opened index; must not be NULL.
 *  @param[in,out] tokenizer  Scratch space; reset by this call.
 *  @param[in]     query      Free text, words in any order.
 *  @param[in]     size       Byte length of @p query.
 *  @param[in]     options    Prefix reading and position; must not be NULL.
 *  @param[out]    hits       Receives up to @p limit results.
 *  @param[in]     limit      Capacity of @p hits, capped at
 *                            @ref GEO_QUERY_LIMIT_MAX.
 *  @param[out]    stats      Receives the counts, zeroed first even when the
 *                            arguments are refused; may be NULL.
 *  @return Number of results written.
 *
 *  @whisper What is asked for meets where it is asked from, and the ground answers first
 */
size_t geo_index_query_options(
    const GeoIndex *index,
    TextTokenizer *tokenizer,
    const char *query,
    size_t size,
    const GeoQueryOptions *options,
    GeoHit *hits,
    size_t limit,
    GeoQueryStats *stats
);

/** @} */

/** @defgroup doc_collector Document collector
 *  @ingroup search
 *  @brief What a query finds and what leads to it — one record per place,
 *         and one posting per word that names it.
 *
 *  A document is the smallest thing an answer can be: a street in its town, a
 *  town, a district.  House numbers are none of these; they are the payload of
 *  their street and join later.  Every non-house entry of the dump becomes
 *  exactly one document, because the dump already writes each street once per
 *  town it runs through.
 *
 *  Words reach documents through postings — pairs of `(word rank, document)`.
 *  Sorted by word, they turn into the lists a search intersects: everything
 *  named *berlin*, everything named *superstrasse*, and where the two meet.
 *
 *  ### Two passes, and why
 *
 *  A posting needs the word's rank, and a rank exists only once every word is
 *  known and sorted.  So the dump is walked twice: the first pass gathers the
 *  vocabulary, the second writes documents and postings against it.  The
 *  alternative — remembering every token instance to renumber it later — costs
 *  more memory than the second walk costs time.
 *
 *  Each thread collects into its own vectors and knows nothing of the others.
 *  Document numbers are handed out per thread and shifted into their final
 *  range when the threads join, which is a single addition per posting.
 *
 *  ### What the widths allow
 *
 *  A document number, a word rank and a posting are all `uint32_t`, here and in
 *  the file both, so the whole index tops out at 4 294 967 295 documents and the
 *  same number of distinct words.  A planet-wide dump uses a fraction of that.
 *  The bucket vectors below count in `uint32_t` as well and are per thread, so
 *  they run out later still.
 *
 *  @whisper Every place keeps its own record, and every word remembers where it was heard
 *  @{
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arnm/bucket_vector.h"
#include "arnm/result.h"

/** Marks a display field the entry did not carry. */
#define GEO_RANK_NONE UINT32_MAX

/**
 * @brief One place, as an answer needs it.
 *
 *  Coordinates are fixed point, degrees × 10⁷ — a resolution of roughly one
 *  centimetre, far below what a centroid means.  The display fields are ranks
 *  into the display dictionary, which keeps the original spelling; the search
 *  words are folded and live elsewhere.
 */
typedef struct GeoDocument {
  int32_t lat_e7;         /**< Latitude × 10⁷. */
  int32_t lon_e7;         /**< Longitude × 10⁷. */
  uint32_t name_rank;     /**< Own name, or GEO_RANK_NONE. */
  uint32_t city_rank;     /**< City as written, or GEO_RANK_NONE. */
  uint32_t postcode_rank; /**< Postal code as written, or GEO_RANK_NONE. */
  uint16_t importance;    /**< Photon's weight, scaled to 0 … 65535. */
  uint8_t type;           /**< A @c PhotonPlaceType. */
  uint8_t flags;          /**< Bit 0: a centroid was present. */
} GeoDocument;

/** Set when the document carries a usable coordinate. */
#define GEO_DOCUMENT_HAS_POINT 0x01u

/**
 * @brief One place as one language writes it, in the file.
 *
 *  A side table beside the documents, and a sparse one: a place appears here
 *  once per language that names it differently, and the overwhelming majority
 *  of places — a field path in Brandenburg, a cul-de-sac in Ohio — appear not
 *  at all.  That sparseness is the whole reason this is a table of its own
 *  rather than three more fields in every document record.
 *
 *  The ranks point into the same display dictionary the documents use, so a
 *  spelling shared between languages costs one entry, not one per language.
 */
typedef struct GeoVariant {
  uint32_t document;  /**< Which place, after merging. */
  uint32_t name_rank; /**< Its name in this language, or GEO_RANK_NONE. */
  uint32_t city_rank; /**< Its town in this language, or GEO_RANK_NONE. */
} GeoVariant;

/**
 * @brief One localized reading as a thread collected it, before renumbering.
 *
 *  @c record is the thread's own document number.  Segments merge and documents
 *  are renumbered when the threads join, and a reading has to travel with its
 *  record through both — which is why this is not a @ref GeoVariant yet.
 */
typedef struct GeoVariantRecord {
  uint32_t record;    /**< Thread-local document number. */
  uint32_t name_rank; /**< Display rank, or GEO_RANK_NONE. */
  uint32_t city_rank; /**< Display rank, or GEO_RANK_NONE. */
  uint32_t language;  /**< Index into the build's language list. */
} GeoVariantRecord;

/* A bucket vector holds at most @ref ARNM_BVEC_MAX_INDEX_CAPACITY buckets, so the exponent
 * alone decides how much *one thread* can gather, and 15 is the largest exponent arnm
 * takes.  @ref GEO_VEC_CEILING is therefore the ceiling of any of these vectors, whatever
 * is written below.
 *
 * What a planet dump asks for, measured on the 2026 master dump with 32 languages:
 * 64.1 M segments, 1.88 G word postings before the merge — 29.4 per segment, because the
 * dump repeats the whole address chain on every entry and every language multiplies it.
 * Divided by the parser threads, and the postings are what runs out first: six threads
 * ask 314 M of @ref GEO_WORD_VEC_BUCKET_LOG2 and are refused, eight ask 235 M and fit.
 *
 * So all four sit at the ceiling now.  Only the bucket size differs, and a partly filled
 * last bucket per thread is all it costs — under a megabyte against a build that holds
 * gigabytes.  There is nothing left to raise here: a dump that outgrows this needs more
 * threads, or a bucket vector that counts its buckets in more than a uint16.
 * @see doc_collector_limit(), which names the vector that refused. */

/** Documents of one thread — 32768 per bucket, 768 KiB; ceiling 268 M records. */
#define GEO_DOCUMENT_VEC_BUCKET_LOG2 15

/** Localized readings of one thread — 32768 per bucket, 512 KiB; ceiling 268 M readings. */
#define GEO_VARIANT_VEC_BUCKET_LOG2 15

/** Word ranks of one thread — 32768 per bucket, 128 KiB; ceiling 268 M postings. */
#define GEO_WORD_VEC_BUCKET_LOG2 15

/** Where each document's words begin — 32768 per bucket, 128 KiB; ceiling 268 M documents. */
#define GEO_START_VEC_BUCKET_LOG2 15

/** Elements one of these vectors can ever hold.
 *
 *  Not quite the bucket cap times the bucket size: the index array grows a fixed number
 *  of slots at a time and stops at the last step that still fits under the cap, so the
 *  buckets a vector really reaches are the cap rounded down to that step — 8184 of them
 *  rather than 8191, and 268 173 312 elements rather than 268 402 688.  Measured against
 *  a real collector, which refuses at exactly this number. */
#define GEO_VEC_CEILING                                                                            \
  ((((size_t)ARNM_BVEC_MAX_INDEX_CAPACITY / ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE) *              \
    ARNM_BVEC_DEFAULT_INDEX_GROW_STEP_SIZE)                                                        \
   << 15)

/** Documents of one thread. */
ARNM_BVEC_DEFINE(geo_document_vec, GeoDocument)

/** Localized readings of one thread. */
ARNM_BVEC_DEFINE(geo_variant_vec, GeoVariantRecord)

/** Word ranks of one thread. */
ARNM_BVEC_DEFINE(geo_word_vec, uint32_t)

/** Where each document's words begin — one entry per document. */
ARNM_BVEC_DEFINE(geo_start_vec, uint32_t)

/** Words of one document compared against each other before anything is stored. */
#define POSTING_RUN_MAX 64

/**
 * @brief One house number on a street.
 *
 *  The number itself is a rank in the display dictionary — house numbers repeat
 *  across the planet beyond counting, and a few hundred thousand distinct
 *  spellings carry all three hundred million of them.
 *
 *  The position stands on its own, in the same fixed point as everything else.
 *  It was once a step away from the street's centre, which fit into four bytes
 *  instead of eight — but a street is a line and its centre only a point, and
 *  every house beyond a few kilometres of that point had to be pulled back to
 *  where the number range ended.  A door either has a place or it has none;
 *  four bytes are the wrong thing to save it on.
 */
typedef struct GeoHouse {
  uint32_t number_rank; /**< The number as written, in the display dictionary. */
  int32_t lat_e7;       /**< Latitude × 10⁷. */
  int32_t lon_e7;       /**< Longitude × 10⁷. */
} GeoHouse;

/** A street as a house refers to it, and the document it became. */
typedef struct GeoStreetKey {
  uint32_t name;     /**< Street name in the display dictionary. */
  uint32_t city;     /**< Town, likewise; GEO_RANK_NONE when absent. */
  uint32_t postcode; /**< Postal code, likewise. */
  uint32_t document; /**< Where the street ended up after merging. */
} GeoStreetKey;

/**
 * @brief The vector that ran out of buckets, and what it held when it did.
 *
 *  A refusal comes back as @c ARNM_ERROR_ARITHMETIC_OVERFLOW, which on its own says
 *  only that some counter would not stretch.  This says which vector it was and how
 *  far it got — the difference between a number to look up and a decision to make.
 */
typedef struct CollectorLimit {
  const char *vector; /**< Name of the vector, or NULL while none has refused. */
  size_t held;        /**< Elements it held at the refusal. */
  size_t ceiling;     /**< Elements it can ever hold. */
} CollectorLimit;

/**
 * @brief One thread's harvest of the second pass.
 *
 *  Documents are numbered from 0 upward in the order the thread meets them.
 *
 *  The words are kept the way they arrive: the entry of one document lies
 *  behind the entry of the one before it, so a word needs to record only
 *  *which* word it is — **which document** follows from where it lies.  That
 *  halves what a connection costs while it is being gathered, and it is by far
 *  the largest thing in memory when the threads are joined.
 */
typedef struct DocCollector {
  arnm_bvec documents;            /**< One record per place this thread met. */
  arnm_bvec variants;             /**< Localized readings, pointing back at those records. */
  arnm_bvec words;                /**< Word ranks, grouped by document. */
  arnm_bvec starts;               /**< First word of each document. */
  uint64_t dropped_words;         /**< Tokens the dictionary did not know — 0 in a sound build. */
  uint64_t dropped_doubles;       /**< Repetitions of a word within one document. */
  uint32_t seen[POSTING_RUN_MAX]; /**< Words of the open document so far. */
  size_t seen_count;              /**< How many of @c seen are filled. */
  CollectorLimit limit;           /**< The first vector to run out, if one did. */
} DocCollector;

/**
 * @brief Which vector ran out of buckets, if one did.
 *
 *  Only the first is kept: what fills after it is a consequence, and the first one is
 *  the one to size the build by.
 *
 *  @param[in]  collector  Collector to ask; NULL answers false.
 *  @param[out] out        Receives the vector, what it held and what it holds; may be NULL.
 *  @return true when a vector refused — which is exactly when a collect call answered
 *          @c ARNM_ERROR_ARITHMETIC_OVERFLOW.
 */
bool doc_collector_limit(const DocCollector *collector, CollectorLimit *out);

/**
 * @brief Prepare an empty collector.
 *
 *  Reserves nothing.  The vectors are set to their empty state and the first
 *  document opens the first bucket, so there is no half-built collector to unwind:
 *  a failure here has taken nothing, and doc_collector_free() on an untouched
 *  collector is a no-op.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @return ARNM_SUCCESS, or ARNM_ERROR_NULL_POINTER when @p collector is
 *          NULL — the only way this can fail.
 */
arnm_result doc_collector_init(DocCollector *collector);

/** @brief Release the vectors. Safe to call with NULL. */
void doc_collector_free(DocCollector *collector);

/**
 * @brief Open a new document and return its thread-local number.
 *
 *  Every word noted afterwards belongs to this document, until the next one
 *  opens.
 *
 *  @param[in,out] collector  Collector receiving the document.
 *  @param[in]     document   Record to store; copied.
 *  @param[out]    out_number Receives the thread-local document number.
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER, or ARNM_ERROR_OUT_OF_MEMORY.
 */
arnm_result doc_collector_add_document(
    DocCollector *collector, const GeoDocument *document, uint32_t *out_number
);

/**
 * @brief Note that @p word names the document opened last.
 *
 *  A word already noted for this document is dropped here rather than stored
 *  and removed later — the dirty data offers the same text as city, as state
 *  and as street, and a planet's worth of those doubles is gigabytes.  A
 *  glance at the last @ref POSTING_RUN_MAX words of the open document settles
 *  it.
 *
 *  @param[in,out] collector  Collector receiving the word.
 *  @param[in]     word       Rank in the word dictionary.
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER, or ARNM_ERROR_OUT_OF_MEMORY.
 */
arnm_result doc_collector_add_posting(DocCollector *collector, uint32_t word);

/**
 * @brief Note how @p language writes the document opened last.
 *
 *  Nothing is stored where the language adds neither a name nor a town: a
 *  reading that repeats the default one is not a reading, and a planet's worth
 *  of those would fill the side table with copies.
 *
 *  @param[in,out] collector  Collector receiving the reading.
 *  @param[in]     language   Index into the build's language list.
 *  @param[in]     name_rank  Display rank of the name, or GEO_RANK_NONE.
 *  @param[in]     city_rank  Display rank of the town, or GEO_RANK_NONE.
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER, or ARNM_ERROR_OUT_OF_MEMORY.
 */
arnm_result doc_collector_add_variant(
    DocCollector *collector, uint32_t language, uint32_t name_rank, uint32_t city_rank
);

/** @brief Documents collected so far. */
size_t doc_collector_document_count(const DocCollector *collector);

/** @brief Word-to-document connections collected so far. */
size_t doc_collector_posting_count(const DocCollector *collector);

/**
 * @brief The joined result: all documents in one array, postings by word.
 *
 *  @c postings holds document numbers, ascending within each word.
 *  @c posting_offsets has @c word_count + 1 entries; word @c w occupies
 *  `[posting_offsets[w], posting_offsets[w + 1])`.  A word nobody used has an
 *  empty range, which costs one number and keeps the lookup a single index.
 */
typedef struct DocSet {
  GeoDocument *documents;     /**< Every document of every thread, renumbered into one range. */
  size_t document_count;      /**< Entries in @c documents. */
  size_t segment_count;       /**< Records before segments were merged. */
  uint32_t *postings;         /**< Document numbers, grouped by the word that names them. */
  size_t posting_count;       /**< Entries in @c postings. */
  uint32_t *posting_offsets;  /**< Where each word's group begins; @c word_count + 1 of them. */
  size_t word_count;          /**< Words the offsets cover. */
  GeoStreetKey *streets;      /**< Street keys, ascending, for the houses to find. */
  size_t street_count;        /**< Entries in @c streets. */
  GeoVariant *variants;       /**< Localized readings, ascending by language then document. */
  size_t variant_count;       /**< Entries in @c variants. */
  uint32_t *language_offsets; /**< Where each language's run begins; @c language_count + 1. */
  size_t language_count;      /**< Languages the offsets cover. */
} DocSet;

/**
 * @brief Find the document a house's street became.
 *
 *  Four attempts, each letting go of something the dump is unreliable about:
 *
 *  1. name, town and postal code — the way it should be;
 *  2. name and town, the street carrying no code, because the dump often gives
 *     the code to the house and withholds it from the street;
 *  3. name and town, any code, because a street running through several codes
 *     carries one per segment and the house may name a third;
 *  4. name alone, nearest to the house, because house and street disagree about
 *     which town they are in — a house on the edge names its postal town, the
 *     street the municipality it was drawn in.  The coordinate does not
 *     disagree, so it decides.
 *
 *  @param[in]  set          Joined set holding the street keys.
 *  @param[in]  name         Street name rank; GEO_RANK_NONE never matches.
 *  @param[in]  city         Town rank, or GEO_RANK_NONE.
 *  @param[in]  postcode     Postal code rank, or GEO_RANK_NONE.
 *  @param[in]  lat_e7       Where the house stands, degrees × 10⁷; read by the fourth
 *                           attempt alone, and only when @p has_point.
 *  @param[in]  lon_e7       The same, east.
 *  @param[in]  has_point    False when the house brought no coordinate, which rules the
 *                           fourth attempt out and leaves the house where it is.
 *  @param[out] out_relaxed  Set when an attempt beyond the first answered, so the caller
 *                           can count how much of the dump needed forgiving.  May be NULL.
 *  @return The document number, or GEO_RANK_NONE when no street answers.
 *
 *  @whisper A house calls out its street's name and waits to hear where it stands
 */
uint32_t doc_set_find_street(
    const DocSet *set,
    uint32_t name,
    uint32_t city,
    uint32_t postcode,
    int32_t lat_e7,
    int32_t lon_e7,
    int has_point,
    int *out_relaxed
);

/** Streets of one name examined at most, when only the distance can decide. */
#define STREET_NEAREST_MAX 1024

/** 0.02° ≈ 2 km — further than this, a street of the same name is another street. */
#define STREET_NEAREST_E7 200000

/**
 * @brief Join the threads' harvests into one set, segments merged.
 *
 *  ### Why segments have to merge
 *
 *  The dump does not write a street once per town — it writes every OSM way,
 *  and a long street is cut into many.  *Karl-Marx-Allee* arrives as a dozen
 *  records with the same name, the same town and the same postal code, each
 *  holding the centre of its own piece.  Left alone they fill a result list
 *  with one and the same place, they cannot be told apart by weight, and a
 *  house number would have no single street to attach itself to.
 *
 *  So records agreeing in name, town, postal code and kind become one
 *  document, and their centres average into the middle of the whole street.
 *  Records without a name never merge — namelessness is not a thing they have
 *  in common.
 *
 *  ### How the postings survive it
 *
 *  Documents are walked in their new order, and each merged document gathers
 *  the words of all its segments; a word already gathered for it is passed
 *  over.  Because the walk is ascending, every word's list comes out ascending
 *  and free of doubles without a single comparison — first a pass to count,
 *  then a pass to place.
 *
 *  @param[out] out              Receives the joined set; zeroed on failure.
 *  @param[in]  collectors       Array of @p collector_count collector pointers.
 *  @param[in]  collector_count  Number of collectors; 0 yields an empty set, and 64 is
 *                               the most that may be joined at once.
 *  @param[in]  word_count       Words in the dictionary the postings refer to.
 *  @param[in]  language_count   The whole language list of the build, the default
 *                               one at place 0 included; a reading numbered at or
 *                               above it belongs to no language of this build and is
 *                               passed over.  0 leaves the variant table empty and
 *                               costs nothing.
 *  @retval ARNM_SUCCESS            The set is joined and owns its arrays.
 *  @retval ARNM_ERROR_NULL_POINTER @p out is NULL, or a collector pointer is.
 *  @retval ARNM_ERROR_INVALID_PARAM @p collector_count is above 64 — one per parser
 *                                  thread, and there are never that many.
 *  @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The collectors hold more than UINT32_MAX
 *                                  documents between them, which no document number
 *                                  could address.
 *  @retval ARNM_ERROR_OUT_OF_MEMORY The arrays could not be taken.
 *
 *  @whisper Many voices name the same places, and the names are gathered under one roof
 */
arnm_result doc_collector_merge(
    DocSet *out,
    DocCollector *const *collectors,
    size_t collector_count,
    size_t word_count,
    size_t language_count
);

/** @brief Release the joined arrays. Safe to call with NULL. */
void doc_set_free(DocSet *set);

/** @} */

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

#include <stddef.h>
#include <stdint.h>

#include "hostmem/result.h"
#include "hostmem/bucket_vector.h"

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

/** Documents of one thread — 512 per bucket, 12 KiB of contiguous records. */
HOSTMEM_BVEC_DECLARE(geo_document_vec, GeoDocument, 9, extern)

/** Word ranks of one thread — 4096 per bucket, 16 KiB. */
HOSTMEM_BVEC_DECLARE(geo_word_vec, uint32_t, 12, extern)

/** Where each document's words begin — one entry per document. */
HOSTMEM_BVEC_DECLARE(geo_start_vec, uint32_t, 12, extern)

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
  geo_document_vec documents;     /**< One record per place this thread met. */
  geo_word_vec words;             /**< Word ranks, grouped by document. */
  geo_start_vec starts;           /**< First word of each document. */
  uint64_t dropped_words;         /**< Tokens the dictionary did not know — 0 in a sound build. */
  uint64_t dropped_doubles;       /**< Repetitions of a word within one document. */
  uint32_t seen[POSTING_RUN_MAX]; /**< Words of the open document so far. */
  size_t seen_count;              /**< How many of @c seen are filled. */
} DocCollector;

/**
 * @brief Prepare an empty collector.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @return HOSTMEM_SUCCESS or HOSTMEM_ERROR_NULL_POINTER.
 */
hostmem_result doc_collector_init(DocCollector *collector);

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
 *  @return HOSTMEM_SUCCESS, HOSTMEM_ERROR_NULL_POINTER, or HOSTMEM_ERROR_OUT_OF_MEMORY.
 */
hostmem_result doc_collector_add_document(
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
 *  @return HOSTMEM_SUCCESS, HOSTMEM_ERROR_NULL_POINTER, or HOSTMEM_ERROR_OUT_OF_MEMORY.
 */
hostmem_result doc_collector_add_posting(DocCollector *collector, uint32_t word);

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
  GeoDocument *documents;    /**< Every document of every thread, renumbered into one range. */
  size_t document_count;     /**< Entries in @c documents. */
  size_t segment_count;      /**< Records before segments were merged. */
  uint32_t *postings;        /**< Document numbers, grouped by the word that names them. */
  size_t posting_count;      /**< Entries in @c postings. */
  uint32_t *posting_offsets; /**< Where each word's group begins; @c word_count + 1 of them. */
  size_t word_count;         /**< Words the offsets cover. */
  GeoStreetKey *streets;     /**< Street keys, ascending, for the houses to find. */
  size_t street_count;       /**< Entries in @c streets. */
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
 *  @param[in]  collector_count  Number of collectors; 0 yields an empty set.
 *  @param[in]  word_count       Words in the dictionary the postings refer to.
 *  @return HOSTMEM_SUCCESS, HOSTMEM_ERROR_NULL_POINTER on a NULL argument, or
 *          HOSTMEM_ERROR_OUT_OF_MEMORY when the arrays could not be taken.
 *
 *  @whisper Many voices name the same places, and the names are gathered under one roof
 */
hostmem_result doc_collector_merge(
    DocSet *out, DocCollector *const *collectors, size_t collector_count, size_t word_count
);

/** @brief Release the joined arrays. Safe to call with NULL. */
void doc_set_free(DocSet *set);

/** @} */

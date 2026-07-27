/** @defgroup doc_collector Document collector
 *  @ingroup data
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
 *  @whisper Every place keeps its own record, and every word remembers where it was heard
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gradido_blockchain_core/result.h"
#include "gradido_blockchain_core/utils/bucket_vector.h"

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

/** One word, once, in one document. */
typedef struct GeoPosting {
  uint32_t word;     /**< Rank in the word dictionary. */
  uint32_t document; /**< Document number, thread-local until the join. */
} GeoPosting;

/** Documents of one thread — 512 per bucket, 12 KiB of contiguous records. */
GRDU_BVEC_DECLARE(geo_document_vec, GeoDocument, 9, extern)

/** Postings of one thread — 4096 per bucket, 32 KiB. */
GRDU_BVEC_DECLARE(geo_posting_vec, GeoPosting, 12, extern)

/**
 * @brief One thread's harvest of the second pass.
 *
 *  Documents are numbered from 0 upward in the order the thread meets them;
 *  postings refer to those numbers until doc_collector_merge() shifts them.
 */
/** Words of one document compared against each other before anything is stored. */
#define POSTING_RUN_MAX 64

typedef struct DocCollector {
  geo_document_vec documents;
  geo_posting_vec postings;
  uint64_t dropped_words;  /**< Tokens the dictionary did not know — 0 in a sound build. */
  uint64_t dropped_doubles;/**< Repetitions of a word within one document. */
  uint32_t open_document;  /**< Document the words currently belong to. */
  uint32_t seen[POSTING_RUN_MAX]; /**< Its words so far. */
  size_t seen_count;
} DocCollector;

/**
 * @brief Prepare an empty collector.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @return GRD_SUCCESS or GRD_ERROR_NULL_POINTER.
 */
grd_result doc_collector_init(DocCollector *collector);

/** @brief Release both vectors. Safe to call with NULL. */
void doc_collector_free(DocCollector *collector);

/**
 * @brief Open a new document and return its thread-local number.
 *
 *  @param[in,out] collector  Collector receiving the document.
 *  @param[in]     document   Record to store; copied.
 *  @param[out]    out_number Receives the thread-local document number.
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER, or GRD_ERROR_OUT_OF_MEMORY.
 */
grd_result doc_collector_add_document(
    DocCollector *collector, const GeoDocument *document, uint32_t *out_number
);

/**
 * @brief Note that @p word names document @p number.
 *
 *  A word already noted for this document is dropped here rather than stored
 *  and removed later — the dirty data offers the same text as city, as state
 *  and as street, and a planet's worth of those doubles is gigabytes.  All
 *  postings of one document arrive together, so a glance at the last
 *  @ref POSTING_RUN_MAX words of the open document settles it.
 *
 *  @param[in,out] collector  Collector receiving the posting.
 *  @param[in]     word       Rank in the word dictionary.
 *  @param[in]     number     Thread-local document number.
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER, or GRD_ERROR_OUT_OF_MEMORY.
 */
grd_result doc_collector_add_posting(DocCollector *collector, uint32_t word, uint32_t number);

/** @brief Documents collected so far. */
size_t doc_collector_document_count(const DocCollector *collector);

/** @brief Postings collected so far, duplicates included. */
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
  GeoDocument *documents;
  size_t document_count;
  size_t segment_count; /**< Records before segments were merged. */
  uint32_t *postings;
  size_t posting_count;
  uint32_t *posting_offsets;
  size_t word_count;
} DocSet;

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
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER on a NULL argument, or
 *          GRD_ERROR_OUT_OF_MEMORY when the arrays could not be taken.
 *
 *  @whisper Many voices name the same places, and the names are gathered under one roof
 */
grd_result doc_collector_merge(
    DocSet *out, DocCollector *const *collectors, size_t collector_count, size_t word_count
);

/** @brief Release the joined arrays. Safe to call with NULL. */
void doc_set_free(DocSet *set);

/** @} */

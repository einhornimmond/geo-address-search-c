/** @defgroup house_collector House collector
 *  @ingroup search
 *  @brief The house numbers of the world, gathered onto the streets they
 *         belong to.
 *
 *  Houses are the larger half of the dump — two of every three entries — and
 *  they are not places one searches for by name.  Nobody types *12A*; one
 *  types a street and a number, and the number is resolved inside the street.
 *  So a house carries no words and no document of its own: it is payload,
 *  hanging on the street it names.
 *
 *  ### Why they come last
 *
 *  A house can only find its street once the streets are documents with
 *  numbers, and that happens when the second pass has been joined.  Rather
 *  than hold three hundred million houses in memory until then, the dump is
 *  walked a third time — the reading costs minutes, the holding would cost
 *  gigabytes, and only one of the two grows with the data.
 *
 *  Each thread gathers what it meets; the join then orders the houses by their
 *  street and hands out, for every document, the stretch of houses that stands
 *  on it.
 *
 *  @whisper Every number finds the street it was always standing on
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "arnm/result.h"
#include "arnm/bucket_vector.h"
#include "search/doc_collector.h"

/** A house as it is gathered, still knowing which street it named. */
typedef struct HouseEntry {
  uint32_t document; /**< The street's document. */
  GeoHouse house;    /**< Number and position relative to that street. */
} HouseEntry;

/** Elements per bucket of a house vector, as a power of two — 32768 per bucket, 512 KiB.
 *
 *  A bucket vector holds at most @ref ARNM_BVEC_MAX_INDEX_CAPACITY buckets, so the exponent
 *  is what decides the ceiling: 32768 × 8191 ≈ 268 M houses per thread.  The planet carries
 *  249 M of them in total, which one thread alone would only just hold. */
#define HOUSE_VEC_BUCKET_LOG2 15

ARNM_BVEC_DEFINE(house_vec, HouseEntry)

/** One thread's harvest of the third pass. */
typedef struct HouseCollector {
  arnm_bvec houses;            /**< What this thread gathered, in the order it met them. */
  uint64_t homeless;           /**< Houses whose street the index never learned. */
  uint64_t pointless;          /**< Houses that brought no coordinate and took their street's. */
  uint64_t without_number;     /**< Entries carrying no house number at all. */
  uint64_t unknown_street;     /**< Street text the display dictionary never saw. */
  uint64_t unknown_key;        /**< Street known by name, but not under this town and code. */
  uint64_t recovered_city;     /**< Found only after dropping the code from the key. */
  uint64_t recovered_postcode; /**< Found only by name and town, any code. */
  uint64_t recovered_nearest;  /**< Found only by standing closest. */
} HouseCollector;

/**
 * @brief Prepare an empty collector.
 *
 *  Reserves nothing.  The vectors are set to their empty state and the first
 *  house opens the first bucket, so there is no half-built collector to unwind:
 *  a failure here has taken nothing, and house_collector_free() on an untouched
 *  collector is a no-op.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @return ARNM_SUCCESS, or ARNM_ERROR_NULL_POINTER when @p collector is
 *          NULL — the only way this can fail.
 */
arnm_result house_collector_init(HouseCollector *collector);

/** @brief Release the vector. Safe to call with NULL. */
void house_collector_free(HouseCollector *collector);

/**
 * @brief Hang one house on its street.
 *
 *  A house without a coordinate of its own is placed where its street stands;
 *  everything else keeps the point the dump gave it.
 *
 *  @param[in,out] collector    Collector receiving the house.
 *  @param[in]     document     The street's document number.
 *  @param[in]     street       That street's record, for its centre.
 *  @param[in]     number_rank  The house number in the display dictionary.
 *  @param[in]     lat_e7       The house's own latitude, or 0 when it has none.
 *  @param[in]     lon_e7       Its longitude.
 *  @param[in]     has_point    Whether the house brought a coordinate at all.
 *  @return ARNM_SUCCESS, ARNM_ERROR_NULL_POINTER, or ARNM_ERROR_OUT_OF_MEMORY.
 */
arnm_result house_collector_add(
    HouseCollector *collector,
    uint32_t document,
    const GeoDocument *street,
    uint32_t number_rank,
    int32_t lat_e7,
    int32_t lon_e7,
    int has_point
);

/** @brief Houses gathered so far. */
size_t house_collector_count(const HouseCollector *collector);

/**
 * @brief The joined houses: ordered by street, and by number within it.
 *
 *  @c offsets has one entry per document plus one; the houses of document @c d
 *  occupy `[offsets[d], offsets[d + 1])`.  A street without houses has an empty
 *  stretch, which costs one number and keeps the lookup a single index.
 */
typedef struct HouseSet {
  GeoHouse *houses;            /**< Every house of every thread, ordered by street. */
  size_t house_count;          /**< Entries in @c houses. */
  uint32_t *offsets;           /**< Where each document's houses begin; @c document_count + 1. */
  size_t document_count;       /**< Documents the offsets cover. */
  uint64_t homeless;           /**< Houses whose street the index never learned. */
  uint64_t pointless;          /**< Houses with no coordinate; they took their street's. */
  uint64_t without_number;     /**< Entries that named a street but no number. */
  uint64_t unknown_street;     /**< Street names the dictionary did not carry. */
  uint64_t unknown_key;        /**< Street, town and postcode known, the combination not. */
  uint64_t recovered_city;     /**< Placed after the postcode was let go of. */
  uint64_t recovered_postcode; /**< Placed after the town was let go of. */
  uint64_t recovered_nearest;  /**< Placed by nearness, with neither one matching. */
} HouseSet;

/**
 * @brief Join the threads' houses and order them by street.
 *
 *  A counting sort places every house under its street — one pass to count,
 *  one to fill.  Within a street the houses are then sorted by their number's
 *  rank, which makes looking one up a binary search over a handful of entries.
 *
 *  @param[out] out              Receives the joined houses; zeroed on failure.
 *  @param[in]  collectors       Array of @p collector_count collector pointers.
 *  @param[in]  collector_count  Number of collectors.
 *  @param[in]  document_count   Documents the offsets must cover.
 *  @retval ARNM_SUCCESS            The houses are joined and ordered by street.
 *  @retval ARNM_ERROR_NULL_POINTER @p out is NULL, or a collector pointer is.
 *  @retval ARNM_ERROR_ARITHMETIC_OVERFLOW The collectors hold more than UINT32_MAX
 *                                  houses between them, which the offsets could not
 *                                  index.
 *  @retval ARNM_ERROR_OUT_OF_MEMORY The offsets or the arrays could not be taken.
 *
 *  @whisper The numbers line up along their street, and the street knows where they start
 */
arnm_result house_collector_merge(
    HouseSet *out, HouseCollector *const *collectors, size_t collector_count, size_t document_count
);

/** @brief Release the joined arrays. Safe to call with NULL. */
void house_set_free(HouseSet *set);

/** @} */

/** @defgroup house_collector House collector
 *  @ingroup data
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

#include "doc_collector.h"
#include "gradido_blockchain_core/result.h"
#include "gradido_blockchain_core/utils/bucket_vector.h"

/** A house as it is gathered, still knowing which street it named. */
typedef struct HouseEntry {
  uint32_t document; /**< The street's document. */
  GeoHouse house;    /**< Number and position relative to that street. */
} HouseEntry;

/** Houses of one thread — 2048 per bucket, 24 KiB. */
GRDU_BVEC_DECLARE(house_vec, HouseEntry, 11, extern)

/** One thread's harvest of the third pass. */
typedef struct HouseCollector {
  house_vec houses;
  uint64_t homeless;       /**< Houses whose street the index never learned. */
  uint64_t pointless;      /**< Houses that brought no coordinate and took their street's. */
  uint64_t without_number; /**< Entries carrying no house number at all. */
  uint64_t unknown_street; /**< Street text the display dictionary never saw. */
  uint64_t unknown_key;    /**< Street known by name, but not under this town and code. */
  uint64_t recovered_city;     /**< Found only after dropping the code from the key. */
  uint64_t recovered_postcode; /**< Found only by name and town, any code. */
  uint64_t recovered_nearest; /**< Found only by standing closest. */
} HouseCollector;

/**
 * @brief Prepare an empty collector.
 *
 *  @param[in,out] collector  Collector to initialise; must not be NULL.
 *  @return GRD_SUCCESS or GRD_ERROR_NULL_POINTER.
 */
grd_result house_collector_init(HouseCollector *collector);

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
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER, or GRD_ERROR_OUT_OF_MEMORY.
 */
grd_result house_collector_add(
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
  GeoHouse *houses;
  size_t house_count;
  uint32_t *offsets;
  size_t document_count;
  uint64_t homeless;
  uint64_t pointless;
  uint64_t without_number;
  uint64_t unknown_street;
  uint64_t unknown_key;
  uint64_t recovered_city;
  uint64_t recovered_postcode;
  uint64_t recovered_nearest;
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
 *  @return GRD_SUCCESS, GRD_ERROR_NULL_POINTER on a NULL argument, or
 *          GRD_ERROR_OUT_OF_MEMORY when the arrays could not be taken.
 *
 *  @whisper The numbers line up along their street, and the street knows where they start
 */
grd_result house_collector_merge(
    HouseSet *out, HouseCollector *const *collectors, size_t collector_count,
    size_t document_count
);

/** @brief Release the joined arrays. Safe to call with NULL. */
void house_set_free(HouseSet *set);

/** @} */

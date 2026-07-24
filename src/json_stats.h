/** @defgroup data Data structures
  *  @brief The shapes that carry meaning — containers, queues, and the census of addresses.
  *
  *  Parent group for every module that defines or processes the program's
  *  internal data representations.
  */

/** @defgroup json_stats JSON statistics
  *  @ingroup data
  *  @brief Counting the shapes inside a Photon dump — how many countries,
  *         streets, and houses pass through the stream.
  *  @{
  */

#pragma once

#include <stdint.h>

#include <yyjson.h>

/**
 * @brief A census of the address hierarchy found in the stream.
 *
 *  Each field counts one leaf or branch of the geographic tree:
 *  records → place objects → content entries → typed address components.
 *  The counters grow monotonically as `json_stats_record` is called.
 */
typedef struct JsonStats {
    uint64_t records;           /**< Total JSON documents seen. */
    uint64_t place_records;     /**< Documents of type "Place". */
    uint64_t place_entries;     /**< Individual entries inside Place content arrays. */
    uint64_t countries;         /**< Entries tagged "country". */
    uint64_t states;            /**< Entries tagged "state". */
    uint64_t counties;          /**< Entries tagged "county". */
    uint64_t cities;            /**< Entries tagged "city". */
    uint64_t streets;           /**< Entries tagged "street". */
    uint64_t houses;            /**< Entries tagged "house". */
    uint64_t other;             /**< Entries with unrecognised or missing address_type. */
    uint64_t invalid_records;   /**< Malformed entries skipped during scan. */
} JsonStats;

/**
 * @brief Fold one Photon JSON document into the running census.
 *
 *  Reads the top-level JSON value, identifies Place documents,
 *  and counts every content entry by its address_type. Also verifies
 *  that the dataset carries postcode information — aborting if the
 *  field is absent after a threshold of scanned entries.
 *
 *  @param[in,out] stats   Running counts, updated in place.
 *  @param[in]     root    Root value of a parsed JSON document.
 *
 *  @whisper Each record passes — the census grows by one
 */
void json_stats_record(JsonStats* stats, yyjson_val* root);

/**
 * @brief Merge a thread-local tally into the grand total.
 *
 *  Adds every counter from @p addend into @p total so a single
 *  summary can speak for many parallel parsers.
 *
 *  @param[in,out] total   Accumulator receiving the sum.
 *  @param[in]     addend  Partial counts to fold in.
 */
void json_stats_add(JsonStats* total, const JsonStats* addend);

/**
 * @brief Print the collected census to stdout.
 *
 *  Renders a compact table of all address-type counts and the
 *  overall document / entry totals.
 *
 *  @param[in] stats   Counts to display.
 */
void json_stats_print(const JsonStats* stats);

/** @} */

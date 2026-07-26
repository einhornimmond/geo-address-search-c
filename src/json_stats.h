/** @defgroup json_stats JSON statistics
 *  @ingroup data
 *  @brief Counting the shapes inside a Photon dump — how many countries,
 *         streets, and houses pass through the stream.
 *  @{
 */

#pragma once

#include <stdint.h>

/** Forward declaration — the full type is in json_parse.h. */
typedef struct PhotonPlace PhotonPlace;

/**
 * @brief A census of the address hierarchy found in the stream.
 *
 *  Each field counts one leaf or branch of the geographic tree:
 *  records → place objects → content entries → typed address components.
 *  The counters grow monotonically as `json_stats_count_*` are called.
 */
typedef struct JsonStats {
  uint64_t records;         /**< Total JSON documents seen. */
  uint64_t place_records;   /**< Documents of type "Place". */
  uint64_t place_entries;   /**< Individual entries inside Place content arrays. */
  uint64_t countries;       /**< Entries tagged "country". */
  uint64_t states;          /**< Entries tagged "state". */
  uint64_t counties;        /**< Entries tagged "county". */
  uint64_t cities;          /**< Entries tagged "city". */
  uint64_t streets;         /**< Entries tagged "street". */
  uint64_t houses;          /**< Entries tagged "house". */
  uint64_t other;           /**< Entries with unrecognised or missing address_type. */
  uint64_t invalid_records; /**< Malformed entries skipped during scan. */
  int postcode_checked;     /**< Internal: set when at least one postcode has been seen. */
} JsonStats;

/**
 * @brief Update document-level counts from a parse result.
 *
 *  Call once per JSON line, passing the @p result filled by
 *  `json_parse_line()`.
 *
 *  @param[in,out] stats   Running counts, updated in place.
 *  @param[in]     result  Parse result from `json_parse_line()`.
 *
 *  @whisper One more document opens or closes — the census shifts
 */
void json_stats_count_document(JsonStats *stats, const void *result);

/**
 * @brief Update entry-level counts from a PhotonPlace.
 *
 *  Call from inside the `json_parse_line` callback, once per place entry.
 *  Also checks that at least one postcode field appears across all
 *  entries; if none is found after a threshold the program exits with a
 *  fatal error.
 *
 *  @param[in,out] stats   Running counts, updated in place.
 *  @param[in]     place   Extracted place data.
 *
 *  @whisper Each address type joins the tally
 */
void json_stats_count_place(JsonStats *stats, const PhotonPlace *place);

/**
 * @brief Merge a thread-local tally into the grand total.
 *
 *  Adds every counter from @p addend into @p total so a single
 *  summary can speak for many parallel parsers.
 *
 *  @param[in,out] total   Accumulator receiving the sum.
 *  @param[in]     addend  Partial counts to fold in.
 */
void json_stats_add(JsonStats *total, const JsonStats *addend);

/**
 * @brief Print the collected census to stdout.
 *
 *  Renders a compact table of all address-type counts and the
 *  overall document / entry totals.
 *
 *  @param[in] stats   Counts to display.
 */
void json_stats_print(const JsonStats *stats);

/** @} */

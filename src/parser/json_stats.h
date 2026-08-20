/** @defgroup json_stats JSON statistics
 *  @ingroup parser
 *  @brief Counting the shapes inside a Photon dump — how many countries,
 *         streets and houses pass through the stream.
 *
 *  Nothing here steers the build; the counts are gathered while the first pass
 *  runs and printed once at its end.  They earn their place by making a dump
 *  legible before an index exists: a file that turns out to hold no streets at
 *  all is better seen in the census than guessed at from an empty search.
 *
 *  Every parser thread keeps its own tally and they are added together at the
 *  end, so counting costs no synchronisation.
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
  uint64_t records;            /**< JSON documents seen, whole or not. */
  uint64_t place_records;      /**< Documents whose type was "Place". */
  uint64_t place_entries;      /**< Entries inside those documents' content arrays. */
  uint64_t countries;          /**< Entries tagged "country". */
  uint64_t states;             /**< Entries tagged "state". */
  uint64_t counties;           /**< Entries tagged "county". */
  uint64_t cities;             /**< Entries tagged "city". */
  uint64_t districts;          /**< Entries tagged "district". */
  uint64_t localities;         /**< Entries tagged "locality". */
  uint64_t streets;            /**< Entries tagged "street". */
  uint64_t houses;             /**< Entries tagged "house". */
  uint64_t state_cities;       /**< Entries tagged "state_county_city". */
  uint64_t independent_cities; /**< Entries tagged "independent_city". */
  uint64_t other;              /**< Entries tagged "other" — the ones that carried a house
                                    number, since the rest never reach the callback. */
  uint64_t search_terms;       /**< Role-free search strings offered by all entries. */
  uint64_t search_dropped;     /**< Strings that did not fit PHOTON_PLACE_SEARCH_MAX. */
  uint64_t invalid_records;    /**< Documents whose root was not a JSON object. */
  int postcode_checked;        /**< Unused.  Nothing sets it; json_stats_add() carries it
                                    across a merge and that is all it does. */
} JsonStats;

/**
 * @brief Update document-level counts from a parse result.
 *
 *  Call once per JSON line, with the result json_parse_line() filled in.
 *
 *  @param[in,out] stats   Running counts, raised in place.
 *  @param[in]     result  A @c JsonParseResult, passed as @c void* so that this
 *                         header need not pull in json_parse.h — the counters are
 *                         useful to a caller that never parses anything itself.
 *
 *  @whisper One more document opens or closes — the census shifts
 */
void json_stats_count_document(JsonStats *stats, const void *result);

/**
 * @brief Update entry-level counts from a PhotonPlace.
 *
 *  Call from inside the json_parse_line() callback, once per entry.  The entry's
 *  type raises one counter, and its search strings raise two more.
 *
 *  @param[in,out] stats   Running counts, raised in place.
 *  @param[in]     place   The entry, as the parser handed it over.
 *  @warning A type of NONE or UNKNOWN ends the process through fatal().  Neither
 *           reaches here from json_parse_line(), which refuses them earlier, so
 *           this catches a caller that built a @ref PhotonPlace by hand.
 *
 *  @whisper Each address type joins the tally
 */
void json_stats_count_place(JsonStats *stats, const PhotonPlace *place);

/**
 * @brief Merge a thread-local tally into the grand total.
 *
 *  Adds every counter of @p addend into @p total, so one summary can speak for
 *  all the threads that gathered it.
 *
 *  @param[in,out] total   Accumulator receiving the sum.
 *  @param[in]     addend  Partial counts to fold in; left unchanged.
 */
void json_stats_add(JsonStats *total, const JsonStats *addend);

/**
 * @brief Print the collected census to stdout.
 *
 *  A compact table: the documents and entries seen, then one line per address
 *  type.
 *
 *  @param[in] stats   Counts to show.
 *
 *  @whisper What passed through, said once and plainly
 */
void json_stats_print(const JsonStats *stats);

/** @} */

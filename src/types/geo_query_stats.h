/** @defgroup geo_query_stats Geo query statistics
 *  @ingroup types
 *  @brief What one query had to touch on its way to an answer — the numbers
 *         behind the duration, for reading by hand.
 *
 *  Nothing here steers a search; the counts are written while it runs and read
 *  afterwards.  They exist because a duration alone says only *that* a query
 *  was slow: whether five hundred posting lists were opened or two, whether a
 *  beginning covered the whole alphabet or one word, is not visible from the
 *  outside.
 *
 *  A caller that passes no statistics pays nothing — every count is written
 *  behind a check for the pointer, and the cardinalities that fill the sums are
 *  never asked for otherwise.
 *
 *  @whisper The search leaves its tracks, and the tracks say where the time went
 *  @{
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Counts of one search, gathered as it ran.
 *
 *  Everything a query does happens in one of four steps, and the fields follow
 *  them in that order: the words are looked up, their posting lists are opened,
 *  the sets narrow one another, and what is left is ranked.
 *
 *  A query may be asked more than once — a postal code that narrowed the answer
 *  to nothing is dropped and the words asked again.  The sums then hold every
 *  reading together, while @c groups and @c narrowed describe the one that
 *  finally answered; @c passes says how many there were.
 */
typedef struct GeoQueryStats {
  /** Dictionary words the typed beginnings covered, the refused ones included. */
  uint64_t prefix_terms;
  /** Posting bitmaps opened — one per word looked up, one per prefix term. */
  uint64_t posting_lists;
  /** Documents those bitmaps held, summed; a document counted once per list. */
  uint64_t posting_documents;
  /** Documents left after every word had narrowed the set, in the answering pass. */
  uint64_t narrowed;
  /** Candidates the ranking weighed, before the limit trimmed them. */
  uint64_t weighed;
  /** Results written for the caller. */
  uint64_t results;
  /** Readings the query needed, 1 … 3. */
  uint32_t passes;
  /** Words that narrowed in the answering pass, after their readings were joined. */
  uint32_t groups;
  /** Beginnings dropped for covering more words than a hint may. */
  uint32_t prefix_refused;
} GeoQueryStats;

#ifdef __cplusplus
}
#endif

/** @} */

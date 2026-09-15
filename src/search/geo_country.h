/** @defgroup geo_country geo_country
 *  @ingroup search
 *  @brief The country a place lies in, carried as a word nobody can type.
 *
 *  The dump gives every entry its country as a two-letter code and never as a
 *  name in the address — `Marienplatz, München, Bayern`, and `de` beside it.  So
 *  no place answers to *Deutschland*, and a query that names the country beside
 *  the town finds nothing at all: every word has to meet, and that one meets no
 *  street, no house and no town.
 *
 *  The code is written into the index as a search word instead, the way a
 *  position is written as a cell word — see @ref geo_cell.  `#de` stands on every
 *  place in Germany, and `#*` on every place that *is* a country, so a query can
 *  learn which of its words name one.  The mark is a byte the tokenizer ends a
 *  word on, so no text a person types folds into either word.
 *
 *  Both words are ordinary postings: an index built before them simply has none,
 *  and a search there behaves exactly as it always did.
 *
 *  @whisper The border is written down beside the street, where no one reads it aloud
 *  @{
 */

#pragma once

#include <stddef.h>

/** The byte no folded word can contain, and every country word begins with. */
#define GEO_COUNTRY_MARK '#'

/** Bytes one country word takes: the mark and the two letters, no terminator. */
#define GEO_COUNTRY_TOKEN_SIZE 3

/** The word every country document carries beside its code. */
#define GEO_COUNTRY_PLACE_TOKEN "#*"

/** Bytes of @ref GEO_COUNTRY_PLACE_TOKEN, without its terminator. */
#define GEO_COUNTRY_PLACE_TOKEN_SIZE 2

/**
 * @brief Write the search word of a country code.
 *
 *  The code is taken as the dump writes it — ISO 3166-1 alpha-2, in either case —
 *  and written in lower case: `DE` and `de` give `#de`.
 *
 *  @param[out] buffer  At least @ref GEO_COUNTRY_TOKEN_SIZE bytes, or NULL; not
 *                      terminated.
 *  @param[in]  code    NUL-terminated code, or NULL.
 *  @return @ref GEO_COUNTRY_TOKEN_SIZE, or 0 — nothing written — when @p buffer
 *          is NULL, or @p code is NULL or not exactly two ASCII letters.
 */
size_t geo_country_token(char *buffer, const char *code);

/** @} */

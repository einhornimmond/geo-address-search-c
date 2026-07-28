/** @defgroup client Geo client
 *  @ingroup core
 *  @brief The reading half of the index, as a library — open a file, ask it
 *         for an address, get a coordinate.
 *
 *  Everything a caller needs and nothing it does not: no builder, no parser,
 *  no dependency beyond the standard headers.  The handle is opaque, the
 *  status is a plain enum, and the whole surface is four functions.  That
 *  keeps it usable from C++, from a foreign function interface, and from a
 *  server that must not be taken down by a failed open.
 *
 *  ### What it promises
 *
 *  - **Nothing fatal.** Every failure comes back as a @ref GeoStatus; the
 *    library never ends the process, never writes to a stream.
 *  - **Concurrent reads.** A client may be searched from many threads at
 *    once.  The mapping is read-only and the scratch a search needs lives on
 *    its own stack.
 *  - **Borrowed text.** The strings in a result point into the mapped file and
 *    stay valid until geo_client_close(); copy what you keep.
 *
 *  ### Opening costs nothing
 *
 *  The file is mapped, not read.  A planet-wide index of several gigabytes is
 *  open in a fraction of a millisecond, and the operating system fetches only
 *  the pages a query actually touches.  The first queries after opening are
 *  therefore slower than the later ones; a server that cares should warm up.
 *
 *  @whisper The index lies still, and many may read it at once
 *  @{
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types/geo_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** An opened index. Create with geo_client_open(), release with geo_client_close(). */
typedef struct GeoClient GeoClient;

/**
 * @brief One place a search found.
 *
 *  The strings are not NUL-terminated — each carries its own length and points
 *  into the mapped file.  A field the entry never had comes back as NULL.
 */
typedef struct GeoAddress {
  const char *name;      /**< Street or place, as it is written. */
  size_t name_size;
  const char *number;    /**< House number, or NULL when the query named none. */
  size_t number_size;
  const char *postcode;  /**< Postal code, or NULL. */
  size_t postcode_size;
  const char *city;      /**< Town, or NULL. */
  size_t city_size;
  double latitude;       /**< Degrees; the house's own point when one was found. */
  double longitude;
  uint32_t document;     /**< Number of the place inside the index. */
  uint32_t matched;      /**< Query words this place carries. */
  uint16_t importance;   /**< Weight the dump gave it, 0 … 65535. */
  uint8_t kind;          /**< A @ref GeoPlaceKind. */
  uint8_t has_point;     /**< False when the place never carried a coordinate. */
} GeoAddress;

/** Counts of an opened index, for a status page or a log line. */
typedef struct GeoClientInfo {
  uint64_t file_size;   /**< Bytes mapped. */
  uint64_t documents;   /**< Places one can find. */
  uint64_t houses;      /**< House numbers hanging on them. */
  uint64_t words;       /**< Folded words in the search dictionary. */
  uint64_t spellings;   /**< Written forms in the display dictionary. */
  uint64_t postings;    /**< Word-to-place connections. */
  uint32_t format;      /**< Format version of the file. */
} GeoClientInfo;

/**
 * @brief Map an index file.
 *
 *  The file is checked before anything is trusted: magic, format version, byte
 *  order, a hash over the record sizes and every section's bounds.  A file
 *  written by another build is refused rather than misread.
 *
 *  @param[out] out   Receives the client; set to NULL on failure.
 *  @param[in]  path  File written by the builder, customarily `.gdx`.
 *  @return GEO_OK, or why not.
 *
 *  @whisper Gigabytes become reachable in the time a key press takes
 */
GeoStatus geo_client_open(GeoClient **out, const char *path);

/**
 * @brief Release the mapping.
 *
 *  Every string handed out before becomes invalid.  Safe to call with NULL.
 *
 *  @param[in,out] client  Client to close.
 */
void geo_client_close(GeoClient *client);

/**
 * @brief Read the counts of an opened index.
 *
 *  @param[in]  client  Opened client; must not be NULL.
 *  @param[out] out     Receives the counts.
 *  @return GEO_OK, or GEO_ERROR_ARGUMENT.
 */
GeoStatus geo_client_info(const GeoClient *client, GeoClientInfo *out);

/**
 * @brief Search for a place, and for a house number within it.
 *
 *  The query is free text: words in any order, upper or lower case, with or
 *  without diacritics, German abbreviations spelled out or not.  A number is
 *  read as a house number first and as a word only if that finds nothing.
 *
 *  Results are ordered by how far they answer what the query said about *where*
 *  — a postcode it named counts for more than a town — then, among places that
 *  answer equally, by whether the house number was actually found there, and
 *  last by the weight the dump gave the place.  A query that names no town and
 *  no postcode is answered by weight alone, heaviest first.
 *
 *  A postcode — four digits or more, standing on its own — narrows the search
 *  instead of only sorting it, which is what lets a quiet street outlive a
 *  famous one that shares its name.  Should it leave nothing standing, it is
 *  dropped and the query asked again without it, so a wrong one costs a place
 *  its position, never its presence.
 *
 *  Safe to call from several threads on the same client.
 *
 *  @param[in]  client       Opened client; must not be NULL.
 *  @param[in]  query        Free text, UTF-8.
 *  @param[in]  query_size   Byte length of @p query.
 *  @param[in]  prefix_last  Read the last word as a beginning as well, for a
 *                           query someone is still typing.  Pass false for one
 *                           they finished — a beginning always matches more.
 *  @param[out] out          Receives up to @p limit results.
 *  @param[in]  limit        Capacity of @p out.  At most 256 are ever written,
 *                           however many are asked for — beyond that a search
 *                           is a listing, and the ordering could no longer
 *                           weigh every place it found.
 *  @return Number of results written; 0 when nothing matched.
 *
 *  @whisper Words that were never spoken together find the one place where they belong
 */
size_t geo_client_search(
    const GeoClient *client,
    const char *query,
    size_t query_size,
    bool prefix_last,
    GeoAddress *out,
    size_t limit
);

/**
 * @brief Search and write the results as JSON into @p buffer.
 *
 *  For callers on the other side of a language boundary: one call, one string,
 *  no walking of foreign structs.  The output is a JSON array of objects with
 *  the keys `name`, `number`, `postcode`, `city`, `lat`, `lon`, `kind`,
 *  `importance` and `matched`; absent fields are `null`.
 *
 *  @param[in]  client       Opened client; must not be NULL.
 *  @param[in]  query        Free text, UTF-8.
 *  @param[in]  query_size   Byte length of @p query.
 *  @param[in]  prefix_last  As in geo_client_search().
 *  @param[in]  limit        Most results to write; capped at 256, as in
 *                           geo_client_search().
 *  @param[out] buffer       Destination; always NUL-terminated when it has room.
 *  @param[in]  buffer_size  Capacity of @p buffer in bytes.
 *  @return Bytes the whole answer needs without the NUL.  A value ≥
 *          @p buffer_size means it was cut short — call again with more room.
 */
size_t geo_client_search_json(
    const GeoClient *client,
    const char *query,
    size_t query_size,
    bool prefix_last,
    size_t limit,
    char *buffer,
    size_t buffer_size
);

#ifdef __cplusplus
}
#endif

/** @} */

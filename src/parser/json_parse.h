/** @defgroup parser Dump parsing
 *  @brief Everything that only the way in needs: unpacking a Photon dump,
 *         reading its lines, handing them between threads and writing them
 *         down once so the later walks are spared the file.
 *
 *  Parent group for the modules the search never links.  What leaves here is
 *  a @ref PhotonPlace; what happens to it afterwards belongs to @ref search.
 */

/** @defgroup json_parse JSON parser
 *  @ingroup parser
 *  @brief Parse Photon JSONL lines — the single translation unit that
 *         knows yyjson.  Extracts one @ref PhotonPlace per Place content
 *         entry: a few fields for the answer, everything else as
 *         role-free search strings.
 *
 *  The dump denormalises the whole address chain into every entry, and it
 *  does so imperfectly — a state may be missing, a city name may sit in the
 *  state field, city and street may carry the same text.  None of that is
 *  repaired here, because none of it needs repairing: a search index asks
 *  which words belong to a coordinate, never which role a word plays.  Only
 *  the fields that appear in an answer keep their role.
 *  @{
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types/photon_place_type.h"

/** A borrowed string with its byte length — points into the parsed document. */
typedef struct PhotonString {
  const char *data; /**< NUL-terminated text, or NULL when the field was absent. */
  size_t size;      /**< Byte length without the terminating NUL. */
} PhotonString;

/** Capacity for the role-free search strings of one entry.
 *
 *  The address block carries a localized variant per language — a German
 *  entry measured 30 distinct texts on average, so this must sit well above
 *  that or names quietly fall off the end. @c search_dropped counts what
 *  still does not fit. */
enum { PHOTON_PLACE_SEARCH_MAX = 128 };

/**
 * @brief One Photon Place content entry, split into answer and index.
 *
 *  Every string points into the parsed JSON document and is valid only
 *  during the callback that receives this struct.  Fields that were not
 *  present in the JSON have a NULL @c data pointer.
 *
 *  @c search holds every text of the entry that someone might type into a
 *  query — name variants in all languages, city, county, state, suburb and
 *  whatever else the address block carries — deduplicated within the entry
 *  and stripped of any role.  House entries leave it empty: they are the
 *  payload of their street, not something one searches for by name, and
 *  indexing their repeated parent text would multiply the term stream
 *  several times over.
 */
typedef struct PhotonPlace {
  const char *type; /**< address_type ("country", "state", …)           */
  PhotonPlaceType typeEnum;
  PhotonString own_name;                        /**< place display name ("name:de" or "name")  */
  PhotonString street;                          /**< street of the address, or the entry itself */
  PhotonString house;                           /**< house number, verbatim — "12A", "12-14", … */
  PhotonString postcode;                        /**< postal code as written                     */
  PhotonString city;                            /**< city of the address, or the entry itself   */
  const char *country_code;                     /**< ISO 3166-1 alpha-2 country code            */
  double importance;                            /**< Photon's own ranking weight, 0 when absent */
  int32_t lon_e7;                               /**< longitude × 10⁷                            */
  int32_t lat_e7;                               /**< latitude  × 10⁷                            */
  int has_point;                                /**< a centroid was present                     */
  PhotonString search[PHOTON_PLACE_SEARCH_MAX]; /**< role-free index texts  */
  uint8_t search_count;                         /**< Strings in @c search.                        */
  uint8_t search_dropped;                       /**< Strings that did not fit — 0 in sane data.   */
} PhotonPlace;

bool photon_place_has_point(const PhotonPlace *place);

/** Callback invoked once per Place content entry. */
typedef int (*PhotonPlaceCallback)(const PhotonPlace *place, void *user_data);

/**
 * @brief Per-document metadata returned by json_parse_line().
 *
 *  Callers aggregate these counters into a @c JsonStats.
 */
typedef struct {
  int is_valid;       /**< root was a JSON object (else malformed) */
  int is_place;       /**< root type was "Place"                  */
  size_t entry_count; /**< number of Place content entries        */
} JsonParseResult;

/**
 * @brief Parse one JSONL line and invoke @p callback for each Place entry.
 *
 *  Owns all yyjson interaction — the only translation unit that includes
 *  `<yyjson.h>`.  The yyjson scratch buffer is thread-local and grows to
 *  the longest line seen.
 *
 *  Strings in @p PhotonPlace point into the parsed JSON document and are
 *  valid only during the callback — copy them if you need them to outlive
 *  the call.
 *
 *  @param[in]  line      JSON text (need not be null-terminated).
 *  @param[in]  len       Byte length of @p line.
 *  @param[in]  callback  Invoked once per Place content entry.
 *  @param[in]  user_data Forwarded to @p callback on every invocation.
 *  @param[out] result    Document-level metadata for stats aggregation.
 *  @return true on success (including non-Place documents), false on
 *          JSON parse error.
 */
int json_parse_line(
    const char *line,
    size_t len,
    PhotonPlaceCallback callback,
    void *user_data,
    JsonParseResult *result
);

/**
 * @brief Serialize a PhotonPlace to a compact JSON string for debugging.
 *
 *  Writes into a thread-local static buffer (4 KiB).  The returned
 *  pointer is valid until the next call on the same thread.
 *  Basic JSON escaping is applied to string values.
 *
 *  @param[in] place  Place to serialize (NULL-safe, returns @c "null").
 *  @return JSON string (thread-local, ephemeral — copy if you need to
 *          keep it across calls).
 *
 *  @whisper A place speaks its full shape — for the debugger's eye alone
 */
char *photon_place_to_json(const PhotonPlace *place);

/** @} */

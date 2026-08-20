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
  const char *type;         /**< The address_type as written: "country", "state", … */
  PhotonPlaceType typeEnum; /**< The same, folded to a number; never UNKNOWN here. */
  PhotonString own_name;    /**< Display name — "name:de" where there is one, else "name". */
  PhotonString street;      /**< Street of the address, or the entry itself. */
  PhotonString house;       /**< House number, verbatim: "12A", "12-14", … */
  PhotonString postcode;    /**< Postal code as written. */
  PhotonString city;        /**< City of the address, or the entry itself. */
  const char *country_code; /**< ISO 3166-1 alpha-2. */
  double importance;        /**< Photon's own weight; 0 when the entry named none. */
  int32_t lon_e7;           /**< Longitude × 10⁷; meaningless unless @c has_point. */
  int32_t lat_e7;           /**< Latitude × 10⁷; meaningless unless @c has_point. */
  int has_point;            /**< A centroid was present. */
  PhotonString search[PHOTON_PLACE_SEARCH_MAX]; /**< Role-free index texts. */
  uint8_t search_count;                         /**< How many of them are filled. */
  uint8_t search_dropped;                       /**< How many did not fit; 0 in sane data. */
} PhotonPlace;

/**
 * @brief Whether the entry brought a coordinate of its own.
 *
 *  @param[in] place  The entry; not NULL.
 *  @return true when @c lon_e7 and @c lat_e7 hold a centroid the dump named.
 *          A house without one takes its street's position later, but that
 *          happens in the collector, not here.
 */
bool photon_place_has_point(const PhotonPlace *place);

/**
 * @brief Called once per Place content entry, with the entry still borrowed.
 *
 *  @param[in] place      The entry; every string in it points into the document
 *                        being parsed and dies with the call.
 *  @param[in] user_data  Whatever was handed to json_parse_line().
 *  @return 0 when the entry was taken.  A non-zero return does not stop the walk
 *          — the line is printed and parsing goes on to the next entry.
 */
typedef int (*PhotonPlaceCallback)(const PhotonPlace *place, void *user_data);

/**
 * @brief Per-document metadata returned by json_parse_line().
 *
 *  Callers aggregate these counters into a @c JsonStats.
 */
typedef struct {
  int is_valid;       /**< The root was a JSON object. */
  int is_place;       /**< …and its `type` was `"Place"`. */
  size_t entry_count; /**< Entries handed to the callback — the skipped ones not counted. */
} JsonParseResult;

/**
 * @brief Parse one JSONL line and invoke @p callback for each Place entry.
 *
 *  Owns all yyjson interaction — the only translation unit that includes
 *  `<yyjson.h>`.  The yyjson scratch buffer is thread-local and grows to
 *  the longest line seen.
 *
 *  Strings in @ref PhotonPlace point into the parsed JSON document and are
 *  valid only during the callback — copy them if you need them to outlive
 *  the call.
 *
 *  An entry whose `address_type` is `"other"` and which carries no house number
 *  is passed over: it names nothing anyone searches for.  It is not counted in
 *  @c entry_count either.
 *
 *  @param[in]  line      JSON text; need not be terminated.
 *  @param[in]  len       Byte length of @p line.
 *  @param[in]  callback  Invoked once per Place content entry that survives.
 *  @param[in]  user_data Forwarded to @p callback unchanged.
 *  @param[out] result    Document-level counts, for the statistics to add up.
 *  @return Always 1.  There is no failure return, because there is no failure a
 *          caller could act on.
 *  @warning Three conditions end the process through fatal() rather than coming
 *          back: JSON that will not parse, an entry without an `address_type`,
 *          and an `address_type` this build does not know.  A dump is either
 *          readable or it is not, and finding out halfway through a 24 GB file
 *          is worse than finding out at the first line.
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
 *  Writes into a thread-local buffer of 4 KiB; the next call on the same thread
 *  overwrites it.  String values are escaped enough to be read back.
 *
 *  @param[in] place  Place to write out; NULL yields @c "null".
 *  @return The text, valid until this thread calls again.  Copy it to keep it.
 *
 *  @whisper A place speaks its full shape — for the debugger's eye alone
 */
char *photon_place_to_json(const PhotonPlace *place);

/** @} */

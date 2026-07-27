/** @defgroup json_parse JSON parser
 *  @ingroup data
 *  @brief Parse Photon JSONL lines — the single translation unit that
 *         knows yyjson.  Extracts every address field into a flat
 *         @ref PhotonPlace struct.
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum PhotonPlaceType {
  PHOTON_PLACE_TYPE_NONE,
  PHOTON_PLACE_TYPE_COUNTRY,
  PHOTON_PLACE_TYPE_STATE,
  PHOTON_PLACE_TYPE_COUNTY,
  PHOTON_PLACE_TYPE_CITY,
  PHOTON_PLACE_TYPE_STREET,
  PHOTON_PLACE_TYPE_HOUSE,
  PHOTON_PLACE_TYPE_OTHER,
  PHOTON_PLACE_TYPE_DISTRICT,
  PHOTON_PLACE_TYPE_LOCALITY,
  PHOTON_PLACE_TYPE_STATE_COUNTY_CITY,
  PHOTON_PLACE_TYPE_INDEPENDENT_CITY,
  PHOTON_PLACE_TYPE_UNKNOWN
} PhotonPlaceType;

/**
 * @brief All address fields extracted from one Photon Place content entry.
 *
 *  Every string points into the parsed JSON document and is valid only
 *  during the callback that receives this struct.  Fields that were not
 *  present in the JSON are NULL.
 */
typedef struct PhotonPlace {
  const char *type;         /**< address_type ("country", "state", …)         */
  PhotonPlaceType typeEnum;
  const char *own_name;     /**< place display name ("name:de" or "name")    */
  size_t own_name_size;
  const char *country_code; /**< ISO 3166-1 alpha-2 country code             */
  int32_t lon_e7;           /**< longitude × 10⁷                             */
  int32_t lat_e7;           /**< latitude  × 10⁷                             */
} PhotonPlace;

bool photon_place_has_point(const PhotonPlace* place);

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
 *  `<yyjson.h>`.  The yyjson scratch buffer is allocated from @p alloc.
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

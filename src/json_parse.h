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

/** Opaque handle — owned by the meta-area allocator module. */
typedef struct MetaAreaAllocator MetaAreaAllocator;

/**
 * @brief All address fields extracted from one Photon Place content entry.
 *
 *  Every string points into the parsed JSON document and is valid only
 *  during the callback that receives this struct.  Fields that were not
 *  present in the JSON are NULL.
 */
typedef struct PhotonPlace {
  const char *type;         /**< address_type ("country", "state", …)         */
  const char *own_name;     /**< place display name ("name:de" or "name")    */
  const char *country_code; /**< ISO 3166-1 alpha-2 country code             */
  const char *country;      /**< country display name                        */
  const char *state;        /**< state display name                          */
  const char *county;       /**< county display name                         */
  const char *city;         /**< city display name                           */
  const char *postcode;     /**< postcode string                             */
  const char *street;       /**< street display name                         */
  const char *house;        /**< housenumber                                 */
  int32_t lon_e7;           /**< longitude × 10⁷                             */
  int32_t lat_e7;           /**< latitude  × 10⁷                             */
  int has_point;            /**< centroid data is present                     */
  int unsupported;          /**< addresslines present but no address object   */
} PhotonPlace;

/** Callback invoked once per Place content entry. */
typedef void (*PhotonPlaceCallback)(const PhotonPlace *place, void *user_data);

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
 *  @param[in]  alloc     Allocator for the yyjson internal buffer.
 *  @param[out] result    Document-level metadata for stats aggregation.
 *  @return true on success (including non-Place documents), false on
 *          JSON parse error.
 */
int json_parse_line(
    const char *line,
    size_t len,
    PhotonPlaceCallback callback,
    void *user_data,
    MetaAreaAllocator *alloc,
    JsonParseResult *result
);

/** @} */

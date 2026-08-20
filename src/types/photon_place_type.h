/** @defgroup photon_place_type Photon place type
 *  @ingroup types
 *  @brief The `address_type` of a Photon entry, read once by the parser and
 *         carried unchanged through collector, index and answer.
 *
 *  The numbers are those of @ref GeoPlaceKind — the builder writes them into
 *  every document record and the client hands them back — so the two enums must
 *  keep step.  client.c holds static assertions that refuse to compile should
 *  they drift.
 *
 *  That kinship is why the enum is wider than the parser is: `detectTypeEnum()`
 *  reads the dump's strings and can produce nine of these, while
 *  @ref PHOTON_PLACE_TYPE_NONE, @ref PHOTON_PLACE_TYPE_STATE_COUNTY_CITY and
 *  @ref PHOTON_PLACE_TYPE_INDEPENDENT_CITY are here to hold their numbers open
 *  for the kinds beside them.  Code that switches over the type still names
 *  them, so a dump that starts offering those strings needs a decoder and
 *  nothing else.  @ref PHOTON_PLACE_TYPE_UNKNOWN is the parser's own addition,
 *  for a string this build does not name; it has no counterpart on the client
 *  side and never reaches an answer — the entry is refused before that.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The `address_type` string of a dump entry, folded into a number. */
typedef enum PhotonPlaceType {
  PHOTON_PLACE_TYPE_NONE,              /**< Reserved for GEO_PLACE_NONE; never decoded. */
  PHOTON_PLACE_TYPE_COUNTRY,           /**< `"country"` */
  PHOTON_PLACE_TYPE_STATE,             /**< `"state"` */
  PHOTON_PLACE_TYPE_COUNTY,            /**< `"county"` */
  PHOTON_PLACE_TYPE_CITY,              /**< `"city"` */
  PHOTON_PLACE_TYPE_STREET,            /**< `"street"` */
  PHOTON_PLACE_TYPE_HOUSE,             /**< `"house"` */
  PHOTON_PLACE_TYPE_OTHER,             /**< `"other"` */
  PHOTON_PLACE_TYPE_DISTRICT,          /**< `"district"` */
  PHOTON_PLACE_TYPE_LOCALITY,          /**< `"locality"` */
  PHOTON_PLACE_TYPE_STATE_COUNTY_CITY, /**< Held open beside GEO_PLACE_STATE_CITY. */
  PHOTON_PLACE_TYPE_INDEPENDENT_CITY,  /**< Held open beside GEO_PLACE_INDEPENDENT_CITY. */
  PHOTON_PLACE_TYPE_UNKNOWN            /**< A string this build does not name; the entry is
                                            refused rather than indexed. */
} PhotonPlaceType;

#ifdef __cplusplus
}
#endif

/** @} */

/** @defgroup photon_place_type Photon place type
 *  @ingroup types
 *  @brief The `address_type` of a Photon entry, read once by the parser and
 *         carried unchanged through collector, index and answer.
 *
 *  The numbers are those of @ref GeoPlaceKind — the builder writes them into
 *  every document record and the client hands them back — so the two enums must
 *  keep step.  client.c holds static assertions that refuse to compile should
 *  they drift.  @c PHOTON_PLACE_TYPE_UNKNOWN is the parser's own addition, for a
 *  string the dump offered that this build does not name; it has no counterpart
 *  on the client side.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

/** @} */

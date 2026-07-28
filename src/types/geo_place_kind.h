/** @defgroup geo_place_kind Geo place kind
 *  @ingroup types
 *  @brief What kind of place an answer carries — country, street, house, and the
 *         handful of shapes between them.
 *
 *  The numbers are part of the file format: they are written into every document
 *  record and read back unchanged, so they may never be renumbered.  They match
 *  @ref PhotonPlaceType one for one, and client.c holds static assertions that
 *  refuse to compile should the two ever drift apart.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** What kind of place a result is; the numbers are part of the file format. */
typedef enum GeoPlaceKind {
  GEO_PLACE_NONE = 0,
  GEO_PLACE_COUNTRY = 1,
  GEO_PLACE_STATE = 2,
  GEO_PLACE_COUNTY = 3,
  GEO_PLACE_CITY = 4,
  GEO_PLACE_STREET = 5,
  GEO_PLACE_HOUSE = 6,
  GEO_PLACE_OTHER = 7,
  GEO_PLACE_DISTRICT = 8,
  GEO_PLACE_LOCALITY = 9,
  GEO_PLACE_STATE_CITY = 10,
  GEO_PLACE_INDEPENDENT_CITY = 11
} GeoPlaceKind;

#ifdef __cplusplus
}
#endif

/** @} */

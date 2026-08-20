/** @defgroup geo_place_kind Geo place kind
 *  @ingroup types
 *  @brief What kind of place an answer carries — country, street, house, and the
 *         handful of shapes between them.
 *
 *  The numbers are part of the file format.  They are written into every
 *  document record as a `uint8_t` and read back unchanged, so they may never be
 *  renumbered: an index built yesterday is read by the client of tomorrow.  They
 *  match @ref PhotonPlaceType one for one, and client.c holds static assertions
 *  that refuse to compile should the two ever drift apart.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What kind of place a result is; the numbers are part of the file format. */
typedef enum GeoPlaceKind {
  GEO_PLACE_NONE = 0,             /**< No kind was carried; the place named none. */
  GEO_PLACE_COUNTRY = 1,          /**< A country as a whole. */
  GEO_PLACE_STATE = 2,            /**< A federal state, province or region. */
  GEO_PLACE_COUNTY = 3,           /**< A county or district between state and town. */
  GEO_PLACE_CITY = 4,             /**< A town or city. */
  GEO_PLACE_STREET = 5,           /**< A street, without a number on it. */
  GEO_PLACE_HOUSE = 6,            /**< A house number standing on a street. */
  GEO_PLACE_OTHER = 7,            /**< Named on the map, but none of the shapes above. */
  GEO_PLACE_DISTRICT = 8,         /**< A quarter within a town. */
  GEO_PLACE_LOCALITY = 9,         /**< A hamlet or settlement below town level. */
  GEO_PLACE_STATE_CITY = 10,      /**< A city that is a state of its own. */
  GEO_PLACE_INDEPENDENT_CITY = 11 /**< A city that answers to no county. */
} GeoPlaceKind;

#ifdef __cplusplus
}
#endif

/** @} */

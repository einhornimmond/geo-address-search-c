/** @cond INTERNAL */

#include "search/geo_cell.h"

#include <stdbool.h>

/** Degrees × 10⁷ from the equator to a pole. */
#define GEO_CELL_LAT_SPAN INT64_C(900000000)
/** Degrees × 10⁷ once around the world. */
#define GEO_CELL_LON_SPAN INT64_C(3600000000)

/** The row a latitude falls into, poles included rather than refused. */
static uint32_t row_of(int32_t lat_e7) {
  int64_t shifted = (int64_t)lat_e7 + GEO_CELL_LAT_SPAN;
  if (shifted < 0) shifted = 0;
  uint32_t row = (uint32_t)(shifted / GEO_CELL_SIZE_E7);
  return row >= GEO_CELL_ROWS ? GEO_CELL_ROWS - 1 : row;
}

/** The column a longitude falls into; past the dateline it comes back around. */
static uint32_t column_of(int32_t lon_e7) {
  int64_t shifted = ((int64_t)lon_e7 + GEO_CELL_LON_SPAN / 2) % GEO_CELL_LON_SPAN;
  if (shifted < 0) shifted += GEO_CELL_LON_SPAN;
  uint32_t column = (uint32_t)(shifted / GEO_CELL_SIZE_E7);
  return column >= GEO_CELL_COLUMNS ? GEO_CELL_COLUMNS - 1 : column;
}

uint32_t geo_cell_of(int32_t lat_e7, int32_t lon_e7) {
  return row_of(lat_e7) * GEO_CELL_COLUMNS + column_of(lon_e7);
}

size_t geo_cell_token(char *buffer, uint32_t cell) {
  static const char DIGITS[37] = "0123456789abcdefghijklmnopqrstuvwxyz";
  buffer[0] = GEO_CELL_MARK;
  for (size_t i = 0; i < GEO_CELL_TOKEN_DIGITS; ++i) {
    buffer[1 + i] = DIGITS[cell % 36];
    cell /= 36;
  }
  return GEO_CELL_TOKEN_SIZE;
}

size_t geo_cell_ring(
    uint32_t *out, size_t capacity, int32_t lat_e7, int32_t lon_e7, unsigned radius
) {
  if (!out || !capacity) return 0;

  int64_t middle_row = row_of(lat_e7);
  int64_t middle_column = column_of(lon_e7);
  size_t count = 0;

  /* The middle cell goes first: it is the one a searcher is standing in, and a
     caller that can only afford one has the right one. */
  out[count++] = (uint32_t)(middle_row * GEO_CELL_COLUMNS + middle_column);

  for (int64_t dr = -(int64_t)radius; dr <= (int64_t)radius && count < capacity; ++dr) {
    int64_t row = middle_row + dr;
    if (row < 0 || row >= GEO_CELL_ROWS) continue; /* past a pole there is no row */
    for (int64_t dc = -(int64_t)radius; dc <= (int64_t)radius && count < capacity; ++dc) {
      if (!dr && !dc) continue; /* the middle already stands at the front */
      /* around the dateline the column comes back on the other side */
      int64_t column = (middle_column + dc) % GEO_CELL_COLUMNS;
      if (column < 0) column += GEO_CELL_COLUMNS;
      uint32_t cell = (uint32_t)(row * GEO_CELL_COLUMNS + column);

      /* near a pole the ring narrows and a column may repeat */
      bool seen = false;
      for (size_t i = 0; i < count; ++i) {
        if (out[i] == cell) {
          seen = true;
          break;
        }
      }
      if (!seen) out[count++] = cell;
    }
  }
  return count;
}

/** @endcond */

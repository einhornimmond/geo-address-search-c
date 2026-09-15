/** @cond INTERNAL */

#include "search/geo_country.h"

#include <stdbool.h>

/** Is @p c an ASCII letter? Then its lower case is `c | 0x20`. */
static bool ascii_letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

size_t geo_country_token(char *buffer, const char *code) {
  if (!buffer || !code || !ascii_letter(code[0]) || !ascii_letter(code[1]) || code[2] != '\0') {
    return 0;
  }
  buffer[0] = GEO_COUNTRY_MARK;
  buffer[1] = (char)(code[0] | 0x20);
  buffer[2] = (char)(code[1] | 0x20);
  return GEO_COUNTRY_TOKEN_SIZE;
}

/** @endcond */

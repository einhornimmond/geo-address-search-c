#include "foundation/format.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hostmem/converter.h"

/** @cond INTERNAL */

int format_byte_units(char *buffer, size_t buffer_size, uint64_t bytes, uint8_t precision) {
  uint64_t divisor;
  const char *suffix;

  /* Beyond fifteen places a double has nothing true left to say. */
  if (precision > 15) { precision = 15; }

  /* A ladder of comparisons rather than a table: five rungs, each one a
     predictable branch, and no indirection to walk. */
  if (bytes < 1024ULL) {
    divisor = 1ULL;
    suffix = " B";
  } else if (bytes < 1024ULL * 1024ULL) {
    divisor = 1024ULL;
    suffix = " KB";
  } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
    divisor = 1024ULL * 1024ULL;
    suffix = " MB";
  } else if (bytes < 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
    divisor = 1024ULL * 1024ULL * 1024ULL;
    suffix = " GB";
  } else {
    divisor = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    suffix = " TB";
  }

  double decimalValue = (double)bytes / divisor;
  int64_t integerPart = (int64_t)decimalValue;
  int64_t fractionalPart = (int64_t)((decimalValue - integerPart) * 1000000000000000ULL);

  uint8_t int_size = hostmem_uint64_to_string_size(integerPart);
  size_t suffix_len = strlen(suffix);
  if (buffer_size < int_size + 2 + precision +
                        suffix_len) { // +2 for possible '.' and +precision for fractional part
    return int_size + 1 + precision + suffix_len;
  }

  size_t written = hostmem_uint64_to_string_known_string_size(buffer, integerPart, int_size);

  /* Bytes are whole by nature — below 1024 there is no fraction to show, so a
     precision asked for there is quietly let go of. */
  if (precision > 0 && divisor > 1) {
    buffer[written++] = '.';
    uint8_t fractionalPartSize = 0;
    if (fractionalPart) { fractionalPartSize = hostmem_uint64_to_string_size(fractionalPart); }
    /* The fraction was scaled by 10^15, so its digit count says how far it sits
       from the point: a short number means leading zeros the converter, which
       writes digits and no padding, would otherwise swallow. */
    size_t zerosBeforeCount = 15 - fractionalPartSize;
    if (zerosBeforeCount > precision) { zerosBeforeCount = precision; }
    if (zerosBeforeCount > 0) {
      memset(buffer + written, '0', zerosBeforeCount);
      written += zerosBeforeCount;
    }
    /* Whatever the zeros did not fill is taken from the digits themselves,
       front first — which is the truncation this function promises. */
    size_t restNumbers = precision - zerosBeforeCount;
    if (restNumbers) {
      char tempBuffer[20]; /* nineteen digits and the terminator */
      uint8_t frac_size = hostmem_uint64_to_string_known_string_size(
          tempBuffer, fractionalPart, fractionalPartSize
      );
      memcpy(buffer + written, tempBuffer, restNumbers);
      written += restNumbers;
    }
  }

  memcpy(buffer + written, suffix, suffix_len);
  written += suffix_len;

  buffer[written] = '\0';
  return written;
}

/** @endcond */

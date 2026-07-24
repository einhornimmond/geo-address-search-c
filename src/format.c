#include "format.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gradido_blockchain_core/utils/converter.h"

int format_byte_units(char *buffer, size_t buffer_size, uint64_t bytes, uint8_t precision) {
  uint64_t divisor;
  const char *suffix;

  if (precision > 15) { precision = 15; }

  // --- unit selection (branch tree, kein array) ---
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

  size_t int_size = grdu_uint64_to_string_size(integerPart);
  size_t suffix_len = strlen(suffix);
  if (buffer_size < int_size + 2 + precision +
                        suffix_len) { // +2 for possible '.' and +precision for fractional part
    return int_size + 1 + precision + suffix_len;
  }

  size_t written = grdu_uint64_to_string_known_string_size(buffer, integerPart, int_size);
  // --- fractional part ---
  if (precision > 0 && divisor > 1) {
    buffer[written++] = '.';
    size_t fractionalPartSize = 0;
    if (fractionalPart) { fractionalPartSize = grdu_uint64_to_string_size(fractionalPart); }
    // 15 = max fractional part size (15 zeros near the double boundary)
    size_t zerosBeforeCount = 15 - fractionalPartSize;
    if (zerosBeforeCount > precision) { zerosBeforeCount = precision; }
    if (zerosBeforeCount > 0) {
      memset(buffer + written, '0', zerosBeforeCount);
      written += zerosBeforeCount;
    }
    size_t restNumbers = precision - zerosBeforeCount;
    if (restNumbers) {
      char tempBuffer[20]; // enough to hold fractional part
      size_t frac_size =
          grdu_uint64_to_string_known_string_size(tempBuffer, fractionalPart, fractionalPartSize);
      memcpy(buffer + written, tempBuffer, restNumbers);
      written += restNumbers;
    }
  }

  // --- suffix ---
  memcpy(buffer + written, suffix, suffix_len);
  written += suffix_len;

  buffer[written] = '\0';
  return written;
}

#include "foundation/format.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hostmem/converter.h"

/** @cond INTERNAL */

int format_byte_units(char *buffer, size_t buffer_size, uint64_t bytes, uint8_t precision) {
  uint64_t divisor;
  const char *suffix;

  /* The cap is what makes the result sizeable. `precision` is a uint8_t, and 255
     places would put 267 bytes on a caller who reasonably expected far fewer; at
     15 the longest result any input can produce is 27 bytes. Nothing is lost by
     it that the scale still holds: a remainder over 2^40 has forty decimal places
     before it terminates, so every one of the fifteen is a real digit. */
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

  /* Integer division throughout.  Going through a double here would be shorter
     and wrong at the top of the range: (double)UINT64_MAX rounds up to 2^64, and
     the count that should read 16777215.99 TB would come back as 16777216.00 TB
     — a truncation that rounded up, which is the one thing it must never do. */
  const uint64_t whole = bytes / divisor;
  uint64_t rest = bytes % divisor;

  /* Bytes are whole by nature — below 1024 there is no fraction to show, so a
     precision asked for there is quietly let go of. */
  const uint8_t places = divisor > 1ULL ? precision : 0;

  const uint8_t int_size = hostmem_uint64_to_string_size(whole);
  const size_t suffix_len = strlen(suffix);
  const size_t needed = (size_t)int_size + (places ? 1u + places : 0u) + suffix_len;

  /* Nothing is written unless the whole of it fits, terminator included. */
  if (buffer_size < needed + 1) { return (int)needed; }

  size_t written = hostmem_uint64_to_string_known_string_size(buffer, whole, int_size);

  if (places) {
    buffer[written++] = '.';
    /* One digit at a time, taken from the remainder itself.  `rest` stays below
       the divisor, so `rest * 10` stays below 2^44 and never troubles a uint64 —
       and every digit is the exact one, with no scaling step to round it. */
    for (uint8_t i = 0; i < places; ++i) {
      rest *= 10ULL;
      buffer[written++] = (char)('0' + (rest / divisor));
      rest %= divisor;
    }
  }

  memcpy(buffer + written, suffix, suffix_len);
  written += suffix_len;

  buffer[written] = '\0';
  return (int)written;
}

/** @endcond */

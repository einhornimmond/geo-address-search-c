/** @defgroup format Formatting
 *  @ingroup foundation
 *  @brief Human-readable representations for bytes that emerge from the raw
 *         count — the final layer of legibility.
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Format a size in bytes into a human-readable string, letting the most
 *        natural unit emerge.
 *
 * Takes a size in bytes and flows it into the most natural scale:
 * B, KB, MB, GB, TB. The precision parameter shapes how many decimal places
 * settle into the output — truncated with clean edges, never rounded.
 *
 * Writes into the provided buffer only if sufficient space exists.
 *
 * @param[out] buffer        Destination buffer for the resulting string.
 * @param[in]  buffer_size   Size of buffer in bytes (must include space for '\0').
 * @param[in]  bytes         Size in bytes (must be >= 0).
 * @param[in]  precision     Decimal places after the point (capped at 15 to prevent overflow).
 *
 * @return
 *   >= 0  - Characters written (excluding '\0'). If buffer is too small,
 *           returns the size that would have been needed.
 *   -1    - Invalid input (negative size).
 *
 * @note
 *   Truncation, not rounding. Example: precision=3 with 1.234567 MB yields "1.234 MB".
 *
 * @whisper Size settles into the scale it needs
 */
int format_byte_units(char *buffer, size_t buffer_size, uint64_t bytes, uint8_t precision);

/** @} */

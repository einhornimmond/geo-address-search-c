/** @defgroup format Formatting
 *  @ingroup foundation
 *  @brief Byte counts made legible — the last layer between a number and a
 *         person reading it.
 *
 *  One function, and it earns its place by being the only spelling of a size in
 *  the program: every progress line, every summary and every report of what an
 *  index cost goes through it, so they all round the same way and carry the same
 *  units.
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Write a byte count in the largest unit that leaves a whole number in
 *        front of the point.
 *
 *  The scale emerges from the size itself: below 1024 bytes it stays `B`, and
 *  from there it climbs through `KB`, `MB`, `GB` to `TB`, each step a factor of
 *  1024.  Decimals are cut, never rounded — `precision` 3 over 1.234567 MB
 *  yields `"1.234 MB"`, and the digits that fall away fall away silently.
 *
 *  Cut exactly, at every size there is.  The whole part and each decimal place
 *  come from integer division by the divisor, never from a `double`: the largest
 *  count of all reads `"16777215.99 TB"`, where a `double` quotient would have
 *  rounded up to `16777216.00` and turned a truncation into its opposite.  `TB`
 *  is the last unit, so beyond 1024 TB the figure in front of the point simply
 *  grows.
 *
 *  Bytes are the one scale with no decimals: a count under 1024 is a whole
 *  number already, so `precision` is ignored there and `"512 B"` comes back
 *  however many places were asked for.
 *
 *  Nothing is written unless the whole result fits.  A buffer too small is left
 *  untouched, terminator included, and the caller is told the length to come
 *  back with.
 *
 *  @param[out] buffer      Destination; untouched when the result would not fit.
 *  @param[in]  buffer_size Bytes available in @p buffer, the terminator included.
 *  @param[in]  bytes       The count to spell out.
 *  @param[in]  precision   Decimal places to keep; anything above 15 is taken as 15.
 *                          The cap is there to bound the result, not because the digits
 *                          run out: a divisor of 2^40 needs forty places before its
 *                          expansion terminates, so all fifteen are real digits at the
 *                          `TB` scale.  Only `KB`, whose divisor is 2^10, runs dry
 *                          inside the cap and pads the last five places with zeros.
 *  @return Characters written, the terminator not counted — so a destination holds the
 *          result when it has one byte more than this.  When @p buffer_size did not
 *          reach, the very same figure comes back for a result that was not written, so
 *          a caller may size from it and call again.  Never negative, and never above
 *          27: the divisor holds the whole part to eight digits, and a point, fifteen
 *          places and the longest suffix follow it.
 *  @note A caller that only wants the length may pass 0 for @p buffer_size: nothing is
 *        written and the figure comes back all the same.
 *  @whisper Size settles into the scale it needs
 */
int format_byte_units(char *buffer, size_t buffer_size, uint64_t bytes, uint8_t precision);

/** @} */

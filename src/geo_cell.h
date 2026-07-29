/** @defgroup geo_cell Geo cells
 *  @ingroup data
 *  @brief Where a place stands, written as a word — so that *near me* narrows
 *         the search instead of only sorting what survived it.
 *
 *  The index knows how to find documents by word and by nothing else.  A
 *  coordinate is not a word, so a search cannot ask *what is close* without
 *  walking everything it already found — and what it already found was cut by
 *  weight long before the question was asked.  Nine thousand streets called
 *  *Hauptstraße* stand on the planet; the ranking sees the sixty-four heaviest
 *  of them, and the one two hundred metres away is never among those.
 *
 *  So the position becomes a word.  The world is laid over a grid of
 *  @ref GEO_CELL_SIZE_E7 degrees, every place carries the cell it stands in as
 *  one more search term, and a query that knows where the searcher is asks for
 *  the ring of cells around them — an ordinary intersection, in the same
 *  machinery every other word goes through, before weight has cut anything.
 *
 *  ### The shape of a cell word
 *
 *  `@` opens it, and the tokenizer can never produce that byte: it splits on
 *  everything that is neither letter nor digit, so no place name will ever fold
 *  into something a cell word could be mistaken for.
 *
 *  What follows is the cell number in base 36 — **least significant digit
 *  first**.  The order looks backwards and is deliberate: a dictionary group is
 *  keyed by the first @ref NAME_PREFIX_DEPTH bytes, so leading with the digit
 *  that changes fastest spreads six million cells over the groups instead of
 *  crowding them into one.  Nothing reads a cell word back; only exact lookups
 *  ask for it, and for those the order of the digits is free.
 *
 *  ### What stands nowhere
 *
 *  An entry the dump gave no centroid carries no cell, and no ring can hold it.
 *  A query that names a position therefore never answers with it — there is no
 *  honest way to say how far away something is that has no *where*.  Asked
 *  without a position, it is found like anything else.
 *
 *  ### What the grid costs
 *
 *  One posting per document — on a planet index that is 34.6 million more on
 *  516 million, near enough 7 %.  The words themselves are a few million short
 *  strings.  Cells are not stored anywhere else: a document already carries its
 *  coordinate, and the cell is derived from it whenever it is needed.
 *
 *  @whisper A place says where it stands in a word, and the words that stand near it answer together
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/** Edge length of one cell in degrees × 10⁷ — 0.1°, some 11 km north to south. */
#define GEO_CELL_SIZE_E7 1000000

/** Cells around the equator: 360° divided by the edge length. */
#define GEO_CELL_COLUMNS 3600

/** Cells from pole to pole: 180° divided by the edge length. */
#define GEO_CELL_ROWS 1800

/** Base-36 digits one cell number is written with; 36⁵ holds every cell twice over. */
#define GEO_CELL_TOKEN_DIGITS 5

/** Bytes one cell word takes: the mark and its digits, no terminator. */
#define GEO_CELL_TOKEN_SIZE (1 + GEO_CELL_TOKEN_DIGITS)

/** The byte no folded word can begin with, and every cell word does. */
#define GEO_CELL_MARK '@'

/**
 * @brief The cell a coordinate falls into.
 *
 *  Latitude is clamped to the poles, longitude wrapped around the dateline, so
 *  every input yields a cell rather than an error.  The result is
 *  `row * GEO_CELL_COLUMNS + column` and stays below
 *  `GEO_CELL_ROWS * GEO_CELL_COLUMNS`.
 *
 *  @param[in] lat_e7  Latitude × 10⁷.
 *  @param[in] lon_e7  Longitude × 10⁷.
 *  @return Cell number in [0, GEO_CELL_ROWS * GEO_CELL_COLUMNS).
 */
uint32_t geo_cell_of(int32_t lat_e7, int32_t lon_e7);

/**
 * @brief Write the search word of a cell.
 *
 *  @param[out] buffer  At least @ref GEO_CELL_TOKEN_SIZE bytes; not terminated.
 *  @param[in]  cell    Cell number from geo_cell_of().
 *  @return Bytes written, always @ref GEO_CELL_TOKEN_SIZE.
 *
 *  @whisper A patch of the earth takes a name only the index will ever say
 */
size_t geo_cell_token(char *buffer, uint32_t cell);

/**
 * @brief The cells within @p radius steps of a coordinate, the middle one first.
 *
 *  A radius of 1 yields the nine cells of a 3 × 3 block — some 33 km across at
 *  the latitudes of Europe.  Rows are clamped at the poles, columns wrap around
 *  the dateline, and no cell appears twice however narrow the world becomes at
 *  the top of it.
 *
 *  @param[out] out       Receives the cell numbers; needs (2·radius + 1)² slots.
 *  @param[in]  capacity  Slots available in @p out.
 *  @param[in]  lat_e7    Latitude × 10⁷ of the searcher.
 *  @param[in]  lon_e7    Longitude × 10⁷ of the searcher.
 *  @param[in]  radius    Cells to either side; 0 yields the one cell they stand in.
 *  @return Cells written, at most @p capacity.
 */
size_t geo_cell_ring(
    uint32_t *out, size_t capacity, int32_t lat_e7, int32_t lon_e7, unsigned radius
);

/** @} */

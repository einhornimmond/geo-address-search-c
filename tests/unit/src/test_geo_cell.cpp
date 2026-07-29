/** @file
 *  @brief The grid the world is laid over, and the words it is written in.
 *
 *  Two things have to hold, and everything else follows from them: a place and
 *  a searcher standing next to each other must land in the same cell or in
 *  neighbouring ones, and the word a cell is written as must be one no folded
 *  place name could ever be.  The rest is the edges — the poles, the dateline,
 *  the meridian — where a grid laid over a sphere stops being a grid.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

/** Degrees as the index keeps them. */
constexpr int32_t E7(double degrees) { return (int32_t)(degrees * 1.0e7); }

std::string Token(int32_t lat_e7, int32_t lon_e7) {
  char buffer[GEO_CELL_TOKEN_SIZE];
  size_t size = geo_cell_token(buffer, geo_cell_of(lat_e7, lon_e7));
  return std::string(buffer, size);
}

std::vector<uint32_t> Ring(int32_t lat_e7, int32_t lon_e7, unsigned radius) {
  std::vector<uint32_t> cells((2 * radius + 1) * (2 * radius + 1));
  size_t count = geo_cell_ring(cells.data(), cells.size(), lat_e7, lon_e7, radius);
  cells.resize(count);
  return cells;
}

} // namespace

// ---------------------------------------------------------------------------
//  Which cell a place falls into
// ---------------------------------------------------------------------------

TEST(GeoCell, NeighboursShareACell) {
  // two doors on the same street, some hundred metres apart
  EXPECT_EQ(geo_cell_of(E7(50.9414), E7(6.9583)), geo_cell_of(E7(50.9421), E7(6.9601)));
}

TEST(GeoCell, ATenthOfADegreeIsTheEdge) {
  uint32_t here = geo_cell_of(E7(50.05), E7(6.05));
  EXPECT_EQ(here, geo_cell_of(E7(50.09), E7(6.09))) << "still inside";
  EXPECT_NE(here, geo_cell_of(E7(50.11), E7(6.05))) << "one row further north";
  EXPECT_NE(here, geo_cell_of(E7(50.05), E7(6.11))) << "one column further east";
}

TEST(GeoCell, TheFourQuartersOfTheWorldStayApart) {
  std::set<uint32_t> cells = {
      geo_cell_of(E7(50.0), E7(6.0)),   // north east
      geo_cell_of(E7(50.0), E7(-6.0)),  // north west
      geo_cell_of(E7(-50.0), E7(6.0)),  // south east
      geo_cell_of(E7(-50.0), E7(-6.0)), // south west
  };
  EXPECT_EQ(cells.size(), 4u);
}

TEST(GeoCell, EveryCellNumberStaysInsideTheGrid) {
  const uint32_t ceiling = (uint32_t)GEO_CELL_ROWS * GEO_CELL_COLUMNS;
  for (int lat = -90; lat <= 90; lat += 5) {
    for (int lon = -180; lon <= 180; lon += 5) {
      EXPECT_LT(geo_cell_of(E7(lat), E7(lon)), ceiling) << lat << ", " << lon;
    }
  }
}

TEST(GeoCell, ThePolesAreACellAndNotAnOverflow) {
  const uint32_t ceiling = (uint32_t)GEO_CELL_ROWS * GEO_CELL_COLUMNS;
  EXPECT_LT(geo_cell_of(E7(90.0), E7(0.0)), ceiling);
  EXPECT_LT(geo_cell_of(E7(-90.0), E7(0.0)), ceiling);
  EXPECT_LT(geo_cell_of(E7(90.0), E7(180.0)), ceiling);
}

TEST(GeoCell, TheDatelineIsAnEdgeAndNotAWall) {
  // the two cells either side of 180° are neighbours on the ground and lie at
  // opposite ends of the row, which is what the ring has to bridge
  uint32_t east = geo_cell_of(E7(0.0), E7(179.95));
  uint32_t west = geo_cell_of(E7(0.0), E7(-179.95));
  EXPECT_EQ(east - west, (uint32_t)GEO_CELL_COLUMNS - 1) << "same row, first column and last";
}

// ---------------------------------------------------------------------------
//  The word a cell is written as
// ---------------------------------------------------------------------------

TEST(GeoCellToken, BeginsWithAByteNoWordCanBeginWith) {
  std::string token = Token(E7(50.9414), E7(6.9583));
  ASSERT_EQ(token.size(), (size_t)GEO_CELL_TOKEN_SIZE);
  EXPECT_EQ(token[0], GEO_CELL_MARK);

  // whatever the tokenizer does with an '@', it never hands one back
  TextTokenizer tok;
  text_tokenizer_init(&tok);
  const char *written = "Am Dom @ Köln";
  size_t count = text_tokenize(&tok, written, std::strlen(written));
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(std::string(tok.tokens[i].data, tok.tokens[i].size).find(GEO_CELL_MARK),
              std::string::npos);
  }
}

TEST(GeoCellToken, DifferentCellsAreDifferentWords) {
  EXPECT_NE(Token(E7(50.0), E7(6.0)), Token(E7(50.2), E7(6.0)));
  EXPECT_NE(Token(E7(50.0), E7(6.0)), Token(E7(50.0), E7(6.2)));
  EXPECT_EQ(Token(E7(50.01), E7(6.01)), Token(E7(50.02), E7(6.02)));
}

TEST(GeoCellToken, TheSameCellIsAlwaysTheSameWord) {
  for (uint32_t cell : {0u, 1u, 35u, 36u, 1295u, 6479999u}) {
    char first[GEO_CELL_TOKEN_SIZE], second[GEO_CELL_TOKEN_SIZE];
    geo_cell_token(first, cell);
    geo_cell_token(second, cell);
    EXPECT_EQ(std::string(first, GEO_CELL_TOKEN_SIZE), std::string(second, GEO_CELL_TOKEN_SIZE));
  }
}

TEST(GeoCellToken, EveryCellOfTheWorldGetsItsOwnWord) {
  // five base-36 digits have to carry the whole grid without two cells meeting
  const uint32_t ceiling = (uint32_t)GEO_CELL_ROWS * GEO_CELL_COLUMNS;
  std::set<std::string> seen;
  for (uint32_t cell = 0; cell < ceiling; cell += 997) { // a prime, so the walk drifts
    char buffer[GEO_CELL_TOKEN_SIZE];
    geo_cell_token(buffer, cell);
    EXPECT_TRUE(seen.insert(std::string(buffer, GEO_CELL_TOKEN_SIZE)).second) << cell;
  }
}

TEST(GeoCellToken, TheDigitThatChangesFastestComesFirst) {
  // that is what spreads the cells over the dictionary's prefix groups instead
  // of crowding six million of them into one
  char zero[GEO_CELL_TOKEN_SIZE], one[GEO_CELL_TOKEN_SIZE];
  geo_cell_token(zero, 0);
  geo_cell_token(one, 1);
  EXPECT_NE(zero[1], one[1]) << "the second byte already tells the two apart";
}

// ---------------------------------------------------------------------------
//  The ring around a searcher
// ---------------------------------------------------------------------------

TEST(GeoCellRing, TheMiddleComesFirst) {
  std::vector<uint32_t> ring = Ring(E7(50.9414), E7(6.9583), 1);
  ASSERT_FALSE(ring.empty());
  EXPECT_EQ(ring[0], geo_cell_of(E7(50.9414), E7(6.9583)));
}

TEST(GeoCellRing, ARadiusOfOneIsNineCells) {
  EXPECT_EQ(Ring(E7(50.0), E7(6.0), 1).size(), 9u);
  EXPECT_EQ(Ring(E7(50.0), E7(6.0), 0).size(), 1u);
  EXPECT_EQ(Ring(E7(50.0), E7(6.0), 2).size(), 25u);
}

TEST(GeoCellRing, NoCellAppearsTwice) {
  for (double lat : {0.0, 50.0, 89.95, -89.95}) {
    for (double lon : {0.0, 6.0, 179.95, -179.95}) {
      std::vector<uint32_t> ring = Ring(E7(lat), E7(lon), 1);
      std::set<uint32_t> distinct(ring.begin(), ring.end());
      EXPECT_EQ(distinct.size(), ring.size()) << lat << ", " << lon;
    }
  }
}

TEST(GeoCellRing, ARingHoldsItsNeighbours) {
  std::vector<uint32_t> ring = Ring(E7(50.05), E7(6.05), 1);
  for (double lat : {49.95, 50.05, 50.15}) {
    for (double lon : {5.95, 6.05, 6.15}) {
      uint32_t neighbour = geo_cell_of(E7(lat), E7(lon));
      EXPECT_NE(std::find(ring.begin(), ring.end(), neighbour), ring.end())
          << lat << ", " << lon;
    }
  }
}

TEST(GeoCellRing, AtThePoleTheRingIsShorterRatherThanWrong) {
  std::vector<uint32_t> ring = Ring(E7(89.99), E7(0.0), 1);
  EXPECT_LT(ring.size(), 9u) << "there is no row north of the pole";
  EXPECT_GT(ring.size(), 0u);
}

TEST(GeoCellRing, AtTheDatelineTheRingReachesAroundTheWorld) {
  std::vector<uint32_t> ring = Ring(E7(0.0), E7(179.99), 1);
  EXPECT_EQ(ring.size(), 9u);
  uint32_t across = geo_cell_of(E7(0.0), E7(-179.99));
  EXPECT_NE(std::find(ring.begin(), ring.end(), across), ring.end())
      << "the cell on the other side of the line is a neighbour";
}

TEST(GeoCellRing, ATooSmallArrayIsFilledAsFarAsItReaches) {
  uint32_t cells[4];
  size_t count = geo_cell_ring(cells, 4, E7(50.0), E7(6.0), 1);
  EXPECT_EQ(count, 4u);
  EXPECT_EQ(cells[0], geo_cell_of(E7(50.0), E7(6.0))) << "and the middle is what it keeps";
}

TEST(GeoCellRing, NoRoomAtAllIsNoCells) {
  uint32_t cells[1];
  EXPECT_EQ(geo_cell_ring(cells, 0, E7(50.0), E7(6.0), 1), 0u);
  EXPECT_EQ(geo_cell_ring(nullptr, 4, E7(50.0), E7(6.0), 1), 0u);
}

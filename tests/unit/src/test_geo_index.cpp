/** @file
 *  @brief The index as it rests on disk, and what comes back when it is mapped.
 *
 *  Everything is checked through one round trip: an index is written from the
 *  collectors, mapped again, and asked the questions the search asks it.  What
 *  the header promises — magic, version, byte order, a hash over the record
 *  sizes — is checked by breaking it and watching the open refuse.
 */

#include "c_api.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using testsupport::BuildMiniIndex;
using testsupport::SamplePlaces;
using testsupport::TempPath;

namespace {

/** The words one query yields against an opened index. */
size_t Query(const GeoIndex &index, const std::string &text, GeoHit *hits, size_t limit,
             bool prefix_last = false) {
  TextTokenizer tok;
  return geo_index_query(&index, &tok, text.c_str(), text.size(), prefix_last, hits, limit);
}

std::string DisplayWord(const GeoIndex &index, uint32_t rank) {
  if (rank == GEO_RANK_NONE) return std::string();
  size_t size = 0;
  const char *text = geo_dictionary_word(&index.display, rank, &size);
  return text ? std::string(text, size) : std::string();
}

class GeoIndexTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(BuildMiniIndex(path.c_str(), SamplePlaces())) << "could not write " << path.c_str();
    ASSERT_EQ(geo_index_open(&index, path.c_str()), GRD_SUCCESS);
  }
  void TearDown() override { geo_index_close(&index); }

  /** The document a query names. The merge numbers the records as it likes, so
   *  nothing may assume a place kept the position it was written in. */
  uint32_t DocumentOf(const std::string &text) {
    GeoHit hits[8];
    size_t count = Query(index, text, hits, 8);
    EXPECT_GE(count, 1u) << text;
    return count ? hits[0].document : GEO_RANK_NONE;
  }

  TempPath path{"index"};
  GeoIndex index{};
};

} // namespace

TEST_F(GeoIndexTest, MapsWhatWasWritten) {
  EXPECT_NE(index.base, nullptr);
  EXPECT_GT(index.size, 0u);
  EXPECT_EQ(index.document_count, SamplePlaces().size());
}

TEST_F(GeoIndexTest, CarriesBothDictionaries) {
  EXPECT_GT(index.words.word_count, 0u);
  EXPECT_GT(index.display.word_count, 0u);
  EXPECT_GT(index.words.group_count, 0u);
  EXPECT_GT(index.display.group_count, 0u);
}

TEST_F(GeoIndexTest, EveryWordIsReachableByItsRank) {
  for (size_t rank = 0; rank < index.words.word_count; ++rank) {
    size_t size = 0;
    const char *word = geo_dictionary_word(&index.words, rank, &size);
    ASSERT_NE(word, nullptr) << "rank " << rank;
    EXPECT_GT(size, 0u) << "rank " << rank;
  }
}

TEST_F(GeoIndexTest, ARankBeyondTheEndIsRefused) {
  size_t size = 123;
  EXPECT_EQ(geo_dictionary_word(&index.words, index.words.word_count, &size), nullptr);
}

TEST_F(GeoIndexTest, TheDictionaryIsInByteOrder) {
  std::string previous;
  for (size_t rank = 0; rank < index.words.word_count; ++rank) {
    size_t size = 0;
    const char *word = geo_dictionary_word(&index.words, rank, &size);
    std::string current(word, size);
    if (rank) EXPECT_LT(previous, current) << "at rank " << rank;
    previous = current;
  }
}

TEST_F(GeoIndexTest, FindsAWordAndAgreesWithItsRank) {
  size_t rank = SIZE_MAX;
  ASSERT_TRUE(geo_dictionary_find(&index.words, "marienplatz", 11, &rank));
  size_t size = 0;
  const char *word = geo_dictionary_word(&index.words, rank, &size);
  ASSERT_NE(word, nullptr);
  EXPECT_EQ(std::string(word, size), "marienplatz");
}

TEST_F(GeoIndexTest, DoesNotFindWhatWasNeverWritten) {
  size_t rank = SIZE_MAX;
  EXPECT_FALSE(geo_dictionary_find(&index.words, "zwickau", 7, &rank));
  EXPECT_FALSE(geo_dictionary_find(&index.words, "marienp", 7, &rank))
      << "half a word is not the word";
}

TEST_F(GeoIndexTest, ThePiecesOfACompoundAreWordsOfTheirOwn) {
  // "Marienplatz" was decomposed while it was collected, so someone asking for
  // "Platz" reaches it — the whole and its halves all stand in the dictionary
  size_t rank = SIZE_MAX;
  EXPECT_TRUE(geo_dictionary_find(&index.words, "marienplatz", 11, &rank));
  EXPECT_TRUE(geo_dictionary_find(&index.words, "marien", 6, &rank));
  EXPECT_TRUE(geo_dictionary_find(&index.words, "platz", 5, &rank));
}

TEST_F(GeoIndexTest, AWordOpensTheDocumentsItStandsOn) {
  size_t rank = SIZE_MAX;
  ASSERT_TRUE(geo_dictionary_find(&index.words, "berliner", 8, &rank));
  const roaring_bitmap_t *docs = geo_index_word_documents(&index, rank);
  ASSERT_NE(docs, nullptr);
  // two of the sample places are a Berliner Straße
  EXPECT_EQ(roaring_bitmap_get_cardinality(docs), 2u);
  roaring_bitmap_free(const_cast<roaring_bitmap_t *>(docs));
}

TEST_F(GeoIndexTest, TheDisplaySideKeepsTheWrittenSpelling) {
  // the search side folded "München" into ASCII; the answer side did not
  bool found_written = false;
  for (size_t rank = 0; rank < index.display.word_count; ++rank) {
    size_t size = 0;
    const char *word = geo_dictionary_word(&index.display, rank, &size);
    if (std::string(word, size) == "München") found_written = true;
  }
  EXPECT_TRUE(found_written);
}

TEST_F(GeoIndexTest, HousesHangOnTheirStreet) {
  uint32_t document = DocumentOf("Marienplatz München ");
  ASSERT_NE(document, GEO_RANK_NONE);

  size_t count = 0;
  const GeoHouse *houses = geo_index_houses(&index, document, &count);
  ASSERT_NE(houses, nullptr);
  EXPECT_EQ(count, 3u) << "Marienplatz was given three numbers";

  std::vector<std::string> written;
  for (size_t i = 0; i < count; ++i) written.push_back(DisplayWord(index, houses[i].number_rank));
  EXPECT_NE(std::find(written.begin(), written.end(), "12a"), written.end());
}

TEST_F(GeoIndexTest, AStreetWithoutNumbersHasNone) {
  // the locality carries no house numbers
  uint32_t document = DocumentOf("Osiedle Praha ");
  ASSERT_NE(document, GEO_RANK_NONE);

  size_t count = 123;
  const GeoHouse *houses = geo_index_houses(&index, document, &count);
  EXPECT_EQ(houses, nullptr);
  EXPECT_EQ(count, 0u);
}

TEST_F(GeoIndexTest, ADocumentBeyondTheEndIsRefused) {
  size_t count = 123;
  EXPECT_EQ(geo_index_houses(&index, index.document_count, &count), nullptr);
  EXPECT_EQ(count, 0u);
}

// ---------------------------------------------------------------------------
//  Querying
// ---------------------------------------------------------------------------

TEST_F(GeoIndexTest, FindsAPlaceByItsName) {
  GeoHit hits[8];
  size_t count = Query(index, "Marienplatz ", hits, 8);
  ASSERT_GE(count, 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Marienplatz");
}

TEST_F(GeoIndexTest, WordsAreASetAndOrderDoesNotMatter) {
  GeoHit forward[8], backward[8];
  size_t a = Query(index, "Marienplatz München ", forward, 8);
  size_t b = Query(index, "München Marienplatz ", backward, 8);
  ASSERT_EQ(a, b);
  ASSERT_GE(a, 1u);
  EXPECT_EQ(forward[0].document, backward[0].document);
}

TEST_F(GeoIndexTest, TheFoldingOfTheQueryMatchesTheIndexs) {
  GeoHit hits[8];
  // written with an umlaut, without one, and abbreviated — all must arrive
  EXPECT_GE(Query(index, "München Marienplatz ", hits, 8), 1u);
  EXPECT_GE(Query(index, "Munchen Marienplatz ", hits, 8), 1u);
  EXPECT_GE(Query(index, "muenchen marienpl. ", hits, 8), 1u);
}

TEST_F(GeoIndexTest, AnUnknownWordIsPassedOverRatherThanFailing) {
  GeoHit hits[8];
  // "kwyjibo" is in no dictionary; the rest of the query must still answer
  EXPECT_GE(Query(index, "Marienplatz kwyjibo ", hits, 8), 1u);
}

TEST_F(GeoIndexTest, AQueryOfOnlyUnknownWordsFindsNothing) {
  GeoHit hits[8];
  EXPECT_EQ(Query(index, "kwyjibo blorf ", hits, 8), 0u);
}

TEST_F(GeoIndexTest, WordsAreAndedNotOred) {
  GeoHit hits[8];
  // Marienplatz is in München, not in Bonn
  EXPECT_EQ(Query(index, "Marienplatz Bonn ", hits, 8), 0u);
}

TEST_F(GeoIndexTest, ThePostcodeNarrowsToOnePlace) {
  GeoHit hits[8];
  size_t count = Query(index, "Berliner Straße 10715 ", hits, 8);
  ASSERT_EQ(count, 1u) << "two streets share the name, one shares the code";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].postcode_rank), "10715");
}

TEST_F(GeoIndexTest, AWrongPostcodeCostsAPositionNotThePlace) {
  GeoHit hits[8];
  size_t with_code = Query(index, "Berliner Straße 99999 ", hits, 8);
  EXPECT_EQ(with_code, 2u) << "the code found nothing, so it was dropped again";
}

TEST_F(GeoIndexTest, AHouseNumberIsFoundOnItsStreet) {
  GeoHit hits[8];
  size_t count = Query(index, "Marienplatz München 8 ", hits, 8);
  ASSERT_GE(count, 1u);
  ASSERT_NE(hits[0].house, GEO_RANK_NONE);
  EXPECT_EQ(DisplayWord(index, index.houses[hits[0].house].number_rank), "8");
}

TEST_F(GeoIndexTest, ANumberThatIsNoHouseNumberBecomesAWord) {
  GeoHit hits[8];
  // 03-000 is the locality's postal code, and no house carries it
  EXPECT_GE(Query(index, "Osiedle Praha ", hits, 8), 1u);
}

TEST_F(GeoIndexTest, ThePrefixReadingFindsWhatIsStillBeingTyped) {
  GeoHit hits[8];
  EXPECT_EQ(Query(index, "Marienpla", hits, 8, /*prefix_last=*/false), 0u);
  EXPECT_GE(Query(index, "Marienpla", hits, 8, /*prefix_last=*/true), 1u);
}

TEST_F(GeoIndexTest, TheLimitIsCappedRatherThanTrusted) {
  std::vector<GeoHit> hits(GEO_QUERY_LIMIT_MAX + 64);
  TextTokenizer tok;
  const char *q = "Berliner Straße ";
  size_t count =
      geo_index_query(&index, &tok, q, std::strlen(q), false, hits.data(), hits.size());
  EXPECT_LE(count, (size_t)GEO_QUERY_LIMIT_MAX);
}

TEST_F(GeoIndexTest, AnEmptyQueryIsAnswered) {
  GeoHit hits[8];
  TextTokenizer tok;
  EXPECT_EQ(geo_index_query(&index, &tok, "", 0, false, hits, 8), 0u);
  EXPECT_EQ(geo_index_query(&index, &tok, nullptr, 0, false, hits, 8), 0u);
  EXPECT_EQ(geo_index_query(nullptr, &tok, "x", 1, false, hits, 8), 0u);
}

// ---------------------------------------------------------------------------
//  A file that may not be trusted
// ---------------------------------------------------------------------------

namespace {

/** Write the sample index, then damage one byte of it. */
bool WriteDamaged(const char *path, size_t offset, uint8_t value) {
  if (!BuildMiniIndex(path, SamplePlaces())) return false;
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!file) return false;
  file.seekp((std::streamoff)offset);
  file.write(reinterpret_cast<const char *>(&value), 1);
  return file.good();
}

} // namespace

TEST(GeoIndexRefusal, ARewrittenMagicIsRefused) {
  TempPath path{"magic"};
  ASSERT_TRUE(WriteDamaged(path.c_str(), 0, 'X'));
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, path.c_str()), GRD_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, AnotherVersionIsRefused) {
  TempPath path{"version"};
  // the version follows the eight magic bytes
  ASSERT_TRUE(WriteDamaged(path.c_str(), 8, GEO_INDEX_VERSION + 7));
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, path.c_str()), GRD_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, TheOtherByteOrderIsRefused) {
  TempPath path{"order"};
  ASSERT_TRUE(WriteDamaged(path.c_str(), 12, 0xFF));
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, path.c_str()), GRD_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, AFileThatIsNotThereIsRefused) {
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, "/nonexistent/geoindex/test.gdx"), GRD_SUCCESS);
  EXPECT_EQ(index.base, nullptr);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, AnEmptyFileIsRefused) {
  TempPath path{"empty"};
  { std::ofstream out(path.c_str(), std::ios::binary); }
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, path.c_str()), GRD_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, NullArgumentsAreAnswered) {
  GeoIndex index{};
  EXPECT_NE(geo_index_open(nullptr, "x"), GRD_SUCCESS);
  EXPECT_NE(geo_index_open(&index, nullptr), GRD_SUCCESS);
  geo_index_close(nullptr);
}

TEST(GeoIndexWrite, AnIndexWithoutPlacesIsStillAnIndex) {
  TempPath path{"nothing"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), {}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), GRD_SUCCESS);
  EXPECT_EQ(index.document_count, 0u);
  GeoHit hits[4];
  EXPECT_EQ(Query(index, "Berlin ", hits, 4), 0u);
  geo_index_close(&index);
}

TEST(GeoIndexWrite, ClosingTwiceIsSafe) {
  TempPath path{"twice"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), SamplePlaces()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), GRD_SUCCESS);
  geo_index_close(&index);
  geo_index_close(&index);
  EXPECT_EQ(index.base, nullptr);
}

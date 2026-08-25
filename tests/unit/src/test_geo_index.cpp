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
size_t Query(
    const GeoIndex &index,
    const std::string &text,
    GeoHit *hits,
    size_t limit,
    bool prefix_last = false
) {
  TextTokenizer tok;
  return geo_index_query(&index, &tok, text.c_str(), text.size(), prefix_last, hits, limit);
}

/** The same query, with a place for it to write down what it touched. */
size_t QueryStats(
    const GeoIndex &index,
    const std::string &text,
    GeoHit *hits,
    size_t limit,
    GeoQueryStats *stats,
    bool prefix_last = false
) {
  TextTokenizer tok;
  GeoQueryOptions options{};
  options.prefix_last = prefix_last;
  return geo_index_query_options(
      &index, &tok, text.c_str(), text.size(), &options, hits, limit, stats
  );
}

/** Degrees as the index keeps them. */
constexpr int32_t E7(double degrees) {
  return (int32_t)(degrees * 1.0e7);
}

/** The same query again, asked from somewhere. */
size_t QueryFrom(
    const GeoIndex &index,
    const std::string &text,
    double latitude,
    double longitude,
    GeoHit *hits,
    size_t limit,
    GeoQueryStats *stats = nullptr
) {
  TextTokenizer tok;
  GeoQueryOptions options{};
  options.has_position = true;
  options.latitude_e7 = E7(latitude);
  options.longitude_e7 = E7(longitude);
  return geo_index_query_options(
      &index, &tok, text.c_str(), text.size(), &options, hits, limit, stats
  );
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
    ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);
  }
  void TearDown() override {
    geo_index_close(&index);
  }

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
  size_t count = geo_index_query(&index, &tok, q, std::strlen(q), false, hits.data(), hits.size());
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
//  What the query says afterwards about the way it went
// ---------------------------------------------------------------------------

TEST_F(GeoIndexTest, TheCountsFollowTheStepsOfTheSearch) {
  GeoHit hits[8];
  GeoQueryStats stats{};
  size_t count = QueryStats(index, "Marienplatz München ", hits, 8, &stats);
  ASSERT_GE(count, 1u);

  EXPECT_EQ(stats.results, count) << "what was written is what was counted";
  EXPECT_EQ(stats.passes, 1u) << "the words answered on the first reading";
  EXPECT_EQ(stats.groups, 2u) << "two words narrowed";
  EXPECT_GE(stats.posting_lists, stats.groups) << "every word that narrowed opened a list";
  EXPECT_GE(stats.posting_documents, stats.narrowed) << "narrowing never adds documents";
  EXPECT_GE(stats.narrowed, stats.weighed);
  EXPECT_GE(stats.weighed, stats.results);
}

TEST_F(GeoIndexTest, AskingForTheCountsChangesNoAnswer) {
  GeoHit with[8], without[8];
  GeoQueryStats stats{};
  size_t counted = QueryStats(index, "Berliner Straße ", with, 8, &stats);
  size_t plain = Query(index, "Berliner Straße ", without, 8);
  ASSERT_EQ(counted, plain);
  for (size_t i = 0; i < counted; ++i) EXPECT_EQ(with[i].document, without[i].document);
}

TEST_F(GeoIndexTest, ABeginningIsCountedByTheWordsItCovers) {
  GeoHit hits[8];
  GeoQueryStats typed{}, finished{};
  ASSERT_GE(QueryStats(index, "Marienpla", hits, 8, &typed, /*prefix_last=*/true), 1u);
  EXPECT_GT(typed.prefix_terms, 0u) << "the beginning reached into the dictionary";
  EXPECT_EQ(typed.prefix_refused, 0u);

  QueryStats(index, "Marienplatz ", hits, 8, &finished, /*prefix_last=*/false);
  EXPECT_EQ(finished.prefix_terms, 0u) << "a finished word is looked up, not expanded";
}

TEST_F(GeoIndexTest, ADroppedPostcodeShowsAsASecondPass) {
  GeoHit hits[8];
  GeoQueryStats stats{};
  // 53111 is Bonn's code — a real word of this index, but no Berliner Straße
  // carries it.  The first reading narrows to nothing and the code is dropped.
  ASSERT_GE(QueryStats(index, "Berliner Straße 53111 ", hits, 8, &stats), 1u);
  EXPECT_EQ(stats.passes, 2u);
  EXPECT_EQ(stats.groups, 2u) << "the second reading kept the words and left the number";
}

TEST_F(GeoIndexTest, ACodeNoOneEverWroteCostsNoSecondPass) {
  GeoHit hits[8];
  GeoQueryStats stats{};
  // 99999 stands in no dictionary, so it narrows nothing and nothing is dropped
  ASSERT_GE(QueryStats(index, "Berliner Straße 99999 ", hits, 8, &stats), 1u);
  EXPECT_EQ(stats.passes, 1u);
  EXPECT_EQ(stats.groups, 2u) << "the number found no reading and simply stood aside";
}

TEST_F(GeoIndexTest, RefusedArgumentsLeaveTheCountsAtZero) {
  GeoHit hits[8];
  GeoQueryStats stats{};
  stats.results = 4711; // whatever stood here may not survive the call
  EXPECT_EQ(QueryStats(index, "", hits, 8, &stats), 0u);
  EXPECT_EQ(stats.results, 0u);
  EXPECT_EQ(stats.passes, 0u) << "nothing was ever read";
  EXPECT_EQ(stats.posting_lists, 0u);
}

TEST_F(GeoIndexTest, AQueryThatFindsNothingStillSaysWhereItLooked) {
  GeoHit hits[8];
  GeoQueryStats stats{};
  EXPECT_EQ(QueryStats(index, "Marienplatz Warschau ", hits, 8, &stats), 0u);
  EXPECT_GT(stats.passes, 0u);
  EXPECT_GT(stats.posting_lists, 0u) << "both words exist; they only never met";
  EXPECT_EQ(stats.narrowed, 0u);
  EXPECT_EQ(stats.results, 0u);
}

// ---------------------------------------------------------------------------
//  Where the searcher stands
// ---------------------------------------------------------------------------

TEST_F(GeoIndexTest, ThePositionDecidesBetweenTwoStreetsOfOneName) {
  GeoHit hits[8];
  // Berlin and Potsdam both have a Berliner Straße, and Potsdam's is the
  // heavier of the two — weight alone would answer with it every time
  ASSERT_GE(QueryFrom(index, "Berliner Straße ", 52.4869, 13.3283, hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Berlin");

  ASSERT_GE(QueryFrom(index, "Berliner Straße ", 52.3956, 13.0649, hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Potsdam");
}

TEST_F(GeoIndexTest, TheHeavierPlaceStillWinsWithinOneBand) {
  GeoHit hits[8];
  // both Berliner Straßen lie within the widest band of a searcher far away;
  // inside a band nothing about distance is said, so weight decides as before
  size_t count = QueryFrom(index, "Berliner Straße ", 48.1374, 11.5755, hits, 8);
  ASSERT_EQ(count, 2u);
  EXPECT_GE(hits[0].importance, hits[1].importance);
}

TEST_F(GeoIndexTest, ANamedTownOutweighsWhereTheSearcherStands) {
  GeoHit hits[8];
  // standing in Berlin and asking for Potsdam's: what was said outright wins
  ASSERT_GE(QueryFrom(index, "Berliner Straße Potsdam ", 52.4869, 13.3283, hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Potsdam");
}

TEST_F(GeoIndexTest, ThePositionNarrowsBeforeItSorts) {
  GeoHit hits[8];
  GeoQueryStats stats{};
  ASSERT_GE(QueryFrom(index, "Berliner Straße ", 52.4869, 13.3283, hits, 8, &stats), 1u);
  EXPECT_GT(stats.near_cells, 0u) << "the ring found cells that hold places";
  EXPECT_GT(stats.near_documents, 0u);
  EXPECT_EQ(stats.position_dropped, 0u);
  EXPECT_EQ(stats.narrowed, 1u) << "one of the two survived the ring, not both";
}

TEST_F(GeoIndexTest, APositionInTheOceanIsLetGoOfRatherThanObeyed) {
  GeoHit hits[8];
  GeoQueryStats stats{};
  // nothing in this index stands anywhere near the middle of the Pacific
  size_t count = QueryFrom(index, "Berliner Straße ", -30.0, -140.0, hits, 8, &stats);
  EXPECT_EQ(count, 2u) << "the words are answered without the position";
  EXPECT_EQ(stats.near_cells, 0u);
  EXPECT_EQ(stats.near_documents, 0u);
  EXPECT_EQ(stats.position_dropped, 1u);
  EXPECT_EQ(stats.passes, 1u) << "an empty ring is let go of before a reading is spent on it";
}

TEST_F(GeoIndexTest, APositionAloneIsNoQuery) {
  GeoHit hits[8];
  // words nobody wrote, from a place full of documents: the ring may not stand
  // in for what was typed
  EXPECT_EQ(QueryFrom(index, "Kwyjibo Blorf ", 52.4869, 13.3283, hits, 8), 0u);
}

TEST(GeoIndexNear, AFormerNameDoesNotOutrunTheCurrentOneJustByStandingCloser) {
  // exactly the Bonn case: the Friedrich-Breuer-Straße was once the Hauptstraße
  // and lies nearer to the searcher than the street that is called that today
  std::vector<testsupport::MiniPlace> places = {
      {"Friedrich-Breuer-Straße",
       "Bonn",
       "53225",
       507391765,
       71194806,
       PHOTON_PLACE_TYPE_STREET,
       3500,
       {},
       true,
       {"Hauptstraße"}},
      {"Hauptstraße", "Bonn", "53229", 507424804, 71783745, PHOTON_PLACE_TYPE_STREET, 3500},
  };
  TempPath path{"formername"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(QueryFrom(index, "Hauptstraße ", 50.7350, 7.0980, hits, 8), 2u)
      << "both answer to the word, and both are found";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Hauptstraße")
      << "what a place is called now outranks what it used to be called";

  // the former name is still an answer — it only stands second
  EXPECT_EQ(
      DisplayWord(index, index.documents[hits[1].document].name_rank), "Friedrich-Breuer-Straße"
  );
  geo_index_close(&index);
}

TEST(GeoIndexNear, WithoutAPositionTheNameIsNotWeighedAtAll) {
  // the same two places, asked without a position: weight decides as it always
  // has, and the former name is worth exactly as much as the current one
  std::vector<testsupport::MiniPlace> places = {
      {"Friedrich-Breuer-Straße",
       "Bonn",
       "53225",
       507391765,
       71194806,
       PHOTON_PLACE_TYPE_STREET,
       9000,
       {},
       true,
       {"Hauptstraße"}},
      {"Hauptstraße", "Bonn", "53229", 507424804, 71783745, PHOTON_PLACE_TYPE_STREET, 3500},
  };
  TempPath path{"formernameplain"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Hauptstraße ", hits, 8), 2u);
  EXPECT_EQ(
      DisplayWord(index, index.documents[hits[0].document].name_rank), "Friedrich-Breuer-Straße"
  ) << "the heavier of the two, however it came by the word";
  geo_index_close(&index);
}

TEST(GeoIndexNear, APlaceWithoutACoordinateIsRankedLastRatherThanLost) {
  // the dump does give entries without a centroid; they stand nowhere, so no
  // ring can hold them — but the words still name them
  std::vector<testsupport::MiniPlace> places = {
      {"Feldweg", "Bonn", "53111", 507350000, 70980000, PHOTON_PLACE_TYPE_STREET, 500, {}},
      {"Feldweg", "Nirgendwo", "00000", 0, 0, PHOTON_PLACE_TYPE_STREET, 60000, {}, false},
  };
  TempPath path{"nopoint"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  GeoQueryStats stats{};
  size_t count = QueryFrom(index, "Feldweg ", 50.735, 7.098, hits, 8, &stats);
  ASSERT_EQ(count, 1u) << "a place standing nowhere is in no ring";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Bonn")
      << "and the one that does stand somewhere is the lighter of the two";

  // asked without a position, both are there and weight orders them again
  GeoHit plain[8];
  ASSERT_EQ(Query(index, "Feldweg ", plain, 8), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[plain[0].document].city_rank), "Nirgendwo");
  geo_index_close(&index);
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
  EXPECT_NE(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, AnotherVersionIsRefused) {
  TempPath path{"version"};
  // the version follows the eight magic bytes
  ASSERT_TRUE(WriteDamaged(path.c_str(), 8, GEO_INDEX_VERSION + 7));
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, TheOtherByteOrderIsRefused) {
  TempPath path{"order"};
  ASSERT_TRUE(WriteDamaged(path.c_str(), 12, 0xFF));
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, AFileThatIsNotThereIsRefused) {
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, "/nonexistent/geoindex/test.gdx"), ARNM_SUCCESS);
  EXPECT_EQ(index.base, nullptr);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, AnEmptyFileIsRefused) {
  TempPath path{"empty"};
  { std::ofstream out(path.c_str(), std::ios::binary); }
  GeoIndex index{};
  EXPECT_NE(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);
  geo_index_close(&index);
}

TEST(GeoIndexRefusal, NullArgumentsAreAnswered) {
  GeoIndex index{};
  EXPECT_NE(geo_index_open(nullptr, "x"), ARNM_SUCCESS);
  EXPECT_NE(geo_index_open(&index, nullptr), ARNM_SUCCESS);
  geo_index_close(nullptr);
}

TEST(GeoIndexWrite, AnIndexWithoutPlacesIsStillAnIndex) {
  TempPath path{"nothing"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), {}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);
  EXPECT_EQ(index.document_count, 0u);
  GeoHit hits[4];
  EXPECT_EQ(Query(index, "Berlin ", hits, 4), 0u);
  geo_index_close(&index);
}

TEST(GeoIndexWrite, ClosingTwiceIsSafe) {
  TempPath path{"twice"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), SamplePlaces()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);
  geo_index_close(&index);
  geo_index_close(&index);
  EXPECT_EQ(index.base, nullptr);
}

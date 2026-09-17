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
    GeoQueryStats *stats = nullptr,
    bool prefix_last = false
) {
  TextTokenizer tok;
  GeoQueryOptions options{};
  options.prefix_last = prefix_last;
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

TEST(GeoIndexTown, ATownFiledAsACountyStillAnswersToItsOwnName) {
  // the planet's Würzburg: a kreisfreie Stadt, filed as a county and carrying
  // no town of its own, while the villages around it carry theirs — and one of
  // those spells the city in its name.  Weight alone would put the city first.
  std::vector<testsupport::MiniPlace> places = {
      {"Würzburg", "", "", 497780356, 99434769, PHOTON_PLACE_TYPE_COUNTY, 43929},
      {"Neubrunn bei Würzburg",
       "Neubrunn bei Würzburg",
       "97277",
       497307037,
       96723536,
       PHOTON_PLACE_TYPE_CITY,
       29626},
      {"Residenzplatz", "Würzburg", "97070", 497929124, 99382411, PHOTON_PLACE_TYPE_STREET, 21888},
  };
  TempPath path{"countytown"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Würzburg ", hits, 8), 3u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Würzburg")
      << "the city that was typed, not a village that mentions it";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[1].document].name_rank), "Residenzplatz")
      << "a square in the city, lighter, still before the village beside it";
  EXPECT_EQ(
      DisplayWord(index, index.documents[hits[2].document].name_rank), "Neubrunn bei Würzburg"
  );
  geo_index_close(&index);
}

TEST(GeoIndexTown, ATownNamedBesideAnotherStandsBehindTheTownItself) {
  // the village street weighs more, and its town holds Würzburg as well — but
  // only as the place it lies beside, so the city's own street comes first
  std::vector<testsupport::MiniPlace> places = {
      {"Schulstraße",
       "Hausen bei Würzburg",
       "97262",
       499270599,
       100264921,
       PHOTON_PLACE_TYPE_STREET,
       9000},
      {"Schulstraße", "Würzburg", "97084", 497700000, 99300000, PHOTON_PLACE_TYPE_STREET, 3500},
  };
  TempPath path{"townbeside"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Schulstraße Würzburg ", hits, 8), 2u)
      << "the village is still an answer";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Würzburg");
  EXPECT_EQ(
      DisplayWord(index, index.documents[hits[1].document].city_rank), "Hausen bei Würzburg"
  );
  geo_index_close(&index);
}

TEST(GeoIndexTown, AWordBeyondTheSixtyFourthOfATownIsNotCounted) {
  // a word repeated sixty-four times leaves a single token behind, so the one
  // after it is only the second token yet the sixty-fifth word — group 64,
  // beyond the bits town_agreement() keeps.  It may not earn the town anything.
  std::string far_town;
  for (int w = 0; w < 64; ++w) far_town += "x ";
  far_town += "Zielort";

  std::vector<testsupport::MiniPlace> places = {
      {"Feldweg", far_town, "", 507350000, 70980000, PHOTON_PLACE_TYPE_STREET, 1000},
      {"Feldweg",
       "Anderswo",
       "",
       535500000,
       100000000,
       PHOTON_PLACE_TYPE_STREET,
       9000,
       {},
       true,
       {"Zielort"}},
  };
  TempPath path{"townfarword"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Feldweg Zielort ", hits, 8), 2u)
      << "both carry the word, one in its town and one among its other names";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Anderswo")
      << "neither town counts, so weight decides";
  geo_index_close(&index);
}

TEST(GeoIndexTown, APrefixOrASuffixStillNamesTheTown) {
  // one word in front (Den Haag) or a qualifier behind (Halle (Saale),
  // Frankfurt am Main) is still the town's own name: weight decides between
  // them and the smaller town that bears the bare word, as it always did
  std::vector<testsupport::MiniPlace> places = {
      {"Den Haag", "Den Haag", "", 520799838, 43113461, PHOTON_PLACE_TYPE_CITY, 47760},
      {"Haag", "Haag", "3350", 481120000, 145650000, PHOTON_PLACE_TYPE_CITY, 28384},
      {"Halle (Saale)", "Halle (Saale)", "", 514824354, 119712985, PHOTON_PLACE_TYPE_CITY, 43648},
      {"Halle", "Halle", "37620", 519913559, 95634922, PHOTON_PLACE_TYPE_CITY, 27151},
      {"Frankfurt am Main",
       "Frankfurt am Main",
       "",
       501106444,
       86820917,
       PHOTON_PLACE_TYPE_CITY,
       49643},
      {"Frankfurt (Oder)", "Frankfurt (Oder)", "", 523412273, 145494520, PHOTON_PLACE_TYPE_CITY,
       40527},
  };
  TempPath path{"townprefix"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Haag ", hits, 8), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Den Haag");

  // the qualifier behind may not cost the town its name: were it to, the
  // smaller Halle, which carries none, would stand before Halle (Saale)
  ASSERT_EQ(Query(index, "Halle ", hits, 8), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Halle (Saale)");

  // two different cities, both with a qualifier behind: both are named, and
  // weight alone decides — this one would hold even if qualifiers cost something
  ASSERT_EQ(Query(index, "Frankfurt ", hits, 8), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Frankfurt am Main");
  geo_index_close(&index);
}

TEST(GeoIndexNear, ACityBeyondTheRingIsNotHiddenByAStreetNamedAfterIt) {
  // the planet's case: Würzburg typed in Berlin.  Read as a beginning, the word
  // meets the Würzburger Straße there, so the ring holds — and the city, far
  // outside it, would never have been a candidate at all
  std::vector<testsupport::MiniPlace> places = {
      {"Würzburger Straße",
       "Berlin",
       "10789",
       524990000,
       133380000,
       PHOTON_PLACE_TYPE_STREET,
       3501},
      {"Würzburg", "", "", 497780356, 99434769, PHOTON_PLACE_TYPE_COUNTY, 43929},
  };
  TempPath path{"farcity"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  GeoQueryStats stats{};
  ASSERT_EQ(QueryFrom(index, "Würzburg", 52.499, 13.338, hits, 8, &stats, true), 2u)
      << "the city joins the street the ring found";
  EXPECT_EQ(stats.position_dropped, 0u) << "the ring held; the city came from beyond it";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Würzburg")
      << "a named city of weight stands before a street that only carries its name";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[1].document].name_rank), "Würzburger Straße");

  // asked from Würzburg itself, the city is inside the ring and found there once
  ASSERT_EQ(QueryFrom(index, "Würzburg", 49.778, 9.943, hits, 8, nullptr, true), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Würzburg");
  geo_index_close(&index);
}

TEST(GeoIndexNear, ACityWithAQualifierBehindItIsTakenInFromBeyondTheRingToo) {
  // the city is named by its first word, not by all of them: Halle (Saale)
  // lacks one word of what was typed, Frankfurt am Main two — both still count
  // as named, and both have to come in from beyond the ring
  std::vector<testsupport::MiniPlace> places = {
      {"Hallesches Ufer", "Berlin", "10963", 525000000, 133800000, PHOTON_PLACE_TYPE_STREET, 14452},
      {"Halle (Saale)", "Halle (Saale)", "", 514824354, 119712985, PHOTON_PLACE_TYPE_CITY, 43648},
      {"Frankfurter Allee",
       "Berlin",
       "10247",
       525150000,
       134600000,
       PHOTON_PLACE_TYPE_STREET,
       29271},
      {"Frankfurt am Main",
       "Frankfurt am Main",
       "",
       501106444,
       86820917,
       PHOTON_PLACE_TYPE_CITY,
       49643},
  };
  TempPath path{"farqualifier"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(QueryFrom(index, "Halle", 52.50, 13.38, hits, 8, nullptr, true), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Halle (Saale)");
  ASSERT_EQ(QueryFrom(index, "Frankfurt", 52.50, 13.38, hits, 8, nullptr, true), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Frankfurt am Main");
  geo_index_close(&index);
}

TEST(GeoIndexNear, ALightPlaceBeyondTheRingDoesNotPushAsideWhatIsNear) {
  // a village named after a common word carries it as its own name, just as a
  // city would; only its weight tells it apart, and it weighs too little
  std::vector<testsupport::MiniPlace> places = {
      {"Bahnhofstraße", "Berlin", "12159", 524701016, 133396361, PHOTON_PLACE_TYPE_STREET, 3501},
      {"Gmünd-Bahnhof",
       "Gmünd-Bahnhof",
       "378 10",
       487685057,
       149636772,
       PHOTON_PLACE_TYPE_CITY,
       33409},
  };
  TempPath path{"farvillage"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(QueryFrom(index, "Bahnhof ", 52.47, 13.34, hits, 8), 1u)
      << "the village stays beyond the ring";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Bahnhofstraße");

  // without a position both answer, and the village is the heavier and named one
  ASSERT_EQ(Query(index, "Bahnhof ", hits, 8), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Gmünd-Bahnhof");
  geo_index_close(&index);
}

TEST(GeoIndexNear, ALighterTownTypedInFullIsTakenInFromBeyondTheRing) {
  // Gera weighs less than a city has to, and asked from Munich the Gerastraße
  // there held the ring; typed in full and named by nothing near, it comes in
  std::vector<testsupport::MiniPlace> places = {
      {"Gerastraße", "München", "80993", 481844000, 115192000, PHOTON_PLACE_TYPE_STREET, 3501},
      {"Gera", "Gera", "", 508766000, 120833000, PHOTON_PLACE_TYPE_CITY, 38608},
      // lighter still, a village stays out however fully it is typed
      {"Tannastraße", "München", "80993", 481850000, 115200000, PHOTON_PLACE_TYPE_STREET, 3501},
      {"Tanna", "Tanna", "", 504950000, 118580000, PHOTON_PLACE_TYPE_CITY, 21000},
  };
  TempPath path{"farlighttown"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(QueryFrom(index, "Gera", 48.1374, 11.5755, hits, 8, nullptr, true), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Gera");
  ASSERT_EQ(QueryFrom(index, "Tanna", 48.1374, 11.5755, hits, 8, nullptr, true), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Tannastraße");
  geo_index_close(&index);
}

TEST(GeoIndexNear, ANearbyPlaceThatBeginsWithTheWordKeepsItFromALighterTown) {
  // Cologne's Neustadt/Süd carries the word first; Neustadt in Holstein, named
  // by it but light, stays where it is
  std::vector<testsupport::MiniPlace> places = {
      {"Neustadt/Süd", "Köln", "", 509266000, 69404000, PHOTON_PLACE_TYPE_DISTRICT, 19678},
      {"Neustadt", "Neustadt", "23730", 541070000, 108150000, PHOTON_PLACE_TYPE_CITY, 32000},
  };
  TempPath path{"farbeginning"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(QueryFrom(index, "Neustadt", 50.9383, 6.9600, hits, 8, nullptr, true), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Neustadt/Süd");
  geo_index_close(&index);
}

TEST(GeoIndexNear, ALighterTownNamedByItsFirstWordComesInBehindTheState) {
  // *Brandenburg* from Munich: the state, then Brandenburg an der Havel, as
  // Google Maps answers — the Brandenburger Straße begins otherwise
  std::vector<testsupport::MiniPlace> places = {
      {"Brandenburger Straße", "München", "80805", 481719000, 115992000, PHOTON_PLACE_TYPE_STREET,
       2627},
      {"Brandenburg", "", "", 528455000, 132461000, PHOTON_PLACE_TYPE_STATE, 46464},
      {"Brandenburg an der Havel", "Brandenburg an der Havel", "", 524108000, 125498000,
       PHOTON_PLACE_TYPE_CITY, 38396},
  };
  TempPath path{"farfirstword"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(QueryFrom(index, "Brandenburg", 48.1374, 11.5755, hits, 8, nullptr, true), 3u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Brandenburg");
  EXPECT_EQ(
      DisplayWord(index, index.documents[hits[1].document].name_rank), "Brandenburg an der Havel"
  );
  geo_index_close(&index);
}

// ---------------------------------------------------------------------------
//  A country named beside the town
// ---------------------------------------------------------------------------

namespace {

/** A place of @p kind with a country, and nothing else the tests below do not need. */
testsupport::MiniPlace Placed(
    const std::string &name,
    const std::string &city,
    const std::string &postcode,
    double lat,
    double lon,
    uint8_t kind,
    uint16_t importance,
    const std::string &country
) {
  testsupport::MiniPlace place;
  place.name = name;
  place.city = city;
  place.postcode = postcode;
  place.lat_e7 = E7(lat);
  place.lon_e7 = E7(lon);
  place.type = kind;
  place.importance = importance;
  place.country = country;
  return place;
}

/** Two countries, a street of one name in each, and a square in Munich. */
std::vector<testsupport::MiniPlace> TwoCountries() {
  testsupport::MiniPlace germany =
      Placed("Deutschland", "", "", 51.08, 10.42, PHOTON_PLACE_TYPE_COUNTRY, 60000, "de");
  germany.readings = {{"en", "Germany", ""}};
  testsupport::MiniPlace austria =
      Placed("Österreich", "", "", 47.59, 14.12, PHOTON_PLACE_TYPE_COUNTRY, 59000, "at");
  austria.readings = {{"en", "Austria", ""}};
  return {
      germany,
      austria,
      Placed(
          "Marienplatz", "München", "80331", 48.1374, 11.5755, PHOTON_PLACE_TYPE_STREET, 30000, "de"
      ),
      Placed("Hauptstraße", "Bonn", "53111", 50.7350, 7.0980, PHOTON_PLACE_TYPE_STREET, 3500, "de"),
      // the heavier Hauptstraße is the Austrian one: weight alone would answer with it
      Placed("Hauptstraße", "Wien", "1010", 48.2083, 16.3725, PHOTON_PLACE_TYPE_STREET, 9000, "at"),
  };
}

} // namespace

TEST(GeoIndexCountry, AnAddressFollowedByItsCountryIsStillFound) {
  TempPath path{"countryaddress"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), TwoCountries(), {"de", "en"}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  // no place carries the word Deutschland, so every word meeting found nothing
  ASSERT_EQ(Query(index, "Marienplatz München Deutschland ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Marienplatz");
  // the country in another language of the index names it as well
  ASSERT_EQ(Query(index, "Marienplatz München Germany ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Marienplatz");
  // and read as a beginning, the way a map asks, it still does
  ASSERT_EQ(Query(index, "Marienplatz München Deutschland", hits, 8, true), 1u);
  geo_index_close(&index);
}

TEST(GeoIndexCountry, TheCountryNarrowsToThePlacesInIt) {
  TempPath path{"countrynarrows"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), TwoCountries(), {"de", "en"}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Hauptstraße Deutschland ", hits, 8), 1u)
      << "the Austrian Hauptstraße is left out, however heavy it is";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Bonn");
  ASSERT_EQ(Query(index, "Hauptstraße Österreich ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].city_rank), "Wien");
  geo_index_close(&index);
}

TEST(GeoIndexCountry, TheCountryAloneIsAPlaceLikeAnyOther) {
  TempPath path{"countryalone"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), TwoCountries(), {"de", "en"}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_GE(Query(index, "Deutschland ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Deutschland");
  EXPECT_EQ(index.documents[hits[0].document].type, PHOTON_PLACE_TYPE_COUNTRY);
  geo_index_close(&index);
}

TEST(GeoIndexCountry, AWordThatNamesNoPlaceOfThatCountryIsAWordAgain) {
  // Atlanta, Georgia: the state, not the country.  Narrowed to the country,
  // nothing is left, and the word is asked as a word the way it always was.
  testsupport::MiniPlace country =
      Placed("Georgia", "", "", 42.3, 43.4, PHOTON_PLACE_TYPE_COUNTRY, 50000, "ge");
  testsupport::MiniPlace atlanta =
      Placed("Atlanta", "Atlanta", "30303", 33.749, -84.388, PHOTON_PLACE_TYPE_CITY, 50000, "us");
  atlanta.aliases = {"Georgia"}; // the state its address block names
  TempPath path{"countrystate"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), {country, atlanta}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Atlanta Georgia ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Atlanta");
  geo_index_close(&index);
}

TEST(GeoIndexCountry, ACountryInsideANameTypedInFullIsPartOfThatName) {
  // Rue de Madagascar in Le Creusot, and West Jordan in Utah: the word names a
  // country, and a heavier place in that country would answer to the rest
  testsupport::MiniPlace madagascar =
      Placed("Madagascar", "", "", -18.9, 47.5, PHOTON_PLACE_TYPE_COUNTRY, 50000, "mg");
  testsupport::MiniPlace jordan =
      Placed("Jordan", "", "", 31.2, 36.5, PHOTON_PLACE_TYPE_COUNTRY, 50000, "jo");
  TempPath path{"countryinname"};
  ASSERT_TRUE(BuildMiniIndex(
      path.c_str(),
      {
          madagascar,
          jordan,
          Placed(
              "Rue de la Réunion", "Antananarivo", "101", -18.91, 47.52, PHOTON_PLACE_TYPE_STREET,
              9000, "mg"
          ),
          Placed(
              "Rue de Madagascar", "Le Creusot", "71200", 46.80, 4.45, PHOTON_PLACE_TYPE_STREET,
              3000, "fr"
          ),
          Placed("West Amman", "Amman", "", 31.95, 35.85, PHOTON_PLACE_TYPE_DISTRICT, 30000, "jo"),
          Placed(
              "West Jordan", "West Jordan", "84088", 40.61, -111.94, PHOTON_PLACE_TYPE_CITY, 20000,
              "us"
          ),
      }
  ));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_GE(Query(index, "Rue de Madagascar ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Rue de Madagascar");
  ASSERT_GE(Query(index, "West Jordan ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "West Jordan");
  // where no name holds every word typed, the country narrows as before
  ASSERT_EQ(Query(index, "Rue de la Madagascar ", hits, 8), 1u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Rue de la Réunion");
  geo_index_close(&index);
}

TEST(GeoIndexCountry, AQueryNamingNoCountryKeepsItsFifteenthWord) {
  // the slot a named country takes is kept free only where one was named: two
  // places share fourteen words, and only the fifteenth tells them apart
  const std::vector<std::string> words = {
      "alpha", "bravo",   "charlie", "delta", "echo", "foxtrot",  "golf",  "hotel",
      "india", "juliett", "kilo",    "lima",  "mike", "november", "oscar",
  };
  std::string fourteen, fifteen;
  for (size_t w = 0; w < words.size(); ++w) {
    if (w) fifteen += " ";
    fifteen += words[w];
    if (w + 1 < words.size()) fourteen = fifteen;
  }
  std::vector<testsupport::MiniPlace> places = TwoCountries();
  places.push_back(
      Placed(fifteen, "Bonn", "53111", 50.73, 7.09, PHOTON_PLACE_TYPE_STREET, 1000, "de")
  );
  places.push_back(
      Placed(fourteen, "Bonn", "53111", 50.74, 7.10, PHOTON_PLACE_TYPE_STREET, 2000, "de")
  );
  TempPath path{"countryfifteen"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places, {"de", "en"}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, fifteen + " ", hits, 8), 1u) << "the fifteenth word narrows too";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), fifteen);
  geo_index_close(&index);
}

TEST(GeoIndexCountry, AnIndexWithoutCountryWordsAnswersAsBefore) {
  // the same places, built as an index from before the country words: nothing
  // names a country there, and the query meets nothing, as it always did
  std::vector<testsupport::MiniPlace> places = TwoCountries();
  for (testsupport::MiniPlace &place : places) place.country.clear();
  TempPath path{"countryless"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places, {"de", "en"}));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  EXPECT_EQ(Query(index, "Marienplatz München Deutschland ", hits, 8), 0u);
  geo_index_close(&index);
}

// ---------------------------------------------------------------------------
//  A house number as people write it
// ---------------------------------------------------------------------------

namespace {

testsupport::MiniPlace Street(
    const std::string &name,
    const std::string &city,
    const std::string &postcode,
    double lat,
    double lon,
    uint16_t importance,
    std::vector<std::string> houses
) {
  testsupport::MiniPlace place =
      Placed(name, city, postcode, lat, lon, PHOTON_PLACE_TYPE_STREET, importance, "");
  place.houses = std::move(houses);
  return place;
}

/** Streets whose doors are written the ways the dump writes them. */
std::vector<testsupport::MiniPlace> Doors() {
  return {
      Street("Osterstraße", "Hannover", "30159", 52.373, 9.745, 3000, {"40", "42A"}),
      Street("Lister Meile", "Hannover", "30161", 52.380, 9.745, 3000, {"29", "29a"}),
      Street("Bruchshöfenstraße", "Nordstemmen", "31171", 52.160, 9.786, 2000, {"1 A"}),
      Street("Rue de la Paix", "Paris", "75002", 48.869, 2.331, 5000, {"12bis", "12"}),
      // the heavier street carries only the plain number
      Street("Avenue Leclerc", "Bordeaux", "33200", 44.850, -0.600, 9000, {"36"}),
      Street("Avenue Leclerc", "Le Bouscat", "33110", 44.865, -0.598, 2000, {"36B"}),
      Street("Anderter Straße", "Hannover", "30629", 52.370, 9.830, 3000, {"1-3", "5", "8"}),
      Street("Kirchweg", "Hannover", "30655", 52.400, 9.800, 3000, {"23 - 25", "40/42"}),
      // Baden-Württemberg numbers the house behind the 12 as 12/1
      Street("Hauptstraße", "Stuttgart", "70173", 48.776, 9.180, 3000, {"12", "12/1"}),
      Street("Lerchenweg", "Riedlingen", "88499", 48.155, 9.472, 3000, {"2/3", "2/6", "7/9"}),
  };
}

/** The spelling of the door @p hit found, or "" when it found none. */
std::string Door(const GeoIndex &index, const GeoHit &hit) {
  if (hit.house == GEO_RANK_NONE) return std::string();
  return DisplayWord(index, index.houses[hit.house].number_rank);
}

} // namespace

TEST(GeoIndexHouse, ALetterWrittenApartFromItsNumberBelongsToTheNumber) {
  TempPath path{"houseletter"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  // the letter names no place, and asked as a word it narrowed the street away
  for (const char *query :
       {"Osterstraße 42 A Hannover ", "Osterstr. 42 a Hannover ", "Osterstraße 42A Hannover ",
        "Osterstraße 42 A"}) {
    ASSERT_GE(Query(index, query, hits, 8, true), 1u) << query;
    EXPECT_EQ(Door(index, hits[0]), "42A") << query;
  }
  geo_index_close(&index);
}

TEST(GeoIndexHouse, ASpaceInsideAWrittenNumberDoesNotMatter) {
  TempPath path{"housespace"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  for (const char *query :
       {"Bruchshöfenstraße 1A Nordstemmen ", "Bruchshöfenstraße 1 a Nordstemmen "}) {
    ASSERT_GE(Query(index, query, hits, 8), 1u) << query;
    EXPECT_EQ(Door(index, hits[0]), "1 A") << query;
  }
  geo_index_close(&index);
}

TEST(GeoIndexHouse, ASuffixNoDoorCarriesFallsBackToThePlainNumber) {
  TempPath path{"housefallback"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  for (const char *query : {"Lister Meile 29D Hannover ", "Lister Meile 29 D Hannover "}) {
    ASSERT_GE(Query(index, query, hits, 8), 1u) << query;
    EXPECT_EQ(Door(index, hits[0]), "29") << query;
  }
  // a suffix that is there is still found as it was asked for
  ASSERT_GE(Query(index, "Lister Meile 29 A Hannover ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "29a");
  geo_index_close(&index);
}

TEST(GeoIndexHouse, TheDoorAskedForStandsBeforeThePlainNumber) {
  TempPath path{"houseexact"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "36B Avenue Leclerc ", hits, 8), 2u);
  EXPECT_EQ(Door(index, hits[0]), "36B") << "not the 36 of the heavier street";
  EXPECT_EQ(Door(index, hits[1]), "36");
  geo_index_close(&index);
}

TEST(GeoIndexHouse, AFrenchSuffixBelongsToTheNumber) {
  TempPath path{"housebis"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  for (const char *query : {"12 bis Rue de la Paix ", "12BIS Rue de la Paix "}) {
    ASSERT_GE(Query(index, query, hits, 8), 1u) << query;
    EXPECT_EQ(Door(index, hits[0]), "12bis") << query;
  }
  geo_index_close(&index);
}

TEST(GeoIndexHouse, ARangeIsFoundAsItIsWritten) {
  TempPath path{"houserange"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  for (const char *query :
       {"Anderter Straße 1-3 Hannover ", "Anderter Straße 1 - 3 Hannover ", "Anderter Straße 1/3 ",
        "Kirchweg 23-25 ", "Kirchweg 40-42 "}) {
    ASSERT_GE(Query(index, query, hits, 8), 1u) << query;
    EXPECT_NE(Door(index, hits[0]), "") << query;
  }
  ASSERT_GE(Query(index, "Anderter Straße 1-3 ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "1-3");
  ASSERT_GE(Query(index, "Kirchweg 23-25 ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "23 - 25");
  geo_index_close(&index);
}

TEST(GeoIndexHouse, ADoorBehindAnotherIsNotTheOneInFront) {
  TempPath path{"housebehind"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_GE(Query(index, "Hauptstraße 12/1 Stuttgart ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "12/1");
  ASSERT_GE(Query(index, "Hauptstraße 12 Stuttgart ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "12");
  geo_index_close(&index);
}

TEST(GeoIndexHouse, ANumberInsideARangeFindsTheRange) {
  TempPath path{"houseinrange"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  // whoever lives in the house numbered 1-3 types one of its numbers
  for (const char *query : {"Anderter Straße 1 ", "Anderter Straße 3 "}) {
    ASSERT_GE(Query(index, query, hits, 8), 1u) << query;
    EXPECT_EQ(Door(index, hits[0]), "1-3") << query;
  }
  ASSERT_GE(Query(index, "Kirchweg 25 ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "23 - 25");
  // 23 to 25 are the odd side of the street; the 24 lies across the road
  ASSERT_GE(Query(index, "Kirchweg 24 ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "");
  // a door behind another is no range
  ASSERT_GE(Query(index, "Hauptstraße 5 Stuttgart ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "");
  geo_index_close(&index);
}

TEST(GeoIndexHouse, ASlashNumbersTheHousesBehindAHouseAndMakesNoRange) {
  TempPath path{"houseslash"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  // 2/3 and 2/6 are two houses behind the 2, and neither of them is the 2
  ASSERT_GE(Query(index, "Lerchenweg 2 Riedlingen ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "");
  ASSERT_GE(Query(index, "Lerchenweg 9 Riedlingen ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "");
  ASSERT_GE(Query(index, "Lerchenweg 2/6 Riedlingen ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "2/6");
  geo_index_close(&index);
}

TEST(GeoIndexHouse, ANumberAndAPostcodeSideBySideStayApart) {
  TempPath path{"housecode"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Doors()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  // read as one number, 3-30629 would be no door at all
  ASSERT_GE(Query(index, "Anderter Straße 3 30629 ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "1-3");
  ASSERT_GE(Query(index, "Anderter Straße 8, 30629 ", hits, 8), 1u);
  EXPECT_EQ(Door(index, hits[0]), "8");
  geo_index_close(&index);
}

// ---------------------------------------------------------------------------
//  A house number the street does not carry
// ---------------------------------------------------------------------------

namespace {

/** A street running north, 1 000 m long, with its houses every 20 m. */
testsupport::MiniPlace Gasse(
    const std::string &name, std::vector<std::pair<std::string, int>> doors
) {
  // the street's own point is its middle, 500 m up
  testsupport::MiniPlace street = Street(name, "Würzburg", "97070", 49.7945, 9.9300, 3000, {});
  for (const auto &door : doors) {
    street.houses.push_back(door.first);
    // 20 m of latitude are 1 800 in degrees × 10⁷
    street.house_points.push_back({E7(49.7900) + door.second * 1800, E7(9.9300)});
  }
  return street;
}

} // namespace

TEST(GeoIndexHouseEstimate, AMissingNumberLiesBetweenItsNeighbours) {
  TempPath path{"estimatebetween"};
  ASSERT_TRUE(
      BuildMiniIndex(path.c_str(), {Gasse("Schulgasse", {{"15", 0}, {"19", 2}, {"16", 5}})})
  );
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Schulgasse 17 ", hits, 8), 1u);
  EXPECT_EQ(hits[0].house, GEO_RANK_NONE) << "the number is still not found";
  ASSERT_EQ(hits[0].estimated, 1u);
  EXPECT_EQ(hits[0].lat_e7, E7(49.7900) + 1800) << "halfway between the 15 and the 19";
  EXPECT_EQ(hits[0].lon_e7, E7(9.9300));
  geo_index_close(&index);
}

TEST(GeoIndexHouseEstimate, ADoorOfTheSameNumberStandsInForIt) {
  TempPath path{"estimatesuffix"};
  ASSERT_TRUE(
      BuildMiniIndex(path.c_str(), {Gasse("Schulgasse", {{"15", 0}, {"17a", 1}, {"21", 3}})})
  );
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Schulgasse 17 ", hits, 8), 1u);
  ASSERT_EQ(hits[0].estimated, 1u);
  EXPECT_EQ(hits[0].lat_e7, E7(49.7900) + 1800);
  geo_index_close(&index);
}

TEST(GeoIndexHouseEstimate, NothingIsGuessedWhereTheNeighboursSayLittle) {
  TempPath path{"estimatenone"};
  ASSERT_TRUE(BuildMiniIndex(
      path.c_str(), {
                        // only the other side of the street near the 17
                        Gasse("Schulgasse", {{"14", 0}, {"18", 2}}),
                        // one neighbour, and nothing above it
                        Gasse("Kirchgasse", {{"15", 0}}),
                        // more than twenty numbers apart
                        Gasse("Domgasse", {{"1", 0}, {"41", 2}}),
                        // close in number, but 400 m apart: a street of two pieces
                        Gasse("Hofgasse", {{"15", 0}, {"19", 20}}),
                    }
  ));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  for (const char *query : {"Schulgasse 17 ", "Kirchgasse 17 ", "Domgasse 17 ", "Hofgasse 17 "}) {
    ASSERT_EQ(Query(index, query, hits, 8), 1u) << query;
    EXPECT_EQ(hits[0].estimated, 0u) << query;
  }
  // and a query that asked for no number asks for no estimate
  ASSERT_EQ(Query(index, "Domgasse ", hits, 8), 1u);
  EXPECT_EQ(hits[0].estimated, 0u);
  geo_index_close(&index);
}

TEST(GeoIndexHouseEstimate, AHouseLeftOutIsEstimatedFromTheOthers) {
  TempPath path{"estimatepassed"};
  ASSERT_TRUE(
      BuildMiniIndex(path.c_str(), {Gasse("Schulgasse", {{"15", 0}, {"17", 1}, {"19", 2}})})
  );
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(Query(index, "Schulgasse 17 ", hits, 8), 1u);
  ASSERT_NE(hits[0].house, GEO_RANK_NONE);
  EXPECT_EQ(hits[0].estimated, 0u) << "a number found needs no estimate";

  int32_t lat = 0, lon = 0;
  EXPECT_TRUE(geo_index_house_estimate(&index, hits[0].document, 17, hits[0].house, &lat, &lon));
  EXPECT_EQ(lat, E7(49.7900) + 1800);
  EXPECT_FALSE(geo_index_house_estimate(&index, hits[0].document, 0, GEO_RANK_NONE, &lat, &lon));
  EXPECT_FALSE(
      geo_index_house_estimate(&index, index.document_count, 17, GEO_RANK_NONE, &lat, &lon)
  );
  geo_index_close(&index);
}

// ---------------------------------------------------------------------------
//  One place, said once
// ---------------------------------------------------------------------------

namespace {

/** A town and the nameless address blocks the dump files inside it. */
std::vector<testsupport::MiniPlace> Kirchheim() {
  std::vector<testsupport::MiniPlace> places = {
      Placed(
          "Kirchheim bei München", "", "", 48.1757, 11.7562, PHOTON_PLACE_TYPE_CITY, 20000, "de"
      ),
  };
  // four streets of the same postcode that carry no name at all, each heavier
  // than the town — an answer shows every one of them as an empty line
  // spread over the town, the last of them a kilometre from the first
  for (int i = 0; i < 4; ++i) {
    places.push_back(Placed(
        "", "Kirchheim bei München", "85551", 48.1750 + i * 0.003, 11.7550,
        PHOTON_PLACE_TYPE_STREET, 30000, "de"
    ));
  }
  return places;
}

} // namespace

TEST(GeoIndexRepeats, ATownStandsBeforeTheNamelessBlocksInsideIt) {
  TempPath path{"repeatsnameless"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), Kirchheim()));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  size_t count = Query(index, "Kirchheim bei München ", hits, 8);
  ASSERT_GE(count, 1u);
  EXPECT_EQ(
      DisplayWord(index, index.documents[hits[0].document].name_rank), "Kirchheim bei München"
  ) << "a line nobody can read is the weakest answer there is";
  EXPECT_EQ(count, 2u) << "the four nameless blocks are one line, not four";
  geo_index_close(&index);
}

TEST(GeoIndexRepeats, TheDatelineDoesNotPullTwoLinesApart) {
  // two nameless blocks of one village, a few hundred metres and the 180th
  // meridian apart — measured the long way round they lie half a world away
  TempPath path{"repeatsdateline"};
  ASSERT_TRUE(BuildMiniIndex(
      path.c_str(),
      {
          Placed("", "Taveuni", "", -16.8000, 179.9990, PHOTON_PLACE_TYPE_STREET, 3000, "fj"),
          Placed("", "Taveuni", "", -16.8000, -179.9990, PHOTON_PLACE_TYPE_STREET, 2900, "fj"),
      }
  ));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  EXPECT_EQ(Query(index, "Taveuni ", hits, 8), 1u);
  geo_index_close(&index);
}

TEST(GeoIndexRepeats, APositionSaysWhichOfTheTwoIsMeant) {
  // the same Heusenstamm, asked from the town itself: one answer, and the
  // record standing in the town rather than the middle of its boundary
  TempPath path{"repeatspositioned"};
  ASSERT_TRUE(BuildMiniIndex(
      path.c_str(), {
                        Placed(
                            "Heusenstamm", "Heusenstamm", "63150", 50.0400, 8.7993,
                            PHOTON_PLACE_TYPE_CITY, 30000, "de"
                        ),
                        Placed(
                            "Heusenstamm", "Heusenstamm", "63150", 50.0547, 8.7993,
                            PHOTON_PLACE_TYPE_INDEPENDENT_CITY, 29000, "de"
                        ),
                    }
  ));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  ASSERT_EQ(QueryFrom(index, "Heusenstamm ", 50.0547, 8.7993, hits, 8), 1u);
  EXPECT_EQ(index.documents[hits[0].document].lat_e7, E7(50.0547)) << "the nearer of the two";
  EXPECT_EQ(index.documents[hits[0].document].type, PHOTON_PLACE_TYPE_INDEPENDENT_CITY);
  geo_index_close(&index);
}

TEST(GeoIndexRepeats, TheRecordCarryingTheNumberIsTheOneKept) {
  // one street written down twice, and only the farther record carries the 5
  testsupport::MiniPlace with_door =
      Placed("Hauptstraße", "Bonn", "53111", 50.7350, 7.0980, PHOTON_PLACE_TYPE_STREET, 3000, "de");
  with_door.houses = {"5"};
  TempPath path{"repeatsdoor"};
  ASSERT_TRUE(BuildMiniIndex(
      path.c_str(), {
                        with_door,
                        Placed(
                            "Hauptstraße", "Bonn", "53111", 50.7420, 7.0980,
                            PHOTON_PLACE_TYPE_LOCALITY, 9000, "de"
                        ),
                    }
  ));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  // asked from beside the record without the number, half a kilometre away
  ASSERT_EQ(QueryFrom(index, "Hauptstraße 5 Bonn ", 50.7420, 7.0980, hits, 8), 1u);
  ASSERT_NE(hits[0].house, GEO_RANK_NONE) << "the door is worth more than a few hundred metres";
  EXPECT_EQ(DisplayWord(index, index.houses[hits[0].house].number_rank), "5");
  geo_index_close(&index);
}

TEST(GeoIndexRepeats, ATownWrittenDownInTwoPlacesStaysTwo) {
  // Heusenstamm, as the planet holds it: the middle of its boundary and the
  // point that carries its name, 1.7 km apart — and the lighter of the two is
  // the one standing where the town is
  TempPath path{"repeatstown"};
  ASSERT_TRUE(BuildMiniIndex(
      path.c_str(), {
                        Placed(
                            "Heusenstamm", "Heusenstamm", "63150", 50.0400, 8.7993,
                            PHOTON_PLACE_TYPE_CITY, 30000, "de"
                        ),
                        Placed(
                            "Heusenstamm", "Heusenstamm", "63150", 50.0547, 8.7993,
                            PHOTON_PLACE_TYPE_INDEPENDENT_CITY, 29000, "de"
                        ),
                    }
  ));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  EXPECT_EQ(Query(index, "Heusenstamm ", hits, 8), 2u)
      << "asked from nowhere, nothing says which of the two is meant";
  // and the build has already joined whatever stood within 300 m of its twin
  geo_index_close(&index);
}

TEST(GeoIndexNear, ALighterTownAbroadComesInOnlyNearTheBorder) {
  // *Halle* in Berlin: a map zoomed onto Germany shows the two German Halles,
  // not the one in Belgium; *Venlo* in Mönchengladbach is across the border
  TempPath path{"farabroad"};
  ASSERT_TRUE(BuildMiniIndex(
      path.c_str(),
      {
          Placed(
              "Hallesches Ufer", "Berlin", "10963", 52.4991, 13.3841, PHOTON_PLACE_TYPE_STREET,
              14452, "de"
          ),
          Placed(
              "Halle (Saale)", "Halle (Saale)", "", 51.4824, 11.9713, PHOTON_PLACE_TYPE_CITY, 43648,
              "de"
          ),
          Placed(
              "Halle (Westf.)", "Halle (Westf.)", "33790", 52.0604, 8.3616, PHOTON_PLACE_TYPE_CITY,
              32909, "de"
          ),
          Placed("Halle", "Halle", "1500", 50.7361, 4.2374, PHOTON_PLACE_TYPE_CITY, 34481, "be"),
          Placed(
              "Venloer Straße", "Willich", "47877", 51.2611, 6.4630, PHOTON_PLACE_TYPE_STREET, 3499,
              "de"
          ),
          Placed("Venlo", "Venlo", "", 51.3702, 6.1689, PHOTON_PLACE_TYPE_CITY, 36848, "nl"),
      }
  ));
  GeoIndex index{};
  ASSERT_EQ(geo_index_open(&index, path.c_str()), ARNM_SUCCESS);

  GeoHit hits[8];
  size_t count = QueryFrom(index, "Halle", 52.52, 13.405, hits, 8, nullptr, true);
  ASSERT_EQ(count, 3u) << "Halle in Belgium lies 650 km off, in another country";
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Halle (Saale)");
  EXPECT_EQ(DisplayWord(index, index.documents[hits[1].document].name_rank), "Halle (Westf.)");

  ASSERT_GE(QueryFrom(index, "Venlo", 51.1805, 6.4428, hits, 8, nullptr, true), 2u);
  EXPECT_EQ(DisplayWord(index, index.documents[hits[0].document].name_rank), "Venlo")
      << "28 km across the border";
  geo_index_close(&index);
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

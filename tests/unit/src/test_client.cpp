/** @file
 *  @brief The library as a caller across a language border sees it.
 *
 *  Four functions, an opaque handle and a plain status — that is the whole
 *  surface, and these tests use nothing else.  What the index does inside is
 *  the business of test_geo_index; here it matters only that no failure ends
 *  the process, that the borrowed strings are what they claim to be, and that
 *  the JSON form says the same as the struct form.
 */

#include "c_api.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

using testsupport::BuildMiniIndex;
using testsupport::SamplePlaces;
using testsupport::TempPath;

namespace {

std::string Field(const char *text, size_t size) {
  return text ? std::string(text, size) : std::string();
}
std::string Name(const GeoAddress &a) { return Field(a.name, a.name_size); }
std::string City(const GeoAddress &a) { return Field(a.city, a.city_size); }
std::string Code(const GeoAddress &a) { return Field(a.postcode, a.postcode_size); }
std::string Number(const GeoAddress &a) { return Field(a.number, a.number_size); }

class ClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(BuildMiniIndex(path.c_str(), SamplePlaces()));
    ASSERT_EQ(geo_client_open(&client, path.c_str()), GEO_OK);
    ASSERT_NE(client, nullptr);
  }
  void TearDown() override { geo_client_close(client); }

  size_t Search(const std::string &q, GeoAddress *out, size_t limit, bool prefix = false) {
    return geo_client_search(client, q.c_str(), q.size(), prefix, out, limit);
  }

  TempPath path{"client"};
  GeoClient *client = nullptr;
};

} // namespace

TEST_F(ClientTest, OpensAndReportsItsCounts) {
  GeoClientInfo info{};
  ASSERT_EQ(geo_client_info(client, &info), GEO_OK);
  EXPECT_EQ(info.documents, SamplePlaces().size());
  EXPECT_GT(info.words, 0u);
  EXPECT_GT(info.spellings, 0u);
  EXPECT_GT(info.file_size, 0u);
  EXPECT_EQ(info.format, GEO_INDEX_VERSION);
  EXPECT_EQ(info.houses, 7u)
      << "three on Marienplatz, two and one on the Berliner Straßen, one on the Hauptstraße";
}

TEST_F(ClientTest, FindsAPlaceAndFillsEveryField) {
  GeoAddress found[8];
  ASSERT_GE(Search("Marienplatz München ", found, 8), 1u);
  EXPECT_EQ(Name(found[0]), "Marienplatz");
  EXPECT_EQ(City(found[0]), "München");
  EXPECT_EQ(Code(found[0]), "80331");
  EXPECT_EQ(found[0].kind, GEO_PLACE_STREET);
  EXPECT_TRUE(found[0].has_point);
  EXPECT_NEAR(found[0].latitude, 48.1374, 1e-6);
  EXPECT_NEAR(found[0].longitude, 11.5755, 1e-6);
}

TEST_F(ClientTest, AFieldThePlaceNeverHadComesBackAsNull) {
  GeoAddress found[8];
  ASSERT_GE(Search("Osiedle Praha ", found, 8), 1u);
  EXPECT_EQ(found[0].number, nullptr) << "the query named no house number";
  EXPECT_EQ(found[0].number_size, 0u);
}

TEST_F(ClientTest, AHouseNumberComesBackWithItsOwnPoint) {
  GeoAddress found[8];
  ASSERT_GE(Search("Marienplatz München 12a ", found, 8), 1u);
  EXPECT_EQ(Number(found[0]), "12a");
  EXPECT_EQ(Name(found[0]), "Marienplatz");
}

TEST_F(ClientTest, ThePostcodeDecidesBetweenTwoStreetsOfTheSameName) {
  GeoAddress found[8];
  ASSERT_EQ(Search("Berliner Straße 10715 ", found, 8), 1u);
  EXPECT_EQ(City(found[0]), "Berlin");
  EXPECT_EQ(Code(found[0]), "10715");
}

TEST_F(ClientTest, ANamedTownLiftsTheRightOneOfTwo) {
  GeoAddress found[8];
  size_t count = Search("Berliner Straße Potsdam ", found, 8);
  ASSERT_GE(count, 1u);
  EXPECT_EQ(City(found[0]), "Potsdam") << "the town the query named comes first";
}

TEST_F(ClientTest, AWrongPostcodeCostsAPositionNotThePresence) {
  GeoAddress found[8];
  size_t count = Search("Berliner Straße 99999 ", found, 8);
  EXPECT_EQ(count, 2u) << "the code found nothing and was dropped again";
}

TEST_F(ClientTest, NothingMatchedIsZeroNotAFailure) {
  GeoAddress found[8];
  EXPECT_EQ(Search("Kwyjibo Blorf ", found, 8), 0u);
}

TEST_F(ClientTest, ReadsTheLastWordAsABeginningWhenAsked) {
  GeoAddress found[8];
  EXPECT_EQ(Search("Marienpla", found, 8, /*prefix=*/false), 0u);
  EXPECT_GE(Search("Marienpla", found, 8, /*prefix=*/true), 1u);
}

TEST_F(ClientTest, WritesNoMoreThanTheLimitAllows) {
  GeoAddress found[8];
  size_t count = Search("Berliner Straße ", found, 1);
  EXPECT_LE(count, 1u);
}

TEST_F(ClientTest, TheStringsPointIntoTheMappingAndStayPut) {
  GeoAddress first[8], second[8];
  ASSERT_GE(Search("Marienplatz ", first, 8), 1u);
  ASSERT_GE(Search("Marienplatz ", second, 8), 1u);
  EXPECT_EQ(first[0].name, second[0].name) << "the same borrowed bytes, not a copy";
}

TEST_F(ClientTest, ManyThreadsMaySearchAtOnce) {
  constexpr int kThreads = 8;
  std::vector<std::thread> workers;
  std::vector<size_t> counts(kThreads, 0);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t] {
      GeoAddress found[8];
      for (int i = 0; i < 200; ++i) {
        const char *q = "Marienplatz München ";
        counts[t] = geo_client_search(client, q, std::strlen(q), false, found, 8);
      }
    });
  }
  for (std::thread &w : workers) w.join();
  for (size_t c : counts) EXPECT_GE(c, 1u);
}

// ---------------------------------------------------------------------------
//  The counts a caller may ask the search to keep
// ---------------------------------------------------------------------------

TEST_F(ClientTest, TheCountedSearchAnswersLikeThePlainOne) {
  GeoAddress counted[8], plain[8];
  GeoQueryStats stats{};
  GeoSearchOptions options{};
  options.stats = &stats;
  const char *q = "Marienplatz München ";
  size_t a = geo_client_search_options(client, q, std::strlen(q), &options, counted, 8);
  size_t b = Search(q, plain, 8);
  ASSERT_EQ(a, b);
  EXPECT_EQ(stats.results, a);
  EXPECT_GT(stats.posting_lists, 0u);
  for (size_t i = 0; i < a; ++i) EXPECT_EQ(Name(counted[i]), Name(plain[i]));
}

TEST_F(ClientTest, NoOptionsAtAllIsThePlainSearch) {
  GeoAddress found[8];
  const char *q = "Marienplatz ";
  EXPECT_GE(geo_client_search_options(client, q, std::strlen(q), nullptr, found, 8), 1u);
}

TEST_F(ClientTest, APositionLiftsTheStreetNearestToIt) {
  GeoAddress found[8];
  GeoSearchOptions options{};
  options.has_position = true;

  options.latitude = 52.4869; // Berlin
  options.longitude = 13.3283;
  const char *q = "Berliner Straße ";
  ASSERT_GE(geo_client_search_options(client, q, std::strlen(q), &options, found, 8), 1u);
  EXPECT_EQ(City(found[0]), "Berlin");

  options.latitude = 52.3956; // Potsdam, whose Berliner Straße is the heavier one
  options.longitude = 13.0649;
  ASSERT_GE(geo_client_search_options(client, q, std::strlen(q), &options, found, 8), 1u);
  EXPECT_EQ(City(found[0]), "Potsdam");
}

TEST_F(ClientTest, DegreesArriveAsTheIndexKeepsThem) {
  // the client speaks degrees and the index fixed point; a position off by a
  // rounding step would land in the wrong cell at a cell border
  GeoAddress found[8];
  GeoSearchOptions options{};
  options.has_position = true;
  options.latitude = 52.4869779;
  options.longitude = 13.3283388;
  const char *q = "Berliner Straße ";
  ASSERT_GE(geo_client_search_options(client, q, std::strlen(q), &options, found, 8), 1u);
  EXPECT_EQ(City(found[0]), "Berlin");
}

TEST_F(ClientTest, AnImpossiblePositionIsHeldAtTheEdgeOfTheWorld) {
  GeoAddress found[8];
  GeoSearchOptions options{};
  options.has_position = true;
  options.latitude = 1000.0; // no such latitude
  options.longitude = -4000.0;
  const char *q = "Berliner Straße ";
  // clamped rather than refused, and nothing stands at the pole, so the
  // position is dropped and the words answer alone
  EXPECT_EQ(geo_client_search_options(client, q, std::strlen(q), &options, found, 8), 2u);
}

TEST(ClientGuards, TheCountsAreClearedBeforeAnythingIsRefused) {
  GeoAddress found[4];
  GeoQueryStats stats{};
  GeoSearchOptions options{};
  options.stats = &stats;
  stats.posting_lists = 99;
  EXPECT_EQ(geo_client_search_options(nullptr, "x", 1, &options, found, 4), 0u);
  EXPECT_EQ(stats.posting_lists, 0u);
  EXPECT_EQ(stats.results, 0u);
}

// ---------------------------------------------------------------------------
//  The JSON form, for callers who do not want to walk foreign structs
// ---------------------------------------------------------------------------

TEST_F(ClientTest, WritesTheSameAnswerAsJson) {
  char buffer[4096];
  const char *q = "Marienplatz München ";
  size_t needed = geo_client_search_json(client, q, std::strlen(q), false, 8, buffer, sizeof(buffer));
  ASSERT_LT(needed, sizeof(buffer)) << "the answer fitted";
  std::string json(buffer);
  EXPECT_EQ(json.front(), '[');
  EXPECT_EQ(json.back(), ']');
  EXPECT_NE(json.find("\"name\":\"Marienplatz\""), std::string::npos);
  EXPECT_NE(json.find("\"city\":\"München\""), std::string::npos);
  EXPECT_NE(json.find("\"postcode\":\"80331\""), std::string::npos);
}

TEST_F(ClientTest, AnAbsentFieldIsJsonNull) {
  char buffer[4096];
  const char *q = "Osiedle Praha ";
  geo_client_search_json(client, q, std::strlen(q), false, 8, buffer, sizeof(buffer));
  EXPECT_NE(std::string(buffer).find("\"number\":null"), std::string::npos);
}

TEST(ClientJson, ARoundCoordinateKeepsItsSevenDecimals) {
  // the fraction is written into a field of seven zeros; nothing may cut it
  // short, and a fraction of zero has to leave all seven standing
  std::vector<testsupport::MiniPlace> places = {
      {"Nullstraße", "Nullstadt", "00000", 480000000, 110000000},
      {"Halbstraße", "Halbstadt", "00001", 5000000, 205000000},
  };
  TempPath path{"degrees"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), places));
  GeoClient *client = nullptr;
  ASSERT_EQ(geo_client_open(&client, path.c_str()), GEO_OK);

  char buffer[2048];
  const char *round = "Nullstraße ";
  geo_client_search_json(client, round, std::strlen(round), false, 4, buffer, sizeof(buffer));
  EXPECT_NE(std::string(buffer).find("\"lat\":48.0000000"), std::string::npos) << buffer;
  EXPECT_NE(std::string(buffer).find("\"lon\":11.0000000"), std::string::npos) << buffer;

  const char *half = "Halbstraße ";
  geo_client_search_json(client, half, std::strlen(half), false, 4, buffer, sizeof(buffer));
  EXPECT_NE(std::string(buffer).find("\"lat\":0.5000000"), std::string::npos) << buffer;
  EXPECT_NE(std::string(buffer).find("\"lon\":20.5000000"), std::string::npos) << buffer;

  geo_client_close(client);
}

TEST_F(ClientTest, NothingMatchedIsAnEmptyJsonArray) {
  char buffer[256];
  const char *q = "Kwyjibo ";
  geo_client_search_json(client, q, std::strlen(q), false, 8, buffer, sizeof(buffer));
  EXPECT_STREQ(buffer, "[]");
}

TEST_F(ClientTest, ATooSmallBufferAsksForRoomInsteadOfOverrunning) {
  char small[16];
  std::memset(small, 'x', sizeof(small));
  const char *q = "Marienplatz München ";
  size_t needed = geo_client_search_json(client, q, std::strlen(q), false, 8, small, sizeof(small));
  EXPECT_GE(needed, sizeof(small)) << "the answer is the size the whole would need";
  EXPECT_NE(std::memchr(small, '\0', sizeof(small)), nullptr) << "and what fitted is terminated";
}

// ---------------------------------------------------------------------------
//  Nothing here may end the process
// ---------------------------------------------------------------------------

TEST(ClientGuards, OpeningAFileThatIsNotThereFails) {
  GeoClient *client = reinterpret_cast<GeoClient *>(0x1);
  EXPECT_EQ(geo_client_open(&client, "/nonexistent/geoindex/test.gdx"), GEO_ERROR_FILE);
  EXPECT_EQ(client, nullptr) << "the handle is cleared even on failure";
}

TEST(ClientGuards, OpeningSomethingThatIsNoIndexFails) {
  TempPath path{"garbage"};
  {
    std::ofstream out(path.c_str(), std::ios::binary);
    out << "das ist kein Index, sondern ein Satz.";
  }
  GeoClient *client = nullptr;
  GeoStatus status = geo_client_open(&client, path.c_str());
  EXPECT_NE(status, GEO_OK);
  EXPECT_EQ(client, nullptr);
}

TEST(ClientGuards, NullArgumentsComeBackAsArgumentErrors) {
  GeoClient *client = nullptr;
  EXPECT_EQ(geo_client_open(nullptr, "x"), GEO_ERROR_ARGUMENT);
  EXPECT_EQ(geo_client_open(&client, nullptr), GEO_ERROR_ARGUMENT);
  EXPECT_EQ(geo_client_info(nullptr, nullptr), GEO_ERROR_ARGUMENT);

  GeoAddress found[4];
  EXPECT_EQ(geo_client_search(nullptr, "x", 1, false, found, 4), 0u);
}

TEST(ClientGuards, ClosingNullIsSafe) {
  geo_client_close(nullptr);
}

TEST(ClientGuards, AnEmptyQueryFindsNothing) {
  TempPath path{"emptyquery"};
  ASSERT_TRUE(BuildMiniIndex(path.c_str(), SamplePlaces()));
  GeoClient *client = nullptr;
  ASSERT_EQ(geo_client_open(&client, path.c_str()), GEO_OK);

  GeoAddress found[4];
  EXPECT_EQ(geo_client_search(client, "", 0, false, found, 4), 0u);
  EXPECT_EQ(geo_client_search(client, "Berlin", 6, false, found, 0), 0u);

  geo_client_close(client);
}

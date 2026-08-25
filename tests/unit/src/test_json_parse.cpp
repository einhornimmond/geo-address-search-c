/** @file
 *  @brief The parser — one line of the dump, split into an answer and an index.
 *
 *  Two things are pinned here.  What keeps its role, because an answer shows
 *  it: name, street, number, postcode, town.  And what loses its role to
 *  become searchable text — including the German reading of an address key,
 *  which is what lets a place be found again under the name it was shown by.
 */

#include "c_api.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace {

/** Everything one line produced, copied out of the borrowed document. */
struct Parsed {
  std::string type;
  PhotonPlaceType type_enum = PHOTON_PLACE_TYPE_NONE;
  std::string name, street, house, postcode, city, country;
  std::vector<std::string> search;
  double importance = 0.0;
  int32_t lat_e7 = 0, lon_e7 = 0;
  int has_point = 0;
  uint8_t search_dropped = 0;
  /** One entry per language the build asked for beside the default, by tag. */
  struct Reading {
    std::string name, city;
  };
  std::map<std::string, Reading> readings;
};

std::string Str(const PhotonString &s) {
  return s.data ? std::string(s.data, s.size) : std::string();
}

int Capture(const PhotonPlace *place, void *user) {
  auto *out = static_cast<std::vector<Parsed> *>(user);
  Parsed p;
  p.type = place->type ? place->type : "";
  p.type_enum = place->typeEnum;
  p.name = Str(place->own_name);
  p.street = Str(place->street);
  p.house = Str(place->house);
  p.postcode = Str(place->postcode);
  p.city = Str(place->city);
  p.country = place->country_code ? place->country_code : "";
  p.importance = place->importance;
  p.lat_e7 = place->lat_e7;
  p.lon_e7 = place->lon_e7;
  p.has_point = place->has_point;
  p.search_dropped = place->search_dropped;
  for (uint8_t i = 0; i < place->search_count; ++i) p.search.push_back(Str(place->search[i]));
  for (uint8_t i = 0; i < place->variant_count; ++i) {
    Parsed::Reading reading;
    reading.name = Str(place->variants[i].name);
    reading.city = Str(place->variants[i].city);
    p.readings[place->variants[i].tag] = reading;
  }
  out->push_back(p);
  return 0;
}

/** The languages every build kept before there was a choice — German alone. */
const PhotonLanguages &German() {
  static const PhotonLanguages languages = [] {
    PhotonLanguages set{};
    photon_languages_parse("de", &set);
    return set;
  }();
  return languages;
}

/** Parse one line with a language list of the test's own choosing. */
std::vector<Parsed> ParseIn(const std::string &line, const PhotonLanguages &languages) {
  std::vector<Parsed> places;
  JsonParseResult result{};
  EXPECT_EQ(json_parse_line(line.c_str(), line.size(), &languages, Capture, &places, &result), 1);
  return places;
}

/** Parse one line and hand back every entry it carried. */
std::vector<Parsed> Parse(const std::string &line, JsonParseResult *result = nullptr) {
  std::vector<Parsed> places;
  JsonParseResult own{};
  JsonParseResult *r = result ? result : &own;
  EXPECT_EQ(json_parse_line(line.c_str(), line.size(), &German(), Capture, &places, r), 1);
  return places;
}

bool Has(const std::vector<std::string> &all, const std::string &one) {
  for (const std::string &s : all) {
    if (s == one) return true;
  }
  return false;
}

/** One entry at @p centroid, written exactly as the test spells it. */
std::string LineWithCentroid(const std::string &centroid) {
  return R"({"type":"Place","content":[{"address_type":"street",)"
         R"("name":{"name":"Seeweg"},"centroid":)" +
         centroid + R"(}]})";
}

/** The fixture lines, in the order tests/data/places.jsonl lists them. */
const std::vector<std::string> &Fixture() {
  static const std::vector<std::string> lines = testsupport::ReadFixtureLines("places.jsonl");
  return lines;
}

} // namespace

TEST(JsonParseFixture, TheFixtureIsThere) {
  ASSERT_FALSE(Fixture().empty()) << "tests/data/places.jsonl — is the test run from the root?";
  EXPECT_EQ(Fixture().size(), 9u);
}

// ---------------------------------------------------------------------------
//  Roles that survive into an answer
// ---------------------------------------------------------------------------

TEST(JsonParseRoles, ReadsAStreetWithAllItsFields) {
  std::vector<Parsed> got = Parse(Fixture()[0]);
  ASSERT_EQ(got.size(), 1u);
  const Parsed &p = got[0];
  EXPECT_EQ(p.type, "street");
  EXPECT_EQ(p.type_enum, PHOTON_PLACE_TYPE_STREET);
  EXPECT_EQ(p.name, "Marienplatz");
  EXPECT_EQ(p.street, "Marienplatz") << "a street entry is its own street";
  EXPECT_EQ(p.city, "München");
  EXPECT_EQ(p.postcode, "80331");
  EXPECT_EQ(p.country, "DE");
  EXPECT_NEAR(p.importance, 0.5, 1e-9);
}

TEST(JsonParseRoles, ReadsTheCentroidAsFixedPoint) {
  std::vector<Parsed> got = Parse(Fixture()[0]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].has_point);
  EXPECT_EQ(got[0].lat_e7, 481374000);
  EXPECT_EQ(got[0].lon_e7, 115755000);
}

TEST(JsonParseRoles, AWholeDegreeIsReadAsADegree) {
  // JSON draws no line between 13 and 13.0, and a serializer may drop the
  // fraction of a coordinate that has none.  Both forms name the same place.
  std::vector<Parsed> got = Parse(LineWithCentroid("[13,48]"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].has_point);
  EXPECT_EQ(got[0].lon_e7, 130000000);
  EXPECT_EQ(got[0].lat_e7, 480000000);
}

TEST(JsonParseRoles, AWholeDegreeAndAFractionMixInOneCentroid) {
  std::vector<Parsed> got = Parse(LineWithCentroid("[13,48.1374]"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].lon_e7, 130000000);
  EXPECT_EQ(got[0].lat_e7, 481374000);
}

TEST(JsonParseRoles, AWholeDegreeKeepsItsSign) {
  // west of Greenwich and south of the equator, both written without a fraction
  std::vector<Parsed> got = Parse(LineWithCentroid("[-58,-34]"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].lon_e7, -580000000);
  EXPECT_EQ(got[0].lat_e7, -340000000);
}

TEST(JsonParseRoles, ZeroIsAPlaceLikeAnyOther) {
  std::vector<Parsed> got = Parse(LineWithCentroid("[0,0]"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].has_point) << "the null island is a coordinate, not an absent one";
  EXPECT_EQ(got[0].lon_e7, 0);
  EXPECT_EQ(got[0].lat_e7, 0);
}

TEST(JsonParseRoles, ACoordinateThatIsNoNumberStaysAtZero) {
  std::vector<Parsed> got = Parse(LineWithCentroid(R"(["13","48"])"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].lon_e7, 0);
  EXPECT_EQ(got[0].lat_e7, 0);
}

TEST(JsonParseRoles, ACityEntryBecomesItsOwnCity) {
  std::vector<Parsed> got = Parse(Fixture()[3]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].type_enum, PHOTON_PLACE_TYPE_CITY);
  EXPECT_EQ(got[0].city, "Prag") << "the display side prefers the German reading";
}

TEST(JsonParseRoles, GermanReadingWinsForTheDisplayedName) {
  // name:de is preferred over name wherever the dump offers both
  std::vector<Parsed> got = Parse(Fixture()[3]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "Prag");
}

TEST(JsonParseRoles, WithoutAGermanReadingTheLocalNameStands) {
  std::vector<Parsed> got = Parse(Fixture()[1]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "Osiedle Praha");
  EXPECT_EQ(got[0].city, "Warschau") << "city:de overrides city for the answer";
}

TEST(JsonParseRoles, AHouseKeepsItsNumberAndItsStreet) {
  std::vector<Parsed> got = Parse(Fixture()[2]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].type_enum, PHOTON_PLACE_TYPE_HOUSE);
  EXPECT_EQ(got[0].house, "17");
  EXPECT_EQ(got[0].street, "Berliner Straße");
  EXPECT_EQ(got[0].postcode, "10715");
}

// ---------------------------------------------------------------------------
//  What becomes searchable text
// ---------------------------------------------------------------------------

TEST(JsonParseSearch, TakesEveryLanguageOfTheEntrysOwnName) {
  std::vector<Parsed> got = Parse(Fixture()[3]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(Has(got[0].search, "Praha"));
  EXPECT_TRUE(Has(got[0].search, "Prag"));
  EXPECT_TRUE(Has(got[0].search, "Prague"));
}

TEST(JsonParseSearch, TakesTheGermanReadingOfAnAddressKey) {
  // the round trip: the answer shows "Warschau", so "Warschau" must find it
  std::vector<Parsed> got = Parse(Fixture()[1]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(Has(got[0].search, "Warszawa")) << "the unlocalized reading";
  EXPECT_TRUE(Has(got[0].search, "Warschau")) << "and the German one beside it";
  EXPECT_TRUE(Has(got[0].search, "Polen"));
}

TEST(JsonParseSearch, LeavesOtherLanguagesOfAnAddressKeyToTheirOwnEntry) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"street","name":{"name":"Ulica"},)"
      R"("address":{"city":"Warszawa","city:de":"Warschau","city:en":"Warsaw",)"
      R"("city:fr":"Varsovie"}}]})";
  std::vector<Parsed> got = Parse(line);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(Has(got[0].search, "Warszawa"));
  EXPECT_TRUE(Has(got[0].search, "Warschau"));
  EXPECT_FALSE(Has(got[0].search, "Warsaw")) << "English belongs to Warsaw's own entry";
  EXPECT_FALSE(Has(got[0].search, "Varsovie"));
}

// ---------------------------------------------------------------------------
//  The languages a build reads
// ---------------------------------------------------------------------------

namespace {
PhotonLanguages Languages(const char *list) {
  PhotonLanguages set{};
  EXPECT_TRUE(photon_languages_parse(list, &set));
  return set;
}
std::vector<std::string> TagsOf(const PhotonLanguages &set) {
  std::vector<std::string> tags;
  for (uint8_t i = 0; i < set.count; ++i) tags.emplace_back(set.tag[i]);
  return tags;
}
const Parsed::Reading *ReadingOf(const Parsed &place, const char *tag) {
  auto found = place.readings.find(tag);
  return found == place.readings.end() ? nullptr : &found->second;
}
} // namespace

TEST(PhotonLanguages, ReadsAListAndLowercasesIt) {
  EXPECT_EQ(TagsOf(Languages("de,EN, fr")), (std::vector<std::string>{"de", "en", "fr"}));
}

TEST(PhotonLanguages, KeepsTheOrderBecauseTheFirstIsTheDefault) {
  EXPECT_EQ(TagsOf(Languages("en,de"))[0], "en");
  EXPECT_EQ(TagsOf(Languages("de,en"))[0], "de");
}

TEST(PhotonLanguages, ATagNamedTwiceIsHeldOnce) {
  EXPECT_EQ(TagsOf(Languages("de,en,de")), (std::vector<std::string>{"de", "en"}));
}

TEST(PhotonLanguages, EmptyPiecesArePassedOver) {
  EXPECT_EQ(TagsOf(Languages("de,,  ,en")), (std::vector<std::string>{"de", "en"}));
  PhotonLanguages none{};
  EXPECT_TRUE(photon_languages_parse("", &none));
  EXPECT_EQ(none.count, 0u);
  EXPECT_TRUE(photon_languages_parse(nullptr, &none));
  EXPECT_EQ(none.count, 0u);
}

TEST(PhotonLanguages, AllIsNoListAtAll) {
  PhotonLanguages every = Languages("de,all,en");
  EXPECT_TRUE(every.every);
  EXPECT_EQ(every.count, 0u) << "all replaces the list rather than joining it";
}

TEST(PhotonLanguages, ATagTooLongIsRefusedAndTheRestSurvives) {
  PhotonLanguages set{};
  EXPECT_FALSE(photon_languages_parse("de,this-tag-is-far-too-long,en", &set));
  EXPECT_EQ(TagsOf(set), (std::vector<std::string>{"de", "en"}));
}

TEST(PhotonLanguages, IndexIsWhereATagStands) {
  PhotonLanguages set = Languages("de,en,fr");
  EXPECT_EQ(photon_languages_index(&set, "de"), 0);
  EXPECT_EQ(photon_languages_index(&set, "fr"), 2);
  EXPECT_EQ(photon_languages_index(&set, "it"), -1);
}

TEST(JsonParseLanguages, TheFirstLanguageIsWhatTheAnswerShows) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"city","name":)"
      R"({"name":"Praha","name:de":"Prag","name:en":"Prague"},"address":{}}]})";

  std::vector<Parsed> german = ParseIn(line, Languages("de,en"));
  ASSERT_EQ(german.size(), 1u);
  EXPECT_EQ(german[0].name, "Prag");

  std::vector<Parsed> english = ParseIn(line, Languages("en,de"));
  ASSERT_EQ(english.size(), 1u);
  EXPECT_EQ(english[0].name, "Prague");
}

TEST(JsonParseLanguages, EveryFurtherLanguageBecomesAReadingBesideIt) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"city","name":)"
      R"({"name":"Praha","name:de":"Prag","name:en":"Prague","name:it":"Praga"},"address":{}}]})";
  std::vector<Parsed> got = ParseIn(line, Languages("de,en"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "Prag");

  const Parsed::Reading *english = ReadingOf(got[0], "en");
  ASSERT_NE(english, nullptr);
  EXPECT_EQ(english->name, "Prague");
  EXPECT_EQ(ReadingOf(got[0], "it"), nullptr) << "Italian was not asked for";
  EXPECT_EQ(ReadingOf(got[0], "de"), nullptr) << "the default is in own_name, not beside it";
}

TEST(JsonParseLanguages, ATownIsItsOwnTownInEveryReading) {
  // a city entry fills its city field from its own name, and so must a reading
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"city","name":)"
      R"({"name":"Praha","name:de":"Prag","name:en":"Prague"},"address":{}}]})";
  std::vector<Parsed> got = ParseIn(line, Languages("de,en"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].city, "Prag");
  const Parsed::Reading *english = ReadingOf(got[0], "en");
  ASSERT_NE(english, nullptr);
  EXPECT_EQ(english->city, "Prague");
}

TEST(JsonParseLanguages, AnAncestorReadingIsTakenAsATermWhenItsLanguageWasAskedFor) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"street","name":{"name":"Marszalkowska"},)"
      R"("address":{"city":"Warszawa","city:de":"Warschau","city:en":"Warsaw",)"
      R"("city:fr":"Varsovie"}}]})";

  std::vector<Parsed> german = ParseIn(line, Languages("de"));
  ASSERT_EQ(german.size(), 1u);
  EXPECT_TRUE(Has(german[0].search, "Warschau"));
  EXPECT_FALSE(Has(german[0].search, "Warsaw")) << "English was not asked for";

  std::vector<Parsed> both = ParseIn(line, Languages("de,en"));
  ASSERT_EQ(both.size(), 1u);
  EXPECT_TRUE(Has(both[0].search, "Warszawa")) << "the unlocalized reading always comes along";
  EXPECT_TRUE(Has(both[0].search, "Warschau"));
  EXPECT_TRUE(Has(both[0].search, "Warsaw"));
  EXPECT_FALSE(Has(both[0].search, "Varsovie")) << "French was not asked for";

  const Parsed::Reading *english = ReadingOf(both[0], "en");
  ASSERT_NE(english, nullptr);
  EXPECT_EQ(english->city, "Warsaw");
  EXPECT_TRUE(english->name.empty()) << "the street has no English name of its own";
}

TEST(JsonParseLanguages, TheDefaultReadingStillOutranksThePlainOne) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"street","name":{"name":"Ulica"},)"
      R"("address":{"city":"Warszawa","city:de":"Warschau","city:en":"Warsaw"}}]})";
  std::vector<Parsed> got = ParseIn(line, Languages("en,de"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].city, "Warsaw") << "English leads the list, so English fills the field";
}

TEST(JsonParseLanguages, EveryTagIsTakenWhenAllWasAskedFor) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"city","name":)"
      R"({"name":"Praha","name:de":"Prag","name:en":"Prague","name:it":"Praga"},"address":{}}]})";
  std::vector<Parsed> got = ParseIn(line, Languages("all"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "Praha") << "without a named default the plain reading stands";
  EXPECT_NE(ReadingOf(got[0], "de"), nullptr);
  EXPECT_NE(ReadingOf(got[0], "en"), nullptr);
  EXPECT_NE(ReadingOf(got[0], "it"), nullptr);
}

TEST(JsonParseLanguages, ALongerTagIsNotTheLanguageItStartsWith) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"street","name":{"name":"Ulica"},)"
      R"("address":{"city":"Warszawa","city:de-formal":"Warschau"}}]})";
  std::vector<Parsed> got = ParseIn(line, Languages("de"));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].city, "Warszawa");
  EXPECT_FALSE(Has(got[0].search, "Warschau"));
}

TEST(JsonParseSearch, ADuplicatedTextIsKeptOnce) {
  // city and city:de carry the same word at home, and it must not double
  std::vector<Parsed> got = Parse(Fixture()[0]);
  ASSERT_EQ(got.size(), 1u);
  size_t muenchen = 0;
  for (const std::string &s : got[0].search) {
    if (s == "München") ++muenchen;
  }
  EXPECT_EQ(muenchen, 1u);
}

TEST(JsonParseSearch, TakesTheAlternateNamesOfTheOtherList) {
  std::vector<Parsed> got = Parse(Fixture()[0]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(Has(got[0].search, "Marienplatz Altstadt"));
}

TEST(JsonParseSearch, TakesThePostcodeAsATermOfItsOwn) {
  std::vector<Parsed> got = Parse(Fixture()[0]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(Has(got[0].search, "80331"));
}

TEST(JsonParseSearch, AHouseOffersNothingToTheIndex) {
  // a numbered entry is the payload of its street, not something searched by name
  std::vector<Parsed> got = Parse(Fixture()[2]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].search.empty());
}

TEST(JsonParseSearch, TheHouseNumberNeverBecomesASearchTerm) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"street","name":{"name":"Seeweg"},)"
      R"("address":{"housenumber":"12","city":"Bonn"}}]})";
  std::vector<Parsed> got = Parse(line);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_FALSE(Has(got[0].search, "12"));
}

// ---------------------------------------------------------------------------
//  Which entries are kept at all
// ---------------------------------------------------------------------------

TEST(JsonParseSelection, APondWithoutANumberStaysOutside) {
  // "other" and no house number: not an address, and no level of the hierarchy
  std::vector<Parsed> got = Parse(Fixture()[5]);
  EXPECT_TRUE(got.empty());
}

TEST(JsonParseSelection, AnOtherEntryWithANumberIsAnAddress) {
  std::vector<Parsed> got = Parse(Fixture()[6]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].house, "3");
  EXPECT_EQ(got[0].street, "Seeweg");
}

TEST(JsonParseSelection, ADocumentThatIsNoPlaceIsPassedOver) {
  JsonParseResult result{};
  std::vector<Parsed> got = Parse(Fixture()[7], &result);
  EXPECT_TRUE(got.empty());
  EXPECT_EQ(result.is_valid, 1);
  EXPECT_EQ(result.is_place, 0);
}

TEST(JsonParseSelection, APlaceWithoutContentIsNoError) {
  JsonParseResult result{};
  std::vector<Parsed> got = Parse(Fixture()[8], &result);
  EXPECT_TRUE(got.empty());
  EXPECT_EQ(result.is_place, 1);
  EXPECT_EQ(result.entry_count, 0u);
}

TEST(JsonParseSelection, CountsTheEntriesItHandedOver) {
  JsonParseResult result{};
  Parse(Fixture()[0], &result);
  EXPECT_EQ(result.is_valid, 1);
  EXPECT_EQ(result.is_place, 1);
  EXPECT_EQ(result.entry_count, 1u);
}

TEST(JsonParseSelection, ReadsEveryEntryOfALineWithSeveral) {
  const std::string line =
      R"({"type":"Place","content":[)"
      R"({"address_type":"street","name":{"name":"Erste"},"address":{"city":"Bonn"}},)"
      R"({"address_type":"street","name":{"name":"Zweite"},"address":{"city":"Bonn"}},)"
      R"({"address_type":"city","name":{"name":"Bonn"},"address":{}}]})";
  JsonParseResult result{};
  std::vector<Parsed> got = Parse(line, &result);
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(result.entry_count, 3u);
  EXPECT_EQ(got[0].name, "Erste");
  EXPECT_EQ(got[2].type_enum, PHOTON_PLACE_TYPE_CITY);
}

TEST(JsonParseSelection, AnEntryWithoutACentroidSaysSo) {
  const std::string line =
      R"({"type":"Place","content":[{"address_type":"street","name":{"name":"Seeweg"},)"
      R"("address":{"city":"Bonn"}}]})";
  std::vector<Parsed> got = Parse(line);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_FALSE(got[0].has_point);
  EXPECT_EQ(got[0].lat_e7, 0);
}

TEST(JsonParseSelection, ARootThatIsNoObjectIsRefusedQuietly) {
  const std::string line = "[1,2,3]";
  JsonParseResult result{};
  std::vector<Parsed> places;
  EXPECT_EQ(json_parse_line(line.c_str(), line.size(), &German(), Capture, &places, &result), 1);
  EXPECT_EQ(result.is_valid, 0);
  EXPECT_TRUE(places.empty());
}

// ---------------------------------------------------------------------------
//  The type mapping the file format depends on
// ---------------------------------------------------------------------------

TEST(JsonParseTypes, RecognisesEveryAddressTypeTheDumpUses) {
  const struct {
    const char *text;
    PhotonPlaceType expected;
  } cases[] = {
      {"country", PHOTON_PLACE_TYPE_COUNTRY},   {"state", PHOTON_PLACE_TYPE_STATE},
      {"county", PHOTON_PLACE_TYPE_COUNTY},     {"city", PHOTON_PLACE_TYPE_CITY},
      {"street", PHOTON_PLACE_TYPE_STREET},     {"district", PHOTON_PLACE_TYPE_DISTRICT},
      {"locality", PHOTON_PLACE_TYPE_LOCALITY}, {"house", PHOTON_PLACE_TYPE_HOUSE},
  };
  for (const auto &c : cases) {
    std::string line = std::string(R"({"type":"Place","content":[{"address_type":")") + c.text +
                       R"(","name":{"name":"X"},"address":{"city":"Bonn"}}]})";
    std::vector<Parsed> got = Parse(line);
    ASSERT_EQ(got.size(), 1u) << c.text;
    EXPECT_EQ(got[0].type_enum, c.expected) << c.text;
  }
}

TEST(JsonParseTypes, OtherIsRecognisedWhenItCarriesANumber) {
  // "other" without a house number is passed over, so it needs one to be seen at all.
  std::string line = R"({"type":"Place","content":[{"address_type":"other","housenumber":"3",)"
                     R"("name":{"name":"X"},"address":{"street":"Weg","city":"Bonn"}}]})";
  std::vector<Parsed> got = Parse(line);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].type_enum, PHOTON_PLACE_TYPE_OTHER);
}

/** One line carrying @p address_type, for the refusals below. */
std::string LineWithType(const std::string &address_type) {
  return R"({"type":"Place","content":[{"address_type":")" + address_type +
         R"(","name":{"name":"X"},"address":{"city":"Bonn"}}]})";
}

TEST(JsonParseTypesDeathTest, AValueThatMerelyStartsAlikeIsNotThatValue) {
  // The old dispatch read the first byte and stopped, so anything beginning
  // with 'h' became a house. Recognition is over the whole string now, and an
  // unrecognised type stops the build rather than being filed under a guess.
  EXPECT_EXIT(Parse(LineWithType("horse")), ::testing::ExitedWithCode(1), "unknown type");
  EXPECT_EXIT(Parse(LineWithType("hamlet")), ::testing::ExitedWithCode(1), "unknown type");
  EXPECT_EXIT(Parse(LineWithType("cityscape")), ::testing::ExitedWithCode(1), "unknown type");
  EXPECT_EXIT(Parse(LineWithType("stateless")), ::testing::ExitedWithCode(1), "unknown type");
}

TEST(JsonParseTypesDeathTest, AValueTooShortToBeAnyOfThemReadsNoFurtherThanItself) {
  // These are the inputs the old dispatch ran off the end of: "s" reached for
  // type[2], "c" for type[5]. Both are refusals now, and under a sanitizer this
  // test is also what proves nothing is read past the terminator.
  EXPECT_EXIT(Parse(LineWithType("s")), ::testing::ExitedWithCode(1), "unknown type");
  EXPECT_EXIT(Parse(LineWithType("c")), ::testing::ExitedWithCode(1), "unknown type");
  EXPECT_EXIT(Parse(LineWithType("")), ::testing::ExitedWithCode(1), "unknown type");
}

TEST(JsonParseTypes, TheKindNumbersMatchWhatTheClientHandsOut) {
  // the file format carries these numbers; client.c asserts the same pairs
  EXPECT_EQ((int)PHOTON_PLACE_TYPE_STREET, (int)GEO_PLACE_STREET);
  EXPECT_EQ((int)PHOTON_PLACE_TYPE_CITY, (int)GEO_PLACE_CITY);
  EXPECT_EQ((int)PHOTON_PLACE_TYPE_HOUSE, (int)GEO_PLACE_HOUSE);
  EXPECT_EQ((int)PHOTON_PLACE_TYPE_DISTRICT, (int)GEO_PLACE_DISTRICT);
  EXPECT_EQ((int)PHOTON_PLACE_TYPE_LOCALITY, (int)GEO_PLACE_LOCALITY);
  EXPECT_EQ((int)PHOTON_PLACE_TYPE_INDEPENDENT_CITY, (int)GEO_PLACE_INDEPENDENT_CITY);
}

// ---------------------------------------------------------------------------
//  Limits
// ---------------------------------------------------------------------------

TEST(JsonParseLimits, CountsTheTextsThatFoundNoSlot) {
  std::string line = R"({"type":"Place","content":[{"address_type":"street","name":{)";
  for (int i = 0; i < PHOTON_PLACE_SEARCH_MAX + 40; ++i) {
    if (i) line += ",";
    line += "\"name:x" + std::to_string(i) + "\":\"Text" + std::to_string(i) + "\"";
  }
  line += R"(},"address":{"city":"Bonn"}}]})";

  std::vector<Parsed> got = Parse(line);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_LE(got[0].search.size(), (size_t)PHOTON_PLACE_SEARCH_MAX);
  EXPECT_GT(got[0].search_dropped, 0) << "what did not fit is counted, not lost in silence";
}

TEST(JsonParseDebug, SerialisesAPlaceForTheDebuggersEye) {
  std::vector<Parsed> got = Parse(Fixture()[0]);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_STREQ(photon_place_to_json(nullptr), "null");
}

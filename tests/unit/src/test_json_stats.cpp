/** @file
 *  @brief The counters that turn a run into a report.
 *
 *  Nothing here decides anything — but a wrong count is a lie in the log, and
 *  the sums of many threads have to agree with the sum of one.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

/** A place of one kind, with just enough filled in to be counted. */
PhotonPlace PlaceOf(PhotonPlaceType type, const char *text = "street") {
  PhotonPlace place{};
  std::memset(&place, 0, sizeof(place));
  place.type = text;
  place.typeEnum = type;
  /* a postcode somewhere in the stream, so the parser's sanity check on the
     dump — which ends the process when no entry ever carries one — stays quiet */
  static const char kCode[] = "80331";
  place.postcode.data = kCode;
  place.postcode.size = 5;
  return place;
}

} // namespace

TEST(JsonStats, StartsAtZero) {
  JsonStats stats{};
  EXPECT_EQ(stats.records, 0u);
  EXPECT_EQ(stats.place_records, 0u);
  EXPECT_EQ(stats.streets, 0u);
}

TEST(JsonStats, CountsDocumentsByWhatTheyWere) {
  JsonStats stats{};
  JsonParseResult valid_place{1, 1, 3};
  JsonParseResult valid_other{1, 0, 0};
  JsonParseResult broken{0, 0, 0};

  json_stats_count_document(&stats, &valid_place);
  json_stats_count_document(&stats, &valid_other);
  json_stats_count_document(&stats, &broken);

  EXPECT_EQ(stats.records, 3u);
  EXPECT_EQ(stats.place_records, 1u);
  EXPECT_EQ(stats.invalid_records, 1u);
}

TEST(JsonStats, CountsEveryKindOfEntryInItsOwnColumn) {
  JsonStats stats{};
  const struct {
    PhotonPlaceType type;
    uint64_t JsonStats::*field;
  } cases[] = {
      {PHOTON_PLACE_TYPE_COUNTRY, &JsonStats::countries},
      {PHOTON_PLACE_TYPE_STATE, &JsonStats::states},
      {PHOTON_PLACE_TYPE_COUNTY, &JsonStats::counties},
      {PHOTON_PLACE_TYPE_CITY, &JsonStats::cities},
      {PHOTON_PLACE_TYPE_DISTRICT, &JsonStats::districts},
      {PHOTON_PLACE_TYPE_LOCALITY, &JsonStats::localities},
      {PHOTON_PLACE_TYPE_STREET, &JsonStats::streets},
      {PHOTON_PLACE_TYPE_HOUSE, &JsonStats::houses},
      {PHOTON_PLACE_TYPE_OTHER, &JsonStats::other},
  };

  for (const auto &c : cases) {
    PhotonPlace place = PlaceOf(c.type);
    json_stats_count_place(&stats, &place);
  }

  for (const auto &c : cases) {
    EXPECT_EQ(stats.*(c.field), 1u) << "kind " << (int)c.type;
  }
}

TEST(JsonStats, TheEntryCountComesFromTheDocumentNotFromThePlaces) {
  // an entry is counted where it was found — in the document that carried it
  JsonStats stats{};
  PhotonPlace place = PlaceOf(PHOTON_PLACE_TYPE_STREET);
  json_stats_count_place(&stats, &place);
  EXPECT_EQ(stats.place_entries, 0u);

  JsonParseResult document{1, 1, 3};
  json_stats_count_document(&stats, &document);
  EXPECT_EQ(stats.place_entries, 3u);
}

TEST(JsonStats, CountsTheSearchTermsAnEntryOffered) {
  JsonStats stats{};
  PhotonPlace place = PlaceOf(PHOTON_PLACE_TYPE_STREET);
  place.search_count = 7;
  place.search_dropped = 2;
  json_stats_count_place(&stats, &place);

  EXPECT_EQ(stats.search_terms, 7u);
  EXPECT_EQ(stats.search_dropped, 2u);
}

TEST(JsonStats, AddPutsTwoTalliesTogether) {
  JsonStats a{};
  JsonStats b{};

  for (int i = 0; i < 3; ++i) {
    PhotonPlace p = PlaceOf(PHOTON_PLACE_TYPE_STREET);
    p.search_count = 2;
    json_stats_count_place(&a, &p);
  }
  for (int i = 0; i < 5; ++i) {
    PhotonPlace p = PlaceOf(PHOTON_PLACE_TYPE_CITY, "city");
    p.search_count = 1;
    json_stats_count_place(&b, &p);
  }
  JsonParseResult doc_a{1, 1, 3};
  JsonParseResult doc_b{1, 1, 5};
  json_stats_count_document(&a, &doc_a);
  json_stats_count_document(&b, &doc_b);

  json_stats_add(&a, &b);

  EXPECT_EQ(a.streets, 3u);
  EXPECT_EQ(a.cities, 5u);
  EXPECT_EQ(a.place_entries, 8u);
  EXPECT_EQ(a.search_terms, 11u);
  EXPECT_EQ(a.records, 2u);
  EXPECT_EQ(a.place_records, 2u);
}

TEST(JsonStats, AddingLeavesTheAddendAlone) {
  JsonStats total{};
  JsonStats part{};
  PhotonPlace place = PlaceOf(PHOTON_PLACE_TYPE_STREET);
  json_stats_count_place(&part, &place);

  json_stats_add(&total, &part);
  EXPECT_EQ(part.streets, 1u) << "a summand is read, never emptied";
  EXPECT_EQ(total.streets, 1u);
}

TEST(JsonStats, AddingIsAssociativeAcrossThreads) {
  // what four threads counted separately must equal what one counted alone
  JsonStats one{};
  std::vector<JsonStats> many(4);
  for (int i = 0; i < 40; ++i) {
    PhotonPlace p = PlaceOf(i % 2 ? PHOTON_PLACE_TYPE_STREET : PHOTON_PLACE_TYPE_CITY);
    p.search_count = 3;
    json_stats_count_place(&one, &p);
    json_stats_count_place(&many[i % 4], &p);
  }

  JsonStats merged{};
  for (const JsonStats &part : many) json_stats_add(&merged, &part);

  EXPECT_EQ(merged.streets, one.streets);
  EXPECT_EQ(merged.cities, one.cities);
  EXPECT_EQ(merged.search_terms, one.search_terms);
}

TEST(JsonStats, AddingAnEmptyTallyChangesNothing) {
  JsonStats total{};
  PhotonPlace place = PlaceOf(PHOTON_PLACE_TYPE_STREET);
  json_stats_count_place(&total, &place);
  JsonStats before = total;

  JsonStats empty{};
  json_stats_add(&total, &empty);

  EXPECT_EQ(total.streets, before.streets);
  EXPECT_EQ(total.place_entries, before.place_entries);
}

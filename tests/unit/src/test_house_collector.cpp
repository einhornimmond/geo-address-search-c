/** @file
 *  @brief House numbers, and the streets they hang on.
 *
 *  A number without a street is nothing anyone can find, so the collector
 *  keeps a tally of the homeless beside the ones it placed.  The merge turns
 *  the loose entries into one array ordered by street, with an offset table
 *  that lets a reader jump straight to the numbers of one document.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace {

GeoDocument Street(uint32_t name_rank, int32_t lat = 481374000, int32_t lon = 115755000) {
  GeoDocument d{};
  d.lat_e7 = lat;
  d.lon_e7 = lon;
  d.name_rank = name_rank;
  d.city_rank = GEO_RANK_NONE;
  d.postcode_rank = GEO_RANK_NONE;
  d.importance = 1000;
  d.type = PHOTON_PLACE_TYPE_STREET;
  d.flags = GEO_DOCUMENT_HAS_POINT;
  return d;
}

/** The number ranks standing on one document of the merged set. */
std::vector<uint32_t> NumbersOf(const HouseSet &set, uint32_t document) {
  std::vector<uint32_t> out;
  if (document + 1 > set.document_count) return out;
  for (uint32_t i = set.offsets[document]; i < set.offsets[document + 1]; ++i) {
    out.push_back(set.houses[i].number_rank);
  }
  return out;
}

class HouseCollectorTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(house_collector_init(&collector), ARNM_SUCCESS);
  }
  void TearDown() override {
    house_set_free(&set);
    house_collector_free(&collector);
  }

  void Add(
      uint32_t document,
      const GeoDocument &street,
      uint32_t number,
      int32_t lat = 0,
      int32_t lon = 0,
      int has_point = 1
  ) {
    EXPECT_EQ(
        house_collector_add(&collector, document, &street, number, lat, lon, has_point),
        ARNM_SUCCESS
    );
  }
  void Merge(size_t document_count) {
    HouseCollector *list[1] = {&collector};
    ASSERT_EQ(house_collector_merge(&set, list, 1, document_count), ARNM_SUCCESS);
  }

  HouseCollector collector{};
  HouseSet set{};
};

} // namespace

TEST_F(HouseCollectorTest, StartsEmpty) {
  EXPECT_EQ(house_collector_count(&collector), 0u);
}

TEST_F(HouseCollectorTest, KeepsWhatItWasGiven) {
  GeoDocument street = Street(1);
  Add(0, street, 42, 481000000, 115000000);
  EXPECT_EQ(house_collector_count(&collector), 1u);

  Merge(1);
  ASSERT_EQ(set.house_count, 1u);
  EXPECT_EQ(set.houses[0].number_rank, 42u);
  EXPECT_EQ(set.houses[0].lat_e7, 481000000);
  EXPECT_EQ(set.houses[0].lon_e7, 115000000);
}

TEST_F(HouseCollectorTest, ANumberWithoutAPointBorrowsItsStreets) {
  GeoDocument street = Street(1, 481374000, 115755000);
  Add(0, street, 7, 0, 0, /*has_point=*/0);
  Merge(1);

  ASSERT_EQ(set.house_count, 1u);
  EXPECT_EQ(set.houses[0].lat_e7, 481374000);
  EXPECT_EQ(set.houses[0].lon_e7, 115755000);
  EXPECT_EQ(set.pointless, 1u) << "and it is counted as having brought none";
}

TEST_F(HouseCollectorTest, NumbersLandOnTheDocumentTheyWereGiven) {
  GeoDocument a = Street(1), b = Street(2);
  Add(0, a, 10);
  Add(0, a, 11);
  Add(2, b, 20);
  Merge(3);

  EXPECT_EQ(NumbersOf(set, 0).size(), 2u);
  EXPECT_TRUE(NumbersOf(set, 1).empty()) << "a street without numbers keeps an empty range";
  EXPECT_EQ(NumbersOf(set, 2), (std::vector<uint32_t>{20}));
}

TEST_F(HouseCollectorTest, OffsetsSpanTheWholeArray) {
  GeoDocument street = Street(1);
  for (uint32_t i = 0; i < 20; ++i) Add(i % 4, street, i);
  Merge(4);

  EXPECT_EQ(set.offsets[0], 0u);
  EXPECT_EQ(set.offsets[set.document_count], set.house_count);
  for (size_t d = 0; d < set.document_count; ++d) {
    EXPECT_LE(set.offsets[d], set.offsets[d + 1]) << "document " << d;
  }
}

TEST_F(HouseCollectorTest, NumbersOfOneStreetAreOrderedByTheirRank) {
  GeoDocument street = Street(1);
  const uint32_t ranks[] = {50, 10, 30, 20, 40};
  for (uint32_t r : ranks) Add(0, street, r);
  Merge(1);

  std::vector<uint32_t> got = NumbersOf(set, 0);
  ASSERT_EQ(got.size(), 5u);
  for (size_t i = 1; i < got.size(); ++i) {
    EXPECT_LT(got[i - 1], got[i]) << "at " << i << " — a reader binary-searches these";
  }
}

TEST_F(HouseCollectorTest, EveryNumberSurvivesTheMerge) {
  GeoDocument street = Street(1);
  const uint32_t kCount = 2000;
  for (uint32_t i = 0; i < kCount; ++i) Add(i % 10, street, i);
  Merge(10);

  EXPECT_EQ(set.house_count, kCount);
  size_t total = 0;
  for (uint32_t d = 0; d < 10; ++d) total += NumbersOf(set, d).size();
  EXPECT_EQ(total, kCount);
}

TEST_F(HouseCollectorTest, CountersAreCarriedIntoTheSet) {
  GeoDocument street = Street(1);
  Add(0, street, 1);
  collector.homeless = 3;
  collector.unknown_street = 2;
  collector.without_number = 1;
  Merge(1);

  EXPECT_EQ(set.homeless, 3u);
  EXPECT_EQ(set.unknown_street, 2u);
  EXPECT_EQ(set.without_number, 1u);
}

TEST_F(HouseCollectorTest, ADocumentBeyondTheCountIsNotStored) {
  GeoDocument street = Street(1);
  Add(0, street, 1);
  Add(99, street, 2); // no such document
  Merge(1);

  EXPECT_EQ(set.document_count, 1u);
  EXPECT_EQ(NumbersOf(set, 0), (std::vector<uint32_t>{1}));
}

TEST_F(HouseCollectorTest, NoDocumentsYieldAnEmptySet) {
  Merge(0);
  EXPECT_EQ(set.house_count, 0u);
  EXPECT_EQ(set.document_count, 0u);
}

TEST(HouseCollectorMerge, JoinsWhatSeveralThreadsGathered) {
  HouseCollector a{}, b{};
  ASSERT_EQ(house_collector_init(&a), ARNM_SUCCESS);
  ASSERT_EQ(house_collector_init(&b), ARNM_SUCCESS);

  GeoDocument street = Street(1);
  ASSERT_EQ(house_collector_add(&a, 0, &street, 10, 0, 0, 1), ARNM_SUCCESS);
  ASSERT_EQ(house_collector_add(&a, 1, &street, 20, 0, 0, 1), ARNM_SUCCESS);
  ASSERT_EQ(house_collector_add(&b, 0, &street, 30, 0, 0, 1), ARNM_SUCCESS);
  a.homeless = 1;
  b.homeless = 2;

  HouseSet set{};
  HouseCollector *list[2] = {&a, &b};
  ASSERT_EQ(house_collector_merge(&set, list, 2, 2), ARNM_SUCCESS);

  EXPECT_EQ(set.house_count, 3u);
  EXPECT_EQ(NumbersOf(set, 0).size(), 2u) << "both threads put a number on document 0";
  EXPECT_EQ(NumbersOf(set, 1).size(), 1u);
  EXPECT_EQ(set.homeless, 3u) << "the tallies add up";

  house_set_free(&set);
  house_collector_free(&a);
  house_collector_free(&b);
}

TEST(HouseCollectorGuards, NullIsAnsweredRatherThanDereferenced) {
  EXPECT_NE(house_collector_init(nullptr), ARNM_SUCCESS);
  EXPECT_EQ(house_collector_count(nullptr), 0u);
  house_collector_free(nullptr);
  house_set_free(nullptr);

  HouseSet set{};
  EXPECT_NE(house_collector_merge(nullptr, nullptr, 0, 0), ARNM_SUCCESS);
  house_set_free(&set);
}

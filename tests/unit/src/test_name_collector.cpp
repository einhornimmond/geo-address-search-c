/** @file
 *  @brief The vocabulary — many names in, each distinct one out, exactly once.
 *
 *  The collector is deliberately not a hash map: it groups by leading bytes and
 *  lets duplicates dissolve when the sorted runs flow together.  So what the
 *  tests hold it to is the outcome, not the road — every name findable, every
 *  duplicate gone, and the count of what was offered kept apart from the count
 *  of what remained.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

class NameCollectorTest : public ::testing::Test {
protected:
  void SetUp() override {
    alloc = meta_area_allocator_create();
    ASSERT_NE(alloc, nullptr);
    ASSERT_EQ(name_collector_init(&collector, alloc), GRD_SUCCESS);
  }
  void TearDown() override {
    name_set_free(&set);
    name_run_free(&run);
    name_collector_free(&collector);
    meta_area_allocator_destroy(alloc);
  }

  void Add(const std::string &name) {
    ASSERT_EQ(name_collector_add(&collector, name.c_str(), name.size()), GRD_SUCCESS) << name;
  }

  /** Close the run and merge it alone, the way a single-threaded build would. */
  void Finish() {
    ASSERT_EQ(name_collector_finish(&collector, &run), GRD_SUCCESS);
    const NameRun *runs[1] = {&run};
    ASSERT_EQ(name_run_merge(&set, runs, 1, 1), GRD_SUCCESS);
  }

  bool Knows(const std::string &name, size_t *rank = nullptr) {
    size_t own = 0;
    return name_set_rank(&set, name.c_str(), name.size(), rank ? rank : &own);
  }

  MetaAreaAllocator *alloc = nullptr;
  NameCollector collector{};
  NameRun run{};
  NameSet set{};
};

} // namespace

TEST_F(NameCollectorTest, StartsEmpty) {
  EXPECT_EQ(name_collector_size(&collector), 0u);
  EXPECT_EQ(name_collector_seen(&collector), 0u);
}

TEST_F(NameCollectorTest, KeepsWhatItWasGiven) {
  Add("berlin");
  Add("hamburg");
  Finish();
  EXPECT_EQ(set.count, 2u);
  EXPECT_TRUE(Knows("berlin"));
  EXPECT_TRUE(Knows("hamburg"));
}

TEST_F(NameCollectorTest, DoesNotKnowWhatItNeverSaw) {
  Add("berlin");
  Finish();
  EXPECT_FALSE(Knows("bremen"));
  EXPECT_FALSE(Knows("berli")) << "a prefix is not the name";
  EXPECT_FALSE(Knows("berlins"));
}

TEST_F(NameCollectorTest, DuplicatesDissolve) {
  for (int i = 0; i < 20; ++i) Add("berlin");
  Add("hamburg");
  Finish();
  EXPECT_EQ(set.count, 2u);
  EXPECT_EQ(set.total, 21u) << "what was offered is remembered apart from what remained";
}

TEST_F(NameCollectorTest, TellsSeenApartFromKept) {
  Add("berlin");
  Add("berlin");
  Add("bremen");
  EXPECT_EQ(name_collector_seen(&collector), 3u);
  Finish();
  EXPECT_EQ(set.count, 2u);
}

TEST_F(NameCollectorTest, RanksAreDistinctAndWithinTheCount) {
  const std::vector<std::string> names = {"berlin", "bremen", "hamburg", "koeln", "muenchen"};
  for (const std::string &n : names) Add(n);
  Finish();

  std::vector<size_t> ranks;
  for (const std::string &n : names) {
    size_t rank = SIZE_MAX;
    ASSERT_TRUE(Knows(n, &rank)) << n;
    EXPECT_LT(rank, set.count) << n;
    ranks.push_back(rank);
  }
  std::sort(ranks.begin(), ranks.end());
  EXPECT_EQ(std::unique(ranks.begin(), ranks.end()), ranks.end()) << "no rank handed out twice";
}

TEST_F(NameCollectorTest, TheSameNameAlwaysGetsTheSameRank) {
  Add("berlin");
  Add("hamburg");
  Finish();
  size_t first = SIZE_MAX, again = SIZE_MAX;
  EXPECT_TRUE(Knows("berlin", &first));
  EXPECT_TRUE(Knows("berlin", &again));
  EXPECT_EQ(first, again);
}

TEST_F(NameCollectorTest, GroupsFollowTheLeadingBytes) {
  Add("berlin");
  Add("bergisch");
  Add("hamburg");
  Finish();
  // "be" twice, "ha" once
  EXPECT_EQ(set.group_count, 2u);
  EXPECT_EQ(prefix_tree_count(&set.prefixes), set.group_count);
}

TEST_F(NameCollectorTest, EveryGroupCarriesAtLeastOneName) {
  for (int i = 0; i < 50; ++i) Add("name" + std::to_string(i));
  Finish();
  size_t total = 0;
  for (size_t g = 0; g < set.group_count; ++g) {
    const NameGroup *group = name_set_group_at(&set, g);
    ASSERT_NE(group, nullptr) << "group " << g;
    EXPECT_GT(group->count, 0u) << "group " << g << " is empty";
    total += group->count;
  }
  EXPECT_EQ(total, set.count);
}

TEST_F(NameCollectorTest, HandlesNamesShorterThanThePrefix) {
  Add("a");
  Add("");
  Add("ab");
  Finish();
  EXPECT_TRUE(Knows("a"));
  EXPECT_TRUE(Knows("ab"));
}

TEST_F(NameCollectorTest, HandlesManyNames) {
  const int kCount = 5000;
  for (int i = 0; i < kCount; ++i) Add("strasse" + std::to_string(i));
  for (int i = 0; i < kCount; ++i) Add("strasse" + std::to_string(i)); // every one twice
  Finish();
  EXPECT_EQ(set.count, (size_t)kCount);
  EXPECT_EQ(set.total, (size_t)kCount * 2);
  EXPECT_TRUE(Knows("strasse0"));
  EXPECT_TRUE(Knows("strasse4999"));
}

TEST_F(NameCollectorTest, TextIsComparedByBytesNotByLength) {
  Add("aa");
  Add("aaa");
  Finish();
  EXPECT_TRUE(Knows("aa"));
  EXPECT_TRUE(Knows("aaa"));
  size_t shortRank = 0, longRank = 0;
  Knows("aa", &shortRank);
  Knows("aaa", &longRank);
  EXPECT_NE(shortRank, longRank);
}

TEST_F(NameCollectorTest, NonAsciiBytesSurvive) {
  Add("münchen");
  Add("北京");
  Finish();
  EXPECT_TRUE(Knows("münchen"));
  EXPECT_TRUE(Knows("北京"));
}

TEST_F(NameCollectorTest, AnEmptyRunMergesToAnEmptySet) {
  Finish();
  EXPECT_EQ(set.count, 0u);
  EXPECT_FALSE(Knows("berlin"));
}

// ---------------------------------------------------------------------------
//  Several runs, as several parser threads would produce them
// ---------------------------------------------------------------------------

TEST(NameRunMerge, JoinsWhatSeveralCollectorsGathered) {
  MetaAreaAllocator *alloc = meta_area_allocator_create();
  ASSERT_NE(alloc, nullptr);

  NameCollector a{}, b{}, c{};
  ASSERT_EQ(name_collector_init(&a, alloc), GRD_SUCCESS);
  ASSERT_EQ(name_collector_init(&b, alloc), GRD_SUCCESS);
  ASSERT_EQ(name_collector_init(&c, alloc), GRD_SUCCESS);

  name_collector_add(&a, "berlin", 6);
  name_collector_add(&a, "hamburg", 7);
  name_collector_add(&b, "berlin", 6); // shared with a
  name_collector_add(&b, "bremen", 6);
  name_collector_add(&c, "koeln", 5);

  NameRun ra{}, rb{}, rc{};
  ASSERT_EQ(name_collector_finish(&a, &ra), GRD_SUCCESS);
  ASSERT_EQ(name_collector_finish(&b, &rb), GRD_SUCCESS);
  ASSERT_EQ(name_collector_finish(&c, &rc), GRD_SUCCESS);

  const NameRun *runs[3] = {&ra, &rb, &rc};
  NameSet set{};
  ASSERT_EQ(name_run_merge(&set, runs, 3, 2), GRD_SUCCESS);

  EXPECT_EQ(set.count, 4u) << "berlin came twice and stayed once";
  EXPECT_EQ(set.total, 5u);
  size_t rank = 0;
  for (const char *name : {"berlin", "hamburg", "bremen", "koeln"}) {
    EXPECT_TRUE(name_set_rank(&set, name, std::strlen(name), &rank)) << name;
  }

  name_set_free(&set);
  name_run_free(&ra);
  name_run_free(&rb);
  name_run_free(&rc);
  name_collector_free(&a);
  name_collector_free(&b);
  name_collector_free(&c);
  meta_area_allocator_destroy(alloc);
}

TEST(NameRunMerge, NoRunsYieldAnEmptySet) {
  NameSet set{};
  EXPECT_EQ(name_run_merge(&set, nullptr, 0, 1), GRD_SUCCESS);
  EXPECT_EQ(set.count, 0u);
  name_set_free(&set);
}

TEST(NameRunMerge, RefusesMoreRunsThanItCanHold) {
  NameSet set{};
  std::vector<const NameRun *> many(NAME_RUN_MAX + 1, nullptr);
  EXPECT_NE(name_run_merge(&set, many.data(), many.size(), 1), GRD_SUCCESS);
  name_set_free(&set);
}

TEST(NameCollectorGuards, NullIsAnsweredRatherThanDereferenced) {
  EXPECT_NE(name_collector_init(nullptr, nullptr), GRD_SUCCESS);
  EXPECT_EQ(name_collector_size(nullptr), 0u);
  EXPECT_EQ(name_collector_seen(nullptr), 0u);
  name_collector_free(nullptr);
  name_set_free(nullptr);
  name_run_free(nullptr);
}

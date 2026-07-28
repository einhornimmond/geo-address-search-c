/** @file
 *  @brief The arena allocator — many small takings, few large givings.
 *
 *  Nothing is ever handed back one piece at a time, so what the tests watch is
 *  that every block stays where it was put and that the arenas grow only when
 *  the one in hand is full.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <vector>

namespace {

class MetaAreaTest : public ::testing::Test {
protected:
  void SetUp() override {
    alloc = meta_area_allocator_create();
    ASSERT_NE(alloc, nullptr);
  }
  void TearDown() override { meta_area_allocator_destroy(alloc); }
  MetaAreaAllocator *alloc = nullptr;
};

} // namespace

TEST_F(MetaAreaTest, StartsWithNothingTaken) {
  EXPECT_EQ(meta_area_total_allocated(alloc), 0u);
}

TEST_F(MetaAreaTest, HandsOutUsableMemory) {
  uint8_t *block = nullptr;
  ASSERT_EQ(meta_area_alloc(alloc, &block, 64), GRD_SUCCESS);
  ASSERT_NE(block, nullptr);
  std::memset(block, 0xAB, 64);
  for (int i = 0; i < 64; ++i) EXPECT_EQ(block[i], 0xAB);
}

TEST_F(MetaAreaTest, TotalFollowsWhatWasAskedFor) {
  uint8_t *block = nullptr;
  ASSERT_EQ(meta_area_alloc(alloc, &block, 100), GRD_SUCCESS);
  EXPECT_GE(meta_area_total_allocated(alloc), 100u);
  ASSERT_EQ(meta_area_alloc(alloc, &block, 200), GRD_SUCCESS);
  EXPECT_GE(meta_area_total_allocated(alloc), 300u);
}

TEST_F(MetaAreaTest, EveryBlockIsItsOwn) {
  std::set<uint8_t *> blocks;
  for (int i = 0; i < 500; ++i) {
    uint8_t *block = nullptr;
    ASSERT_EQ(meta_area_alloc(alloc, &block, 32), GRD_SUCCESS) << "block " << i;
    ASSERT_NE(block, nullptr);
    EXPECT_TRUE(blocks.insert(block).second) << "block " << i << " handed out twice";
  }
}

TEST_F(MetaAreaTest, EarlierBlocksSurviveLaterOnes) {
  std::vector<uint8_t *> blocks;
  for (int i = 0; i < 300; ++i) {
    uint8_t *block = nullptr;
    ASSERT_EQ(meta_area_alloc(alloc, &block, 48), GRD_SUCCESS);
    std::memset(block, (uint8_t)i, 48);
    blocks.push_back(block);
  }
  // nothing moved while the arenas grew behind them
  for (size_t i = 0; i < blocks.size(); ++i) {
    for (int b = 0; b < 48; ++b) {
      ASSERT_EQ(blocks[i][b], (uint8_t)i) << "block " << i << " byte " << b;
    }
  }
}

TEST_F(MetaAreaTest, SmallTakingsShareOneArena) {
  uint8_t *block = nullptr;
  ASSERT_EQ(meta_area_alloc(alloc, &block, 16), GRD_SUCCESS);
  size_t after_first = meta_area_arena_count(alloc);
  EXPECT_GE(after_first, 1u);

  // that is the whole point of an arena: many takings, one allocation
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(meta_area_alloc(alloc, &block, 16), GRD_SUCCESS);
  EXPECT_EQ(meta_area_arena_count(alloc), after_first);
}

TEST_F(MetaAreaTest, ArenasNeverGoAway) {
  uint8_t *block = nullptr;
  ASSERT_EQ(meta_area_alloc(alloc, &block, 16), GRD_SUCCESS);
  size_t count = meta_area_arena_count(alloc);
  for (int i = 0; i < 5000; ++i) {
    ASSERT_EQ(meta_area_alloc(alloc, &block, 4096), GRD_SUCCESS);
    size_t now = meta_area_arena_count(alloc);
    ASSERT_GE(now, count) << "an arena in use may never be given back";
    count = now;
  }
}

TEST_F(MetaAreaTest, AnAllocationLargerThanAnArenaStillSucceeds) {
  uint8_t *block = nullptr;
  ASSERT_EQ(meta_area_alloc(alloc, &block, 8u * 1024u * 1024u), GRD_SUCCESS);
  ASSERT_NE(block, nullptr);
  std::memset(block, 7, 8u * 1024u * 1024u);
  EXPECT_EQ(block[8u * 1024u * 1024u - 1], 7);
}

TEST_F(MetaAreaTest, ZeroBytesIsRefusedRatherThanAnswered) {
  // a block of no size is a caller's mistake, not something to hand out
  uint8_t *block = nullptr;
  EXPECT_NE(meta_area_alloc(alloc, &block, 0), GRD_SUCCESS);
}

TEST_F(MetaAreaTest, NullArgumentsAreAnswered) {
  uint8_t *block = nullptr;
  EXPECT_NE(meta_area_alloc(nullptr, &block, 16), GRD_SUCCESS);
  EXPECT_NE(meta_area_alloc(alloc, nullptr, 16), GRD_SUCCESS);
}

TEST(MetaAreaLifetime, DestroyingNullIsSafe) {
  meta_area_allocator_destroy(nullptr);
  EXPECT_EQ(meta_area_total_allocated(nullptr), 0u);
  EXPECT_EQ(meta_area_arena_count(nullptr), 0u);
}

TEST(MetaAreaLifetime, AnEmptyAllocatorCanBeDestroyed) {
  MetaAreaAllocator *alloc = meta_area_allocator_create();
  ASSERT_NE(alloc, nullptr);
  meta_area_allocator_destroy(alloc);
}

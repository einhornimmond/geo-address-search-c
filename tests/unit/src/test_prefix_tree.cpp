/** @file
 *  @brief The prefix tree — leading bytes in, a stable number out.
 *
 *  Two promises carry everything built on it: the same key always returns the
 *  same index, and a key that was never seen creates the next one in order.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace {

/** Intern by text, the way a name collector reaches the tree. */
size_t Intern(PrefixTree *tree, const std::string &name, bool *created = nullptr) {
  PrefixKey key;
  prefix_tree_key(name.c_str(), name.size(), tree->depth, key);
  size_t index = SIZE_MAX;
  bool made = false;
  EXPECT_EQ(prefix_tree_intern(tree, key, &index, &made), ARNM_SUCCESS) << name;
  if (created) *created = made;
  return index;
}

bool Find(const PrefixTree *tree, const std::string &name, size_t *index) {
  PrefixKey key;
  prefix_tree_key(name.c_str(), name.size(), tree->depth, key);
  return prefix_tree_find(tree, key, index);
}

class PrefixTreeTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(prefix_tree_init(&tree, 2), ARNM_SUCCESS);
  }
  void TearDown() override {
    prefix_tree_free(&tree);
  }
  PrefixTree tree{};
};

} // namespace

TEST_F(PrefixTreeTest, StartsEmpty) {
  EXPECT_EQ(prefix_tree_count(&tree), 0u);
  EXPECT_EQ(tree.depth, 2u);
  size_t index = 0;
  EXPECT_FALSE(Find(&tree, "berlin", &index));
}

TEST_F(PrefixTreeTest, HandsOutIndicesInOrderOfArrival) {
  EXPECT_EQ(Intern(&tree, "berlin"), 0u);
  EXPECT_EQ(Intern(&tree, "hamburg"), 1u);
  EXPECT_EQ(Intern(&tree, "muenchen"), 2u);
  EXPECT_EQ(prefix_tree_count(&tree), 3u);
}

TEST_F(PrefixTreeTest, SameKeyReturnsSameIndex) {
  size_t first = Intern(&tree, "berlin");
  size_t again = Intern(&tree, "berlin");
  EXPECT_EQ(first, again);
  EXPECT_EQ(prefix_tree_count(&tree), 1u);
}

TEST_F(PrefixTreeTest, OnlyTheLeadingBytesDecide) {
  // depth 2: everything starting "be" is one key
  size_t a = Intern(&tree, "berlin");
  size_t b = Intern(&tree, "bergisch gladbach");
  EXPECT_EQ(a, b);
  EXPECT_EQ(prefix_tree_count(&tree), 1u);
  EXPECT_NE(a, Intern(&tree, "bonn"));
}

TEST_F(PrefixTreeTest, ReportsWhetherAKeyWasCreated) {
  bool created = false;
  Intern(&tree, "berlin", &created);
  EXPECT_TRUE(created);
  Intern(&tree, "berlin", &created);
  EXPECT_FALSE(created);
}

TEST_F(PrefixTreeTest, FindSeesWhatWasInterned) {
  size_t interned = Intern(&tree, "berlin");
  size_t found = SIZE_MAX;
  EXPECT_TRUE(Find(&tree, "berlin", &found));
  EXPECT_EQ(found, interned);
  EXPECT_FALSE(Find(&tree, "zwickau", &found));
}

TEST_F(PrefixTreeTest, FindDoesNotCreate) {
  size_t index = 0;
  EXPECT_FALSE(Find(&tree, "berlin", &index));
  EXPECT_EQ(prefix_tree_count(&tree), 0u) << "a question must not change the answer";
}

TEST_F(PrefixTreeTest, ShortNamesArePaddedWithZeroes) {
  // "a" and "a\0" are the same key at depth 2
  size_t a = Intern(&tree, "a");
  size_t found = SIZE_MAX;
  EXPECT_TRUE(Find(&tree, "a", &found));
  EXPECT_EQ(a, found);
}

TEST_F(PrefixTreeTest, EmptyNameIsAKeyOfItsOwn) {
  size_t empty = Intern(&tree, "");
  EXPECT_EQ(prefix_tree_count(&tree), 1u);
  EXPECT_NE(empty, Intern(&tree, "berlin"));
}

TEST_F(PrefixTreeTest, MemoryGrowsWithTheTreeAndIsNeverZeroOnceUsed) {
  EXPECT_EQ(prefix_tree_memory(&tree), 0u);
  Intern(&tree, "berlin");
  size_t after_one = prefix_tree_memory(&tree);
  EXPECT_GT(after_one, 0u);
  for (int i = 0; i < 200; ++i) Intern(&tree, std::string(1, (char)('a' + i % 26)) + "x");
  EXPECT_GE(prefix_tree_memory(&tree), after_one);
}

TEST_F(PrefixTreeTest, ForeachVisitsEveryKeyExactlyOnce) {
  const std::vector<std::string> names = {"berlin", "bonn", "hamburg", "muenchen", "zwickau"};
  std::set<size_t> interned;
  for (const std::string &n : names) interned.insert(Intern(&tree, n));
  ASSERT_EQ(interned.size(), names.size());

  struct Seen {
    std::set<size_t> indices;
    size_t calls = 0;
  } seen;

  int rc = prefix_tree_foreach(
      &tree,
      [](const uint8_t *key, size_t index, void *user) -> int {
        (void)key;
        Seen *s = static_cast<Seen *>(user);
        s->indices.insert(index);
        ++s->calls;
        return 0;
      },
      &seen
  );
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(seen.calls, names.size());
  EXPECT_EQ(seen.indices, interned);
}

TEST_F(PrefixTreeTest, ForeachStopsWhenTheVisitorSaysSo) {
  for (int i = 0; i < 10; ++i) Intern(&tree, std::string(1, (char)('a' + i)) + "z");
  int calls = 0;
  int rc = prefix_tree_foreach(
      &tree,
      [](const uint8_t *, size_t, void *user) -> int {
        int *c = static_cast<int *>(user);
        return ++(*c) == 3 ? 42 : 0;
      },
      &calls
  );
  EXPECT_EQ(rc, 42);
  EXPECT_EQ(calls, 3);
}

TEST(PrefixTreeDepth, EveryDepthUpToTheMaximumWorks) {
  for (unsigned depth = 1; depth <= PREFIX_TREE_DEPTH_MAX; ++depth) {
    PrefixTree tree{};
    ASSERT_EQ(prefix_tree_init(&tree, depth), ARNM_SUCCESS) << "depth " << depth;
    EXPECT_EQ(tree.depth, depth);
    EXPECT_EQ(Intern(&tree, "berlin"), 0u);
    EXPECT_EQ(Intern(&tree, "berlin"), 0u);
    prefix_tree_free(&tree);
  }
}

TEST(PrefixTreeDepth, DeeperTreesTellApartWhatShallowOnesJoin) {
  PrefixTree shallow{}, deep{};
  ASSERT_EQ(prefix_tree_init(&shallow, 1), ARNM_SUCCESS);
  ASSERT_EQ(prefix_tree_init(&deep, 4), ARNM_SUCCESS);

  EXPECT_EQ(Intern(&shallow, "berlin"), Intern(&shallow, "bonn"));
  EXPECT_NE(Intern(&deep, "berlin"), Intern(&deep, "bonn"));

  prefix_tree_free(&shallow);
  prefix_tree_free(&deep);
}

TEST(PrefixTreeGuards, RefusesADepthItCannotHold) {
  PrefixTree tree{};
  EXPECT_NE(prefix_tree_init(&tree, 0), ARNM_SUCCESS);
  EXPECT_NE(prefix_tree_init(&tree, PREFIX_TREE_DEPTH_MAX + 1), ARNM_SUCCESS);
}

TEST(PrefixTreeGuards, NullIsAnsweredRatherThanDereferenced) {
  EXPECT_NE(prefix_tree_init(nullptr, 2), ARNM_SUCCESS);
  EXPECT_EQ(prefix_tree_count(nullptr), 0u);
  EXPECT_EQ(prefix_tree_memory(nullptr), 0u);
  prefix_tree_free(nullptr);
}

TEST(PrefixTreeGuards, FreeingTwiceIsSafe) {
  PrefixTree tree{};
  ASSERT_EQ(prefix_tree_init(&tree, 2), ARNM_SUCCESS);
  Intern(&tree, "berlin");
  prefix_tree_free(&tree);
  prefix_tree_free(&tree);
  EXPECT_EQ(prefix_tree_count(&tree), 0u);
}

/** @file
 *  @brief Documents and the words that point at them.
 *
 *  A collector belongs to one thread and numbers its documents from zero; the
 *  merge is what turns several such local streams into the one numbering the
 *  file will carry.  The postings are inverted on the way: collected per
 *  document, stored per word.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

namespace {

GeoDocument Doc(uint32_t name_rank, uint32_t city_rank = GEO_RANK_NONE,
                uint32_t postcode_rank = GEO_RANK_NONE, int32_t lat = 0, int32_t lon = 0) {
  GeoDocument d{};
  d.lat_e7 = lat;
  d.lon_e7 = lon;
  d.name_rank = name_rank;
  d.city_rank = city_rank;
  d.postcode_rank = postcode_rank;
  d.importance = 1000;
  d.type = PHOTON_PLACE_TYPE_STREET;
  d.flags = GEO_DOCUMENT_HAS_POINT;
  return d;
}

/** Every document a given word points at, read out of the merged set. */
std::set<uint32_t> DocumentsOf(const DocSet &set, uint32_t word) {
  std::set<uint32_t> out;
  if (word >= set.word_count) return out;
  for (uint32_t i = set.posting_offsets[word]; i < set.posting_offsets[word + 1]; ++i) {
    out.insert(set.postings[i]);
  }
  return out;
}

class DocCollectorTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_EQ(doc_collector_init(&collector), GRD_SUCCESS); }
  void TearDown() override {
    doc_set_free(&set);
    doc_collector_free(&collector);
  }

  uint32_t AddDoc(const GeoDocument &d) {
    uint32_t number = UINT32_MAX;
    EXPECT_EQ(doc_collector_add_document(&collector, &d, &number), GRD_SUCCESS);
    return number;
  }
  void AddWord(uint32_t word) {
    EXPECT_EQ(doc_collector_add_posting(&collector, word), GRD_SUCCESS);
  }
  void Merge(size_t word_count) {
    DocCollector *list[1] = {&collector};
    ASSERT_EQ(doc_collector_merge(&set, list, 1, word_count), GRD_SUCCESS);
  }

  DocCollector collector{};
  DocSet set{};
};

} // namespace

TEST_F(DocCollectorTest, StartsEmpty) {
  EXPECT_EQ(doc_collector_document_count(&collector), 0u);
  EXPECT_EQ(doc_collector_posting_count(&collector), 0u);
}

TEST_F(DocCollectorTest, NumbersDocumentsFromZeroUpwards) {
  EXPECT_EQ(AddDoc(Doc(1)), 0u);
  EXPECT_EQ(AddDoc(Doc(2)), 1u);
  EXPECT_EQ(AddDoc(Doc(3)), 2u);
  EXPECT_EQ(doc_collector_document_count(&collector), 3u);
}

TEST_F(DocCollectorTest, KeepsTheRecordItWasGiven) {
  AddDoc(Doc(7, 8, 9, 481374000, 115755000));
  Merge(16);
  ASSERT_EQ(set.document_count, 1u);
  EXPECT_EQ(set.documents[0].name_rank, 7u);
  EXPECT_EQ(set.documents[0].city_rank, 8u);
  EXPECT_EQ(set.documents[0].postcode_rank, 9u);
  EXPECT_EQ(set.documents[0].lat_e7, 481374000);
  EXPECT_EQ(set.documents[0].lon_e7, 115755000);
  EXPECT_EQ(set.documents[0].flags & GEO_DOCUMENT_HAS_POINT, GEO_DOCUMENT_HAS_POINT);
}

TEST_F(DocCollectorTest, AWordPointsAtTheDocumentOpenedLast) {
  AddDoc(Doc(1));
  AddWord(5);
  AddDoc(Doc(2));
  AddWord(6);
  Merge(16);

  EXPECT_EQ(DocumentsOf(set, 5), (std::set<uint32_t>{0}));
  EXPECT_EQ(DocumentsOf(set, 6), (std::set<uint32_t>{1}));
}

TEST_F(DocCollectorTest, OneWordMayPointAtManyDocuments) {
  AddDoc(Doc(1));
  AddWord(3);
  AddDoc(Doc(2));
  AddWord(3);
  AddDoc(Doc(3));
  AddWord(3);
  Merge(8);

  EXPECT_EQ(DocumentsOf(set, 3), (std::set<uint32_t>{0, 1, 2}));
}

TEST_F(DocCollectorTest, TheSameWordTwiceOnOneDocumentIsStoredOnce) {
  // the dump offers the same text as city, as state and as street
  AddDoc(Doc(1));
  for (int i = 0; i < 10; ++i) AddWord(4);
  Merge(8);

  EXPECT_EQ(DocumentsOf(set, 4), (std::set<uint32_t>{0}));
  EXPECT_EQ(set.posting_count, 1u);
}

TEST_F(DocCollectorTest, PostingOffsetsSpanTheWholeArray) {
  AddDoc(Doc(1));
  AddWord(0);
  AddWord(2);
  AddDoc(Doc(2));
  AddWord(2);
  Merge(4);

  ASSERT_EQ(set.word_count, 4u);
  EXPECT_EQ(set.posting_offsets[0], 0u);
  EXPECT_EQ(set.posting_offsets[set.word_count], set.posting_count);
  for (size_t w = 0; w < set.word_count; ++w) {
    EXPECT_LE(set.posting_offsets[w], set.posting_offsets[w + 1]) << "word " << w;
  }
}

TEST_F(DocCollectorTest, AWordNobodyUsedHasAnEmptyRange) {
  AddDoc(Doc(1));
  AddWord(2);
  Merge(5);
  EXPECT_TRUE(DocumentsOf(set, 0).empty());
  EXPECT_TRUE(DocumentsOf(set, 4).empty());
  EXPECT_EQ(set.posting_offsets[0], set.posting_offsets[1]);
}

TEST_F(DocCollectorTest, DocumentsWithoutWordsAreStillDocuments) {
  AddDoc(Doc(1));
  AddDoc(Doc(2));
  Merge(4);
  EXPECT_EQ(set.document_count, 2u);
  EXPECT_EQ(set.posting_count, 0u);
}

TEST_F(DocCollectorTest, PostingsOfOneWordAreAscending) {
  for (uint32_t d = 0; d < 200; ++d) {
    AddDoc(Doc(d));
    AddWord(1);
  }
  Merge(2);
  for (uint32_t i = set.posting_offsets[1] + 1; i < set.posting_offsets[2]; ++i) {
    EXPECT_LT(set.postings[i - 1], set.postings[i]) << "at " << i;
  }
}

TEST_F(DocCollectorTest, HandlesManyDocuments) {
  const uint32_t kDocs = 3000;
  for (uint32_t d = 0; d < kDocs; ++d) {
    AddDoc(Doc(d));
    AddWord(d % 50);
  }
  Merge(50);
  EXPECT_EQ(set.document_count, kDocs);
  EXPECT_EQ(set.posting_count, kDocs);
  EXPECT_EQ(DocumentsOf(set, 0).size(), kDocs / 50);
}

// ---------------------------------------------------------------------------
//  Several collectors, as several parser threads would fill them
// ---------------------------------------------------------------------------

TEST(DocCollectorMerge, RenumbersTheDocumentsOfEveryThread) {
  DocCollector a{}, b{};
  ASSERT_EQ(doc_collector_init(&a), GRD_SUCCESS);
  ASSERT_EQ(doc_collector_init(&b), GRD_SUCCESS);

  uint32_t number = 0;
  GeoDocument first = Doc(10);
  GeoDocument second = Doc(20);
  GeoDocument third = Doc(30);
  ASSERT_EQ(doc_collector_add_document(&a, &first, &number), GRD_SUCCESS);
  EXPECT_EQ(number, 0u);
  doc_collector_add_posting(&a, 1);
  ASSERT_EQ(doc_collector_add_document(&b, &second, &number), GRD_SUCCESS);
  EXPECT_EQ(number, 0u) << "each thread counts from zero on its own";
  doc_collector_add_posting(&b, 1);
  ASSERT_EQ(doc_collector_add_document(&b, &third, &number), GRD_SUCCESS);
  EXPECT_EQ(number, 1u);
  doc_collector_add_posting(&b, 2);

  DocSet set{};
  DocCollector *list[2] = {&a, &b};
  ASSERT_EQ(doc_collector_merge(&set, list, 2, 4), GRD_SUCCESS);

  EXPECT_EQ(set.document_count, 3u) << "and the merge gives them one numbering";
  std::set<uint32_t> names;
  for (size_t i = 0; i < set.document_count; ++i) names.insert(set.documents[i].name_rank);
  EXPECT_EQ(names, (std::set<uint32_t>{10, 20, 30}));

  // word 1 stands on one document of each thread, under their new numbers
  EXPECT_EQ(DocumentsOf(set, 1).size(), 2u);
  EXPECT_EQ(DocumentsOf(set, 2).size(), 1u);

  doc_set_free(&set);
  doc_collector_free(&a);
  doc_collector_free(&b);
}

TEST(DocCollectorMerge, NoCollectorsYieldAnEmptySet) {
  DocSet set{};
  EXPECT_EQ(doc_collector_merge(&set, nullptr, 0, 0), GRD_SUCCESS);
  EXPECT_EQ(set.document_count, 0u);
  doc_set_free(&set);
}

TEST(DocCollectorGuards, NullIsAnsweredRatherThanDereferenced) {
  EXPECT_NE(doc_collector_init(nullptr), GRD_SUCCESS);
  EXPECT_EQ(doc_collector_document_count(nullptr), 0u);
  EXPECT_EQ(doc_collector_posting_count(nullptr), 0u);
  doc_collector_free(nullptr);
  doc_set_free(nullptr);
}

TEST(DocCollectorGuards, APostingBeforeAnyDocumentIsRefused) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), GRD_SUCCESS);
  // no document is open, so the word has nothing to point at
  doc_collector_add_posting(&collector, 1);
  EXPECT_EQ(doc_collector_document_count(&collector), 0u);
  doc_collector_free(&collector);
}

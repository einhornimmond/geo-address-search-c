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

GeoDocument Doc(
    uint32_t name_rank,
    uint32_t city_rank = GEO_RANK_NONE,
    uint32_t postcode_rank = GEO_RANK_NONE,
    int32_t lat = 0,
    int32_t lon = 0
) {
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
  void SetUp() override {
    ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  }
  void TearDown() override {
    doc_set_free(&set);
    doc_collector_free(&collector);
  }

  uint32_t AddDoc(const GeoDocument &d) {
    uint32_t number = UINT32_MAX;
    EXPECT_EQ(doc_collector_add_document(&collector, &d, &number), HOSTMEM_SUCCESS);
    return number;
  }
  void AddWord(uint32_t word) {
    EXPECT_EQ(doc_collector_add_posting(&collector, word), HOSTMEM_SUCCESS);
  }
  void Merge(size_t word_count) {
    DocCollector *list[1] = {&collector};
    ASSERT_EQ(doc_collector_merge(&set, list, 1, word_count, 0), HOSTMEM_SUCCESS);
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
  ASSERT_EQ(doc_collector_init(&a), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_init(&b), HOSTMEM_SUCCESS);

  uint32_t number = 0;
  GeoDocument first = Doc(10);
  GeoDocument second = Doc(20);
  GeoDocument third = Doc(30);
  ASSERT_EQ(doc_collector_add_document(&a, &first, &number), HOSTMEM_SUCCESS);
  EXPECT_EQ(number, 0u);
  doc_collector_add_posting(&a, 1);
  ASSERT_EQ(doc_collector_add_document(&b, &second, &number), HOSTMEM_SUCCESS);
  EXPECT_EQ(number, 0u) << "each thread counts from zero on its own";
  doc_collector_add_posting(&b, 1);
  ASSERT_EQ(doc_collector_add_document(&b, &third, &number), HOSTMEM_SUCCESS);
  EXPECT_EQ(number, 1u);
  doc_collector_add_posting(&b, 2);

  DocSet set{};
  DocCollector *list[2] = {&a, &b};
  ASSERT_EQ(doc_collector_merge(&set, list, 2, 4, 0), HOSTMEM_SUCCESS);

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
  EXPECT_EQ(doc_collector_merge(&set, nullptr, 0, 0, 0), HOSTMEM_SUCCESS);
  EXPECT_EQ(set.document_count, 0u);
  doc_set_free(&set);
}

TEST(DocCollectorGuards, NullIsAnsweredRatherThanDereferenced) {
  EXPECT_NE(doc_collector_init(nullptr), HOSTMEM_SUCCESS);
  EXPECT_EQ(doc_collector_document_count(nullptr), 0u);
  EXPECT_EQ(doc_collector_posting_count(nullptr), 0u);
  doc_collector_free(nullptr);
  doc_set_free(nullptr);
}

TEST(DocCollectorGuards, APostingBeforeAnyDocumentIsRefused) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  // no document is open, so the word has nothing to point at
  doc_collector_add_posting(&collector, 1);
  EXPECT_EQ(doc_collector_document_count(&collector), 0u);
  doc_collector_free(&collector);
}

// ---------------------------------------------------------------------------
//  Localized readings, which have to survive the same renumbering the postings do
// ---------------------------------------------------------------------------

namespace {

/** The reading @p language holds for @p document, or nullptr. */
const GeoVariant *VariantOf(const DocSet &set, size_t language, uint32_t document) {
  if (!set.language_offsets || language >= set.language_count) return nullptr;
  for (uint32_t i = set.language_offsets[language]; i < set.language_offsets[language + 1]; ++i) {
    if (set.variants[i].document == document) return &set.variants[i];
  }
  return nullptr;
}

/** The document a name rank ended up as, after the merge renumbered everything. */
uint32_t DocumentNamed(const DocSet &set, uint32_t name_rank) {
  for (size_t i = 0; i < set.document_count; ++i) {
    if (set.documents[i].name_rank == name_rank) return (uint32_t)i;
  }
  return GEO_RANK_NONE;
}

} // namespace

TEST(DocCollectorVariants, AReadingFollowsItsDocumentIntoTheNewNumbering) {
  DocCollector a{}, b{};
  ASSERT_EQ(doc_collector_init(&a), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_init(&b), HOSTMEM_SUCCESS);

  uint32_t number = 0;
  GeoDocument first = Doc(10), second = Doc(20), third = Doc(30);
  ASSERT_EQ(doc_collector_add_document(&a, &first, &number), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&a, 1, 110, 111), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_document(&b, &second, &number), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&b, 1, 120, GEO_RANK_NONE), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_document(&b, &third, &number), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&b, 2, 230, GEO_RANK_NONE), HOSTMEM_SUCCESS);

  DocSet set{};
  DocCollector *list[2] = {&a, &b};
  ASSERT_EQ(doc_collector_merge(&set, list, 2, 4, 3), HOSTMEM_SUCCESS);

  EXPECT_EQ(set.language_count, 3u);
  EXPECT_EQ(set.variant_count, 3u);
  /* language 0 is the default: its reading is the document record itself */
  EXPECT_EQ(set.language_offsets[1] - set.language_offsets[0], 0u);
  EXPECT_EQ(set.language_offsets[2] - set.language_offsets[1], 2u);
  EXPECT_EQ(set.language_offsets[3] - set.language_offsets[2], 1u);

  const GeoVariant *one = VariantOf(set, 1, DocumentNamed(set, 10));
  ASSERT_NE(one, nullptr) << "the reading of the first thread's document";
  EXPECT_EQ(one->name_rank, 110u);
  EXPECT_EQ(one->city_rank, 111u);

  const GeoVariant *two = VariantOf(set, 1, DocumentNamed(set, 20));
  ASSERT_NE(two, nullptr) << "and of the second thread's, whose number shifted";
  EXPECT_EQ(two->name_rank, 120u);
  EXPECT_EQ(two->city_rank, GEO_RANK_NONE);

  EXPECT_EQ(VariantOf(set, 2, DocumentNamed(set, 30))->name_rank, 230u);
  EXPECT_EQ(VariantOf(set, 2, DocumentNamed(set, 10)), nullptr);

  doc_set_free(&set);
  doc_collector_free(&a);
  doc_collector_free(&b);
}

TEST(DocCollectorVariants, EveryLanguagesRunIsAscendingByDocument) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  uint32_t number = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    /* The names descend, and the merge numbers documents by ascending name: the
       document a reading hangs on therefore falls as the readings are collected.
       Handed over in ascending order the run would come out sorted by itself and
       the test would hold even if the merge never sorted at all. */
    GeoDocument d = Doc(14 - i);
    ASSERT_EQ(doc_collector_add_document(&collector, &d, &number), HOSTMEM_SUCCESS);
    ASSERT_EQ(doc_collector_add_variant(&collector, 1, 100 + i, GEO_RANK_NONE), HOSTMEM_SUCCESS);
  }

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), HOSTMEM_SUCCESS);
  ASSERT_EQ(set.variant_count, 5u);
  for (uint32_t i = set.language_offsets[1] + 1; i < set.language_offsets[2]; ++i) {
    EXPECT_LT(set.variants[i - 1].document, set.variants[i].document)
        << "a binary search depends on it";
  }
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, SegmentsOfOneStreetJoinTheirFields) {
  // two pieces of the same street, standing in the same spot: they become one
  // document, and one reading out of what each piece knew
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  uint32_t number = 0;
  GeoDocument piece = Doc(10, 20, 30);
  ASSERT_EQ(doc_collector_add_document(&collector, &piece, &number), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 1, 110, GEO_RANK_NONE), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_document(&collector, &piece, &number), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 1, GEO_RANK_NONE, 111), HOSTMEM_SUCCESS);

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), HOSTMEM_SUCCESS);
  ASSERT_EQ(set.document_count, 1u) << "the two pieces are one street";
  ASSERT_EQ(set.variant_count, 1u) << "and carry one reading between them";
  EXPECT_EQ(set.variants[0].name_rank, 110u);
  EXPECT_EQ(set.variants[0].city_rank, 111u);
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, AReadingThatSaysNothingIsNotStored) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  uint32_t number = 0;
  GeoDocument d = Doc(10);
  ASSERT_EQ(doc_collector_add_document(&collector, &d, &number), HOSTMEM_SUCCESS);
  EXPECT_EQ(
      doc_collector_add_variant(&collector, 1, GEO_RANK_NONE, GEO_RANK_NONE), HOSTMEM_SUCCESS
  );

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), HOSTMEM_SUCCESS);
  EXPECT_EQ(set.variant_count, 0u);
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, ALanguageBeyondTheListIsPassedOver) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  uint32_t number = 0;
  GeoDocument d = Doc(10);
  ASSERT_EQ(doc_collector_add_document(&collector, &d, &number), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 7, 110, 111), HOSTMEM_SUCCESS);

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), HOSTMEM_SUCCESS);
  EXPECT_EQ(set.variant_count, 0u) << "the build named two languages, not eight";
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, WithoutLanguagesNothingIsGatheredAtAll) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  uint32_t number = 0;
  GeoDocument d = Doc(10);
  ASSERT_EQ(doc_collector_add_document(&collector, &d, &number), HOSTMEM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 1, 110, 111), HOSTMEM_SUCCESS);

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 0), HOSTMEM_SUCCESS);
  EXPECT_EQ(set.variant_count, 0u);
  EXPECT_EQ(set.language_count, 0u);
  EXPECT_EQ(set.language_offsets, nullptr);
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, AReadingWithoutAnOpenDocumentIsRefusedQuietly) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), HOSTMEM_SUCCESS);
  EXPECT_EQ(doc_collector_add_variant(&collector, 1, 110, 111), HOSTMEM_SUCCESS);
  EXPECT_EQ(doc_collector_add_variant(nullptr, 1, 110, 111), HOSTMEM_ERROR_NULL_POINTER);
  doc_collector_free(&collector);
}

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

#include <algorithm>
#include <cstring>
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
    ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  }
  void TearDown() override {
    doc_set_free(&set);
    doc_collector_free(&collector);
  }

  uint32_t AddDoc(const GeoDocument &d) {
    uint32_t number = UINT32_MAX;
    EXPECT_EQ(doc_collector_add_document(&collector, &d, 0, &number), ARNM_SUCCESS);
    return number;
  }
  void AddWord(uint32_t word) {
    EXPECT_EQ(doc_collector_add_posting(&collector, word), ARNM_SUCCESS);
  }
  void Merge(size_t word_count) {
    DocCollector *list[1] = {&collector};
    ASSERT_EQ(doc_collector_merge(&set, list, 1, word_count, 0), ARNM_SUCCESS);
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
  ASSERT_EQ(doc_collector_init(&a), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_init(&b), ARNM_SUCCESS);

  uint32_t number = 0;
  GeoDocument first = Doc(10);
  GeoDocument second = Doc(20);
  GeoDocument third = Doc(30);
  ASSERT_EQ(doc_collector_add_document(&a, &first, 0, &number), ARNM_SUCCESS);
  EXPECT_EQ(number, 0u);
  doc_collector_add_posting(&a, 1);
  ASSERT_EQ(doc_collector_add_document(&b, &second, 0, &number), ARNM_SUCCESS);
  EXPECT_EQ(number, 0u) << "each thread counts from zero on its own";
  doc_collector_add_posting(&b, 1);
  ASSERT_EQ(doc_collector_add_document(&b, &third, 0, &number), ARNM_SUCCESS);
  EXPECT_EQ(number, 1u);
  doc_collector_add_posting(&b, 2);

  DocSet set{};
  DocCollector *list[2] = {&a, &b};
  ASSERT_EQ(doc_collector_merge(&set, list, 2, 4, 0), ARNM_SUCCESS);

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

namespace {

/** One entry of a made-up dump: the record, the batch it came in, its words, its readings. */
struct DumpEntry {
  GeoDocument record;
  uint32_t batch;
  std::vector<uint32_t> words;
  std::vector<GeoVariantRecord> readings; /**< @c record of each is ignored. */
};

/**
 * @brief Hand the entries to @p threads collectors, batch by batch, as @p thread_of says.
 *
 *  Each collector receives its batches in dump order, the way a parser thread
 *  takes them off the queue — only which thread gets which batch is chosen here.
 */
void Distribute(
    const std::vector<DumpEntry> &dump,
    std::vector<DocCollector> &threads,
    uint32_t (*thread_of)(uint32_t batch)
) {
  for (DocCollector &collector : threads) ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  /* the dump is read front to back; entries of one batch keep the order given */
  std::vector<DumpEntry> in_order = dump;
  std::stable_sort(in_order.begin(), in_order.end(), [](const DumpEntry &a, const DumpEntry &b) {
    return a.batch < b.batch;
  });
  for (const DumpEntry &entry : in_order) {
    DocCollector &collector = threads[thread_of(entry.batch)];
    uint32_t number = 0;
    ASSERT_EQ(
        doc_collector_add_document(&collector, &entry.record, entry.batch, &number), ARNM_SUCCESS
    );
    for (uint32_t word : entry.words) doc_collector_add_posting(&collector, word);
    for (const GeoVariantRecord &reading : entry.readings) {
      doc_collector_add_variant(&collector, reading.language, reading.name_rank, reading.city_rank);
    }
  }
}

/** Every array a merged set will write into the file, compared byte for byte. */
void ExpectSameSet(const DocSet &a, const DocSet &b) {
  ASSERT_EQ(a.document_count, b.document_count);
  ASSERT_EQ(a.posting_count, b.posting_count);
  ASSERT_EQ(a.street_count, b.street_count);
  ASSERT_EQ(a.variant_count, b.variant_count);
  ASSERT_EQ(a.word_count, b.word_count);
  EXPECT_EQ(memcmp(a.documents, b.documents, a.document_count * sizeof(GeoDocument)), 0);
  EXPECT_EQ(memcmp(a.postings, b.postings, a.posting_count * sizeof(uint32_t)), 0);
  EXPECT_EQ(memcmp(a.posting_offsets, b.posting_offsets, (a.word_count + 1) * sizeof(uint32_t)), 0);
  EXPECT_EQ(memcmp(a.streets, b.streets, a.street_count * sizeof(GeoStreetKey)), 0);
  EXPECT_EQ(memcmp(a.variants, b.variants, a.variant_count * sizeof(GeoVariant)), 0);
}

uint32_t AllInOne(uint32_t) {
  return 0;
}

/** The later batches to the first thread, so the threads' order is the dump's reversed. */
uint32_t LateBatchesFirst(uint32_t batch) {
  return batch >= 4 ? 0 : (batch >= 2 ? 1 : 2);
}

} // namespace

TEST(DocCollectorMerge, TheThreadsABatchFellToDoNotChangeTheSet) {
  // everything that used to be decided by the order the threads were joined in
  std::vector<DumpEntry> dump;

  // three records of one key, 260 m apart in a row: joined greedily, the first
  // founds a cluster and the running centre decides where the third belongs —
  // taken the other way round, a different pair ends up together
  GeoDocument kiosk = Doc(5, 7);
  kiosk.type = PHOTON_PLACE_TYPE_OTHER;
  for (int32_t step = 0; step < 3; ++step) {
    GeoDocument record = kiosk;
    record.lat_e7 = 480000000 + step * 26000;
    dump.push_back({record, (uint32_t)step * 2, {(uint32_t)(10 + step)}, {}});
  }

  // two records alike to the byte but without a point: each stays a document of
  // its own, and only the order says which of the two carries which word
  GeoDocument nowhere = Doc(6, 7);
  nowhere.type = PHOTON_PLACE_TYPE_OTHER;
  nowhere.flags = 0;
  dump.push_back({nowhere, 1, {20}, {}});
  dump.push_back({nowhere, 3, {21}, {}});

  // two segments of one street that disagree about its English name
  GeoDocument street = Doc(8, 7, 9, 480500000, 115000000);
  dump.push_back({street, 5, {30}, {{0, 41, GEO_RANK_NONE, 1}}});
  dump.push_back({street, 5, {31}, {{0, 40, GEO_RANK_NONE, 1}}});

  std::vector<DocCollector> one(1), three(3);
  Distribute(dump, one, AllInOne);
  Distribute(dump, three, LateBatchesFirst);

  DocSet in_one{}, in_three{};
  DocCollector *one_list[1] = {&one[0]};
  DocCollector *three_list[3] = {&three[0], &three[1], &three[2]};
  ASSERT_EQ(doc_collector_merge(&in_one, one_list, 1, 64, 2), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_merge(&in_three, three_list, 3, 64, 2), ARNM_SUCCESS);

  ExpectSameSet(in_one, in_three);
  EXPECT_EQ(in_one.document_count, 5u) << "two kiosks, two nowheres, one street";
  ASSERT_EQ(in_one.variant_count, 1u);
  EXPECT_EQ(in_one.variants[0].name_rank, 40u) << "the spelling that sorts first names it";

  doc_set_free(&in_one);
  doc_set_free(&in_three);
  for (DocCollector &collector : one) doc_collector_free(&collector);
  for (DocCollector &collector : three) doc_collector_free(&collector);
}

TEST(DocCollectorMerge, ABatchMayNotGoBackBehindTheOneBeforeIt) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  GeoDocument record = Doc(1);
  uint32_t number = 0;
  ASSERT_EQ(doc_collector_add_document(&collector, &record, 4, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_document(&collector, &record, 4, &number), ARNM_SUCCESS);
  EXPECT_EQ(doc_collector_add_document(&collector, &record, 3, &number), ARNM_ERROR_INVALID_PARAM)
      << "a thread takes its batches in the order they were cut";
  EXPECT_EQ(doc_collector_document_count(&collector), 2u) << "and the refused one is not stored";
  doc_collector_free(&collector);
}

namespace {

/** A town-level record at a point, playing @p role — see GEO_DOCUMENT_SETTLEMENT. */
GeoDocument Place(
    uint32_t name, double lat, double lon, uint8_t type, unsigned role, uint16_t weight
) {
  GeoDocument d =
      Doc(name, GEO_RANK_NONE, GEO_RANK_NONE, (int32_t)(lat * 1e7), (int32_t)(lon * 1e7));
  d.type = type;
  d.flags = (uint8_t)(GEO_DOCUMENT_HAS_POINT | role);
  d.importance = weight;
  return d;
}

/** Merge @p records, each with the words beside it, in one collector. */
void MergeAll(
    DocSet *set, const std::vector<std::pair<GeoDocument, std::vector<uint32_t>>> &records
) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  for (const auto &[record, words] : records) {
    uint32_t number = 0;
    ASSERT_EQ(doc_collector_add_document(&collector, &record, 0, &number), ARNM_SUCCESS);
    for (uint32_t word : words)
      ASSERT_EQ(doc_collector_add_posting(&collector, word), ARNM_SUCCESS);
  }
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(set, list, 1, 8, 0), ARNM_SUCCESS);
  doc_collector_free(&collector);
}

} // namespace

TEST(DocCollectorMerge, ATownsBoundaryJoinsTheTownItGoverns) {
  // Würzburg: the city's point on the market square, and the boundary of the
  // city's land, 1.9 km off and more than three times as heavy
  DocSet set{};
  MergeAll(
      &set,
      {
          {Place(10, 49.7934, 9.9310, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_SETTLEMENT, 12000), {1}},
          {Place(10, 49.7780, 9.9435, PHOTON_PLACE_TYPE_COUNTY, GEO_DOCUMENT_ADMIN_AREA, 44000),
           {2}},
      }
  );
  ASSERT_EQ(set.document_count, 1u) << "one town, not the town and its land";
  const GeoDocument &town = set.documents[0];
  EXPECT_EQ(town.lat_e7, (int32_t)(49.7934 * 1e7)) << "standing where the town is";
  EXPECT_EQ(town.lon_e7, (int32_t)(9.9310 * 1e7));
  EXPECT_EQ(town.type, PHOTON_PLACE_TYPE_CITY);
  EXPECT_EQ(town.importance, 44000) << "as heavy as the heavier of the two";
  EXPECT_EQ(DocumentsOf(set, 1), (std::set<uint32_t>{0}));
  EXPECT_EQ(DocumentsOf(set, 2), (std::set<uint32_t>{0})) << "answering to the words of both";
  doc_set_free(&set);
}

TEST(DocCollectorMerge, TheBoundarysPostalCodesSurviveATownWithoutOne) {
  // Paris: the point of the city carries no code, its boundary all twenty
  GeoDocument town =
      Place(10, 48.8535, 2.3484, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_SETTLEMENT, 30000);
  GeoDocument land =
      Place(10, 48.8566, 2.3522, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_ADMIN_AREA, 50000);
  land.postcode_rank = 7;
  DocSet set{};
  MergeAll(&set, {{town, {1}}, {land, {2}}});
  ASSERT_EQ(set.document_count, 1u);
  EXPECT_EQ(set.documents[0].postcode_rank, 7u);
  EXPECT_EQ(set.documents[0].lat_e7, town.lat_e7);
  doc_set_free(&set);

  // and a town that brings a code of its own keeps it
  town.postcode_rank = 5;
  DocSet own{};
  MergeAll(&own, {{town, {1}}, {land, {2}}});
  ASSERT_EQ(own.document_count, 1u);
  EXPECT_EQ(own.documents[0].postcode_rank, 5u);
  doc_set_free(&own);
}

TEST(DocCollectorMerge, ACountyOfTheTownsNameFarAwayStaysApart) {
  // the Landkreis Görlitz is called Görlitz too, and lies 11 km from the town
  DocSet set{};
  MergeAll(
      &set,
      {
          {Place(10, 51.1528, 14.9873, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_SETTLEMENT, 40000),
           {1}},
          {Place(10, 51.2500, 14.9500, PHOTON_PLACE_TYPE_COUNTY, GEO_DOCUMENT_ADMIN_AREA, 35000),
           {2}},
      }
  );
  EXPECT_EQ(set.document_count, 2u);
  doc_set_free(&set);
}

TEST(DocCollectorMerge, TheDatelineDoesNotKeepATownFromItsLand) {
  // a town just east of the 180th meridian and its boundary just west of it,
  // 180 m apart the short way and nearly the whole world apart the long way
  DocSet set{};
  MergeAll(
      &set,
      {
          {Place(10, -16.8000, -179.9990, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_SETTLEMENT, 5000),
           {1}},
          {Place(10, -16.8000, 179.9993, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_ADMIN_AREA, 9000),
           {2}},
      }
  );
  ASSERT_EQ(set.document_count, 1u);
  EXPECT_EQ(set.documents[0].lon_e7, (int32_t)(-179.9990 * 1e7)) << "standing where the town is";
  doc_set_free(&set);
}

TEST(DocCollectorMerge, ABoundaryJoinsTheNearerOfTwoTowns) {
  DocSet set{};
  MergeAll(
      &set,
      {
          {Place(10, 50.0000, 8.0000, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_SETTLEMENT, 5000), {1}},
          {Place(10, 50.0300, 8.0000, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_SETTLEMENT, 5000), {2}},
          // 1.1 km from the first, 2.2 km from the second
          {Place(10, 50.0100, 8.0000, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_ADMIN_AREA, 9000), {3}},
      }
  );
  ASSERT_EQ(set.document_count, 2u);
  std::set<uint32_t> first = DocumentsOf(set, 1);
  EXPECT_EQ(DocumentsOf(set, 3), first) << "the land belongs to the town it lies nearer";
  EXPECT_NE(DocumentsOf(set, 2), first);
  doc_set_free(&set);
}

TEST(DocCollectorMerge, ABoundaryWithoutATownNearIsLeftAsItWas) {
  DocSet set{};
  MergeAll(
      &set,
      {
          {Place(10, 49.7780, 9.9435, PHOTON_PLACE_TYPE_COUNTY, GEO_DOCUMENT_ADMIN_AREA, 44000),
           {1}},
          // a place of another name, and a village of this name without a role
          {Place(11, 49.7934, 9.9310, PHOTON_PLACE_TYPE_CITY, GEO_DOCUMENT_SETTLEMENT, 12000), {2}},
          {Place(10, 49.7900, 9.9400, PHOTON_PLACE_TYPE_CITY, 0, 3000), {3}},
      }
  );
  ASSERT_EQ(set.document_count, 3u);
  bool land_kept = false;
  for (size_t d = 0; d < set.document_count; ++d) {
    const GeoDocument &doc = set.documents[d];
    if (doc.type == PHOTON_PLACE_TYPE_COUNTY) {
      land_kept = doc.lat_e7 == (int32_t)(49.7780 * 1e7) && (doc.flags & GEO_DOCUMENT_ADMIN_AREA);
    }
  }
  EXPECT_TRUE(land_kept);
  doc_set_free(&set);
}

TEST(DocCollectorMerge, NoCollectorsYieldAnEmptySet) {
  DocSet set{};
  EXPECT_EQ(doc_collector_merge(&set, nullptr, 0, 0, 0), ARNM_SUCCESS);
  EXPECT_EQ(set.document_count, 0u);
  doc_set_free(&set);
}

TEST(DocCollectorGuards, NullIsAnsweredRatherThanDereferenced) {
  EXPECT_NE(doc_collector_init(nullptr), ARNM_SUCCESS);
  EXPECT_EQ(doc_collector_document_count(nullptr), 0u);
  EXPECT_EQ(doc_collector_posting_count(nullptr), 0u);
  EXPECT_FALSE(doc_collector_limit(nullptr, nullptr));
  doc_collector_free(nullptr);
  doc_set_free(nullptr);
}

// ---------------------------------------------------------------------------
//  The ceiling a per-thread vector carries
// ---------------------------------------------------------------------------

TEST(DocCollectorLimit, ACollectorThatFitsReportsNoLimit) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  GeoDocument document{};
  uint32_t number = 0;
  ASSERT_EQ(doc_collector_add_document(&collector, &document, 0, &number), ARNM_SUCCESS);
  for (uint32_t word = 0; word < 1000; ++word) {
    ASSERT_EQ(doc_collector_add_posting(&collector, word), ARNM_SUCCESS);
  }
  CollectorLimit limit{};
  limit.vector = "poisoned";
  EXPECT_FALSE(doc_collector_limit(&collector, &limit));
  EXPECT_STREQ(limit.vector, "poisoned") << "a collector that fits writes nothing";
  doc_collector_free(&collector);
}

TEST(DocCollectorLimit, TheCeilingIsWhatABucketVectorReallyReaches) {
  // The index array grows in fixed steps and stops at the last one under the cap,
  // so the reachable bucket count is the cap rounded down to that step. A ceiling
  // that named the cap itself would promise room no vector ever has.
  EXPECT_EQ(GEO_VEC_CEILING % (size_t{1} << GEO_WORD_VEC_BUCKET_LOG2), 0u);
  EXPECT_LE(GEO_VEC_CEILING, (size_t)ARNM_BVEC_MAX_INDEX_CAPACITY << GEO_WORD_VEC_BUCKET_LOG2);
  EXPECT_GT(GEO_VEC_CEILING, 0u);

  // and every vector of a collector is sized for exactly that many
  EXPECT_EQ(GEO_DOCUMENT_VEC_BUCKET_LOG2, GEO_WORD_VEC_BUCKET_LOG2);
  EXPECT_EQ(GEO_VARIANT_VEC_BUCKET_LOG2, GEO_WORD_VEC_BUCKET_LOG2);
  EXPECT_EQ(GEO_START_VEC_BUCKET_LOG2, GEO_WORD_VEC_BUCKET_LOG2);
}

TEST(DocCollectorGuards, APostingBeforeAnyDocumentIsRefused) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
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
  ASSERT_EQ(doc_collector_init(&a), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_init(&b), ARNM_SUCCESS);

  uint32_t number = 0;
  GeoDocument first = Doc(10), second = Doc(20), third = Doc(30);
  ASSERT_EQ(doc_collector_add_document(&a, &first, 0, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&a, 1, 110, 111), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_document(&b, &second, 0, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&b, 1, 120, GEO_RANK_NONE), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_document(&b, &third, 0, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&b, 2, 230, GEO_RANK_NONE), ARNM_SUCCESS);

  DocSet set{};
  DocCollector *list[2] = {&a, &b};
  ASSERT_EQ(doc_collector_merge(&set, list, 2, 4, 3), ARNM_SUCCESS);

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
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  uint32_t number = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    /* The names descend, and the merge numbers documents by ascending name: the
       document a reading hangs on therefore falls as the readings are collected.
       Handed over in ascending order the run would come out sorted by itself and
       the test would hold even if the merge never sorted at all. */
    GeoDocument d = Doc(14 - i);
    ASSERT_EQ(doc_collector_add_document(&collector, &d, 0, &number), ARNM_SUCCESS);
    ASSERT_EQ(doc_collector_add_variant(&collector, 1, 100 + i, GEO_RANK_NONE), ARNM_SUCCESS);
  }

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), ARNM_SUCCESS);
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
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  uint32_t number = 0;
  GeoDocument piece = Doc(10, 20, 30);
  ASSERT_EQ(doc_collector_add_document(&collector, &piece, 0, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 1, 110, GEO_RANK_NONE), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_document(&collector, &piece, 0, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 1, GEO_RANK_NONE, 111), ARNM_SUCCESS);

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), ARNM_SUCCESS);
  ASSERT_EQ(set.document_count, 1u) << "the two pieces are one street";
  ASSERT_EQ(set.variant_count, 1u) << "and carry one reading between them";
  EXPECT_EQ(set.variants[0].name_rank, 110u);
  EXPECT_EQ(set.variants[0].city_rank, 111u);
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, AReadingThatSaysNothingIsNotStored) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  uint32_t number = 0;
  GeoDocument d = Doc(10);
  ASSERT_EQ(doc_collector_add_document(&collector, &d, 0, &number), ARNM_SUCCESS);
  EXPECT_EQ(doc_collector_add_variant(&collector, 1, GEO_RANK_NONE, GEO_RANK_NONE), ARNM_SUCCESS);

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), ARNM_SUCCESS);
  EXPECT_EQ(set.variant_count, 0u);
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, ALanguageBeyondTheListIsPassedOver) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  uint32_t number = 0;
  GeoDocument d = Doc(10);
  ASSERT_EQ(doc_collector_add_document(&collector, &d, 0, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 7, 110, 111), ARNM_SUCCESS);

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 2), ARNM_SUCCESS);
  EXPECT_EQ(set.variant_count, 0u) << "the build named two languages, not eight";
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, WithoutLanguagesNothingIsGatheredAtAll) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  uint32_t number = 0;
  GeoDocument d = Doc(10);
  ASSERT_EQ(doc_collector_add_document(&collector, &d, 0, &number), ARNM_SUCCESS);
  ASSERT_EQ(doc_collector_add_variant(&collector, 1, 110, 111), ARNM_SUCCESS);

  DocSet set{};
  DocCollector *list[1] = {&collector};
  ASSERT_EQ(doc_collector_merge(&set, list, 1, 2, 0), ARNM_SUCCESS);
  EXPECT_EQ(set.variant_count, 0u);
  EXPECT_EQ(set.language_count, 0u);
  EXPECT_EQ(set.language_offsets, nullptr);
  doc_set_free(&set);
  doc_collector_free(&collector);
}

TEST(DocCollectorVariants, AReadingWithoutAnOpenDocumentIsRefusedQuietly) {
  DocCollector collector{};
  ASSERT_EQ(doc_collector_init(&collector), ARNM_SUCCESS);
  EXPECT_EQ(doc_collector_add_variant(&collector, 1, 110, 111), ARNM_SUCCESS);
  EXPECT_EQ(doc_collector_add_variant(nullptr, 1, 110, 111), ARNM_ERROR_NULL_POINTER);
  doc_collector_free(&collector);
}

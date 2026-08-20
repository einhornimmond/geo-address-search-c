/** @file
 *  @brief The dump written down in the shape the later passes need it.
 *
 *  Two promises have to hold, and everything else follows: what goes in comes
 *  out unchanged — absent stays absent, empty stays empty, and a text is still
 *  NUL-terminated on the other side — and a cache that does not answer for this
 *  dump is refused rather than read.  The second is the one that matters: a
 *  cache read when it should not be builds an index nobody can tell from a
 *  whole one.
 */

#include "c_api.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

/** A directory of one's own, gone when the test ends. */
class TempDir {
public:
  explicit TempDir(const char *stem) {
    char buffer[256];
    std::snprintf(
        buffer, sizeof(buffer), "/tmp/geoindex_cache_%s_%d_%p", stem, (int)getpid(), (void *)this
    );
    path_ = buffer;
    mkdir(path_.c_str(), 0700);
  }
  ~TempDir() {
    // the cache never nests, so one level of removal is the whole of it
    for (unsigned t = 0; t < 8; ++t) place_cache_discard(path_.c_str(), t + 1);
    rmdir(path_.c_str());
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;

  const char *c_str() const {
    return path_.c_str();
  }
  std::string file(unsigned thread, const char *suffix) const {
    return path_ + "/photon-cache-" + std::to_string(thread) + "." + suffix;
  }

private:
  std::string path_;
};

PhotonString Text(const char *value) {
  PhotonString text{};
  text.data = value;
  text.size = value ? std::strlen(value) : 0;
  return text;
}

std::string Value(PhotonString text) {
  return text.data ? std::string(text.data, text.size) : std::string("<absent>");
}

/** A street, as the parser would hand it over. */
PhotonPlace Street(const char *name, const char *city, const char *postcode) {
  PhotonPlace place{};
  place.typeEnum = PHOTON_PLACE_TYPE_STREET;
  place.own_name = Text(name);
  place.city = Text(city);
  place.postcode = Text(postcode);
  place.lat_e7 = 507350000;
  place.lon_e7 = 70980000;
  place.has_point = 1;
  place.importance = 0.0534;
  return place;
}

/** A door on a street. */
PhotonPlace House(const char *street, const char *city, const char *postcode, const char *number) {
  PhotonPlace place{};
  place.typeEnum = PHOTON_PLACE_TYPE_HOUSE;
  place.street = Text(street);
  place.city = Text(city);
  place.postcode = Text(postcode);
  place.house = Text(number);
  place.lat_e7 = 507391765;
  place.lon_e7 = 71194806;
  place.has_point = 1;
  return place;
}

/** Everything one file holds, read back. */
std::vector<PhotonPlace> ReadAll(
    const char *directory, unsigned thread, PlaceCacheKind kind, bool *out_broken = nullptr
) {
  std::vector<PhotonPlace> places;
  PlaceCacheReader reader{};
  if (place_cache_reader_open(&reader, directory, thread, kind) != HOSTMEM_SUCCESS) {
    if (out_broken) *out_broken = true;
    return places;
  }
  PhotonPlace place;
  while (place_cache_read(&reader, &place)) {
    // the strings point into the reader's buffer, so what is kept is a copy
    places.push_back(place);
    places.back().own_name = Text(
        place.own_name.data ? strdup(std::string(place.own_name.data, place.own_name.size).c_str())
                            : nullptr
    );
    places.back().city = Text(
        place.city.data ? strdup(std::string(place.city.data, place.city.size).c_str()) : nullptr
    );
    places.back().postcode = Text(
        place.postcode.data ? strdup(std::string(place.postcode.data, place.postcode.size).c_str())
                            : nullptr
    );
    places.back().street = Text(
        place.street.data ? strdup(std::string(place.street.data, place.street.size).c_str())
                          : nullptr
    );
    places.back().house = Text(
        place.house.data ? strdup(std::string(place.house.data, place.house.size).c_str()) : nullptr
    );
  }
  if (out_broken) *out_broken = reader.broken;
  place_cache_reader_close(&reader);
  return places;
}

} // namespace

// ---------------------------------------------------------------------------
//  What goes in comes out
// ---------------------------------------------------------------------------

TEST(PlaceCache, AStreetSurvivesTheRoundTrip) {
  TempDir directory{"street"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  PhotonPlace out = Street("Hauptstraße", "Bonn", "53111");
  out.search_count = 2;
  out.search[0] = Text("Hauptstraße");
  out.search[1] = Text("Bonn");
  ASSERT_EQ(place_cache_write(&writer, &out), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  std::vector<PhotonPlace> back = ReadAll(directory.c_str(), 0, PLACE_CACHE_DOCUMENTS);
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(Value(back[0].own_name), "Hauptstraße");
  EXPECT_EQ(Value(back[0].city), "Bonn");
  EXPECT_EQ(Value(back[0].postcode), "53111");
  EXPECT_EQ(back[0].typeEnum, PHOTON_PLACE_TYPE_STREET);
  EXPECT_EQ(back[0].lat_e7, 507350000);
  EXPECT_EQ(back[0].lon_e7, 70980000);
  EXPECT_EQ(back[0].has_point, 1);
  EXPECT_NEAR(back[0].importance, 0.0534, 1e-12) << "the weight is kept to the bit";
  EXPECT_EQ(back[0].search_count, 2);
}

TEST(PlaceCache, AHouseSurvivesTheRoundTrip) {
  TempDir directory{"house"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  PhotonPlace out = House("Hauptstraße", "Bonn", "53111", "12a");
  ASSERT_EQ(place_cache_write(&writer, &out), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  std::vector<PhotonPlace> back = ReadAll(directory.c_str(), 0, PLACE_CACHE_HOUSES);
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(Value(back[0].street), "Hauptstraße");
  EXPECT_EQ(Value(back[0].house), "12a");
  EXPECT_EQ(Value(back[0].city), "Bonn");
  EXPECT_EQ(Value(back[0].postcode), "53111");
}

TEST(PlaceCache, AbsentAndEmptyStayApart) {
  TempDir directory{"absent"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  PhotonPlace out = Street("Feldweg", nullptr, "");
  ASSERT_EQ(place_cache_write(&writer, &out), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  PlaceCacheReader reader{};
  ASSERT_EQ(
      place_cache_reader_open(&reader, directory.c_str(), 0, PLACE_CACHE_DOCUMENTS), HOSTMEM_SUCCESS
  );
  PhotonPlace back{};
  ASSERT_TRUE(place_cache_read(&reader, &back));
  EXPECT_EQ(back.city.data, nullptr) << "a field the entry never had comes back as nothing";
  ASSERT_NE(back.postcode.data, nullptr) << "an empty text is a text";
  EXPECT_EQ(back.postcode.size, 0u);
  place_cache_reader_close(&reader);
}

TEST(PlaceCache, TextsComeBackTerminated) {
  // the parser hands over NUL-terminated strings; anything reading past the
  // length would find a terminator there, and must here too
  TempDir directory{"terminated"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  PhotonPlace out = Street("Marienplatz", "München", "80331");
  ASSERT_EQ(place_cache_write(&writer, &out), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  PlaceCacheReader reader{};
  ASSERT_EQ(
      place_cache_reader_open(&reader, directory.c_str(), 0, PLACE_CACHE_DOCUMENTS), HOSTMEM_SUCCESS
  );
  PhotonPlace back{};
  ASSERT_TRUE(place_cache_read(&reader, &back));
  EXPECT_STREQ(back.own_name.data, "Marienplatz");
  EXPECT_STREQ(back.city.data, "München");
  place_cache_reader_close(&reader);
}

TEST(PlaceCache, EverySearchTextIsKept) {
  TempDir directory{"search"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  PhotonPlace out = Street("Praha", "Praha", "11000");
  out.search_count = PHOTON_PLACE_SEARCH_MAX;
  std::vector<std::string> texts;
  for (int i = 0; i < PHOTON_PLACE_SEARCH_MAX; ++i) texts.push_back("term-" + std::to_string(i));
  for (int i = 0; i < PHOTON_PLACE_SEARCH_MAX; ++i) out.search[i] = Text(texts[i].c_str());
  ASSERT_EQ(place_cache_write(&writer, &out), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  PlaceCacheReader reader{};
  ASSERT_EQ(
      place_cache_reader_open(&reader, directory.c_str(), 0, PLACE_CACHE_DOCUMENTS), HOSTMEM_SUCCESS
  );
  PhotonPlace back{};
  ASSERT_TRUE(place_cache_read(&reader, &back));
  ASSERT_EQ(back.search_count, PHOTON_PLACE_SEARCH_MAX);
  EXPECT_EQ(Value(back.search[0]), "term-0");
  EXPECT_EQ(
      Value(back.search[PHOTON_PLACE_SEARCH_MAX - 1]),
      "term-" + std::to_string(PHOTON_PLACE_SEARCH_MAX - 1)
  );
  place_cache_reader_close(&reader);
}

// ---------------------------------------------------------------------------
//  Which file an entry belongs in
// ---------------------------------------------------------------------------

TEST(PlaceCache, TheTwoHalvesOfTheDumpAreWrittenApart) {
  TempDir directory{"halves"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);

  PhotonPlace street = Street("Hauptstraße", "Bonn", "53111");
  PhotonPlace house = House("Hauptstraße", "Bonn", "53111", "5");
  ASSERT_EQ(place_cache_write(&writer, &street), HOSTMEM_SUCCESS);
  ASSERT_EQ(place_cache_write(&writer, &house), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  EXPECT_EQ(ReadAll(directory.c_str(), 0, PLACE_CACHE_DOCUMENTS).size(), 1u);
  EXPECT_EQ(ReadAll(directory.c_str(), 0, PLACE_CACHE_HOUSES).size(), 1u);
}

TEST(PlaceCache, AHouseWithoutANumberIsKeptAllTheSame) {
  // it becomes neither document nor door — but the first pass still takes its
  // town and its postal code into the dictionary, so the cache may not drop it
  TempDir directory{"numberless"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);

  PhotonPlace nameless{};
  nameless.typeEnum = PHOTON_PLACE_TYPE_HOUSE;
  nameless.own_name = Text("Villa Sonnenschein");
  nameless.city = Text("Bonn");
  nameless.postcode = Text("53111");
  ASSERT_EQ(place_cache_write(&writer, &nameless), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  std::vector<PhotonPlace> back = ReadAll(directory.c_str(), 0, PLACE_CACHE_HOUSES);
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(Value(back[0].city), "Bonn");
  EXPECT_EQ(back[0].house.data, nullptr) << "and the third pass will pass over it";
  EXPECT_EQ(ReadAll(directory.c_str(), 0, PLACE_CACHE_DOCUMENTS).size(), 0u);
}

// ---------------------------------------------------------------------------
//  A cache that may not be read
// ---------------------------------------------------------------------------

TEST(PlaceCacheRefusal, AFileThatIsNotThereIsRefused) {
  TempDir directory{"missing"};
  PlaceCacheReader reader{};
  EXPECT_EQ(
      place_cache_reader_open(&reader, directory.c_str(), 0, PLACE_CACHE_DOCUMENTS),
      HOSTMEM_ERROR_DECODE_FAILED
  );
}

TEST(PlaceCacheRefusal, TheOtherHalfIsRefused) {
  TempDir directory{"kind"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  // the documents file, opened as if it held houses
  std::string documents = directory.file(0, "documents");
  std::string houses = directory.file(0, "houses");
  std::remove(houses.c_str());
  std::ifstream source(documents, std::ios::binary);
  std::ofstream sink(houses, std::ios::binary);
  sink << source.rdbuf();
  sink.close();

  PlaceCacheReader reader{};
  EXPECT_EQ(
      place_cache_reader_open(&reader, directory.c_str(), 0, PLACE_CACHE_HOUSES),
      HOSTMEM_ERROR_INVALID_PARAM
  );
}

TEST(PlaceCacheRefusal, ARewrittenMagicIsRefused) {
  TempDir directory{"magic"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  std::fstream file(
      directory.file(0, "documents"), std::ios::in | std::ios::out | std::ios::binary
  );
  ASSERT_TRUE(file.good());
  file.seekp(0);
  file.put('X');
  file.close();

  PlaceCacheReader reader{};
  EXPECT_EQ(
      place_cache_reader_open(&reader, directory.c_str(), 0, PLACE_CACHE_DOCUMENTS),
      HOSTMEM_ERROR_INVALID_PARAM
  );
}

TEST(PlaceCacheRefusal, ATruncatedFileSaysSoInsteadOfEnding) {
  TempDir directory{"truncated"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  for (int i = 0; i < 8; ++i) {
    PhotonPlace place = Street("Hauptstraße", "Bonn", "53111");
    ASSERT_EQ(place_cache_write(&writer, &place), HOSTMEM_SUCCESS);
  }
  place_cache_writer_close(&writer);

  std::string path = directory.file(0, "documents");
  struct stat status;
  ASSERT_EQ(stat(path.c_str(), &status), 0);
  ASSERT_EQ(truncate(path.c_str(), status.st_size - 12), 0) << "cut through the last record";

  bool broken = false;
  std::vector<PhotonPlace> back = ReadAll(directory.c_str(), 0, PLACE_CACHE_DOCUMENTS, &broken);
  EXPECT_LT(back.size(), 8u);
  EXPECT_TRUE(broken) << "half a cache must not look like a whole one";
}

TEST(PlaceCacheRefusal, AWholeFileEndsWithoutComplaint) {
  TempDir directory{"whole"};
  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  for (int i = 0; i < 8; ++i) {
    PhotonPlace place = Street("Hauptstraße", "Bonn", "53111");
    ASSERT_EQ(place_cache_write(&writer, &place), HOSTMEM_SUCCESS);
  }
  place_cache_writer_close(&writer);

  bool broken = true;
  std::vector<PhotonPlace> back = ReadAll(directory.c_str(), 0, PLACE_CACHE_DOCUMENTS, &broken);
  EXPECT_EQ(back.size(), 8u);
  EXPECT_FALSE(broken);
}

// ---------------------------------------------------------------------------
//  The manifest — what makes a cache readable again
// ---------------------------------------------------------------------------

namespace {

/** A file standing in for a dump, so a stamp has something to be taken from. */
class FakeDump {
public:
  explicit FakeDump(const char *stem, size_t size) {
    char buffer[256];
    std::snprintf(
        buffer, sizeof(buffer), "/tmp/geoindex_dump_%s_%d_%p.zst", stem, (int)getpid(), (void *)this
    );
    path_ = buffer;
    std::ofstream out(path_, std::ios::binary);
    out << std::string(size, 'z');
  }
  ~FakeDump() {
    std::remove(path_.c_str());
  }
  const char *c_str() const {
    return path_.c_str();
  }

private:
  std::string path_;
};

} // namespace

TEST(PlaceCacheManifest, ASealedCacheIsReadAgain) {
  TempDir directory{"sealed"};
  FakeDump dump{"sealed", 1024};

  PlaceCacheStamp stamp{};
  ASSERT_TRUE(place_cache_stamp_of(dump.c_str(), 1, &stamp));
  EXPECT_EQ(stamp.dump_bytes, 1024u);
  EXPECT_EQ(stamp.threads, 1u);

  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);

  EXPECT_FALSE(place_cache_is_current(directory.c_str(), &stamp)) << "not sealed yet";
  ASSERT_EQ(place_cache_seal(directory.c_str(), &stamp), HOSTMEM_SUCCESS);
  EXPECT_TRUE(place_cache_is_current(directory.c_str(), &stamp));
}

TEST(PlaceCacheManifest, AnotherDumpIsNotThisOne) {
  TempDir directory{"otherdump"};
  FakeDump dump{"otherdump", 1024};
  PlaceCacheStamp stamp{};
  ASSERT_TRUE(place_cache_stamp_of(dump.c_str(), 1, &stamp));

  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);
  ASSERT_EQ(place_cache_seal(directory.c_str(), &stamp), HOSTMEM_SUCCESS);

  PlaceCacheStamp grown = stamp;
  grown.dump_bytes += 1;
  EXPECT_FALSE(place_cache_is_current(directory.c_str(), &grown)) << "the dump changed size";

  PlaceCacheStamp touched = stamp;
  touched.dump_mtime += 1;
  EXPECT_FALSE(place_cache_is_current(directory.c_str(), &touched)) << "the dump was rewritten";

  PlaceCacheStamp shifted = stamp;
  shifted.layout += 1;
  EXPECT_FALSE(place_cache_is_current(directory.c_str(), &shifted))
      << "a record gained a field the old file does not carry";

  PlaceCacheStamp wider = stamp;
  wider.threads = 4;
  EXPECT_FALSE(place_cache_is_current(directory.c_str(), &wider))
      << "four threads cannot read what one thread wrote";
}

TEST(PlaceCacheManifest, AMissingFileUndoesTheSeal) {
  TempDir directory{"halfgone"};
  FakeDump dump{"halfgone", 1024};
  PlaceCacheStamp stamp{};
  ASSERT_TRUE(place_cache_stamp_of(dump.c_str(), 1, &stamp));

  PlaceCacheWriter writer{};
  ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), 0), HOSTMEM_SUCCESS);
  place_cache_writer_close(&writer);
  ASSERT_EQ(place_cache_seal(directory.c_str(), &stamp), HOSTMEM_SUCCESS);
  ASSERT_TRUE(place_cache_is_current(directory.c_str(), &stamp));

  std::remove(directory.file(0, "houses").c_str());
  EXPECT_FALSE(place_cache_is_current(directory.c_str(), &stamp))
      << "the manifest promised a file that is gone";
}

TEST(PlaceCacheManifest, DiscardLeavesNothingBehind) {
  TempDir directory{"discard"};
  FakeDump dump{"discard", 1024};
  PlaceCacheStamp stamp{};
  ASSERT_TRUE(place_cache_stamp_of(dump.c_str(), 2, &stamp));

  for (unsigned t = 0; t < 2; ++t) {
    PlaceCacheWriter writer{};
    ASSERT_EQ(place_cache_writer_open(&writer, directory.c_str(), t), HOSTMEM_SUCCESS);
    place_cache_writer_close(&writer);
  }
  ASSERT_EQ(place_cache_seal(directory.c_str(), &stamp), HOSTMEM_SUCCESS);
  ASSERT_TRUE(place_cache_is_current(directory.c_str(), &stamp));

  place_cache_discard(directory.c_str(), 2);
  EXPECT_FALSE(place_cache_is_current(directory.c_str(), &stamp));
  EXPECT_EQ(access(directory.file(0, "documents").c_str(), F_OK), -1);
  EXPECT_EQ(access(directory.file(1, "houses").c_str(), F_OK), -1);
}

TEST(PlaceCacheManifest, NoStampWithoutADump) {
  PlaceCacheStamp stamp{};
  EXPECT_FALSE(place_cache_stamp_of("/nonexistent/photon/dump.zst", 1, &stamp));
  EXPECT_FALSE(place_cache_stamp_of(nullptr, 1, &stamp));
}

// ---------------------------------------------------------------------------
//  Room on the disk
// ---------------------------------------------------------------------------

TEST(PlaceCacheRoom, ADirectoryThatIsNotThereIsMade) {
  // a mistyped path used to look exactly like a full disk; now the path is made
  // and only what really cannot be made is refused
  char wanted[256];
  std::snprintf(wanted, sizeof(wanted), "/tmp/geoindex_made_%d/deep/deeper", (int)getpid());
  uint64_t free_bytes = 0;
  EXPECT_EQ(place_cache_make_room(wanted, 1024, &free_bytes), PLACE_CACHE_ROOM_OK);
  EXPECT_GT(free_bytes, 0u);
  EXPECT_EQ(access(wanted, W_OK), 0) << "and the parents were made with it";

  rmdir(wanted);
  char middle[256];
  std::snprintf(middle, sizeof(middle), "/tmp/geoindex_made_%d/deep", (int)getpid());
  rmdir(middle);
  char top[256];
  std::snprintf(top, sizeof(top), "/tmp/geoindex_made_%d", (int)getpid());
  rmdir(top);
}

TEST(PlaceCacheRoom, AFileIsNoDirectory) {
  FakeDump dump{"notadir", 16};
  EXPECT_EQ(place_cache_make_room(dump.c_str(), 1, nullptr), PLACE_CACHE_ROOM_NOT_DIRECTORY);
}

TEST(PlaceCacheRoom, WhatCannotBeMadeIsSaidSo) {
  FakeDump dump{"blocked", 16};
  // a directory below a file cannot come into being
  std::string below = std::string(dump.c_str()) + "/cache";
  EXPECT_EQ(place_cache_make_room(below.c_str(), 1, nullptr), PLACE_CACHE_ROOM_UNMAKEABLE);
  EXPECT_EQ(place_cache_make_room(nullptr, 1024, nullptr), PLACE_CACHE_ROOM_UNMAKEABLE);
  EXPECT_EQ(place_cache_make_room("", 1024, nullptr), PLACE_CACHE_ROOM_UNMAKEABLE);
}

TEST(PlaceCacheRoom, HalfTheDumpAgainIsWhatIsAskedFor) {
  // measured: a planet cache is 119 % of the packed dump, and the demand sits
  // at 150 % of it
  EXPECT_EQ(place_cache_wanted(16), 24u);
  EXPECT_EQ(place_cache_wanted(0), 0u);
  EXPECT_GT(place_cache_wanted(UINT64_C(26000000000)), UINT64_C(38000000000))
      << "and it does not overflow on a dump of twenty-six gigabytes";
  EXPECT_LT(place_cache_wanted(UINT64_C(26000000000)), UINT64_C(40000000000));

  TempDir directory{"room"};
  uint64_t free_bytes = 0;
  ASSERT_EQ(place_cache_make_room(directory.c_str(), 1024, &free_bytes), PLACE_CACHE_ROOM_OK);
  ASSERT_GT(free_bytes, 0u);

  // a dump whose cache would not fit is refused
  EXPECT_EQ(place_cache_make_room(directory.c_str(), free_bytes / 2, nullptr), PLACE_CACHE_ROOM_OK);
  EXPECT_EQ(
      place_cache_make_room(directory.c_str(), free_bytes, nullptr), PLACE_CACHE_ROOM_TOO_SMALL
  );
}

TEST(PlaceCacheRoom, EveryReasonHasAWording) {
  for (int room = PLACE_CACHE_ROOM_OK; room <= PLACE_CACHE_ROOM_TOO_SMALL; ++room) {
    const char *reason = place_cache_room_reason((PlaceCacheRoom)room);
    ASSERT_NE(reason, nullptr);
    EXPECT_GT(std::strlen(reason), 0u) << room;
  }
}

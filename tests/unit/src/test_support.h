/** @file
 *  @brief What more than one test needs: a scratch path, a fixture reader, and
 *         a whole index built small enough to hold in the head.
 *
 *  The index tests could have been given a prebuilt `.gdx` to open, but a
 *  binary fixture ages badly — it carries a format version, and the day the
 *  layout shifts it becomes a file nobody can regenerate.  So the index is
 *  built here, through the same collectors the real builder uses, and the
 *  fixture that ages is the one that cannot: plain text.
 */

#pragma once

#include "c_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace testsupport {

/** A path in the system's scratch space, removed again when it goes out of scope. */
class TempPath {
public:
  explicit TempPath(const char *stem) {
    char buffer[256];
    std::snprintf(
        buffer, sizeof(buffer), "/tmp/geoindex_test_%s_%d_%p.gdx", stem, (int)getpid(),
        (void *)this
    );
    path_ = buffer;
  }
  ~TempPath() { std::remove(path_.c_str()); }

  TempPath(const TempPath &) = delete;
  TempPath &operator=(const TempPath &) = delete;

  const char *c_str() const { return path_.c_str(); }
  const std::string &str() const { return path_; }

private:
  std::string path_;
};

/** Read a fixture from tests/data; the tests run from the project root. */
inline std::string ReadFixture(const std::string &name) {
  std::ifstream in("tests/data/" + name, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream all;
  all << in.rdbuf();
  return all.str();
}

/** Every line of a fixture, empty lines dropped — one JSON document per line. */
inline std::vector<std::string> ReadFixtureLines(const std::string &name) {
  std::vector<std::string> lines;
  std::istringstream in(ReadFixture(name));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

/** One place the miniature index shall carry. */
struct MiniPlace {
  std::string name;
  std::string city;
  std::string postcode;
  int32_t lat_e7 = 0;
  int32_t lon_e7 = 0;
  uint8_t type = PHOTON_PLACE_TYPE_STREET;
  uint16_t importance = 1000;
  std::vector<std::string> houses;
  /** Clear it for an entry the dump gave no centroid — it then carries no cell
   *  either, exactly as the builder writes it. */
  bool has_point = true;
  /** Names the place also answers to and is no longer shown as: a former name,
   *  a name in another language.  Searchable, never displayed — which is what
   *  the dump's `old_name` and `name:xx` fields amount to. */
  std::vector<std::string> aliases;
};

/**
 * @brief Write a whole index of @p places to @p path.
 *
 *  Walks the three passes the real builder walks — vocabulary, documents,
 *  house numbers — with one thread and one run each.  The searchable words of
 *  a place are its name, its town and its postal code, folded by the same
 *  tokenizer a query goes through, so what the tests type finds what they
 *  wrote.
 *
 *  @return true when the file was written; on failure everything taken is
 *          given back and the file does not exist.
 */
inline bool BuildMiniIndex(const char *path, const std::vector<MiniPlace> &places) {
  MetaAreaAllocator *alloc = meta_area_allocator_create();
  if (!alloc) return false;

  NameCollector words{};
  NameCollector display{};
  DocCollector docs{};
  HouseCollector houses{};
  NameRun word_run{};
  NameRun display_run{};
  NameSet word_set{};
  NameSet display_set{};
  DocSet doc_set{};
  HouseSet house_set{};
  bool ok = false;
  uint64_t total_terms = 0;

  TextTokenizer tok;
  text_tokenizer_init(&tok);
  /* every text counts here, even one that repeats — the filter serves a
     planet-sized dump, not a handful of rows */
  tok.repetition_filter = 0;

  auto add_written = [&](const std::string &text) {
    if (text.empty()) return;
    name_collector_add(&display, text.c_str(), text.size());
  };
  auto add_folded = [&](const std::string &text) {
    if (text.empty()) return;
    size_t n = text_tokenize(&tok, text.c_str(), text.size());
    for (size_t i = 0; i < n; ++i) {
      name_collector_add(&words, tok.tokens[i].data, tok.tokens[i].size);
      ++total_terms;
    }
  };
  auto rank_of = [](const NameSet *set, const std::string &text) -> uint32_t {
    size_t rank = 0;
    if (text.empty()) return GEO_RANK_NONE;
    if (!name_set_rank(set, text.c_str(), text.size(), &rank)) return GEO_RANK_NONE;
    return (uint32_t)rank;
  };

  if (name_collector_init(&words, alloc) != GRD_SUCCESS) goto done;
  if (name_collector_init(&display, alloc) != GRD_SUCCESS) goto done;

  /* --- pass one: everything anyone might type, and everything shown back.
         The cell a place stands in joins the search words exactly as the
         builder writes it, or nothing here could be found by position. --- */
  for (const MiniPlace &p : places) {
    if (p.has_point) {
      char cell[GEO_CELL_TOKEN_SIZE];
      size_t cell_size = geo_cell_token(cell, geo_cell_of(p.lat_e7, p.lon_e7));
      name_collector_add(&words, cell, cell_size);
      ++total_terms;
    }
    add_folded(p.name);
    add_folded(p.city);
    add_folded(p.postcode);
    for (const std::string &alias : p.aliases) add_folded(alias);
    add_written(p.name);
    add_written(p.city);
    add_written(p.postcode);
    for (const std::string &number : p.houses) add_written(number);
  }

  if (name_collector_finish(&words, &word_run) != GRD_SUCCESS) goto done;
  if (name_collector_finish(&display, &display_run) != GRD_SUCCESS) goto done;
  {
    const NameRun *word_runs[1] = {&word_run};
    const NameRun *display_runs[1] = {&display_run};
    if (name_run_merge(&word_set, word_runs, 1, 1) != GRD_SUCCESS) goto done;
    if (name_run_merge(&display_set, display_runs, 1, 1) != GRD_SUCCESS) goto done;
  }

  /* --- pass two: a record per place, and the words that point at it --- */
  if (doc_collector_init(&docs) != GRD_SUCCESS) goto done;
  for (const MiniPlace &p : places) {
    GeoDocument record{};
    record.lat_e7 = p.lat_e7;
    record.lon_e7 = p.lon_e7;
    record.name_rank = rank_of(&display_set, p.name);
    record.city_rank = rank_of(&display_set, p.city);
    record.postcode_rank = rank_of(&display_set, p.postcode);
    record.importance = p.importance;
    record.type = p.type;
    record.flags = p.has_point ? GEO_DOCUMENT_HAS_POINT : 0u;

    uint32_t number = 0;
    if (doc_collector_add_document(&docs, &record, &number) != GRD_SUCCESS) goto done;

    {
      std::vector<const std::string *> texts = {&p.name, &p.city, &p.postcode};
      for (const std::string &alias : p.aliases) texts.push_back(&alias);
      for (const std::string *text : texts) {
        if (text->empty()) continue;
        size_t n = text_tokenize(&tok, text->c_str(), text->size());
        for (size_t i = 0; i < n; ++i) {
          size_t rank = 0;
          if (!name_set_rank(&word_set, tok.tokens[i].data, tok.tokens[i].size, &rank)) continue;
          if (doc_collector_add_posting(&docs, (uint32_t)rank) != GRD_SUCCESS) goto done;
        }
      }
    }
    if (p.has_point) {
      char cell[GEO_CELL_TOKEN_SIZE];
      size_t cell_size = geo_cell_token(cell, geo_cell_of(p.lat_e7, p.lon_e7));
      size_t rank = 0;
      if (name_set_rank(&word_set, cell, cell_size, &rank)) {
        if (doc_collector_add_posting(&docs, (uint32_t)rank) != GRD_SUCCESS) goto done;
      }
    }
  }
  {
    DocCollector *doc_list[1] = {&docs};
    if (doc_collector_merge(&doc_set, doc_list, 1, word_set.count) != GRD_SUCCESS) goto done;
  }

  /* --- pass three: the numbers hang on the streets they belong to.
         The merge gave the documents a numbering of its own, so a place's own
         position in the list says nothing about where its record ended up —
         the street has to be looked up by what identifies it, exactly as the
         builder's third pass does. --- */
  if (house_collector_init(&houses) != GRD_SUCCESS) goto done;
  for (const MiniPlace &p : places) {
    if (p.houses.empty()) continue;
    int relaxed = 0;
    uint32_t document = doc_set_find_street(
        &doc_set, rank_of(&display_set, p.name), rank_of(&display_set, p.city),
        rank_of(&display_set, p.postcode), p.lat_e7, p.lon_e7, 1, &relaxed
    );
    if (document == GEO_RANK_NONE || document >= doc_set.document_count) continue;
    for (const std::string &written : p.houses) {
      uint32_t number = rank_of(&display_set, written);
      if (number == GEO_RANK_NONE) continue;
      if (house_collector_add(
              &houses, document, &doc_set.documents[document], number, p.lat_e7, p.lon_e7, 1
          ) != GRD_SUCCESS) {
        goto done;
      }
    }
  }
  {
    HouseCollector *house_list[1] = {&houses};
    if (house_collector_merge(&house_set, house_list, 1, doc_set.document_count) != GRD_SUCCESS) {
      goto done;
    }
  }

  ok = geo_index_write(path, &word_set, &display_set, &doc_set, &house_set, total_terms) ==
       GRD_SUCCESS;

done:
  house_set_free(&house_set);
  house_collector_free(&houses);
  doc_set_free(&doc_set);
  doc_collector_free(&docs);
  name_set_free(&word_set);
  name_set_free(&display_set);
  name_run_free(&word_run);
  name_run_free(&display_run);
  name_collector_free(&words);
  name_collector_free(&display);
  /* the names point into the arenas, so these outlive everything above */
  meta_area_allocator_destroy(alloc);
  if (!ok) std::remove(path);
  return ok;
}

/** The handful of places every index test shares. */
inline std::vector<MiniPlace> SamplePlaces() {
  return {
      {"Marienplatz", "München", "80331", 481374000, 115755000, PHOTON_PLACE_TYPE_STREET, 60000,
       {"1", "8", "12a"}},
      {"Berliner Straße", "Berlin", "10715", 524869779, 133283388, PHOTON_PLACE_TYPE_STREET, 3501,
       {"17", "88"}},
      {"Berliner Straße", "Potsdam", "14467", 523956000, 130649000, PHOTON_PLACE_TYPE_STREET, 40000,
       {"17"}},
      {"Osiedle Praha", "Warschau", "03-000", 522533694, 210843160, PHOTON_PLACE_TYPE_LOCALITY,
       5248, {}},
      {"Hauptstraße", "Bonn", "53111", 507350000, 70980000, PHOTON_PLACE_TYPE_STREET, 2000, {"5"}},
  };
}

} // namespace testsupport

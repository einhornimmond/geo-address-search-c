/** @defgroup geo_index_section_kind Geo index section kind
 *  @ingroup types
 *  @brief What a section of the index file holds — the table of contents the
 *         writer lays down and the reader walks.
 *
 *  The numbers are part of the file format.  They are stored as a `uint32_t` in
 *  every section descriptor, so they may never be renumbered — the reader finds
 *  a section by its kind, not by its place in the table.
 *  @ref GEO_INDEX_SECTION_COUNT is how many the writer always lays down, and
 *  what a reader insists on is not that figure but the sections it needs.
 *
 *  @whisper A table of contents, written before anyone reads
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a section of the index file holds. */
typedef enum GeoIndexSectionKind {
  GEO_INDEX_SECTION_WORD_GROUPS = 1,     /**< @ref GeoIndexGroup, ascending by key. */
  GEO_INDEX_SECTION_WORD_OFFSETS = 2,    /**< uint32, word_count + 1 of them. */
  GEO_INDEX_SECTION_WORD_TEXT = 3,       /**< Folded words, concatenated. */
  GEO_INDEX_SECTION_DISPLAY_GROUPS = 4,  /**< As above, for the written spelling. */
  GEO_INDEX_SECTION_DISPLAY_OFFSETS = 5, /**< uint32, display_count + 1 of them. */
  GEO_INDEX_SECTION_DISPLAY_TEXT = 6,    /**< Spellings as they are written, concatenated. */
  GEO_INDEX_SECTION_DOCUMENTS = 7,       /**< @ref GeoDocument records. */
  GEO_INDEX_SECTION_POSTING_OFFSETS = 8, /**< uint64 byte offsets, word_count + 1 of them. */
  GEO_INDEX_SECTION_POSTINGS = 9,        /**< One serialized bitmap per word. */
  GEO_INDEX_SECTION_IMPORTANCE = 10,     /**< uint16 per document, for ranking alone. */
  GEO_INDEX_SECTION_HOUSES = 11,         /**< @ref GeoHouse records, by street. */
  GEO_INDEX_SECTION_HOUSE_OFFSETS = 12   /**< uint32, document_count + 1 of them. */
} GeoIndexSectionKind;

/** @brief Sections the writer always lays down.
 *
 *  The reader does not insist on this figure — it accepts a header naming 1 … 64 descriptors
 *  and looks each section up by its kind, so a file gained a section since is still readable.
 *  What it refuses is a section it needs and does not find. */
enum {
  GEO_INDEX_SECTION_COUNT = 12 /**< Sections the writer always lays down. */
};

#ifdef __cplusplus
}
#endif

/** @} */

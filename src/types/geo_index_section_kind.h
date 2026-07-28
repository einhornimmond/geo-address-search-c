/** @defgroup geo_index_section_kind Geo index section kind
 *  @ingroup types
 *  @brief What a section of the index file holds — the table of contents the
 *         writer lays down and the reader walks.
 *
 *  The numbers are part of the file format and are stored in every section
 *  descriptor, so they may never be renumbered.  @ref GEO_INDEX_SECTION_COUNT is
 *  how many the writer always produces; a reader checks it before trusting a
 *  header.
 *  @{
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** What a section holds. */
typedef enum GeoIndexSectionKind {
  GEO_INDEX_SECTION_WORD_GROUPS = 1,     /**< @ref GeoIndexGroup, ascending by key. */
  GEO_INDEX_SECTION_WORD_OFFSETS = 2,    /**< uint32, word_count + 1 of them. */
  GEO_INDEX_SECTION_WORD_TEXT = 3,       /**< Folded words, concatenated. */
  GEO_INDEX_SECTION_DISPLAY_GROUPS = 4,  /**< As above, for the written spelling. */
  GEO_INDEX_SECTION_DISPLAY_OFFSETS = 5,
  GEO_INDEX_SECTION_DISPLAY_TEXT = 6,
  GEO_INDEX_SECTION_DOCUMENTS = 7,       /**< @ref GeoDocument records. */
  GEO_INDEX_SECTION_POSTING_OFFSETS = 8, /**< uint64 byte offsets, word_count + 1 of them. */
  GEO_INDEX_SECTION_POSTINGS = 9,        /**< One serialized bitmap per word. */
  GEO_INDEX_SECTION_IMPORTANCE = 10,     /**< uint16 per document, for ranking alone. */
  GEO_INDEX_SECTION_HOUSES = 11,         /**< @ref GeoHouse records, by street. */
  GEO_INDEX_SECTION_HOUSE_OFFSETS = 12   /**< uint32, document_count + 1 of them. */
} GeoIndexSectionKind;

/** Sections the writer always produces. */
enum { GEO_INDEX_SECTION_COUNT = 12 };

#ifdef __cplusplus
}
#endif

/** @} */

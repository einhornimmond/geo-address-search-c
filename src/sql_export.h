/** @defgroup sql_export SQL export
 *  @ingroup io
 *  @brief Write the normalised address hierarchy as a self-contained
 *         PostgreSQL script with COPY data, PostGIS geometry, and indexes.
 *  @{
 */

#pragma once

/** Opaque handle — full definition in storage_stats_internal.h. */
typedef struct StorageStats StorageStats;

/**
 * @brief Write the deduplicated store as a normalised PostgreSQL script.
 *
 *  Produces a self-contained .sql file with DDL for seven hierarchy tables,
 *  tab-separated COPY data sections with resolved foreign keys, PostGIS
 *  geometry columns, pg_trgm indexes, and a final VACUUM ANALYZE.
 *
 *  @param[in] stats     Collector containing the deduplicated data.
 *  @param[in] filename  Path to the output .sql file (overwritten).
 *
 *  @whisper The stream settles into stone
 */
void storage_stats_write_sql(const StorageStats *stats, const char *filename);

/** @} */

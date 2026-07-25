#include "stb_ds.h"

#include "storage_stats_internal.h"
#include "sql_export.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "gradido_blockchain_core/utils/mono_timer.h"
#include "gradido_blockchain_core/utils/converter.h"

/* =========================================================================
 *  Helpers
 * ========================================================================= */

/** stb_ds string→int64 map — wrapper required for hmgeti/hmput on scalar values. */
typedef struct { BinKey key; int64_t value; } KeyToId;

/** Write an int64_t to file — no printf format-string overhead. */
static void fput_int64(FILE *f, int64_t val)
{
    char buf[21];
    size_t n = grdu_int64_to_string(buf, sizeof(buf), val);
    fwrite(buf, 1, n, f);
}

/** Format a centroid as WKT POINT, or \N if no point.
 *
 *  Converts int32_t × 10⁷ directly to "xx.xxxxxxx" without floating-point.
 *  Fractional part uses the +10⁷ trick: 3456789 → "13456789" → overwrite
 *  leading '1' with '.', yielding ".3456789".
 */
static void format_centroid(char *buf, size_t buf_size, const Entity *e)
{
    if (!e->has_point) {
        memcpy(buf, "\\N", 3);
        return;
    }

    char *p = buf;
    size_t rem = buf_size;

    memcpy(p, "POINT(", 6); p += 6; rem -= 6;

    for (int c = 0; c < 2; ++c) {
        if (c == 1) { *p++ = ' '; --rem; }
        int32_t val = c == 0 ? e->centroid_lon_e7 : e->centroid_lat_e7;
        size_t n;
        if (val < 0) { *p++ = '-'; --rem; val = -val; }
        n = grdu_int64_to_string(p, rem, (int64_t)(val / 10000000));
        p += n; rem -= n;
        *p++ = '.'; --rem;
        uint32_t frac = (uint32_t)(val % 10000000) + 10000000u;
        n = grdu_uint64_to_string(p, rem, (uint64_t)frac);
        *p = '.';  /* overwrite leading '1' of "1xxxxxxx" */
        p += n; rem -= n;
    }
    *p++ = ')';
    *p = '\0';
    (void)rem;
}

/** Escape a string for tab-separated COPY (backslash, tab, newline). */
static void copy_escape(char *dst, const char *src)
{
    while (*src) {
        switch (*src) {
        case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
        case '\t': *dst++ = '\\'; *dst++ = 't';  break;
        case '\n': *dst++ = '\\'; *dst++ = 'n';  break;
        case '\r': *dst++ = '\\'; *dst++ = 'r';  break;
        default:   *dst++ = *src;                break;
        }
        src++;
    }
    *dst = '\0';
}

/* =========================================================================
 *  Progress (200 ms throttle, same pattern as progress.c)
 * ========================================================================= */

typedef struct {
    grdu_mono_timer start;
    grdu_mono_timer since_last;
    uint64_t        total;
    uint64_t        written;
} SqlProgress;

static void sql_progress_init(SqlProgress *sp, uint64_t total)
{
    sp->total   = total;
    sp->written = 0;
    grdu_mono_timer_reset(&sp->start);
    sp->since_last = sp->start;
}

static void sql_progress_tick(SqlProgress *sp, const char *table, uint64_t row_count)
{
    sp->written += row_count;
    double elapsed = grdu_mono_timer_millis(sp->since_last);
    if (elapsed < 200.0) return;
    grdu_mono_timer_reset(&sp->since_last);

    double pct  = sp->total > 0 ? 100.0 * ((double)sp->written / (double)sp->total) : 0;
    double secs = grdu_mono_timer_seconds(sp->start);
    uint64_t rate = secs > 0.0 ? (uint64_t)((double)sp->written / secs) : 0;

    char wr_buf[21], rt_buf[21];
    grdu_uint64_to_string(wr_buf, sizeof(wr_buf), sp->written);
    grdu_uint64_to_string(rt_buf, sizeof(rt_buf), rate);
    printf("\r  SQL-Export: %6.2f%%  %s (%s Zeilen, %s Zeilen/s)  ",
           pct, table, wr_buf, rt_buf);
    fflush(stdout);
}

static void sql_progress_finish(SqlProgress *sp)
{
    double secs = grdu_mono_timer_seconds(sp->start);
    uint64_t rate = secs > 0.0 ? (uint64_t)((double)sp->written / secs) : 0;
    char wr_buf[21], rt_buf[21];
    grdu_uint64_to_string(wr_buf, sizeof(wr_buf), sp->written);
    grdu_uint64_to_string(rt_buf, sizeof(rt_buf), rate);
    printf("\r  SQL-Export: 100.00%%  %s Zeilen in %.1f s (%s Zeilen/s)    \n",
           wr_buf, secs, rt_buf);
    fflush(stdout);
}

/* =========================================================================
 *  COPY writer
 * ========================================================================= */

/** Write a COPY section for one hierarchy level. */
static void write_copy_section(
    FILE *f,
    const KeySet *set,
    const char *table_name,
    const char *column_list,
    KeyToId   **parent_map,    /* key→id hash (NULL for root) */
    int        *next_id,
    KeyToId   **out_map,       /* key→id hash to build for this level */
    SqlProgress *sp)           /* optional progress tracker            */
{
    KeyToId *key_to_id = NULL;

    fputs("\nCOPY ", f);
    fputs(table_name, f);
    fputs(" ", f);
    fputs(column_list, f);
    fputs(" FROM stdin;\n", f);

    for (ptrdiff_t i = 0; i < hmlen(set->entries); ++i) {
        const Entity *e = &set->entries[i];
        int64_t id = (*next_id)++;

        /* resolve FK */
        int64_t parent_id = 0;
        if (parent_map && *parent_map && memcmp(&e->parent_key, &BINKEY_NULL, 16) != 0) {
            ptrdiff_t pi = hmgeti(*parent_map, e->parent_key);
            if (pi >= 0) parent_id = (*parent_map)[pi].value;
        }

        /* build output columns */
        char name_buf[512], centroid_buf[128];
        copy_escape(name_buf, e->name ? e->name : "");
        format_centroid(centroid_buf, sizeof(centroid_buf), e);

        if (table_name[0] == 'c' && table_name[1] == 'o') {
            /* countries: id, code, name, centroid */
            fput_int64(f, id);                fputc('\t', f);
            fputs(e->code ? e->code : "", f); fputc('\t', f);
            fputs(name_buf, f);               fputc('\t', f);
            fputs(centroid_buf, f);           fputc('\n', f);
        } else {
            /* states, counties, cities, postcodes, streets, houses: id, parent_id, name, centroid */
            fput_int64(f, id);                fputc('\t', f);
            fput_int64(f, parent_id);         fputc('\t', f);
            fputs(name_buf, f);               fputc('\t', f);
            fputs(centroid_buf, f);           fputc('\n', f);
        }

        /* register in this level's map */
        if (out_map) {
            hmput(key_to_id, e->key, id);
        }
        if (sp) sql_progress_tick(sp, table_name, 1);
    }

    fputs("\\.\n", f);
    if (out_map) {
        *out_map = key_to_id;    
    }
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

/* =========================================================================
 *  zstd-on-the-fly writer via fopencookie
 * ========================================================================= */

typedef struct {
    ZSTD_CCtx *cctx;
    FILE      *dst;
    void      *outBuf;
    size_t     outSize;
    int        closed;
} ZstdCookie;

static ssize_t zstd_write(void *cookie, const char *buf, size_t size)
{
    ZstdCookie *zc = cookie;
    if (zc->closed) return -1;
    ZSTD_inBuffer input = {buf, size, 0};
    while (input.pos < input.size) {
        ZSTD_outBuffer output = {zc->outBuf, zc->outSize, 0};
        size_t ret = ZSTD_compressStream2(zc->cctx, &output, &input, ZSTD_e_continue);
        if (ZSTD_isError(ret)) return -1;
        fwrite(zc->outBuf, 1, output.pos, zc->dst);
    }
    return (ssize_t)size;
}

static int zstd_close(void *cookie)
{
    ZstdCookie *zc = cookie;
    if (zc->closed) return 0;
    zc->closed = 1;

    ZSTD_inBuffer input = {NULL, 0, 0};
    for (;;) {
        ZSTD_outBuffer output = {zc->outBuf, zc->outSize, 0};
        size_t ret = ZSTD_compressStream2(zc->cctx, &output, &input, ZSTD_e_end);
        if (ZSTD_isError(ret)) return -1;
        fwrite(zc->outBuf, 1, output.pos, zc->dst);
        if (ret == 0) break;
    }

    ZSTD_freeCCtx(zc->cctx);
    free(zc->outBuf);
    fclose(zc->dst);
    free(zc);
    return 0;
}

void storage_stats_write_sql(const StorageStats *stats, const char *filename)
{
    /* --- open destination file --- */
    FILE *dst = fopen(filename, "wb");
    if (!dst) fatal(ERROR_IO, "Cannot open output file '%s' for writing.", filename);

    /* --- build zstd cookie --- */
    ZstdCookie *zc = malloc(sizeof(*zc));
    if (!zc) fatal(ERROR_MEMORY, "zstd cookie allocation failed.");
    zc->cctx = ZSTD_createCCtx();
    if (!zc->cctx) fatal(ERROR_MEMORY, "ZSTD_createCCtx failed.");
    ZSTD_CCtx_setParameter(zc->cctx, ZSTD_c_compressionLevel, 3);
    ZSTD_CCtx_setParameter(zc->cctx, ZSTD_c_checksumFlag, 1);
    zc->dst      = dst;
    zc->outSize  = ZSTD_CStreamOutSize();
    zc->outBuf   = malloc(zc->outSize);
    zc->closed   = 0;
    if (!zc->outBuf) fatal(ERROR_MEMORY, "zstd output buffer allocation failed.");

    cookie_io_functions_t io = {NULL, zstd_write, NULL, zstd_close};
    FILE *f = fopencookie(zc, "w", io);
    if (!f) fatal(ERROR_MEMORY, "fopencookie failed.");

    /* --- DDL --- */
    fputs(
        "BEGIN;\n"
        "\n"
        "CREATE EXTENSION IF NOT EXISTS postgis;\n"
        "CREATE EXTENSION IF NOT EXISTS pg_trgm;\n"
        "\n"
        "CREATE TABLE countries (\n"
        "    id           BIGINT PRIMARY KEY,\n"
        "    country_code CHAR(2) NOT NULL UNIQUE,\n"
        "    name         TEXT NOT NULL,\n"
        "    centroid     GEOMETRY(Point, 4326)\n"
        ");\n"
        "\n"
        "CREATE TABLE states (\n"
        "    id         BIGINT PRIMARY KEY,\n"
        "    country_id BIGINT NOT NULL REFERENCES countries(id),\n"
        "    name       TEXT NOT NULL,\n"
        "    centroid   GEOMETRY(Point, 4326),\n"
        "    UNIQUE(country_id, name)\n"
        ");\n"
        "\n"
        "CREATE TABLE counties (\n"
        "    id       BIGINT PRIMARY KEY,\n"
        "    state_id BIGINT NOT NULL REFERENCES states(id),\n"
        "    name     TEXT NOT NULL,\n"
        "    centroid GEOMETRY(Point, 4326),\n"
        "    UNIQUE(state_id, name)\n"
        ");\n"
        "\n"
        "CREATE TABLE cities (\n"
        "    id        BIGINT PRIMARY KEY,\n"
        "    county_id BIGINT NOT NULL REFERENCES counties(id),\n"
        "    name      TEXT NOT NULL,\n"
        "    centroid  GEOMETRY(Point, 4326),\n"
        "    UNIQUE(county_id, name)\n"
        ");\n"
        "\n"
        "CREATE TABLE postcodes (\n"
        "    id       BIGINT PRIMARY KEY,\n"
        "    city_id  BIGINT NOT NULL REFERENCES cities(id),\n"
        "    name     TEXT NOT NULL,\n"
        "    centroid GEOMETRY(Point, 4326),\n"
        "    UNIQUE(city_id, name)\n"
        ");\n"
        "\n"
        "CREATE TABLE streets (\n"
        "    id          BIGINT PRIMARY KEY,\n"
        "    postcode_id BIGINT NOT NULL REFERENCES postcodes(id),\n"
        "    name        TEXT NOT NULL,\n"
        "    centroid    GEOMETRY(Point, 4326),\n"
        "    UNIQUE(postcode_id, name)\n"
        ");\n"
        "\n"
        "CREATE TABLE houses (\n"
        "    id          BIGINT PRIMARY KEY,\n"
        "    street_id   BIGINT NOT NULL REFERENCES streets(id),\n"
        "    housenumber TEXT NOT NULL,\n"
        "    centroid    GEOMETRY(Point, 4326),\n"
        "    UNIQUE(street_id, housenumber)\n"
        ");\n"
        , f);

    /* --- COPY sections --- */
    KeyToId *country_map = NULL, *state_map = NULL, *county_map = NULL;
    KeyToId *city_map    = NULL, *postcode_map = NULL, *street_map = NULL;
    int next_id = 1;

    uint64_t total_rows = (uint64_t)hmlen(stats->countries.entries)
                        + (uint64_t)hmlen(stats->states.entries)
                        + (uint64_t)hmlen(stats->counties.entries)
                        + (uint64_t)hmlen(stats->cities.entries)
                        + (uint64_t)hmlen(stats->postcodes.entries)
                        + (uint64_t)hmlen(stats->streets.entries)
                        + (uint64_t)hmlen(stats->houses.entries);

    SqlProgress sp;
    sql_progress_init(&sp, total_rows);

    write_copy_section(f, &stats->countries, "countries",
        "(id, country_code, name, centroid)",
        NULL, &next_id, &country_map, &sp);

    write_copy_section(f, &stats->states, "states",
        "(id, country_id, name, centroid)",
        &country_map, &next_id, &state_map, &sp);

    hmfree(country_map);

    write_copy_section(f, &stats->counties, "counties",
        "(id, state_id, name, centroid)",
        &state_map, &next_id, &county_map, &sp);
    
    hmfree(state_map);

    write_copy_section(f, &stats->cities, "cities",
        "(id, county_id, name, centroid)",
        &county_map, &next_id, &city_map, &sp);

    hmfree(county_map);

    write_copy_section(f, &stats->postcodes, "postcodes",
        "(id, city_id, name, centroid)",
        &city_map, &next_id, &postcode_map, &sp);

    hmfree(city_map);

    write_copy_section(f, &stats->streets, "streets",
        "(id, postcode_id, name, centroid)",
        &postcode_map, &next_id, &street_map, &sp);

    hmfree(postcode_map);

    write_copy_section(f, &stats->houses, "houses",
        "(id, street_id, housenumber, centroid)",
        &street_map, &next_id, NULL, &sp);

    hmfree(street_map); 

    sql_progress_finish(&sp);

    /* --- indexes --- */
    fputs(
        "\n"
        "CREATE INDEX ON postcodes USING GIN (name gin_trgm_ops);\n"
        "CREATE INDEX ON streets   USING GIN (name gin_trgm_ops);\n"
        "CREATE INDEX ON houses    USING GIN (housenumber gin_trgm_ops);\n"
        "CREATE INDEX ON houses    USING GIST (centroid);\n"
        "\n"
        "COMMIT;\n"
        "VACUUM ANALYZE;\n"
        , f);
    
    fclose(f);  /* triggers zstd_close → flush, free, fclose(dst) */

    info("Compressed PostgreSQL script written to '%s'.", filename);
}


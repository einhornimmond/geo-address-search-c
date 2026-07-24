#include "stb_ds.h"

#include "storage_stats_internal.h"
#include "sql_export.h"
#include "error.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gradido_blockchain_core/utils/mono_timer.h"

/* =========================================================================
 *  Helpers
 * ========================================================================= */

/** stb_ds string→int64 map — wrapper required for shgeti/shput on scalar values. */
typedef struct { int64_t key; int64_t value; } KeyToId;

/** Format a centroid as WKT POINT, or \N if no point. */
static void format_centroid(char *buf, size_t buf_size, const Entity *e)
{
    if (!e->has_point) {
        snprintf(buf, buf_size, "\\N");
    } else {
        double lon = (double)e->centroid_lon_e7 / 1.0e7;
        double lat = (double)e->centroid_lat_e7 / 1.0e7;
        snprintf(buf, buf_size, "POINT(%.7f %.7f)", lon, lat);
    }
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

    printf("\r  SQL-Export: %6.2f%%  %s (%" PRIu64 " Zeilen, %" PRIu64 " Zeilen/s)  ",
           pct, table, sp->written, rate);
    fflush(stdout);
}

static void sql_progress_finish(SqlProgress *sp)
{
    double secs = grdu_mono_timer_seconds(sp->start);
    uint64_t rate = secs > 0.0 ? (uint64_t)((double)sp->written / secs) : 0;
    printf("\r  SQL-Export: 100.00%%  %" PRIu64 " Zeilen in %.1f s (%" PRIu64 " Zeilen/s)    \n",
           sp->written, secs, rate);
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
    sh_new_strdup(key_to_id);

    fprintf(f, "\nCOPY %s %s FROM stdin;\n", table_name, column_list);

    for (ptrdiff_t i = 0; i < shlen(set->entries); ++i) {
        const Entity *e = &set->entries[i];
        int64_t id = (*next_id)++;

        /* resolve FK */
        int64_t parent_id = 0;
        if (parent_map && *parent_map && e->parent_key) {
            ptrdiff_t pi = shgeti(*parent_map, e->parent_key);
            if (pi >= 0) parent_id = (*parent_map)[pi].value;
        }

        /* build output columns */
        char name_buf[512], centroid_buf[128];
        copy_escape(name_buf, e->name ? e->name : "");
        format_centroid(centroid_buf, sizeof(centroid_buf), e);

        if (table_name[0] == 'c' && table_name[1] == 'o') {
            /* countries: id, code, name, centroid */
            fprintf(f, "%" PRId64 "\t%s\t%s\t%s\n",
                id, e->code ? e->code : "", name_buf, centroid_buf);
        } else if (table_name[0] == 'h') {
            /* houses: id, street_id, housenumber, postcode, centroid */
            fprintf(f, "%" PRId64 "\t%" PRId64 "\t%s\t%s\t%s\n",
                id, parent_id, name_buf,
                e->postcode ? e->postcode : "\\N", centroid_buf);
        } else {
            /* states, counties, cities, streets: id, parent_id, name, centroid */
            fprintf(f, "%" PRId64 "\t%" PRId64 "\t%s\t%s\n",
                id, parent_id, name_buf, centroid_buf);
        }

        /* register in this level's map */
        shput(key_to_id, e->key, id);
        if (sp) sql_progress_tick(sp, table_name, 1);
    }

    fprintf(f, "\\.\n");
    *out_map = key_to_id;    
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

void storage_stats_write_sql(const StorageStats *stats, const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fatal(ERROR_IO, "Cannot open output file '%s' for writing.", filename);
    }

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
        "CREATE TABLE streets (\n"
        "    id       BIGINT PRIMARY KEY,\n"
        "    city_id  BIGINT NOT NULL REFERENCES cities(id),\n"
        "    name     TEXT NOT NULL,\n"
        "    centroid GEOMETRY(Point, 4326),\n"
        "    UNIQUE(city_id, name)\n"
        ");\n"
        "\n"
        "CREATE TABLE houses (\n"
        "    id          BIGINT PRIMARY KEY,\n"
        "    street_id   BIGINT NOT NULL REFERENCES streets(id),\n"
        "    housenumber TEXT NOT NULL,\n"
        "    postcode    TEXT,\n"
        "    centroid    GEOMETRY(Point, 4326),\n"
        "    UNIQUE(street_id, housenumber)\n"
        ");\n"
        , f);

    /* --- COPY sections --- */
    KeyToId *country_map = NULL, *state_map = NULL, *county_map = NULL;
    KeyToId *city_map    = NULL, *street_map = NULL, *house_map   = NULL;
    int next_id = 1;

    uint64_t total_rows = (uint64_t)shlen(stats->countries.entries)
                        + (uint64_t)shlen(stats->states.entries)
                        + (uint64_t)shlen(stats->counties.entries)
                        + (uint64_t)shlen(stats->cities.entries)
                        + (uint64_t)shlen(stats->streets.entries)
                        + (uint64_t)shlen(stats->houses.entries);

    SqlProgress sp;
    sql_progress_init(&sp, total_rows);

    write_copy_section(f, &stats->countries, "countries",
        "(id, country_code, name, centroid)",
        NULL, &next_id, &country_map, &sp);

    write_copy_section(f, &stats->states, "states",
        "(id, country_id, name, centroid)",
        &country_map, &next_id, &state_map, &sp);

    write_copy_section(f, &stats->counties, "counties",
        "(id, state_id, name, centroid)",
        &state_map, &next_id, &county_map, &sp);

    write_copy_section(f, &stats->cities, "cities",
        "(id, county_id, name, centroid)",
        &county_map, &next_id, &city_map, &sp);

    write_copy_section(f, &stats->streets, "streets",
        "(id, city_id, name, centroid)",
        &city_map, &next_id, &street_map, &sp);

    write_copy_section(f, &stats->houses, "houses",
        "(id, street_id, housenumber, postcode, centroid)",
        &street_map, &next_id, &house_map, &sp);

    sql_progress_finish(&sp);

    /* --- indexes --- */
    fputs(
        "\n"
        "CREATE INDEX ON cities   USING GIN (name gin_trgm_ops);\n"
        "CREATE INDEX ON streets  USING GIN (name gin_trgm_ops);\n"
        "CREATE INDEX ON houses   USING GIN (housenumber gin_trgm_ops);\n"
        "CREATE INDEX ON houses   USING GIST (centroid);\n"
        "CREATE INDEX ON houses   (postcode);\n"
        "\n"
        "COMMIT;\n"
        "VACUUM ANALYZE;\n"
        , f);

    /* cleanup temporary maps */
    shfree(country_map); shfree(state_map); shfree(county_map);
    shfree(city_map);    shfree(street_map); shfree(house_map);

    fclose(f);

    info("PostgreSQL script written to '%s'.", filename);
}


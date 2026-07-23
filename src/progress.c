#include "progress.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static uint64_t g_totalBytes;
static struct timespec g_start;
static struct timespec g_last;

static double nowSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}


void formatHumanReadableSize(uint64_t bytes, char *buf, size_t len)
{
    static const char *units[] = {
        "B",
        "KB",
        "MB",
        "GB",
        "TB"
    };

    double value = (double)bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }

    if (unit == 0)
        snprintf(buf, len, "%" PRIu64 " %s", bytes, units[unit]);
    else
        snprintf(buf, len, "%.2f %s", value, units[unit]);
}

void progress_init(uint64_t totalBytes)
{
    g_totalBytes = totalBytes;
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    g_last = g_start;
}

void progress_update(uint64_t currentBytes)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double sinceLast =
        (now.tv_sec - g_last.tv_sec) +
        (now.tv_nsec - g_last.tv_nsec) / 1e9;

    if (sinceLast < 0.1)
        return;

    g_last = now;

    double elapsed =
        (now.tv_sec - g_start.tv_sec) +
        (now.tv_nsec - g_start.tv_nsec) / 1e9;

    char current[32];
    char total[32];
    char speed[32];

    formatHumanReadableSize(currentBytes, current, sizeof(current));
    formatHumanReadableSize(g_totalBytes, total, sizeof(total));

    uint64_t bytesPerSecond = elapsed > 0.0
        ? (uint64_t)(currentBytes / elapsed)
        : 0;

    formatHumanReadableSize(bytesPerSecond, speed, sizeof(speed));

    printf(
        "\rProgress: %6.2f%%   %s / %s   (%s/s)",
        100.0 * currentBytes / g_totalBytes,
        current,
        total,
        speed
    );

    fflush(stdout);
}

void progress_finish(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed =
        (now.tv_sec - g_start.tv_sec) +
        (now.tv_nsec - g_start.tv_nsec) / 1e9;

    putchar('\n');

    if (elapsed < 60.0) {
        printf("Total time: %.3f s\n", elapsed);
    } else {
        unsigned hours   = (unsigned)(elapsed / 3600.0);
        unsigned minutes = ((unsigned)elapsed % 3600) / 60;
        unsigned seconds = (unsigned)elapsed % 60;

        printf("Total time: %02u:%02u:%02u\n",
               hours,
               minutes,
               seconds);
    }
}

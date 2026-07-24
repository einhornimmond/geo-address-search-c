#include "progress.h"

#include "gradido_blockchain_core/utils/mono_timer.h"

#include <inttypes.h>
#include <stdio.h>

#include "format.h"

static uint64_t g_totalBytes;
static grdu_mono_timer g_start;
static grdu_mono_timer g_since_last;

void progress_init(uint64_t totalBytes)
{
    g_totalBytes = totalBytes;
    grdu_mono_timer_reset(&g_start);
    g_since_last = g_start;
}

void progress_update(uint64_t currentBytes)
{
    double elapsed_since_last_call = grdu_mono_timer_millis(g_since_last);
    if (elapsed_since_last_call < 200.0) {
        return;
    }
    grdu_mono_timer_reset(&g_since_last);
    
    char current[32];
    char total[32];
    char speed[32];

    format_byte_units(current, sizeof(current), currentBytes, 2);
    format_byte_units(total, sizeof(total), g_totalBytes, 2);

    double elapsed = grdu_mono_timer_seconds(g_start);
    uint64_t bytesPerSecond = elapsed > 0.0
        ? (uint64_t)(currentBytes / elapsed)
        : 0;

    format_byte_units(speed, sizeof(speed), bytesPerSecond, 2);

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
    char elapsed_str[32];
    grdu_mono_timer_string(elapsed_str, sizeof(elapsed_str), g_start);

    printf("Total time: %s\n", elapsed_str);
}

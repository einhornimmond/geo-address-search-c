#pragma once

#include <stdint.h>
#include <stddef.h>

void progress_init(uint64_t totalBytes);
void progress_update(uint64_t currentBytes);
void progress_finish(void);
/**
 * Format a size in bytes to a human readable string.
 *
 * @param bytes The size in bytes.
 * @param buf The buffer to write the formatted string into.
 * @param len The length of the buffer.
 */
void formatHumanReadableSize(uint64_t bytes, char *buf, size_t len);  

#pragma once

#include <stdint.h>
#include <stddef.h>

void progress_init(uint64_t totalBytes);
void progress_update(uint64_t currentBytes);
void progress_finish(void);


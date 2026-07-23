// error.h

#pragma once

#include <stdarg.h>

typedef enum ErrorArt {
    ERROR_USAGE,
    ERROR_IO,
    ERROR_JSON,
    ERROR_ZSTD,
    ERROR_ASSERT,
    ERROR_MEMORY,
} ErrorArt;

_Noreturn void fatal(ErrorArt art, const char* fmt, ...);

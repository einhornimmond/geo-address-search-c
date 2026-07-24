#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct LineBuffer {
    char* buffer;
    size_t capacity;
    size_t position;
} LineBuffer;

LineBuffer* line_buffer_create(size_t capacity);
void line_buffer_destroy(LineBuffer* lb);

void line_buffer_append(LineBuffer* lb, const char* data, size_t len);
void line_buffer_reset(LineBuffer* lb);
void line_buffer_process(LineBuffer* lb, void (*process_line)(const char* line, size_t len));
void line_buffer_flush(LineBuffer* lb, void (*process_line)(const char* line, size_t len));

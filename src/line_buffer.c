#include "line_buffer.h"
#include "error.h"
#include "progress.h"

#include <stdlib.h>
#include <string.h>

LineBuffer* line_buffer_create(size_t capacity)
{
    LineBuffer* lb = malloc(sizeof(LineBuffer));
    if (!lb) {
        return NULL;
    }

    lb->buffer = malloc(capacity);
    if (!lb->buffer) {
        free(lb);
        return NULL;
    }

    lb->capacity = capacity;
    lb->position = 0;
    lb->dropped_bytes = 0;

    return lb;
}

void line_buffer_destroy(LineBuffer* lb)
{
    if (lb) {
        free(lb->buffer);
        free(lb);
    }
}

void line_buffer_append(LineBuffer* lb, const char* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (lb->position >= lb->capacity - 1) {
            lb->dropped_bytes++;
        } else {
            lb->buffer[lb->position++] = data[i];
        }
        
        // If we dropped bytes and hit a newline, the line is corrupted
        if (data[i] == '\n' && lb->dropped_bytes > 0) {
            char sizeBuf[32];
            char sizeBufCapacity[32];
            formatHumanReadableSize(lb->capacity, sizeBufCapacity, 32);
            formatHumanReadableSize(lb->dropped_bytes + lb->capacity, sizeBuf, sizeof(sizeBuf));
            fatal(ERROR_JSON, "Line buffer (%s) used with %s", sizeBufCapacity, sizeBuf);
        }
    }
}

void line_buffer_process(LineBuffer* lb, void (*process_line)(const char* line, size_t len))
{
    size_t line_start = 0;

    for (size_t i = 0; i < lb->position; i++) {
        if (lb->buffer[i] == '\n') {
            lb->buffer[i] = '\0';
            
            if (i > line_start) {
                process_line(lb->buffer + line_start, i - line_start);
            }
            
            line_start = i + 1;
        }
    }

    // Move remaining data to beginning of buffer
    if (line_start > 0 && line_start < lb->position) {
        memmove(lb->buffer, lb->buffer + line_start, lb->position - line_start);
        lb->position -= line_start;
    } else if (line_start >= lb->position) {
        lb->position = 0;
    }
}

void line_buffer_flush(LineBuffer* lb, void (*process_line)(const char* line, size_t len))
{
    if (lb->position > 0) {
        lb->buffer[lb->position] = '\0';
        process_line(lb->buffer, lb->position);
        lb->position = 0;
    }
}

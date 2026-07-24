#include "line_buffer.h"
#include "error.h"
#include "progress.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
    // Ensure enough space (including space for trailing '\0' during flush)
    while (lb->position + len >= lb->capacity) {
        size_t old_capacity = lb->capacity;
        size_t new_capacity = lb->capacity * 3 / 2;
        while (new_capacity <= lb->position + len) {
            new_capacity = new_capacity * 3 / 2;
        }
        char* new_buffer = realloc(lb->buffer, new_capacity);
        if (!new_buffer) {
            fatal(ERROR_MEMORY, "Failed to reallocate line buffer to %zu bytes", new_capacity);
        }
        lb->buffer = new_buffer;
        lb->capacity = new_capacity;

        char oldBuf[32], newBuf[32];
        formatHumanReadableSize(old_capacity, oldBuf, sizeof(oldBuf));
        formatHumanReadableSize(new_capacity, newBuf, sizeof(newBuf));
        info("Line buffer reallocated from %s to %s", oldBuf, newBuf);
    }

    memcpy(lb->buffer + lb->position, data, len);
    lb->position += len;
}

void line_buffer_reset(LineBuffer* lb)
{
    lb->position = 0;
}

void line_buffer_process(LineBuffer* lb, void (*process_line)(const char* line, size_t len))
{
    char* begin = lb->buffer;
    char* end = lb->buffer + lb->position;
    char* line = begin;

    while (line < end) {

        char* newline = memchr(line, '\n', (size_t)(end - line));

        if (!newline)
            break;

        size_t length = (size_t)(newline - line);

        if (length > 0 && newline[-1] == '\r')
            length--;

        process_line(line, length);

        line = newline + 1;
    }

    size_t remaining = (size_t)(end - line);

    if (remaining) {
        memmove(lb->buffer, line, remaining);
    }

    lb->position = remaining;
}

void line_buffer_flush(LineBuffer* lb, void (*process_line)(const char* line, size_t len))
{
    if (lb->position > 0) {
        lb->buffer[lb->position] = '\0';
        process_line(lb->buffer, lb->position);
        lb->position = 0;
    }
}

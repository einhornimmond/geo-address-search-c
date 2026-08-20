#include "foundation/line_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundation/error.h"
#include "foundation/format.h"
#include "foundation/progress.h"

/** @cond INTERNAL */

LineBuffer *line_buffer_create(size_t capacity) {
  LineBuffer *lb = malloc(sizeof(LineBuffer));
  if (!lb) { return NULL; }

  lb->buffer = malloc(capacity);
  if (!lb->buffer) {
    free(lb);
    return NULL;
  }

  lb->capacity = capacity;
  lb->position = 0;

  return lb;
}

void line_buffer_destroy(LineBuffer *lb) {
  if (lb) {
    free(lb->buffer);
    free(lb);
  }
}

void line_buffer_append(LineBuffer *lb, const char *data, size_t len) {
  /* `>=` rather than `>`: one byte above the contents stays free for the
     terminator line_buffer_flush() writes there. */
  while (lb->position + len >= lb->capacity) {
    size_t old_capacity = lb->capacity;
    size_t new_capacity = lb->capacity * 3 / 2;
    while (new_capacity <= lb->position + len) { new_capacity = new_capacity * 3 / 2; }
    /* Growing by half again rather than doubling: the blocks arriving here are
       of a similar size, so the buffer settles after a few steps and does not
       carry twice what it needs. */
    char *new_buffer = realloc(lb->buffer, new_capacity);
    if (!new_buffer) {
      fatal(ERROR_MEMORY, "Failed to reallocate line buffer to %zu bytes", new_capacity);
    }
    lb->buffer = new_buffer;
    lb->capacity = new_capacity;

    /* Offered on purpose: a buffer that keeps growing means the size it was
       opened with was guessed wrong. Whether the note is printed is info()'s
       decision, and at present it prints nothing. */
    char oldBuf[32], newBuf[32];
    format_byte_units(oldBuf, sizeof(oldBuf), old_capacity, 2);
    format_byte_units(newBuf, sizeof(newBuf), new_capacity, 2);
    info("Line buffer reallocated from %s to %s", oldBuf, newBuf);
  }

  memcpy(lb->buffer + lb->position, data, len);
  lb->position += len;
}

void line_buffer_reset(LineBuffer *lb) {
  lb->position = 0;
}

void line_buffer_process(LineBuffer *lb, void (*process_line)(const char *line, size_t len)) {
  char *begin = lb->buffer;
  char *end = lb->buffer + lb->position;
  char *line = begin;

  while (line < end) {
    char *newline = memchr(line, '\n', (size_t)(end - line));
    if (!newline) break;

    size_t length = (size_t)(newline - line);
    /* A CRLF file loses its carriage return here, so the callback sees the same
       line either way. */
    if (length > 0 && newline[-1] == '\r') length--;

    process_line(line, length);
    line = newline + 1;
  }

  /* Whatever stands after the last newline is not a line yet; it moves to the
     front and waits for the block that finishes it. */
  size_t remaining = (size_t)(end - line);
  if (remaining) { memmove(lb->buffer, line, remaining); }

  lb->position = remaining;
}

void line_buffer_flush(LineBuffer *lb, void (*process_line)(const char *line, size_t len)) {
  if (lb->position > 0) {
    /* The byte append() kept free — a stream can end without a newline, and the
       fragment still has to read as a string. */
    lb->buffer[lb->position] = '\0';
    process_line(lb->buffer, lb->position);
    lb->position = 0;
  }
}

/** @endcond */

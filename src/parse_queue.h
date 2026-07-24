#pragma once

#include <stddef.h>

#include "line_buffer.h"

enum { PARSE_QUEUE_CAPACITY = 8 };

typedef struct ParseBatch {
    LineBuffer* buffer;
    size_t len;
} ParseBatch;

typedef struct ParseQueue ParseQueue;

ParseQueue* parse_queue_create(void);
void parse_queue_destroy(ParseQueue* queue);
void parse_queue_push(ParseQueue* queue, ParseBatch batch);
int parse_queue_pop(ParseQueue* queue, ParseBatch* batch);
void parse_queue_close(ParseQueue* queue);

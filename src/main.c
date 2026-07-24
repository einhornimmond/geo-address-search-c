#include "error.h"
#include "progress.h"
#include "line_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>
#include <yyjson.h>

static void process_json_line(const char* line, size_t len)
{
    return;
    // Roughly estimate size: yyjson needs about len to 2*len for value storage
    // depending on nesting depth. Better to dimension generously.
    size_t buf_size = yyjson_read_max_memory_usage(len, 0); // yyjson helper, if available
    static __thread char* alc_buf = NULL;
    static __thread size_t alc_buf_size = 0;

    if (alc_buf_size < buf_size) {
        free(alc_buf);
        alc_buf = malloc(buf_size);
        if (!alc_buf) {
          char buf_size_buf[32];
          formatHumanReadableSize(buf_size, buf_size_buf, sizeof(buf_size_buf));
          fatal(ERROR_MEMORY, "Failed to allocate %s for JSON parsing.", buf_size_buf);
        } else {
          char oldBuf[32], newBuf[32];
          formatHumanReadableSize(alc_buf_size, oldBuf, sizeof(oldBuf));
          formatHumanReadableSize(buf_size, newBuf, sizeof(newBuf));
          info("Json buffer reallocated from %s to %s", oldBuf, newBuf);
        }
        alc_buf_size = buf_size;
    }
    return;

    yyjson_alc alc;
    yyjson_alc_pool_init(&alc, alc_buf, alc_buf_size);

    yyjson_doc* doc = yyjson_read_opts((char*)line, len, 0, &alc, NULL);
    if (doc) {
        yyjson_val* root = yyjson_doc_get_root(doc);
        // TODO: Process JSON object
        yyjson_doc_free(doc); // returns NOTHING to malloc internally, since alc_buf remains static
    } else {
        fatal(ERROR_JSON, "Failed to parse JSON line: %s", line);
    }
}

int main(int argc, char* argv[]) 
{
    if (argc != 2) {
        fatal(ERROR_USAGE,"Usage: %s <photon_dump.jsonl.zst>", argv[0]);
    }

    FILE* fp = fopen(argv[1], "rb");
    if (!fp) {
        fatal(ERROR_IO, "Cannot open '%s'.", argv[1]);
    }

    ZSTD_DStream* dstream = ZSTD_createDStream();
    if (!dstream) {
        fatal(ERROR_MEMORY, "Failed to create ZSTD_DStream.");
    }

    size_t ret = ZSTD_initDStream(dstream);
    if (ZSTD_isError(ret)) {
        fatal(ERROR_ZSTD, "%s", ZSTD_getErrorName(ret));
    }

    const size_t inputSize = ZSTD_DStreamInSize();
    const size_t outputSize = ZSTD_DStreamOutSize();

    char* inputBuffer = malloc(inputSize);
    void* outputBuffer = malloc(outputSize);
    LineBuffer* lineBuffer = line_buffer_create(outputSize * 2);
    
    char inputSizeBuf[32], outputSizeBuf[32];
    formatHumanReadableSize(inputSize, inputSizeBuf, 32);
    formatHumanReadableSize(outputSize, outputSizeBuf, 32);
    printf("inputSize: %s, outputSize: %s\n", inputSizeBuf, outputSizeBuf);

    if (!inputBuffer || !outputBuffer || !lineBuffer) {
        fatal(ERROR_MEMORY, "Failed to allocate streaming buffers.");
    }

    fseek(fp, 0, SEEK_END);
    uint64_t totalBytes = ftell(fp);
    rewind(fp);

    progress_init(totalBytes);

    ZSTD_inBuffer input = {
        .src = inputBuffer,
        .size = 0,
        .pos = 0,
    };

    ZSTD_outBuffer output = {
       .dst = outputBuffer,
       .size = outputSize,
       .pos = 0,
    };

    while (1) 
    {
      size_t read = fread(inputBuffer, 1, inputSize, fp);
      progress_update(ftell(fp));
      if (read == 0) { break; }
      input.size = read;
      input.pos = 0;
      
      while (input.pos < input.size || output.pos == output.size) 
      {
          ret = ZSTD_decompressStream(
              dstream,
              &output,
              &input
          );

          if (ZSTD_isError(ret)) {
              fatal(ERROR_ZSTD, "%s", ZSTD_getErrorName(ret));
          }
          
          // Process decompressed data
          char* data = (char*)output.dst;
          size_t dataSize = output.pos;
          
          
          line_buffer_append(lineBuffer, data, dataSize + YYJSON_PADDING_SIZE);
          line_buffer_process(lineBuffer, process_json_line);
          output.pos = 0;

          // if fully flushed
          if (0 == ret) { break;}

      }      
    }
    
    // Flush remaining data in line buffer
    line_buffer_flush(lineBuffer, process_json_line);
    
    progress_finish();

    line_buffer_destroy(lineBuffer);
    free(outputBuffer);
    free(inputBuffer);

    ZSTD_freeDStream(dstream);

    fclose(fp);

    return 0;
}

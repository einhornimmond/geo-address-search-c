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
    yyjson_doc* doc = yyjson_read(line, len, 0);
    if (doc) {
        yyjson_val* root = yyjson_doc_get_root(doc);
        // TODO: Process JSON object
        yyjson_doc_free(doc);
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

    while (1) 
    {
      size_t read = fread(inputBuffer, 1, inputSize, fp);
      progress_update(ftell(fp));
      if (read == 0) { break; }

      ZSTD_inBuffer input = {
          .src = inputBuffer,
          .size = read,
          .pos = 0,
      };
      
      while (input.pos < input.size) 
      {
          ZSTD_outBuffer output = {
              .dst = outputBuffer,
              .size = outputSize,
              .pos = 0,
          };

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
          
          line_buffer_append(lineBuffer, data, dataSize);
          line_buffer_process(lineBuffer, process_json_line);

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

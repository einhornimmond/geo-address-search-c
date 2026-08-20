#include "foundation/error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/** @cond INTERNAL */

/** A rule above and below the title, so a failure is findable in a wall of output. */
static void printHeader(const char *title) {
  fprintf(
      stderr,
      "\n"
      "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
      " %s\n"
      "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n",
      title
  );
}

/* Luna is the character who wrote most of this code, and the banners are hers.
   They exist because the moment a build of 24 GB gives up is the moment the
   reader is least inclined to read: a face and one plain sentence carry further
   than a stack trace. Each banner says what happened; the line beneath it, from
   fatal()'s format string, says which file, which offset, which size. */

static void printUsage(void) {
  printHeader("Luna is confused...");

  fputs(
      "      /\\_/\\\n"
      "     ( >_< )\n"
      "     /  ^  \\\n"
      "\n"
      " I don't know what you want me to parse.\n"
      " Please tell me which Photon dump to open.\n"
      "\n",
      stderr
  );
}

static void printIo(void) {
  printHeader("Luna searched everywhere...");

  fputs(
      "       /\\   /\\\n"
      "       { ;_; }\n"
      "      /|     |\\\n"
      "     /_|_____|_\\\n"
      "\n"
      " I couldn't find your file.\n"
      " Maybe it's hiding... or maybe the path is wrong.\n"
      "\n",
      stderr
  );
}

static void printJson(void) {
  printHeader("Luna broke your JSON...");

  fputs(
      "      /\\_/\\\\\n"
      "    =( x.x )=\n"
      "     /  |  \\\n"
      "\n"
      " ...okay, maybe you broke it.\n"
      "\n",
      stderr
  );
}

static void printZstd(void) {
  printHeader("Luna fell asleep inside the .zst stream...");

  fputs(
      "      /\\___/\\\\\n"
      "    =( -.- )= zZz\n"
      "     /  |  \\\n"
      "\n"
      " The compressed stream doesn't look right.\n"
      "\n",
      stderr
  );
}

static void printMemory(void) {
  printHeader("Luna ate all your RAM...");

  fputs(
      "      /\\_/\\\\\n"
      "    =( @o@ )=\n"
      "     /[RAM]\\\n"
      "\n"
      " She apologizes.\n"
      " (No she doesn't.)\n"
      "\n",
      stderr
  );
}

static void printAssert(void) {
  printHeader("Luna is judging your code...");

  fputs(
      "     /\\___/\\\n"
      "    =( ಠ_ಠ )=\n"
      "      /   \\\n"
      "\n"
      " Something impossible just happened.\n"
      "\n",
      stderr
  );
}

static void printHashCollision(void) {
  printHeader("Luna witnessed a hash collision...");

  fputs(
      "      /\\_/\\     /\\_/\\\n"
      "     ( o.o )   ( O.O )\n"
      "      \\   /     \\   /\n"
      "       \\ /_______\\ /\n"
      "        X         X\n"
      "\n"
      " Two different places folded into one key.\n"
      " The hash space has betrayed its promise.\n"
      " Call a mathematician. Or check your data.\n"
      "\n",
      stderr
  );
}

static void printInfo(void) {
  printHeader("Luna has an update...");

  fputs(
      "      /\\_/\\\n"
      "     ( ^_^ )\n"
      "     /  !  \\\n"
      "\n"
      " Here's something you should know.\n"
      "\n",
      stdout
  );
}

/* No default branch on purpose: adding an art without a banner should be a
   compiler warning, not a silent blank. */
_Noreturn void fatal(ErrorArt art, const char *fmt, ...) {
  switch (art) {
  case ERROR_USAGE:
    printUsage();
    break;
  case ERROR_IO:
    printIo();
    break;
  case ERROR_JSON:
    printJson();
    break;
  case ERROR_ZSTD:
    printZstd();
    break;
  case ERROR_MEMORY:
    printMemory();
    break;
  case ERROR_ASSERT:
    printAssert();
    break;
  case ERROR_HASH_COLLISION:
    printHashCollision();
    break;
  case ERROR_INFO:
    printInfo();
    break;
  }

  fprintf(stderr, "Error: ");

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fputc('\n', stderr);

  exit(EXIT_FAILURE);
}

/* Silenced, not removed: the calls scattered through the build are worth
   keeping, and the body waits here for the day the notes are wanted again.
   Everything after the return is unreachable by design. */
void info(const char *fmt, ...) {
  return;
  // printInfo();
  fprintf(stdout, "Info: ");

  va_list args;
  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  va_end(args);

  fputc('\n', stdout);
}

/** @endcond */

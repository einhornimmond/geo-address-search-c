/** @file
 *  @brief The two ways the program speaks: a note in passing, and a last word.
 *
 *  `fatal` ends the process, so it is examined from a child that is allowed to
 *  die.  `info` only writes, so it is examined by reading what it wrote.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace {

/** Read back what info() wrote. It goes to stdout — a note is not an error —
 *  and it is buffered, so the stream has to be pushed before it can be read. */
std::string Capture(void (*write)()) {
  testing::internal::CaptureStdout();
  write();
  std::fflush(stdout);
  return testing::internal::GetCapturedStdout();
}

} // namespace

/**
 * @brief info() is silent, and that is a decision — not an accident of this test.
 *
 *  `error.c` opens the function with a bare `return;`, so its whole body —
 *  the "Info: " prefix, the formatting, the newline — is unreachable.  Someone
 *  quietened the channel and left it that way; the one caller in
 *  `line_buffer.c` therefore says nothing when the buffer grows.
 *
 *  The test pins the behaviour as it stands rather than the behaviour the code
 *  reads as having.  Should the `return;` ever go, this fails and the choice
 *  has to be made deliberately.
 */
TEST(ErrorInfo, IsSilentBecauseItsBodyIsUnreachable) {
  std::string written = Capture([] { info("eine Notiz"); });
  EXPECT_TRUE(written.empty()) << "info() gained a voice — was that meant? (error.c: the "
                                  "function opens with `return;`)";
}

TEST(ErrorInfo, TakesItsArgumentsWithoutReadingThem) {
  // a varargs call whose body never runs must still be safe at every call site
  std::string written = Capture([] { info("%s hat %d Zeichen", "Berlin", 6); });
  EXPECT_TRUE(written.empty());
}

TEST(ErrorInfo, ReturnsSoTheCallerCarriesOn) {
  int reached = 0;
  info("erste");
  ++reached;
  info("zweite");
  ++reached;
  EXPECT_EQ(reached, 2);
}

TEST(ErrorInfo, AnEmptyMessageIsNoCrash) {
  Capture([] { info(""); });
  SUCCEED();
}

// ---------------------------------------------------------------------------
//  fatal — the path that does not return
// ---------------------------------------------------------------------------

TEST(ErrorFatalDeathTest, EndsTheProcessWithAFailure) {
  EXPECT_EXIT(fatal(ERROR_IO, "die Datei blieb zu"), ::testing::ExitedWithCode(1), "die Datei blieb zu");
}

TEST(ErrorFatalDeathTest, FormatsBeforeItGoes) {
  EXPECT_EXIT(
      fatal(ERROR_MEMORY, "%zu Bytes waren zu viel", (size_t)4096), ::testing::ExitedWithCode(1),
      "4096 Bytes waren zu viel"
  );
}

TEST(ErrorFatalDeathTest, EveryKindEndsTheSameWay) {
  const ErrorArt kinds[] = {ERROR_USAGE, ERROR_IO,     ERROR_JSON,           ERROR_ZSTD,
                            ERROR_ASSERT, ERROR_MEMORY, ERROR_HASH_COLLISION, ERROR_INFO};
  for (ErrorArt art : kinds) {
    EXPECT_EXIT(fatal(art, "Schluss"), ::testing::ExitedWithCode(1), "Schluss")
        << "kind " << (int)art;
  }
}

/** @file
 *  @brief The line buffer — bytes arrive in blocks, lines leave it whole.
 *
 *  A block boundary falls wherever the reader happened to stop, which is never
 *  where a line ends.  What the tests watch is that a line split across two
 *  appends comes out in one piece, and that what has no newline yet waits.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

/* line_buffer_process takes a bare function pointer, so what it produces has
   to be gathered somewhere both it and the test can reach. */
std::vector<std::string> g_lines;

void Collect(const char *line, size_t len) {
  g_lines.emplace_back(line, len);
}

class LineBufferTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_lines.clear();
    lb = line_buffer_create(1024);
    ASSERT_NE(lb, nullptr);
  }
  void TearDown() override {
    line_buffer_destroy(lb);
  }

  void Append(const std::string &text) {
    line_buffer_append(lb, text.c_str(), text.size());
  }

  LineBuffer *lb = nullptr;
};

} // namespace

TEST_F(LineBufferTest, StartsEmpty) {
  EXPECT_EQ(lb->position, 0u);
  EXPECT_GE(lb->capacity, 1024u);
  EXPECT_NE(lb->buffer, nullptr);
}

TEST_F(LineBufferTest, HandsOutWholeLines) {
  Append("eins\nzwei\ndrei\n");
  line_buffer_process(lb, Collect);
  EXPECT_EQ(g_lines, (std::vector<std::string>{"eins", "zwei", "drei"}));
  EXPECT_EQ(lb->position, 0u) << "nothing was left over";
}

TEST_F(LineBufferTest, KeepsWhatHasNoNewlineYet) {
  Append("eins\nzwei");
  line_buffer_process(lb, Collect);
  EXPECT_EQ(g_lines, (std::vector<std::string>{"eins"}));
  EXPECT_EQ(lb->position, 4u) << "\"zwei\" waits for its ending";
}

TEST_F(LineBufferTest, JoinsALineSplitAcrossTwoAppends) {
  Append("Berliner Str");
  line_buffer_process(lb, Collect);
  EXPECT_TRUE(g_lines.empty());

  Append("asse 17\n");
  line_buffer_process(lb, Collect);
  EXPECT_EQ(g_lines, (std::vector<std::string>{"Berliner Strasse 17"}));
}

TEST_F(LineBufferTest, DropsTheCarriageReturnOfADosLine) {
  Append("eins\r\nzwei\r\n");
  line_buffer_process(lb, Collect);
  EXPECT_EQ(g_lines, (std::vector<std::string>{"eins", "zwei"}));
}

TEST_F(LineBufferTest, AnEmptyLineIsALineOfLengthZero) {
  Append("eins\n\nzwei\n");
  line_buffer_process(lb, Collect);
  ASSERT_EQ(g_lines.size(), 3u);
  EXPECT_EQ(g_lines[1], "");
}

TEST_F(LineBufferTest, ProcessingAnEmptyBufferProducesNothing) {
  line_buffer_process(lb, Collect);
  EXPECT_TRUE(g_lines.empty());
}

TEST_F(LineBufferTest, FlushReleasesTheLastLineWithoutANewline) {
  Append("eins\nzwei");
  line_buffer_process(lb, Collect);
  line_buffer_flush(lb, Collect);
  EXPECT_EQ(g_lines, (std::vector<std::string>{"eins", "zwei"}));
  EXPECT_EQ(lb->position, 0u);
}

TEST_F(LineBufferTest, FlushingAnEmptyBufferProducesNothing) {
  line_buffer_flush(lb, Collect);
  EXPECT_TRUE(g_lines.empty());
}

TEST_F(LineBufferTest, ResetForgetsWhatWasWaiting) {
  Append("halbe Zeile");
  line_buffer_reset(lb);
  EXPECT_EQ(lb->position, 0u);
  line_buffer_flush(lb, Collect);
  EXPECT_TRUE(g_lines.empty());
}

TEST_F(LineBufferTest, GrowsBeyondItsFirstCapacity) {
  LineBuffer *small = line_buffer_create(16);
  ASSERT_NE(small, nullptr);
  std::string long_line(4096, 'x');
  line_buffer_append(small, long_line.c_str(), long_line.size());
  line_buffer_append(small, "\n", 1);
  EXPECT_GE(small->capacity, 4097u);

  g_lines.clear();
  line_buffer_process(small, Collect);
  ASSERT_EQ(g_lines.size(), 1u);
  EXPECT_EQ(g_lines[0].size(), 4096u);
  line_buffer_destroy(small);
}

TEST_F(LineBufferTest, ManyLinesInOneBlockAllArrive) {
  std::string block;
  for (int i = 0; i < 1000; ++i) block += "Zeile " + std::to_string(i) + "\n";
  Append(block);
  line_buffer_process(lb, Collect);
  ASSERT_EQ(g_lines.size(), 1000u);
  EXPECT_EQ(g_lines.front(), "Zeile 0");
  EXPECT_EQ(g_lines.back(), "Zeile 999");
}

TEST_F(LineBufferTest, AppendingNothingChangesNothing) {
  Append("eins\n");
  size_t before = lb->position;
  line_buffer_append(lb, "", 0);
  EXPECT_EQ(lb->position, before);
}

TEST(LineBufferLifetime, DestroyingNullIsSafe) {
  line_buffer_destroy(nullptr);
}

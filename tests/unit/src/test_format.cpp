/** @file
 *  @brief Byte counts as a reader sees them — the unit chosen, the point placed.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

std::string Format(uint64_t bytes, uint8_t precision) {
  char buffer[64];
  int rc = format_byte_units(buffer, sizeof(buffer), bytes, precision);
  EXPECT_GE(rc, 0);
  return std::string(buffer);
}

constexpr uint64_t kKB = 1024ULL;
constexpr uint64_t kMB = 1024ULL * 1024ULL;
constexpr uint64_t kGB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kTB = 1024ULL * 1024ULL * 1024ULL * 1024ULL;

} // namespace

TEST(Format, PlainBytesCarryNoFraction) {
  EXPECT_EQ(Format(0, 2), "0 B");
  EXPECT_EQ(Format(1, 2), "1 B");
  EXPECT_EQ(Format(1023, 2), "1023 B");
}

TEST(Format, TheUnitTurnsAtEveryThreshold) {
  EXPECT_EQ(Format(kKB, 2), "1.00 KB");
  EXPECT_EQ(Format(kMB, 2), "1.00 MB");
  EXPECT_EQ(Format(kGB, 2), "1.00 GB");
  EXPECT_EQ(Format(kTB, 2), "1.00 TB");
}

TEST(Format, JustBelowAThresholdKeepsTheSmallerUnit) {
  EXPECT_EQ(Format(kKB - 1, 2), "1023 B");
  EXPECT_NE(Format(kMB - 1, 2).find("KB"), std::string::npos);
  EXPECT_NE(Format(kGB - 1, 2).find("MB"), std::string::npos);
  EXPECT_NE(Format(kTB - 1, 2).find("GB"), std::string::npos);
}

TEST(Format, BeyondTerabytesTheUnitStandsStill) {
  EXPECT_NE(Format(4096ULL * kTB, 2).find("TB"), std::string::npos);
  EXPECT_NE(Format(UINT64_MAX, 2).find("TB"), std::string::npos);
}

TEST(Format, TheLargestCountTruncatesInsteadOfRoundingUp) {
  // UINT64_MAX / 2^40 is 16777215.999999999999, so truncation owes 16777215.99.
  // Reaching that through a double cannot work: (double)UINT64_MAX is 2^64, and
  // the quotient lands on exactly 16777216 — a place the real value never gets to.
  EXPECT_EQ(Format(UINT64_MAX, 2), "16777215.99 TB");
  EXPECT_EQ(Format(UINT64_MAX, 0), "16777215 TB");
  EXPECT_EQ(Format(UINT64_MAX, 6), "16777215.999999 TB");
}

TEST(Format, OneByteShortOfTheNextThousandStaysShortOfIt) {
  // Not the rounding trap above — both of these fit a double exactly, and the old
  // implementation got them right too. They pin the plainer promise: a count just
  // under the next round figure must never be shown as having reached it.
  EXPECT_EQ(Format(1024ULL * kTB - 1, 2), "1023.99 TB");
  EXPECT_EQ(Format(kTB - 1, 2), "1023.99 GB");
}

TEST(Format, PrecisionDecidesHowManyPlacesFollow) {
  EXPECT_EQ(Format(kKB + kKB / 2, 0), "1 KB");
  EXPECT_EQ(Format(kKB + kKB / 2, 1), "1.5 KB");
  EXPECT_EQ(Format(kKB + kKB / 2, 3), "1.500 KB");
}

TEST(Format, KnownSizesReadAsExpected) {
  EXPECT_EQ(Format(256 * kKB, 2), "256.00 KB");
  EXPECT_EQ(Format(kMB / 2, 2), "512.00 KB");
  EXPECT_EQ(Format(5 * kGB + kGB / 2, 1), "5.5 GB");
}

TEST(Format, AlwaysTerminatesWhatItWrites) {
  char buffer[64];
  std::memset(buffer, 'x', sizeof(buffer));
  int rc = format_byte_units(buffer, sizeof(buffer), 5 * kGB, 2);
  ASSERT_GE(rc, 0);
  EXPECT_NE(std::memchr(buffer, '\0', sizeof(buffer)), nullptr);
}

TEST(Format, TooSmallABufferAsksForRoomInsteadOfOverrunning) {
  char buffer[4];
  std::memset(buffer, 'x', sizeof(buffer));
  int needed = format_byte_units(buffer, sizeof(buffer), 5 * kGB, 6);
  EXPECT_GT(needed, (int)sizeof(buffer)) << "the answer is the size it wanted";
  // nothing was written past the end
  EXPECT_EQ(buffer[3], 'x');
}

TEST(Format, AnAbsurdPrecisionIsCappedRatherThanTrusted) {
  char buffer[128];
  int rc = format_byte_units(buffer, sizeof(buffer), 5 * kGB, 200);
  EXPECT_GE(rc, 0);
  // 15 places is the ceiling; the remainders run dry well before it
  std::string text(buffer);
  size_t point = text.find('.');
  ASSERT_NE(point, std::string::npos);
  size_t digits = text.find(' ') - point - 1;
  EXPECT_LE(digits, 15u);
}

/** @file
 *  @brief The tokenizer — what a written name becomes before anyone searches it.
 *
 *  Index and query walk this same path, so every rule proven here is a rule the
 *  search obeys twice.  The tests are written the way the folding is described:
 *  fold, split, expand, decompose, and the ring that lets a repetition pass.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

/** The words one input yields, in the order the tokenizer produced them. */
std::vector<std::string> Tokens(TextTokenizer *tok, const std::string &text) {
  std::vector<std::string> out;
  size_t n = text_tokenize(tok, text.c_str(), text.size());
  for (size_t i = 0; i < n; ++i) out.emplace_back(tok->tokens[i].data, tok->tokens[i].size);
  return out;
}

/** Only the whole words — the pieces a compound was broken into stay out. */
std::vector<std::string> WholeWords(TextTokenizer *tok, const std::string &text) {
  std::vector<std::string> out;
  size_t n = text_tokenize(tok, text.c_str(), text.size());
  for (size_t i = 0; i < n; ++i) {
    if (!tok->tokens[i].part) out.emplace_back(tok->tokens[i].data, tok->tokens[i].size);
  }
  return out;
}

bool Contains(const std::vector<std::string> &all, const std::string &one) {
  for (const std::string &s : all) {
    if (s == one) return true;
  }
  return false;
}

/** A tokenizer with the repetition filter off — most tests want every input folded. */
class TextTokenizeTest : public ::testing::Test {
protected:
  void SetUp() override {
    text_tokenizer_init(&tok);
    tok.repetition_filter = 0;
  }
  TextTokenizer tok{};
};

} // namespace

TEST_F(TextTokenizeTest, InitLeavesNothingBehind) {
  TextTokenizer fresh;
  text_tokenizer_init(&fresh);
  EXPECT_EQ(fresh.token_count, 0u);
  EXPECT_EQ(fresh.used, 0u);
  EXPECT_EQ(fresh.inputs, 0u);
  EXPECT_EQ(fresh.repeated, 0u);
  EXPECT_EQ(fresh.dropped, 0u);
  // the filter is on until someone turns it off
  EXPECT_EQ(fresh.repetition_filter, 1);
}

TEST_F(TextTokenizeTest, EmptyInputYieldsNothing) {
  EXPECT_EQ(text_tokenize(&tok, nullptr, 0), 0u);
  EXPECT_EQ(text_tokenize(&tok, "", 0), 0u);
  EXPECT_EQ(tok.token_count, 0u);
}

TEST_F(TextTokenizeTest, WordlessInputYieldsNothing) {
  EXPECT_EQ(text_tokenize(&tok, "--- , . /", 9), 0u);
}

TEST_F(TextTokenizeTest, LowercasesAndDropsDiacritics) {
  EXPECT_EQ(WholeWords(&tok, "Café"), (std::vector<std::string>{"cafe"}));
  EXPECT_EQ(WholeWords(&tok, "MALMÖ").size(), 2u); // German reading and plain one differ
  EXPECT_TRUE(Contains(WholeWords(&tok, "MALMÖ"), "malmoe"));
  EXPECT_TRUE(Contains(WholeWords(&tok, "MALMÖ"), "malmo"));
}

TEST_F(TextTokenizeTest, GermanUmlautYieldsBothReadings) {
  std::vector<std::string> words = WholeWords(&tok, "München");
  ASSERT_EQ(words.size(), 2u);
  EXPECT_TRUE(Contains(words, "muenchen"));
  EXPECT_TRUE(Contains(words, "munchen"));
  // both readings come from the same word of the input, so a searcher may or them
  ASSERT_EQ(tok.token_count, 2u);
  EXPECT_EQ(tok.tokens[0].group, tok.tokens[1].group);
}

TEST_F(TextTokenizeTest, SharpSHasOneReadingOnly) {
  // ß becomes ss in both readings, so nothing is doubled
  EXPECT_EQ(WholeWords(&tok, "Straße"), (std::vector<std::string>{"strasse"}));
}

TEST_F(TextTokenizeTest, SplitsOnEverythingThatIsNoLetterOrDigit) {
  EXPECT_EQ(
      WholeWords(&tok, "Sankt Peter-Ording"), (std::vector<std::string>{"sankt", "peter", "ording"})
  );
  EXPECT_EQ(WholeWords(&tok, "a,b/c.d e"), (std::vector<std::string>{"a", "b", "c", "d", "e"}));
}

TEST_F(TextTokenizeTest, SeparateWordsGetSeparateGroups) {
  ASSERT_EQ(text_tokenize(&tok, "Berliner Strasse", 16), 2u);
  EXPECT_NE(tok.tokens[0].group, tok.tokens[1].group);
}

TEST_F(TextTokenizeTest, ExpandsGermanAbbreviations) {
  EXPECT_TRUE(Contains(WholeWords(&tok, "Superstr."), "superstrasse"));
  EXPECT_TRUE(Contains(WholeWords(&tok, "St. Peter"), "sankt"));
  EXPECT_TRUE(Contains(WholeWords(&tok, "Marienpl."), "marienplatz"));
}

TEST_F(TextTokenizeTest, AbbreviationAndSpelledOutFormMeet) {
  std::vector<std::string> shortForm = WholeWords(&tok, "Superstr.");
  std::vector<std::string> longForm = WholeWords(&tok, "Superstrasse");
  EXPECT_EQ(shortForm, longForm);
}

TEST_F(TextTokenizeTest, DecomposesCompoundsAndKeepsTheWhole) {
  std::vector<std::string> all = Tokens(&tok, "Leopoldstraße");
  EXPECT_TRUE(Contains(all, "leopoldstrasse"));
  EXPECT_TRUE(Contains(all, "leopold"));
  EXPECT_TRUE(Contains(all, "strasse"));

  // the compound itself is a whole word, its halves are marked as pieces
  size_t whole = 0, parts = 0;
  for (size_t i = 0; i < tok.token_count; ++i) (tok.tokens[i].part ? parts : whole)++;
  EXPECT_EQ(whole, 1u);
  EXPECT_EQ(parts, 2u);
}

TEST_F(TextTokenizeTest, FoldsTheWholeLatinScript) {
  // Latin Extended-A was always covered; these blocks were not
  EXPECT_EQ(WholeWords(&tok, "Ǎnhuī"), (std::vector<std::string>{"anhui"}));        // Extended-B
  EXPECT_EQ(WholeWords(&tok, "Đà Nẵng"), (std::vector<std::string>{"da", "nang"})); // Vietnamese
  EXPECT_EQ(WholeWords(&tok, "Hồ"), (std::vector<std::string>{"ho"}));
  EXPECT_EQ(WholeWords(&tok, "Ɇ"), (std::vector<std::string>{"e"}));
  // U+2C65 and U+2C66 are the lowercase of U+023A and U+023E: each pair has to
  // fold to the same letter, though the two halves live in different blocks
  EXPECT_EQ(WholeWords(&tok, "\u2C65"), (std::vector<std::string>{"a"}));
  EXPECT_EQ(WholeWords(&tok, "\u2C65"), WholeWords(&tok, "\u023A"));
  EXPECT_EQ(WholeWords(&tok, "\u2C66"), (std::vector<std::string>{"t"}));
  EXPECT_EQ(WholeWords(&tok, "\u2C66"), WholeWords(&tok, "\u023E"));
}

TEST_F(TextTokenizeTest, TheTwoRomanianCommasMeet) {
  // U+0219 is the correct letter, U+015F the cedilla it is written with just as
  // often; a name spelled either way must reach the same word
  EXPECT_EQ(WholeWords(&tok, "București"), (std::vector<std::string>{"bucuresti"}));
  EXPECT_EQ(WholeWords(&tok, "Bucureşti"), (std::vector<std::string>{"bucuresti"}));
  EXPECT_EQ(WholeWords(&tok, "Ștefan"), WholeWords(&tok, "Ştefan"));
}

TEST_F(TextTokenizeTest, DecomposedSpellingsFoldLikeComposedOnes) {
  // "u" + U+0308 is the same name as "ü", and must yield the same words —
  // the German reading among them
  EXPECT_EQ(WholeWords(&tok, "Mu\u0308nchen"), WholeWords(&tok, "München"));
  EXPECT_TRUE(Contains(WholeWords(&tok, "Mu\u0308nchen"), "muenchen"));
  EXPECT_TRUE(Contains(WholeWords(&tok, "Mu\u0308nchen"), "munchen"));
  EXPECT_EQ(WholeWords(&tok, "Krako\u0301w"), (std::vector<std::string>{"krakow"}));
  // a mark never ends a word: the letters on both sides stay together
  EXPECT_EQ(WholeWords(&tok, "Malmo\u0308"), WholeWords(&tok, "Malmö"));
}

TEST_F(TextTokenizeTest, CapitalSharpSIsTwoSLikeItsSmallForm) {
  EXPECT_EQ(WholeWords(&tok, "GROẞE"), (std::vector<std::string>{"grosse"}));
}

TEST_F(TextTokenizeTest, LigaturesFallApart) {
  EXPECT_EQ(WholeWords(&tok, "ﬂughafen"), (std::vector<std::string>{"flughafen"}));
}

TEST_F(TextTokenizeTest, ModifierLettersPartWordsLikeAnApostrophe) {
  // the Hawaiian ʻokina stands where an apostrophe stands
  EXPECT_EQ(WholeWords(&tok, "Wai\u02bbale"), WholeWords(&tok, "Wai'ale"));
}

TEST_F(TextTokenizeTest, ScriptsWithoutCasePassThrough) {
  // Chinese has no case and no diacritics to shed; it must survive intact
  std::vector<std::string> words = WholeWords(&tok, "北京");
  ASSERT_EQ(words.size(), 1u);
  EXPECT_EQ(words[0], "北京");
}

TEST_F(TextTokenizeTest, CyrillicIsLoweredAndKept) {
  std::vector<std::string> words = WholeWords(&tok, "МОСКВА");
  ASSERT_FALSE(words.empty());
  EXPECT_EQ(words[0], "москва");
}

TEST_F(TextTokenizeTest, RepetitionsWithinOneInputDissolve) {
  std::vector<std::string> words = WholeWords(&tok, "Berlin Berlin");
  EXPECT_EQ(words, (std::vector<std::string>{"berlin"}));
}

TEST_F(TextTokenizeTest, DigitsSurviveAsWords) {
  EXPECT_EQ(WholeWords(&tok, "10715"), (std::vector<std::string>{"10715"}));
  EXPECT_TRUE(Contains(WholeWords(&tok, "Straße des 17. Juni"), "17"));
}

TEST_F(TextTokenizeTest, CountersFollowTheInputs) {
  text_tokenize(&tok, "eins", 4);
  text_tokenize(&tok, "zwei", 4);
  EXPECT_EQ(tok.inputs, 2u);
}

// ---------------------------------------------------------------------------
//  The repetition filter, which is a different tokenizer in the same struct
// ---------------------------------------------------------------------------

TEST(TextTokenizeFilter, SwallowsAnInputItJustSaw) {
  TextTokenizer tok;
  text_tokenizer_init(&tok); // filter on
  EXPECT_GT(text_tokenize(&tok, "München", 8), 0u);
  EXPECT_EQ(text_tokenize(&tok, "München", 8), 0u) << "the same text twice in a row";
  EXPECT_EQ(tok.repeated, 1u);
}

TEST(TextTokenizeFilter, SwallowsAnInputFromTheRing) {
  TextTokenizer tok;
  text_tokenizer_init(&tok);
  text_tokenize(&tok, "Berlin", 6);
  text_tokenize(&tok, "Hamburg", 7);
  text_tokenize(&tok, "Bremen", 6);
  // Berlin is no longer the previous input, but it is still in the ring
  EXPECT_EQ(text_tokenize(&tok, "Berlin", 6), 0u);
  EXPECT_GT(tok.repeated, 0u);
}

TEST(TextTokenizeFilter, ClearedFilterFoldsEveryInput) {
  TextTokenizer tok;
  text_tokenizer_init(&tok);
  tok.repetition_filter = 0;
  size_t first = text_tokenize(&tok, "München", 8);
  size_t again = text_tokenize(&tok, "München", 8);
  EXPECT_GT(first, 0u);
  EXPECT_EQ(first, again) << "with the filter off the words are handed out again";
}

TEST(TextTokenizeFilter, LongInputsAreNeverFilteredCheaply) {
  TextTokenizer tok;
  text_tokenizer_init(&tok);
  // longer than TEXT_RECENT_BYTES, so the ring cannot hold it
  std::string on(TEXT_RECENT_BYTES + 10, 'a');
  EXPECT_GT(text_tokenize(&tok, on.c_str(), on.size()), 0u);
  EXPECT_GT(text_tokenize(&tok, on.c_str(), on.size()), 0u);
}

TEST(TextTokenizeLimits, TooManyWordsAreCounted) {
  TextTokenizer tok;
  text_tokenizer_init(&tok);
  tok.repetition_filter = 0;
  std::string many;
  for (int i = 0; i < TEXT_TOKEN_MAX * 2; ++i) many += "wort" + std::to_string(i) + " ";
  size_t n = text_tokenize(&tok, many.c_str(), many.size());
  EXPECT_LE(n, (size_t)TEXT_TOKEN_MAX);
  EXPECT_GT(tok.dropped, 0u) << "what found no slot is counted, not lost in silence";
}

TEST(TextTokenizeLimits, NullTokenizerIsAnswered) {
  EXPECT_EQ(text_tokenize(nullptr, "Berlin", 6), 0u);
  text_tokenizer_init(nullptr); // must not fall over
}

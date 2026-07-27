/** @defgroup text_tokenize Text tokenizer
 *  @ingroup data
 *  @brief Turning a written place name into the words a query is made of —
 *         folded, split, and stripped of the spelling a keyboard cannot
 *         reproduce.
 *
 *  Index and query must walk the same path, or they never meet: whatever
 *  `Sankt Peter-Ording` becomes here, `sankt peter ording` must become too.
 *  The transformation is deterministic and has no state beyond the scratch
 *  buffer it writes into.
 *
 *  ### What happens to a string
 *
 *  1. **Folding** — every character becomes lowercase and loses its
 *     diacritics: `é → e`, `ø → o`, `Č → c`.  German spellings additionally
 *     take the written form a keyboard offers: `ß → ss`, `ü → ue`, and — as
 *     a second variant of the same input — `ü → u`, so that both *München*
 *     and *Munchen* reach *muenchen* or *munchen*.  Cyrillic and Greek are
 *     lowercased and kept; scripts without a case, Chinese and Arabic among
 *     them, pass through untouched.
 *  2. **Splitting** — anything that is neither letter nor digit ends a word.
 *     Hyphen, dot, comma, slash and space all part *Sankt Peter-Ording* into
 *     three.
 *  3. **Expanding** — the abbreviations a German address is written with
 *     open up: `str → strasse`, `st → sankt`, `pl → platz`.
 *  4. **Decomposing** — a compound ending in a street word yields its parts
 *     as well: `superstrasse` also becomes `super` and `strasse`.  The
 *     compound itself is kept, so an exact query stays exact.
 *
 *  Every word appears once; repetitions within one input dissolve.
 *
 *  ### Repetition filter
 *
 *  The dump offers the same parent text again and again in immediate
 *  succession.  A small ring of recently seen inputs, compared byte for
 *  byte, lets those pass before any folding happens — the work saved is the
 *  work never begun.
 *
 *  The filter is right while a vocabulary is being gathered, where a
 *  repetition adds nothing, and wrong wherever every occurrence counts — a
 *  posting belongs to its document even if the word came by a moment ago.
 *  Clearing @c repetition_filter turns it off.
 *
 *  @whisper A name is spoken plainly, so that it can be recognised however it was written
 *  @{
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
  /** Words one input may yield; a compound with many parts is the upper case. */
  TEXT_TOKEN_MAX = 48,
  /** Scratch bytes for the folded forms of one input. */
  TEXT_BUFFER_MAX = 1024,
  /** Inputs remembered for the repetition filter. */
  TEXT_RECENT_SLOTS = 16,
  /** Longest input the repetition filter keeps; longer ones are never repeated cheaply. */
  TEXT_RECENT_BYTES = 64
};

/** One folded word, pointing into the tokenizer's buffer. */
typedef struct TextToken {
  const char *data; /**< Folded bytes, not NUL-terminated. */
  size_t size;      /**< Byte length. */
} TextToken;

/** An input the filter remembers verbatim. */
typedef struct TextRecent {
  uint8_t size;                 /**< Byte length, or 0 while the slot is unused. */
  char bytes[TEXT_RECENT_BYTES]; /**< The input as it arrived. */
} TextRecent;

/**
 * @brief Per-thread scratch space for tokenizing.
 *
 *  Holds the folded bytes and the words pointing into them.  Both are valid
 *  until the next call of text_tokenize() on the same tokenizer.  A
 *  tokenizer belongs to one thread; nothing here is synchronised.
 */
typedef struct TextTokenizer {
  char buffer[TEXT_BUFFER_MAX];        /**< Folded bytes of the current input. */
  size_t used;                         /**< Bytes taken from @c buffer. */
  TextToken tokens[TEXT_TOKEN_MAX];    /**< Words of the current input. */
  size_t token_count;                  /**< Words produced by the last call. */
  uint64_t inputs;                     /**< Inputs offered since init. */
  uint64_t repeated;                   /**< Inputs the filter absorbed. */
  uint64_t dropped;                    /**< Words that found no slot or no space. */
  TextRecent recent[TEXT_RECENT_SLOTS]; /**< Ring of recently seen inputs. */
  TextRecent previous;                 /**< The input right before this one. */
  unsigned recent_next;                /**< Slot the next input overwrites. */
  int repetition_filter;               /**< Set by init; clear it to fold every input. */
} TextTokenizer;

/**
 * @brief Prepare an empty tokenizer.
 *
 *  @param[in,out] tokenizer  Tokenizer to initialise; must not be NULL.
 */
void text_tokenizer_init(TextTokenizer *tokenizer);

/**
 * @brief Fold @p text and split it into the words a query would carry.
 *
 *  The words are left in @c tokenizer->tokens and stay valid until the next
 *  call.  An input identical to one of the last @ref TEXT_RECENT_SLOTS
 *  inputs yields nothing at all — it has already been through.
 *
 *  @param[in,out] tokenizer  Scratch space; must not be NULL.
 *  @param[in]     text       UTF-8 bytes; may be NULL when @p size is 0.
 *  @param[in]     size       Byte length of @p text.
 *  @return Number of words produced — 0 for empty, repeated, or wordless input.
 *
 *  @whisper What was written once is heard in every spelling it might take
 */
size_t text_tokenize(TextTokenizer *tokenizer, const char *text, size_t size);

/** @} */

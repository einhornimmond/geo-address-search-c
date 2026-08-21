/** @cond INTERNAL */

#include "search/text_tokenize.h"

#include <string.h>

/* =========================================================================
 *  UTF-8
 * ========================================================================= */

/**
 * @brief Read one code point starting at @p pos.
 *
 *  Malformed sequences yield code point 0 and advance by one byte, which the
 *  caller treats as a word boundary — broken bytes separate, they never join.
 *
 *  @return Byte length of the sequence consumed, always ≥ 1.
 */
static size_t utf8_next(const char *text, size_t size, size_t pos, uint32_t *out) {
  uint8_t lead = (uint8_t)text[pos];
  if (lead < 0x80) {
    *out = lead;
    return 1;
  }

  size_t length;
  uint32_t code;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    code = lead & 0x1Fu;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    code = lead & 0x0Fu;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    code = lead & 0x07u;
  } else {
    *out = 0;
    return 1;
  }
  if (pos + length > size) {
    *out = 0;
    return 1;
  }
  for (size_t i = 1; i < length; ++i) {
    uint8_t next = (uint8_t)text[pos + i];
    if ((next & 0xC0) != 0x80) {
      *out = 0;
      return 1;
    }
    code = (code << 6) | (next & 0x3Fu);
  }
  *out = code;
  return length;
}

/** Write @p code point as UTF-8; returns the bytes written (0 if no room). */
static size_t utf8_write(char *out, size_t room, uint32_t code) {
  if (code < 0x80) {
    if (room < 1) return 0;
    out[0] = (char)code;
    return 1;
  }
  if (code < 0x800) {
    if (room < 2) return 0;
    out[0] = (char)(0xC0 | (code >> 6));
    out[1] = (char)(0x80 | (code & 0x3F));
    return 2;
  }
  if (code < 0x10000) {
    if (room < 3) return 0;
    out[0] = (char)(0xE0 | (code >> 12));
    out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[2] = (char)(0x80 | (code & 0x3F));
    return 3;
  }
  if (room < 4) return 0;
  out[0] = (char)(0xF0 | (code >> 18));
  out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
  out[3] = (char)(0x80 | (code & 0x3F));
  return 4;
}

/* =========================================================================
 *  Folding
 * ========================================================================= */

/** Base letter of every code point in Latin Extended-A (U+0100 … U+017F). */
static const char LATIN_A_BASE[] = "aaaaaa"       /* U+0100 … Ā ā Ă ă Ą ą   */
                                   "cccccccc"     /* U+0106 … Ć … č          */
                                   "dddd"         /* U+010E … Ď ď Đ đ        */
                                   "eeeeeeeeee"   /* U+0112 … Ē … ě          */
                                   "gggggggg"     /* U+011C … Ĝ … ģ          */
                                   "hhhh"         /* U+0124 … Ĥ ĥ Ħ ħ        */
                                   "iiiiiiiiii"   /* U+0128 … Ĩ … ı          */
                                   "ii"           /* U+0132 Ĳ ĳ (see below)  */
                                   "jj"           /* U+0134 Ĵ ĵ              */
                                   "kkk"          /* U+0136 Ķ ķ ĸ            */
                                   "llllllllll"   /* U+0139 … Ĺ … ł          */
                                   "nnnnnnnnn"    /* U+0143 … Ń … ŋ          */
                                   "oooooo"       /* U+014C … Ō … ő          */
                                   "oo"           /* U+0152 Œ œ (see below)  */
                                   "rrrrrr"       /* U+0154 … Ŕ … ř          */
                                   "ssssssss"     /* U+015A … Ś … š          */
                                   "tttttt"       /* U+0162 … Ţ … ŧ          */
                                   "uuuuuuuuuuuu" /* U+0168 … Ũ … ų          */
                                   "ww"           /* U+0174 Ŵ ŵ              */
                                   "yyy"          /* U+0176 Ŷ ŷ Ÿ            */
                                   "zzzzzz"       /* U+0179 … Ź … ž          */
                                   "s";           /* U+017F ſ                */

/** Base letter of every code point in Latin Extended-B (U+0180 … U+024F).
 *
 *  The block is a rummage room rather than an alphabet: Romanian's comma-below
 *  ș and ț live here beside pinyin's carons, Vietnamese's horned ơ and ư, and a
 *  scattering of African letters.  A `*` marks the four click letters, which are
 *  punctuation to a keyboard and part a word rather than join it.  The handful
 *  of digraphs — Ǆ Ǉ Ǌ Ǣ Ǽ ƕ — carry only their first letter here; fold_code()
 *  catches them before the lookup and writes both. */
static const char LATIN_B_BASE[] = "bbbbbboccdddddee"  /* U+0180 ƀ … Ə */
                                   "effgghiikkllmnno"  /* U+0190 Ɛ … Ɵ */
                                   "oooopprssssttttu"  /* U+01A0 Ơ … Ư */
                                   "uuvyyzzzzzzzzzzw"  /* U+01B0 ư … ƿ */
                                   "****dddlllnnnaai"  /* U+01C0 ǀ … Ǐ */
                                   "ioouuuuuuuuuueaa"  /* U+01D0 ǐ … ǟ */
                                   "aaaaggggkkoooozz"  /* U+01E0 Ǡ … ǯ */
                                   "jdddgghwnnaaaaoo"  /* U+01F0 ǰ … ǿ */
                                   "aaaaeeeeiiiioooo"  /* U+0200 Ȁ … ȏ */
                                   "rrrruuuussttgghh"  /* U+0210 Ȑ … ȟ */
                                   "ndoozzaaeeoooooo"  /* U+0220 Ƞ … ȯ */
                                   "ooyylntjdqacclts"  /* U+0230 Ȱ … ȿ */
                                   "z**buveejjqqrryy"; /* U+0240 ɀ … ɏ */

/** Base letter of every code point in Latin Extended Additional (U+1E00 … U+1EFF).
 *
 *  Two halves.  The first (U+1E00 … U+1E9F) is one Latin letter under every mark
 *  Europe ever put on it — dots above and below, lines, rings.  The second
 *  (U+1EA0 … U+1EF9) is the whole Vietnamese alphabet, where a vowel carries a
 *  shape and a tone at once and every one of them folds back to the same five
 *  letters.  ẞ alone yields two bytes and is caught before the lookup. */
static const char LATIN_ADDITIONAL_BASE[] = "aabbbbbbccdddddd"  /* U+1E00 Ḁ … ḏ */
                                            "ddddeeeeeeeeeeff"  /* U+1E10 Ḑ … ḟ */
                                            "gghhhhhhhhhhiiii"  /* U+1E20 Ḡ … ḯ */
                                            "kkkkkkllllllllmm"  /* U+1E30 Ḱ … ḿ */
                                            "mmmmnnnnnnnnoooo"  /* U+1E40 Ṁ … ṏ */
                                            "oooopppprrrrrrrr"  /* U+1E50 Ṑ … ṟ */
                                            "sssssssssstttttt"  /* U+1E60 Ṡ … ṯ */
                                            "ttuuuuuuuuuuvvvv"  /* U+1E70 Ṱ … ṿ */
                                            "wwwwwwwwwwxxxxyy"  /* U+1E80 Ẁ … ẏ */
                                            "zzzzzzhtwyassssd"  /* U+1E90 Ẑ … ẟ */
                                            "aaaaaaaaaaaaaaaa"  /* U+1EA0 Ạ … ắ */
                                            "aaaaaaaaeeeeeeee"  /* U+1EB0 Ằ … ế */
                                            "eeeeeeeeiiiioooo"  /* U+1EC0 Ề … ỏ */
                                            "oooooooooooooooo"  /* U+1ED0 Ố … ở */
                                            "oooouuuuuuuuuuuu"  /* U+1EE0 Ỡ … ữ */
                                            "uuyyyyyyyyllvvyy"; /* U+1EF0 Ự … ỿ */

/** Outcome of folding one code point. */
typedef enum FoldKind {
  FOLD_SEPARATOR, /**< Ends the current word. */
  FOLD_TEXT,      /**< Bytes were written. */
  FOLD_SKIP       /**< Nothing written, and the word goes on — a combining mark. */
} FoldKind;

/**
 * @brief Fold one code point into keyboard-reachable bytes.
 *
 *  @param[in]  code    Code point to fold.
 *  @param[in]  german  Whether umlauts expand (`ü → ue`) or reduce (`ü → u`).
 *  @param[out] out     Receives up to 4 bytes.
 *  @param[out] written Bytes written.
 *  @param[out] special Set when the code point has a German spelling at all —
 *                      the caller then knows a second variant is worth folding.
 *  @return FOLD_SEPARATOR when the code point ends a word, FOLD_SKIP when it
 *          belongs to the letter before it and leaves nothing of its own.
 */
static FoldKind fold_code(uint32_t code, int german, char *out, size_t *written, int *special) {
  *written = 0;

  /* --- ASCII --- */
  if (code < 0x80) {
    if ((code >= 'a' && code <= 'z') || (code >= '0' && code <= '9')) {
      out[(*written)++] = (char)code;
      return FOLD_TEXT;
    }
    if (code >= 'A' && code <= 'Z') {
      out[(*written)++] = (char)(code - 'A' + 'a');
      return FOLD_TEXT;
    }
    return FOLD_SEPARATOR;
  }

  /* --- Latin-1 punctuation and symbols --- */
  if (code < 0xC0) return FOLD_SEPARATOR;

  /* --- Latin-1 letters, German spellings among them --- */
  if (code < 0x100) {
    static const char LATIN_1_BASE[] = "aaaaaaaceeeeiiii"  /* U+00C0 … À … Ï */
                                       "dnooooo*ouuuuyts"  /* U+00D0 … Ð … ß */
                                       "aaaaaaaceeeeiiii"  /* U+00E0 … à … ï */
                                       "dnooooo*ouuuuyty"; /* U+00F0 … ð … ÿ */
    switch (code) {
    case 0xC4:
    case 0xE4: /* Ä ä */
      *special = 1;
      out[(*written)++] = 'a';
      if (german) out[(*written)++] = 'e';
      return FOLD_TEXT;
    case 0xD6:
    case 0xF6: /* Ö ö */
      *special = 1;
      out[(*written)++] = 'o';
      if (german) out[(*written)++] = 'e';
      return FOLD_TEXT;
    case 0xDC:
    case 0xFC: /* Ü ü */
      *special = 1;
      out[(*written)++] = 'u';
      if (german) out[(*written)++] = 'e';
      return FOLD_TEXT;
    case 0xC6:
    case 0xE6: /* Æ æ */
      out[(*written)++] = 'a';
      out[(*written)++] = 'e';
      return FOLD_TEXT;
    case 0xDF: /* ß — always two s, in every reading */
      out[(*written)++] = 's';
      out[(*written)++] = 's';
      return FOLD_TEXT;
    default:
      break;
    }
    char base = LATIN_1_BASE[code - 0xC0];
    if (base == '*') return FOLD_SEPARATOR; /* × and ÷ are not letters */
    out[(*written)++] = base;
    return FOLD_TEXT;
  }

  /* --- Latin Extended-A --- */
  if (code < 0x180) {
    if (code == 0x132 || code == 0x133) { /* Ĳ ĳ */
      out[(*written)++] = 'i';
      out[(*written)++] = 'j';
      return FOLD_TEXT;
    }
    if (code == 0x152 || code == 0x153) { /* Œ œ */
      out[(*written)++] = 'o';
      out[(*written)++] = 'e';
      return FOLD_TEXT;
    }
    out[(*written)++] = LATIN_A_BASE[code - 0x100];
    return FOLD_TEXT;
  }

  /* --- Latin Extended-B --- */
  if (code < 0x250) {
    switch (code) {
    case 0x195: /* ƕ */
    case 0x1F6: /* Ƕ */
      out[(*written)++] = 'h';
      out[(*written)++] = 'v';
      return FOLD_TEXT;
    case 0x1C4: /* Ǆ ǅ ǆ */
    case 0x1C5:
    case 0x1C6:
    case 0x1F1: /* Ǳ ǲ ǳ */
    case 0x1F2:
    case 0x1F3:
      out[(*written)++] = 'd';
      out[(*written)++] = 'z';
      return FOLD_TEXT;
    case 0x1C7: /* Ǉ ǈ ǉ */
    case 0x1C8:
    case 0x1C9:
      out[(*written)++] = 'l';
      out[(*written)++] = 'j';
      return FOLD_TEXT;
    case 0x1CA: /* Ǌ ǋ ǌ */
    case 0x1CB:
    case 0x1CC:
      out[(*written)++] = 'n';
      out[(*written)++] = 'j';
      return FOLD_TEXT;
    case 0x1E2: /* Ǣ ǣ Ǽ ǽ */
    case 0x1E3:
    case 0x1FC:
    case 0x1FD:
      out[(*written)++] = 'a';
      out[(*written)++] = 'e';
      return FOLD_TEXT;
    default:
      break;
    }
    char base = LATIN_B_BASE[code - 0x180];
    if (base == '*') return FOLD_SEPARATOR; /* the click letters ǀ ǁ ǂ ǃ */
    out[(*written)++] = base;
    return FOLD_TEXT;
  }

  /* --- IPA extensions (U+0250 … U+02AF) are left alone: a phonetic symbol in a
         place name is the name, not a spelling of it --- */

  /* --- Modifier letters: the Hawaiian ʻokina and its kin stand where an
         apostrophe stands, and part a word the same way --- */
  if (code >= 0x2B0 && code <= 0x2FF) return FOLD_SEPARATOR;

  /* --- Combining marks: a decomposed spelling drops its mark and keeps its
         letter, so that whichever way the dump writes ü, the same word comes
         out.  The word does not end here — the mark belongs to the letter
         before it. --- */
  if (code >= 0x300 && code <= 0x36F) return FOLD_SKIP;

  /* --- Latin Extended Additional --- */
  if (code >= 0x1E00 && code <= 0x1EFF) {
    if (code == 0x1E9E) { /* ẞ — the capital of ß, and two s like it */
      out[(*written)++] = 's';
      out[(*written)++] = 's';
      return FOLD_TEXT;
    }
    out[(*written)++] = LATIN_ADDITIONAL_BASE[code - 0x1E00];
    return FOLD_TEXT;
  }

  /* --- Latin Extended-C holds the lowercase of two letters whose capitals live
         in Extended-B: Ⱥ and Ⱦ fold to a and t there, and their small forms
         must fold with them. --- */
  if (code == 0x2C65 || code == 0x2C66) { /* ⱥ ⱦ */
    out[(*written)++] = code == 0x2C65 ? 'a' : 't';
    return FOLD_TEXT;
  }

  /* --- Typographic ligatures: what a typesetter joined, a keyboard parts --- */
  if (code >= 0xFB00 && code <= 0xFB06) {
    static const char *const LIGATURES[] = {"ff", "fi", "fl", "ffi", "ffl", "st", "st"};
    for (const char *letter = LIGATURES[code - 0xFB00]; *letter; ++letter) {
      out[(*written)++] = *letter;
    }
    return FOLD_TEXT;
  }

  /* --- Greek and Cyrillic: lowercase, otherwise left as they are --- */
  if (code >= 0x391 && code <= 0x3A9)
    code += 0x20; /* Α … Ω */
  else if (code >= 0x410 && code <= 0x42F)
    code += 0x20; /* А … Я */
  else if (code >= 0x400 && code <= 0x40F)
    code += 0x50; /* Ѐ … Џ */
  else if (code >= 0x2000 && code <= 0x206F)
    return FOLD_SEPARATOR; /* general punctuation */
  else if (code >= 0xFF00 && code <= 0xFF0F)
    return FOLD_SEPARATOR; /* fullwidth punctuation */

  *written = utf8_write(out, 4, code);
  return *written ? FOLD_TEXT : FOLD_SEPARATOR;
}

/* =========================================================================
 *  Abbreviations and compounds
 * ========================================================================= */

/** An abbreviation as written, and the word it stands for. */
typedef struct Abbreviation {
  const char *shortened;
  size_t shortened_size;
  const char *full;
  size_t full_size;
} Abbreviation;

#define ABBREVIATION(short_form, long_form)                                                        \
  {short_form, sizeof(short_form) - 1, long_form, sizeof(long_form) - 1}

/** Abbreviations standing on their own: `St. Peter`, `Alter Pl.` */
static const Abbreviation ABBREVIATIONS[] = {
    ABBREVIATION("str", "strasse"),
    ABBREVIATION("st", "sankt"),
    ABBREVIATION("pl", "platz"),
};

/** Abbreviations grown onto a compound: `Bahnhofstr.` is one word, not two. */
static const Abbreviation TAIL_ABBREVIATIONS[] = {
    ABBREVIATION("str", "strasse"),
    ABBREVIATION("pl", "platz"),
};

/** The words a German street compound ends in, and splits at. */
static const struct {
  const char *word;
  size_t size;
} COMPOUND_TAILS[] = {
    {"strasse", 7}, {"weg", 3},  {"allee", 5}, {"platz", 5},    {"gasse", 5},
    {"ring", 4},    {"damm", 4}, {"ufer", 4},  {"chaussee", 8}, {"steig", 5},
};

/** Shortest part a compound split may leave in front — below that it is noise. */
#define COMPOUND_HEAD_MIN 3

/* =========================================================================
 *  Collecting words
 * ========================================================================= */

/** Append one word unless it is already there. */
static void token_add(
    TextTokenizer *tokenizer, const char *data, size_t size, uint16_t group, uint8_t part
) {
  if (!size) return;
  for (size_t i = 0; i < tokenizer->token_count; ++i) {
    if (tokenizer->tokens[i].size == size && memcmp(tokenizer->tokens[i].data, data, size) == 0) {
      return;
    }
  }
  if (tokenizer->token_count >= TEXT_TOKEN_MAX) {
    ++tokenizer->dropped;
    return;
  }
  tokenizer->tokens[tokenizer->token_count].data = data;
  tokenizer->tokens[tokenizer->token_count].size = size;
  tokenizer->tokens[tokenizer->token_count].group = group;
  tokenizer->tokens[tokenizer->token_count].part = part;
  ++tokenizer->token_count;
}

/** Copy @p text into the scratch buffer; returns NULL when it no longer fits. */
static const char *buffer_put(TextTokenizer *tokenizer, const char *text, size_t size) {
  if (tokenizer->used + size > TEXT_BUFFER_MAX) {
    ++tokenizer->dropped;
    return NULL;
  }
  char *slot = tokenizer->buffer + tokenizer->used;
  memcpy(slot, text, size);
  tokenizer->used += size;
  return slot;
}

/**
 * @brief Close one folded word: expand it, keep it, and let a compound fall apart.
 */
static void word_finish(TextTokenizer *tokenizer, const char *word, size_t size, uint16_t group) {
  if (!size) return;

  /* --- an abbreviation stands for its full word, and only for that --- */
  for (size_t i = 0; i < sizeof(ABBREVIATIONS) / sizeof(ABBREVIATIONS[0]); ++i) {
    const Abbreviation *abbreviation = &ABBREVIATIONS[i];
    if (size != abbreviation->shortened_size) continue;
    if (memcmp(word, abbreviation->shortened, size) != 0) continue;
    const char *full = buffer_put(tokenizer, abbreviation->full, abbreviation->full_size);
    if (full) {
      word = full;
      size = abbreviation->full_size;
    }
    break;
  }

  /* --- an abbreviation grown onto a compound opens up with it --- */
  for (size_t i = 0; i < sizeof(TAIL_ABBREVIATIONS) / sizeof(TAIL_ABBREVIATIONS[0]); ++i) {
    const Abbreviation *abbreviation = &TAIL_ABBREVIATIONS[i];
    size_t tail = abbreviation->shortened_size;
    if (size <= tail + COMPOUND_HEAD_MIN - 1) continue;
    if (memcmp(word + size - tail, abbreviation->shortened, tail) != 0) continue;

    size_t head = size - tail;
    size_t grown_size = head + abbreviation->full_size;
    const char *grown = buffer_put(tokenizer, word, head);
    if (!grown) break;
    if (!buffer_put(tokenizer, abbreviation->full, abbreviation->full_size)) break;
    word = grown; /* the two pieces landed side by side */
    size = grown_size;
    break;
  }

  token_add(tokenizer, word, size, group, 0);

  /* --- a compound also names its parts --- */
  for (size_t i = 0; i < sizeof(COMPOUND_TAILS) / sizeof(COMPOUND_TAILS[0]); ++i) {
    size_t tail = COMPOUND_TAILS[i].size;
    if (size <= tail + COMPOUND_HEAD_MIN - 1) continue;
    if (memcmp(word + size - tail, COMPOUND_TAILS[i].word, tail) != 0) continue;
    token_add(tokenizer, word, size - tail, group, 1);
    token_add(tokenizer, word + size - tail, tail, group, 1);
    break;
  }
}

/** Fold the whole input once and cut it into words. */
static int fold_pass(TextTokenizer *tokenizer, const char *text, size_t size, int german) {
  int special = 0;
  size_t word_start = tokenizer->used;
  /* both readings walk the same input and break at the same places, so the
     n-th word of one is the n-th word of the other */
  uint16_t group = 0;
  /* the last letter written on its own — what a combining mark attaches to */
  char base = 0;

  for (size_t pos = 0; pos < size;) {
    uint32_t code;
    pos += utf8_next(text, size, pos, &code);

    char folded[4];
    size_t written = 0;
    FoldKind kind = fold_code(code, german, folded, &written, &special);

    if (FOLD_SKIP == kind) {
      /* A decomposed umlaut is the same umlaut: the mark itself is gone, but
         the German reading it stands for must appear all the same.  Otherwise
         a dump that writes u + combining diaeresis would reach "munchen"
         only, while the composed ü reaches "muenchen" beside it. */
      if (0x308 == code && ('a' == base || 'o' == base || 'u' == base)) {
        special = 1;
        if (german) {
          if (tokenizer->used >= TEXT_BUFFER_MAX) {
            ++tokenizer->dropped;
            break;
          }
          tokenizer->buffer[tokenizer->used++] = 'e';
        }
      }
      base = 0;
      continue;
    }

    if (FOLD_SEPARATOR == kind || !written) {
      if (tokenizer->used > word_start) {
        word_finish(tokenizer, tokenizer->buffer + word_start, tokenizer->used - word_start, group);
        ++group;
      }
      word_start = tokenizer->used;
      base = 0;
      continue;
    }
    if (tokenizer->used + written > TEXT_BUFFER_MAX) {
      ++tokenizer->dropped;
      break;
    }
    memcpy(tokenizer->buffer + tokenizer->used, folded, written);
    tokenizer->used += written;
    /* only a lone letter can carry a mark; a digraph has already said its piece */
    base = 1 == written ? folded[0] : 0;
  }
  word_finish(tokenizer, tokenizer->buffer + word_start, tokenizer->used - word_start, group);
  return special;
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

void text_tokenizer_init(TextTokenizer *tokenizer) {
  if (!tokenizer) return;
  memset(tokenizer, 0, sizeof(*tokenizer));
  tokenizer->repetition_filter = 1;
}

size_t text_tokenize(TextTokenizer *tokenizer, const char *text, size_t size) {
  if (!tokenizer) return 0;
  if (!text || !size) {
    tokenizer->used = 0;
    tokenizer->token_count = 0;
    return 0;
  }
  ++tokenizer->inputs;
  int cacheable = size <= TEXT_RECENT_BYTES;

  /* --- the very same text as the call before: its words are still lying here --- */
  if (cacheable && tokenizer->previous.size == size &&
      memcmp(tokenizer->previous.bytes, text, size) == 0) {
    ++tokenizer->repeated;
    if (!tokenizer->repetition_filter) return tokenizer->token_count;
    tokenizer->used = 0;
    tokenizer->token_count = 0;
    return 0;
  }

  tokenizer->used = 0;
  tokenizer->token_count = 0;

  /* --- the same text as one of the last few, while a vocabulary is gathered --- */
  if (cacheable && tokenizer->repetition_filter) {
    for (unsigned slot = 0; slot < TEXT_RECENT_SLOTS; ++slot) {
      const TextRecent *recent = &tokenizer->recent[slot];
      if (recent->size != size) continue;
      if (memcmp(recent->bytes, text, size) != 0) continue;
      ++tokenizer->repeated;
      return 0;
    }
  }

  /* --- the German reading, then the plain one if they differ --- */
  if (fold_pass(tokenizer, text, size, 1)) { fold_pass(tokenizer, text, size, 0); }

  if (cacheable) {
    tokenizer->previous.size = (uint8_t)size;
    memcpy(tokenizer->previous.bytes, text, size);
    if (tokenizer->repetition_filter) {
      TextRecent *recent = &tokenizer->recent[tokenizer->recent_next];
      recent->size = (uint8_t)size;
      memcpy(recent->bytes, text, size);
      tokenizer->recent_next = (tokenizer->recent_next + 1) % TEXT_RECENT_SLOTS;
    }
  }
  return tokenizer->token_count;
}

/** @endcond */

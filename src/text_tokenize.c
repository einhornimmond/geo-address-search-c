/** @cond INTERNAL */

#include "text_tokenize.h"

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

/** Outcome of folding one code point. */
typedef enum FoldKind {
  FOLD_SEPARATOR, /**< Ends the current word. */
  FOLD_TEXT       /**< Bytes were written. */
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
 *  @return FOLD_SEPARATOR when the code point ends a word.
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
static void token_add(TextTokenizer *tokenizer, const char *data, size_t size) {
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
static void word_finish(TextTokenizer *tokenizer, const char *word, size_t size) {
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

  token_add(tokenizer, word, size);

  /* --- a compound also names its parts --- */
  for (size_t i = 0; i < sizeof(COMPOUND_TAILS) / sizeof(COMPOUND_TAILS[0]); ++i) {
    size_t tail = COMPOUND_TAILS[i].size;
    if (size <= tail + COMPOUND_HEAD_MIN - 1) continue;
    if (memcmp(word + size - tail, COMPOUND_TAILS[i].word, tail) != 0) continue;
    token_add(tokenizer, word, size - tail);
    token_add(tokenizer, word + size - tail, tail);
    break;
  }
}

/** Fold the whole input once and cut it into words. */
static int fold_pass(TextTokenizer *tokenizer, const char *text, size_t size, int german) {
  int special = 0;
  size_t word_start = tokenizer->used;

  for (size_t pos = 0; pos < size;) {
    uint32_t code;
    pos += utf8_next(text, size, pos, &code);

    char folded[4];
    size_t written = 0;
    if (fold_code(code, german, folded, &written, &special) == FOLD_SEPARATOR || !written) {
      word_finish(tokenizer, tokenizer->buffer + word_start, tokenizer->used - word_start);
      word_start = tokenizer->used;
      continue;
    }
    if (tokenizer->used + written > TEXT_BUFFER_MAX) {
      ++tokenizer->dropped;
      break;
    }
    memcpy(tokenizer->buffer + tokenizer->used, folded, written);
    tokenizer->used += written;
  }
  word_finish(tokenizer, tokenizer->buffer + word_start, tokenizer->used - word_start);
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

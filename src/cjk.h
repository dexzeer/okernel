#ifndef CJK_H
#define CJK_H

#include <stdint.h>

// CJK ideograph/syllabary renderer (Wikipedia ???? census 2026-10-01).
// The single-byte slot table is full, so CJK travels the pipeline as raw
// UTF-8 bytes with a per-run flag (LAYOUT_FLAG_CJK): layout counts display
// columns per CHARACTER, and the blit decodes each char and draws from the
// bitmap table instead of the slot font. Table misses behave exactly like
// unmapped codepoints do today (caller renders '?').
static inline int cjk_is_passthrough(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Ext-A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified
           (cp >= 0xAC00 && cp <= 0xD7AF) ||   // Hangul syllables
           (cp >= 0x3040 && cp <= 0x30FF) ||   // Hiragana + Katakana
           (cp >= 0x20000 && cp <= 0x2FFFF);   // CJK Ext-B..F (astral)
}

// 16x16 MSB-first bitmap (32 bytes) for cp, or 0 when the table has no
// glyph for it. Freestanding-safe (binary search, no libc).
const uint8_t* cjk_glyph_for(uint32_t cp);

// ---- UTF-8 helpers for CJK runs (layout counts display columns per
// CHARACTER while the text arenas store bytes; all freestanding-safe) ----
static inline int utf8_seq_len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;
    return 0; // stray continuation / invalid lead
}

// Display columns in s[0..n): one per ASCII byte, one per valid multibyte
// sequence; a broken byte counts 1 (never 0 — progress guaranteed).
static inline int utf8_count_chars(const char* s, int n) {
    int c = 0;
    for (int i = 0; i < n;) {
        int L = utf8_seq_len((unsigned char)s[i]);
        if (L <= 1) { i++; c++; continue; }
        int ok = (i + L <= n);
        for (int q = 1; ok && q < L; q++)
            if (((unsigned char)s[i + q] & 0xC0) != 0x80) ok = 0;
        i += ok ? L : 1;
        c++;
    }
    return c;
}

// Byte length of the first `nchars` display columns (<= n, > 0 when
// nchars > 0 and n > 0).
static inline int utf8_bytes_of_first_n(const char* s, int n, int nchars) {
    int i = 0, c = 0;
    while (i < n && c < nchars) {
        int L = utf8_seq_len((unsigned char)s[i]);
        if (L <= 1) { i++; c++; continue; }
        int ok = (i + L <= n);
        for (int q = 1; ok && q < L; q++)
            if (((unsigned char)s[i + q] & 0xC0) != 0x80) ok = 0;
        i += ok ? L : 1;
        c++;
    }
    return i;
}

// Decode one char at s (<= n bytes left): codepoint + byte length.
// Invalid sequences decode as '?' consuming 1 byte (never 0).
static inline uint32_t utf8_decode_char(const char* s, int n, int* blen) {
    if (n <= 0) { if (blen) *blen = 0; return (uint32_t)'?'; }
    unsigned char b = (unsigned char)s[0];
    int L = utf8_seq_len(b);
    if (L <= 1) { if (blen) *blen = 1; return (uint32_t)b; }
    if (L > n) { if (blen) *blen = 1; return (uint32_t)'?'; }
    for (int q = 1; q < L; q++)
        if (((unsigned char)s[q] & 0xC0) != 0x80) {
            if (blen) *blen = 1;
            return (uint32_t)'?';
        }
    uint32_t cp;
    if (L == 2) cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)((unsigned char)s[1] & 0x3F);
    else if (L == 3) cp = ((uint32_t)(b & 0x0F) << 12) |
                          ((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
                          (uint32_t)((unsigned char)s[2] & 0x3F);
    else cp = ((uint32_t)(b & 0x07) << 18) |
              ((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
              ((uint32_t)((unsigned char)s[2] & 0x3F) << 6) |
              (uint32_t)((unsigned char)s[3] & 0x3F);
    if (blen) *blen = L;
    return cp;
}

// Encode cp as UTF-8 into out (caller provides >= 4 bytes); returns the
// byte count, 0 when out of range. Used for CJK numeric entities.
static inline int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

#endif

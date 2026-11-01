// Predefined CSS counter styles (css-counter-styles-3): the table data is in
// cstyles_data.h; this file implements the name lookup and the formatting
// systems (numeric, alphabetic, symbolic, additive, fixed, hebrew, ethiopic).

#include <stdint.h>
#include <string.h>
#include "cstyles_data.h"

static int cs_nsym(int i) { return cs_tab[i].nsym; }
static const char* cs_sym(int i, int k) { return cs_pool + cs_tab[i].sym0 + 0; }

// pool strings are NUL-terminated in order; symbol k of style i starts at
// sym0 and the k-th NUL walks forward
static const char* cs_pool_str(uint16_t off, int k) {
    const char* p = cs_pool + off;
    while (k-- > 0) p += strlen(p) + 1;
    return p;
}

static const char* cs_addsym(int i, int k, int32_t* val) {
    *val = cs_add[cs_tab[i].add0 + k].v;
    return cs_pool + cs_add[cs_tab[i].add0 + k].off;
}

int cstyle_find(const char* s, int len) {
    int lo = 0, hi = CS_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(cs_tab[mid].name, s, (size_t)len);
        if (c == 0) {
            if (cs_tab[mid].name[len] == 0) return mid;
            hi = mid - 1;         // the table name is longer: the target sorts before it
        } else if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// hebrew: additive with the 15/16 special forms; range 1..9999
static int hebrew_fmt(int v, char* out) {
    static const struct { int v; const char* s; } T[] = {
        {400, "\xD7\xAA"}, {300, "\xD7\xA9"}, {200, "\xD7\xA8"}, {100, "\xD7\xA7"},
        {90, "\xD7\xA6"}, {80, "\xD7\xA4"}, {70, "\xD7\xA2"}, {60, "\xD7\xA1"},
        {50, "\xD7\xA0"}, {40, "\xD7\x9E"}, {30, "\xD7\x9C"}, {20, "\xD7\x9B"},
        {10, "\xD7\x99"}, {9, "\xD7\x98"}, {8, "\xD7\x97"}, {7, "\xD7\x96"},
        {6, "\xD7\x95"}, {5, "\xD7\x94"}, {4, "\xD7\x93"}, {3, "\xD7\x92"},
        {2, "\xD7\x91"}, {1, "\xD7\x90"},
    };
    if (v < 1 || v > 9999) return 0;
    int o = 0;
    if (v == 15) { memcpy(out, "\xD7\x98\xD7\xB4\xD7\x95", 6); return 6; }   // ט״ו
    if (v == 16) { memcpy(out, "\xD7\x98\xD7\xB4\xD7\x96", 6); return 6; }   // ט״ז
    if (v >= 1000) {
        static const char* TH[] = { 0, "\xD7\x90", "\xD7\x91", "\xD7\x92", "\xD7\x93", "\xD7\x94", "\xD7\x95", "\xD7\x96", "\xD7\x97", "\xD7\x98" };
        const char* s = TH[v / 1000];
        while (*s) out[o++] = *s++;
        if ((v / 1000) >= 5) { out[o++] = '\''; }   // thousands apostrophe (geresh) for 5000+
        v %= 1000;
        if (v > 0 && v < 100) out[o++] = '\'';
    }
    for (unsigned k = 0; k < sizeof T / sizeof T[0]; k++)
        while (v >= T[k].v) {
            const char* s = T[k].s;
            while (*s) out[o++] = *s++;
            v -= T[k].v;
        }
    return o;
}

// ethiopic-numeric: 2-digit groups, separators ፻ (x100) and ፼ (x10000)
static int ethiopic_fmt(int v, char* out) {
    static const char* ONES[] = { 0, "\xE1\x8D\xA9", "\xE1\x8D\xAA", "\xE1\x8D\xAB", "\xE1\x8D\xAC", "\xE1\x8D\xAD",
                                  "\xE1\x8D\xAE", "\xE1\x8D\xAF", "\xE1\x8D\xB0", "\xE1\x8D\xB1" };
    static const char* TENS[] = { 0, "\xE1\x8D\xB2", "\xE1\x8D\xB3", "\xE1\x8D\xB4", "\xE1\x8D\xB5", "\xE1\x8D\xB6",
                                  "\xE1\x8D\xB7", "\xE1\x8D\xB8", "\xE1\x8D\xB9", "\xE1\x8D\xBA" };
    static const char* HUND = "\xE1\x8D\xBB";   // ፻
    static const char* TTM = "\xE1\x8D\xBC";    // ፼
    if (v < 1) return 0;
    int groups[8], ng = 0, t = v;
    while (t > 0) { groups[ng++] = t % 100; t /= 100; }
    int o = 0;
    for (int g = ng - 1; g >= 0; g--) {
        int tens = groups[g] / 10, ones = groups[g] % 10;
        int omit = (g == ng - 1) && groups[g] == 1 && ng > 1;   // a leading 1 group renders nothing
        if (!omit) {
            if (tens > 0) { const char* s = TENS[tens]; while (*s) out[o++] = *s++; }
            if (ones > 0) { const char* s = ONES[ones]; while (*s) out[o++] = *s++; }
        }
        if (g > 0) {
            const char* sep = ((ng - 1 - g) % 2) ? TTM : HUND;  // x100, x10000, ...
            while (*sep) out[o++] = *sep++;
        }
    }
    return o;
}

int cstyle_format(int i, int value, char* out) {
    // the callers give a small buffer (80): hard-cap every write path
    int cap = 60;
    int o = 0;
    switch (cs_tab[i].system) {
    case CS_NUMERIC: {
        if (value < 0) return 0;
        int n = cs_nsym(i);
        if (n < 2) return 0;
        char tmp[24]; int t = 0, v = value;
        do { const char* s = cs_pool_str(cs_tab[i].sym0, v % n); int l = (int)strlen(s); while (l--) tmp[t++] = s[l]; v /= n; } while (v && t < 20);
        while (t < cs_tab[i].pad_n && t < 20) { const char* s = cs_pool + cs_tab[i].pad_c; int l = (int)strlen(s); while (l--) tmp[t++] = s[l]; }
        while (t) out[o++] = tmp[--t];
        break;
    }
    case CS_ALPHABETIC: {
        if (value < 1) return 0;
        int n = cs_nsym(i);
        if (n < 1) return 0;
        char tmp[24]; int t = 0, v = value;
        while (v > 0 && t < 20) {
            v--;
            const char* s = cs_pool_str(cs_tab[i].sym0, v % n);
            int l = (int)strlen(s);
            while (l--) tmp[t++] = s[l];
            v /= n;
        }
        while (t) out[o++] = tmp[--t];
        break;
    }
    case CS_SYMBOLIC: {
        if (value < 1) return 0;
        int n = cs_nsym(i);
        const char* s = cs_pool_str(cs_tab[i].sym0, (value - 1) % n);
        int l = (int)strlen(s);
        for (int k = 0; k < value && o < cap; k++)
            for (int q = 0; q < l; q++) out[o++] = s[q];
        break;
    }
    case CS_ADDITIVE: {
        if (value < 0) return 0;
        if (value == 0) {
            // the zero symbol, if the list defines one
            for (int k = 0; k < cs_tab[i].nadd; k++) {
                int32_t v; const char* s = cs_addsym(i, k, &v);
                if (v == 0) { while (*s) out[o++] = *s++; return o; }
            }
            return 0;
        }
        for (int k = 0; k < cs_tab[i].nadd && value > 0; k++) {
            int32_t v; const char* s = cs_addsym(i, k, &v);
            while (value >= v && v > 0) {
                const char* q = s;
                while (*q && o < cap) out[o++] = *q++;
                value -= v;
            }
            if (o >= cap) return o;
        }
        break;
    }
    case CS_FIXED: {
        if (value < 1 || value > cs_nsym(i)) return 0;
        const char* s = cs_pool_str(cs_tab[i].sym0, value - 1);
        while (*s) out[o++] = *s++;
        break;
    }
    case CS_HEBREW:
        o = hebrew_fmt(value, out);
        break;
    case CS_CJK: {
        // the complex CJK systems: digit + power per position, a single zero
        // for consecutive zeros, and 一 is omitted before a power
        if (value < 0 || value > 99999999) return 0;
        int32_t pw[5] = { 10000, 1000, 100, 10, 1 };
        int started = 0, zero_pending = 0;
        for (int pi = 0; pi < 5; pi++) {
            int d = (value / pw[pi]) % 10;
            if (d == 0) {
                if (started) zero_pending = 1;
                continue;
            }
            for (int k = 0; k < cs_tab[i].nadd && o < cap; k++) {
                int32_t v; const char* s = cs_addsym(i, k, &v);
                if (v == d && !(d == 1 && pw[pi] != 1)) while (*s && o < cap) out[o++] = *s++;
            }
            if (pw[pi] != 1) {
                for (int k = 0; k < cs_tab[i].nadd && o < cap; k++) {
                    int32_t v; const char* s = cs_addsym(i, k, &v);
                    if (v == pw[pi]) while (*s && o < cap) out[o++] = *s++;
                }
            }
            started = 1;
            zero_pending = 0;
        }
        if (!started && zero_pending) { }
        break;
    }
    case CS_ETHIOPIC:
        o = ethiopic_fmt(value, out);
        break;
    }
    return o;
}

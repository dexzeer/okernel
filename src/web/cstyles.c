// Predefined CSS counter styles (css-counter-styles-3): the table data is in
// cstyles_data.h; this file implements the name lookup and the formatting
// systems (numeric, alphabetic, symbolic, additive, fixed, cyclic, hebrew,
// ethiopic) plus dynamic @counter-style rules (cstyles.h).

#include <stdint.h>
#include <string.h>
#include "wcommon.h"
#include "cstyles_data.h"
#include "cstyles.h"

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
        if (value < 0) {   // additive systems render negatives with a bare minus
            int n = cstyle_format(i, -value, out + 1);
            if (!n) return 0;
            out[0] = '-';
            return n + 1;
        }
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

// ---- dynamic @counter-style ------------------------------------------------

// Symbol/pair accessors: one set of core formatters serves the predefined
// table, a dynamic style and an extends-merge view of both.
struct csacc {
    int system;
    int nsym, nadd;
    int fixed_start;                 // CS_FIXED first symbol's value
    const char* (*symk)(const void* c, int k);
    int32_t (*addv)(const void* c, int k);
    const char* (*adds)(const void* c, int k);
    int pad_n;                       // 0 = no pad
    const char* pad_s;
    const void* c;
};

static const char* acc_dsym(const void* c, int k) {
    const struct cstyle_dyn* d = c;
    return d->pool + d->sym_off[k];
}
static int32_t acc_daddv(const void* c, int k) { return ((const struct cstyle_dyn*)c)->add[k].v; }
static const char* acc_dadds(const void* c, int k) {
    const struct cstyle_dyn* d = c;
    return d->pool + d->add[k].off;
}
static const char* acc_ssym(const void* c, int k) {
    return cs_pool_str(cs_tab[*(const int*)c].sym0, k);
}
static const char* acc_decsym(const void* c, int k) {
    (void)c;
    static const char* const D[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
    return D[k];
}
// decimal (not in the predefined table): plain numeric 0-9
static void acc_dec(struct csacc* a) {
    memset(a, 0, sizeof(*a));
    a->system = CS_NUMERIC;
    a->nsym = 10;
    a->symk = acc_decsym;
    a->c = 0;
}
static int32_t acc_saddv(const void* c, int k) {
    return cs_add[cs_tab[*(const int*)c].add0 + k].v;
}
static const char* acc_sadds(const void* c, int k) {
    return cs_pool + cs_add[cs_tab[*(const int*)c].add0 + k].off;
}

// extends-merge view: the overriding style's fields fall back to the base's
struct csmerge {
    const struct cstyle_dyn* d;
    const struct csacc* b;
};
static const char* acc_msym(const void* c, int k) {
    const struct csmerge* m = c;
    return m->d->nsym ? m->d->pool + m->d->sym_off[k] : m->b->symk(m->b->c, k);
}
static int32_t acc_maddv(const void* c, int k) {
    const struct csmerge* m = c;
    return m->d->nadd ? m->d->add[k].v : m->b->addv(m->b->c, k);
}
static const char* acc_madds(const void* c, int k) {
    const struct csmerge* m = c;
    return m->d->nadd ? m->d->pool + m->d->add[k].off : m->b->adds(m->b->c, k);
}

static int out_str(const char* s, char* out, int* o, int cap) {
    while (*s) {
        if (*o >= cap) return 0;
        out[(*o)++] = *s++;
    }
    return 1;
}

// Core value formatting for the mechanical systems (abs value; the caller
// handles negatives and the prefix/suffix/negative wrapping). NUL-terminates.
static int fmt_core(const struct csacc* a, int value, char* out, int cap) {
    int o = 0;
    int v = value < 0 ? -value : value;
    switch (a->system) {
    case CS_NUMERIC: {
        if (a->nsym < 2) return 0;
        char tmp[192];
        int t = 0, x = v;
        do {
            if (!out_str(a->symk(a->c, x % a->nsym), tmp, &t, 180)) return 0;
            x /= a->nsym;
        } while (x && t < 170);
        while (t) {
            if (o >= cap - 1) return 0;
            out[o++] = tmp[--t];
        }
        break;
    }
    case CS_ALPHABETIC: {
        if (a->nsym < 1) return 0;
        char tmp[192];
        int t = 0, x = v;
        while (x > 0 && t < 180) {
            x--;
            if (!out_str(a->symk(a->c, x % a->nsym), tmp, &t, 180)) return 0;
            x /= a->nsym;
        }
        if (x > 0) return 0;
        while (t) {
            if (o >= cap - 1) return 0;
            out[o++] = tmp[--t];
        }
        break;
    }
    case CS_SYMBOLIC: {
        if (a->nsym < 1 || value < 1) return 0;
        int k = (value - 1) % a->nsym;
        int rep = (value - 1) / a->nsym + 1;
        const char* s = a->symk(a->c, k);
        if ((int)strlen(s) * rep > cap - 1) return 0;
        for (int r = 0; r < rep; r++) if (!out_str(s, out, &o, cap - 1)) return 0;
        break;
    }
    case CS_CYCLIC: {
        if (a->nsym < 1 || value < 1) return 0;
        if (!out_str(a->symk(a->c, (value - 1) % a->nsym), out, &o, cap - 1)) return 0;
        break;
    }
    case CS_FIXED: {
        int fs = a->fixed_start ? a->fixed_start : 1;
        if (value < fs || value >= fs + a->nsym) return 0;
        if (!out_str(a->symk(a->c, value - fs), out, &o, cap - 1)) return 0;
        break;
    }
    case CS_ADDITIVE: {
        if (value == 0) {
            for (int k = 0; k < a->nadd; k++)
                if (a->addv(a->c, k) == 0) return out_str(a->adds(a->c, k), out, &o, cap - 1) ? o : 0;
            return 0;
        }
        for (int k = 0; k < a->nadd && v > 0; k++) {
            int32_t pv = a->addv(a->c, k);
            if (pv <= 0) continue;
            const char* s = a->adds(a->c, k);
            while (v >= pv) {
                if (!out_str(s, out, &o, cap - 1)) return 0;
                v -= pv;
            }
        }
        break;
    }
    default:
        return 0;   // hebrew/ethiopic/CJK have no dynamic form here
    }
    out[o] = 0;
    return o;
}

struct cstyle_dyn* cstyle_dyn_new(const char* name, int len) {
    if (len <= 0 || len > 120) return 0;
    struct cstyle_dyn* d = (struct cstyle_dyn*)w_malloc(sizeof(*d));
    if (!d) return 0;
    memset(d, 0, sizeof(*d));
    d->name = (char*)w_malloc((size_t)len + 1);
    if (!d->name) { w_free(d); return 0; }
    memcpy(d->name, name, (size_t)len);
    d->name[len] = 0;
    d->prefix_off = 0xFFFF;
    d->suffix_off = 0xFFFF;
    d->pad_off = 0xFFFF;
    d->neg_off = 0xFFFF;
    return d;
}

void cstyle_dyn_free(struct cstyle_dyn* d) {
    if (!d) return;
    w_free(d->name);
    w_free(d->pool);
    w_free(d->fallback);
    w_free(d->extends_nm);
    w_free(d);
}

uint16_t cstyle_dyn_put(struct cstyle_dyn* d, const char* s, int len) {
    if (len < 0) len = (int)strlen(s);
    if (d->pool_len + len + 1 > 60000) return 0xFFFF;
    if (d->pool_len + len + 1 > d->pool_cap) {
        int nc = d->pool_cap ? d->pool_cap * 2 : 128;
        while (nc < d->pool_len + len + 1) nc *= 2;
        char* np = (char*)w_malloc((size_t)nc);
        if (!np) return 0xFFFF;
        if (d->pool) { memcpy(np, d->pool, (size_t)d->pool_len); w_free(d->pool); }
        d->pool = np;
        d->pool_cap = nc;
    }
    uint16_t off = (uint16_t)d->pool_len;
    memcpy(d->pool + d->pool_len, s, (size_t)len);
    d->pool_len += len;
    d->pool[d->pool_len++] = 0;
    return off;
}

struct cstyle_dyn* cstyle_dyn_find(struct cstyle_dyn** list, int n, const char* s, int len, int* out_at) {
    for (int i = n - 1; i >= 0; i--)   // later rules override earlier
        if ((int)strlen(list[i]->name) == len && !memcmp(list[i]->name, s, (size_t)len)) {
            if (out_at) *out_at = i;
            return list[i];
        }
    return 0;
}

static void acc_of_dyn(struct csacc* a, const struct cstyle_dyn* d) {
    memset(a, 0, sizeof(*a));
    a->system = d->system;
    a->nsym = d->nsym;
    a->nadd = d->nadd;
    a->fixed_start = d->fixed_start;
    a->symk = acc_dsym;
    a->addv = acc_daddv;
    a->adds = acc_dadds;
    a->pad_n = d->has_pad ? d->pad_n : 0;
    a->pad_s = d->has_pad ? d->pool + d->pad_off : 0;
    a->c = d;
}

static void acc_of_static(struct csacc* a, int idx) {
    memset(a, 0, sizeof(*a));
    a->system = cs_tab[idx].system;
    a->nsym = cs_tab[idx].nsym;
    a->nadd = cs_tab[idx].nadd;
    a->fixed_start = 1;
    a->symk = acc_ssym;
    a->addv = acc_saddv;
    a->adds = acc_sadds;
    a->pad_n = cs_tab[idx].pad_n;
    a->pad_s = cs_tab[idx].pad_c >= 0 ? cs_pool + cs_tab[idx].pad_c : 0;
    // a->c must be set by the caller to a stable int holding the table index
    // (this function's parameter dies on return)
}

// Per-system default range; the explicit range descriptor overrides.
static void sys_range(int system, int nsym, int fixed_start, int* rmin, int* rmax) {
    *rmin = 1;
    *rmax = 1 << 30;
    switch (system) {
    case CS_NUMERIC: *rmin = -(1 << 30); break;
    case CS_ADDITIVE: *rmin = 0; break;
    case CS_FIXED:
        *rmin = fixed_start ? fixed_start : 1;
        *rmax = *rmin + (nsym > 0 ? nsym : 0) - 1;
        break;
    }
}

// The style's effective view: d's descriptors win over its extends base's.
// Returns the byte length, 0 = cannot format (caller falls back).
int cstyle_dyn_format(const struct cstyle_dyn* d, int value, char* out, int cap,
                      void* rctx,
                      void (*resolve)(void* rctx, const char* s, int len,
                                      const struct cstyle_dyn** dyn_out, int* idx_out),
                      int depth) {
    if (cap <= 4) return 0;
    int o = 0;
    const struct cstyle_dyn* bd = 0;
    int bi = -1;
    if (d->system == CS_DYN_EXTENDS) {
        if (depth > 4 || !d->extends_nm) return 0;
        resolve(rctx, d->extends_nm, (int)strlen(d->extends_nm), &bd, &bi);
        if (bd && bd->system == CS_DYN_EXTENDS)
            return cstyle_dyn_format(bd, value, out, cap, rctx, resolve, depth + 1);
    }
    struct csacc a;
    // "decimal" is not in the predefined table: synthesize it
    int is_dec = (!bd && bi < 0 && d->system == CS_DYN_EXTENDS &&
                  d->extends_nm && !strcmp(d->extends_nm, "decimal"));
    if (!bd && bi < 0 && !is_dec) return 0;
    if (bd) acc_of_dyn(&a, bd);
    else if (bi >= 0) { acc_of_static(&a, bi); a.c = &bi; }   // bi lives through the call
    else if (is_dec) { acc_dec(&a); }
    else acc_of_dyn(&a, d);

    struct csmerge m;
    if (is_dec || bd || bi >= 0) {
        m.d = d;
        m.b = &a;
        if (d->nsym && a.system != CS_ADDITIVE) { a.symk = acc_msym; a.nsym = d->nsym; a.c = &m; }
        if (d->nadd && a.system == CS_ADDITIVE) { a.addv = acc_maddv; a.adds = acc_madds; a.nadd = d->nadd; a.c = &m; }
        if (d->has_pad) { a.pad_n = d->pad_n; a.pad_s = d->pool + d->pad_off; }
    }

    int rmin, rmax;
    if (d->has_range) { rmin = d->rmin; rmax = d->rmax; }
    else if (is_dec) sys_range(CS_NUMERIC, 10, 1, &rmin, &rmax);
    else if (bd) sys_range(bd->system, a.nsym, a.fixed_start, &rmin, &rmax);
    else if (bi >= 0) sys_range(cs_tab[bi].system, a.nsym, a.fixed_start, &rmin, &rmax);
    else sys_range(d->system, a.nsym, a.fixed_start, &rmin, &rmax);
    if (value < rmin || value > rmax) return 0;

    // negative: only the numeric system renders negatives (descriptor wraps)
    const char* np = 0;
    const char* ns = 0;
    const struct cstyle_dyn* nd = 0;
    if (d->neg_off != 0xFFFF) nd = d;
    else if (bd && bd->neg_off != 0xFFFF) nd = bd;
    if (nd) {
        np = nd->pool + nd->neg_off;
        ns = np + nd->neg_pre_len + 1;   // past the NUL that ends the prefix string
    }
    if (value < 0) {
        if (a.system != CS_NUMERIC && a.system != CS_ADDITIVE) return 0;
        if (!np) { np = "-"; ns = ""; }   // additive systems render negatives with a bare minus
        int n = fmt_core(&a, -value, out, cap);
        if (!n) return 0;
        int pl = (int)strlen(np), sl = (int)strlen(ns);
        if (n + pl + sl >= cap) return 0;
        memmove(out + pl, out, (size_t)n);
        memcpy(out, np, (size_t)pl);
        memcpy(out + pl + n, ns, (size_t)sl);
        o = n + pl + sl;
    } else {
        o = fmt_core(&a, value, out, cap);
        if (!o) return 0;
    }
    // pad: the final string (sign included) is at least pad_n characters
    if (a.pad_n > 0 && a.pad_s) {
        int chars = 0;
        for (int k = 0; k < o; k++) if ((out[k] & 0xC0) != 0x80) chars++;
        while (chars < a.pad_n) {
            int pl = 0;
            char ps[24];
            { const char* s = a.pad_s; while (*s && pl < 22) ps[pl++] = *s++; }
            if (!pl || o + pl >= cap) return 0;
            memmove(out + pl, out, (size_t)o);
            memcpy(out, ps, (size_t)pl);
            o += pl;
            chars++;
        }
    }
    return o;
}

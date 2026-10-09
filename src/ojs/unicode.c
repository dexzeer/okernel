// unicode.c — lookups over the generated UCD tables, and Unicode
// normalization (UAX #15: canonical/compatibility decomposition, canonical
// ordering, canonical composition; Hangul syllables algorithmically).

#include "unicode.h"
#include "ojs_int.h"

int uni_in(const struct urange_set* s, uint32_t cp) {
    int lo = 0, hi = s->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (cp < s->r[2 * m]) hi = m - 1;
        else if (cp > s->r[2 * m + 1]) lo = m + 1;
        else return 1;
    }
    return 0;
}

int uni_id_start_cp(uint32_t cp) {
    if (cp < 128) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '$' || cp == '_';
    return uni_in(&uni_id_start, cp);
}

int uni_id_continue_cp(uint32_t cp) {
    if (cp < 128)
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '$' || cp == '_';
    if (cp == 0x200C || cp == 0x200D) return 1;
    return uni_in(&uni_id_continue, cp);
}

static int pair_find(const struct upairs* t, uint32_t key) {
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t k = t->p[2 * m];
        if (key < k) hi = m - 1;
        else if (key > k) lo = m + 1;
        else return m;
    }
    return -1;
}

uint32_t uni_simple_upper(uint32_t cp) {
    if (cp < 128) return (cp >= 'a' && cp <= 'z') ? cp - 32 : cp;
    int i = pair_find(&uni_upper, cp);
    return i >= 0 ? uni_upper.p[2 * i + 1] : cp;
}

uint32_t uni_simple_lower(uint32_t cp) {
    if (cp < 128) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    int i = pair_find(&uni_lower, cp);
    return i >= 0 ? uni_lower.p[2 * i + 1] : cp;
}

uint32_t uni_simple_fold(uint32_t cp) {
    if (cp < 128) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    int i = pair_find(&uni_fold, cp);
    return i >= 0 ? uni_fold.p[2 * i + 1] : cp;
}

static int multi_find(const struct umulti* t, uint32_t cp) {
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t k = t->p[5 * m];
        if (cp < k) hi = m - 1;
        else if (cp > k) lo = m + 1;
        else return m;
    }
    return -1;
}

int uni_full_upper(uint32_t cp, uint32_t* out) {
    int i = cp >= 128 ? multi_find(&uni_special_upper, cp) : -1;
    if (i >= 0) {
        int n = (int)uni_special_upper.p[5 * i + 1];
        for (int k = 0; k < n; k++) out[k] = uni_special_upper.p[5 * i + 2 + k];
        return n;
    }
    out[0] = uni_simple_upper(cp);
    return 1;
}

int uni_full_lower(uint32_t cp, uint32_t* out) {
    int i = cp >= 128 ? multi_find(&uni_special_lower, cp) : -1;
    if (i >= 0) {
        int n = (int)uni_special_lower.p[5 * i + 1];
        for (int k = 0; k < n; k++) out[k] = uni_special_lower.p[5 * i + 2 + k];
        return n;
    }
    out[0] = uni_simple_lower(cp);
    return 1;
}

int uni_ccc_of(uint32_t cp) {
    if (cp < 0x300) return 0;
    int i = pair_find(&uni_ccc, cp);
    return i >= 0 ? (int)uni_ccc.p[2 * i + 1] : 0;
}

int uni_is_white_space(uint32_t cp) {
    return cp == 9 || cp == 11 || cp == 12 || cp == 32 || cp == 0xA0 || cp == 0xFEFF || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// "Long|alias|alias" contains name exactly?
static int name_match(const char* names, const char* name) {
    size_t n = strlen(name);
    const char* p = names;
    for (;;) {
        const char* bar = p;
        while (*bar && *bar != '|') bar++;
        if ((size_t)(bar - p) == n && !memcmp(p, name, n)) return 1;
        if (!*bar) return 0;
        p = bar + 1;
    }
}

const struct urange_set* uni_find_prop(int kind, const char* name, int* is_cn) {
    *is_cn = 0;
    const struct uprop* t = kind == 0 ? uni_gc : kind == 1 ? uni_binary : kind == 2 ? uni_sc : uni_scx;
    if (kind == 0 && (!strcmp(name, "Cn") || !strcmp(name, "Unassigned"))) { *is_cn = 1; return 0; }
    if (kind == 0 && (!strcmp(name, "C") || !strcmp(name, "Other"))) *is_cn = 2;   // C = Cc|Cf|Cs|Co + Cn
    for (int i = 0; t[i].names; i++)
        if (name_match(t[i].names, name)) return &t[i].set;
    return 0;
}

// ---------------------------------------------------------------- normalization

#define SBASE 0xAC00
#define LBASE 0x1100
#define VBASE 0x1161
#define TBASE 0x11A7
#define LCOUNT 19
#define VCOUNT 21
#define TCOUNT 28
#define NCOUNT (VCOUNT * TCOUNT)
#define SCOUNT (LCOUNT * NCOUNT)

static int decomp_find(uint32_t cp) {
    int lo = 0, hi = uni_decomp.n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t k = uni_decomp.e[3 * m];
        if (cp < k) hi = m - 1;
        else if (cp > k) lo = m + 1;
        else return m;
    }
    return -1;
}

struct cpbuf { uint32_t* v; uint32_t n, cap; int oom; };
static void cb_push(struct cpbuf* b, uint32_t cp) {
    if (b->oom) return;
    if (b->n >= b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 64;
        uint32_t* t = (uint32_t*)ojs_sys_realloc(b->v, (size_t)nc * 4);
        if (!t) { b->oom = 1; return; }
        b->v = t;
        b->cap = nc;
    }
    b->v[b->n++] = cp;
}

static void decompose(uint32_t cp, int compat, struct cpbuf* out) {
    if (cp >= SBASE && cp < SBASE + SCOUNT) {
        uint32_t s = cp - SBASE;
        cb_push(out, LBASE + s / NCOUNT);
        cb_push(out, VBASE + (s % NCOUNT) / TCOUNT);
        if (s % TCOUNT) cb_push(out, TBASE + s % TCOUNT);
        return;
    }
    int i = cp >= 0xC0 ? decomp_find(cp) : -1;
    if (i >= 0) {
        uint32_t fl = uni_decomp.e[3 * i + 1];
        if (!(fl >> 7) || compat) {
            uint32_t len = fl & 0x7F, off = uni_decomp.e[3 * i + 2];
            for (uint32_t k = 0; k < len; k++) decompose(uni_decomp.seq[off + k], compat, out);
            return;
        }
    }
    cb_push(out, cp);
}

static uint32_t compose_pair(uint32_t a, uint32_t b) {
    // Hangul LV / LVT
    if (a >= LBASE && a < LBASE + LCOUNT && b >= VBASE && b < VBASE + VCOUNT)
        return SBASE + ((a - LBASE) * VCOUNT + (b - VBASE)) * TCOUNT;
    if (a >= SBASE && a < SBASE + SCOUNT && (a - SBASE) % TCOUNT == 0 && b > TBASE && b < TBASE + TCOUNT)
        return a + (b - TBASE);
    int lo = 0, hi = uni_compose.n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t x = uni_compose.p[3 * m], y = uni_compose.p[3 * m + 1];
        if (a < x || (a == x && b < y)) hi = m - 1;
        else if (a > x || (a == x && b > y)) lo = m + 1;
        else return uni_compose.p[3 * m + 2];
    }
    return 0;
}

// form: 0 NFC, 1 NFD, 2 NFKC, 3 NFKD. in/out: code points (out sys-malloc'd)
int uni_normalize(const uint32_t* in, uint32_t n, int form, uint32_t** out, uint32_t* outn) {
    int compat = form >= 2, comp = form == 0 || form == 2;
    struct cpbuf b = {0, 0, 0, 0};
    for (uint32_t i = 0; i < n; i++) decompose(in[i], compat, &b);
    if (b.oom) { ojs_sys_free(b.v); return -1; }
    // canonical ordering: stable sort of runs of non-starters by ccc
    for (uint32_t i = 1; i < b.n; i++) {
        int c = uni_ccc_of(b.v[i]);
        if (!c) continue;
        uint32_t j = i, x = b.v[i];
        while (j > 0 && uni_ccc_of(b.v[j - 1]) > c) { b.v[j] = b.v[j - 1]; j--; }
        b.v[j] = x;
    }
    if (comp && b.n) {
        uint32_t starter = 0, w = 1;
        int last_cc = uni_ccc_of(b.v[0]) ? 256 : 0;   // a leading non-starter blocks composition
        if (last_cc) starter = (uint32_t)-1;
        for (uint32_t i = 1; i < b.n; i++) {
            uint32_t c = b.v[i];
            int cc = uni_ccc_of(c);
            uint32_t comp_cp = (starter != (uint32_t)-1 && (last_cc < cc || last_cc == 0)) ? compose_pair(b.v[starter], c) : 0;
            if (comp_cp && !(last_cc == 0 && w - 1 != starter)) {
                b.v[starter] = comp_cp;
                continue;
            }
            if (cc == 0) { starter = w; last_cc = 0; }
            else last_cc = cc;
            b.v[w++] = c;
        }
        b.n = w;
    }
    *out = b.v;
    *outn = b.n;
    return 0;
}

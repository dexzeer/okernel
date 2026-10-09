// re.c — regular expressions (ECMA-262 §22.2): pattern parser (with the
// Annex B grammar outside unicode mode), compiler and backtracking matcher.
//
// Pattern -> tree of rnodes -> code (uint32 words) -> matched by a loop
// that keeps its choice points and undo records ("trail") on an explicit
// stack, so deep backtracking never touches the C stack. Lookbehind
// bodies are compiled to match right-to-left. Lookarounds are atomic:
// they run as a nested match whose choice points are dropped on success.

#include "re.h"
#include "gc_int.h"
#include "unicode.h"

#define MAXCP 0x10FFFF

// ---------------------------------------------------------------- code point sets

struct cset {
    uint32_t* r;            // sorted, merged [lo, hi] pairs
    int n, cap;
    struct cstr* strs;      // v-mode class strings (length != 1)
};
struct cstr { uint32_t* cp; int n; struct cstr* next; };

struct arena { struct arena* next; size_t used, cap; char data[]; };

struct rp {
    ojs* J;
    const uint32_t* s;      // pattern code points (units outside unicode mode)
    uint32_t n, i;
    int flags;              // RF_*
    int unicode;            // u or v
    int vmode;
    int named;              // the pattern has named groups (\k is special)
    int ncaps;              // capturing groups seen (group 0 not counted)
    int total_caps;         // from a pre-scan (for \N decisions)
    char* err;
    int errcap;
    int failed;
    struct arena* arena;
    char** names;           // per capture group (index 1..), UTF-8 or NULL
    int names_cap;
    int dup_names;
    int depth;
    // pending named backrefs to resolve after parsing
    struct rnode* backrefs;
};

static void* aalloc(struct rp* p, size_t n) {
    n = (n + 7) & ~(size_t)7;
    struct arena* a = p->arena;
    if (!a || a->used + n > a->cap) {
        size_t cap = n > 16384 ? n : 16384;
        a = (struct arena*)ojs_sys_malloc(sizeof(struct arena) + cap);
        if (!a) { p->failed = 1; ojs_snprintf(p->err, p->errcap, "out of memory"); return 0; }
        a->next = p->arena;
        a->used = 0;
        a->cap = cap;
        p->arena = a;
    }
    void* r = a->data + a->used;
    a->used += n;
    memset(r, 0, n);
    return r;
}

static void fail(struct rp* p, const char* msg) {
    if (p->failed) return;
    p->failed = 1;
    ojs_snprintf(p->err, p->errcap, "Invalid regular expression: %s", msg);
}

static int cs_add(struct rp* p, struct cset* c, uint32_t lo, uint32_t hi) {
    if (lo > hi) return 0;
    if (c->n * 2 + 2 > c->cap) {
        int nc = c->cap ? c->cap * 2 : 16;
        uint32_t* r = (uint32_t*)aalloc(p, (size_t)nc * 4);
        if (!r) return -1;
        if (c->n) memcpy(r, c->r, (size_t)c->n * 8);
        c->r = r;
        c->cap = nc;
    }
    // insert keeping order, then merge
    int i = c->n;
    while (i > 0 && c->r[2 * (i - 1)] > lo) {
        c->r[2 * i] = c->r[2 * (i - 1)];
        c->r[2 * i + 1] = c->r[2 * (i - 1) + 1];
        i--;
    }
    c->r[2 * i] = lo;
    c->r[2 * i + 1] = hi;
    c->n++;
    int w = 0;
    for (int k = 1; k < c->n; k++) {
        if (c->r[2 * k] <= c->r[2 * w + 1] + 1 && c->r[2 * w + 1] != 0xFFFFFFFFu) {
            if (c->r[2 * k + 1] > c->r[2 * w + 1]) c->r[2 * w + 1] = c->r[2 * k + 1];
        } else {
            w++;
            c->r[2 * w] = c->r[2 * k];
            c->r[2 * w + 1] = c->r[2 * k + 1];
        }
    }
    c->n = w + 1;
    return 0;
}

static int cs_add_set(struct rp* p, struct cset* c, const struct cset* o) {
    for (int i = 0; i < o->n; i++) if (cs_add(p, c, o->r[2 * i], o->r[2 * i + 1]) < 0) return -1;
    for (struct cstr* s = o->strs; s; s = s->next) {
        struct cstr* t = (struct cstr*)aalloc(p, sizeof *t);
        if (!t) return -1;
        *t = *s;
        t->next = c->strs;
        c->strs = t;
    }
    return 0;
}

static int cs_has(const struct cset* c, uint32_t x) {
    int lo = 0, hi = c->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (x < c->r[2 * m]) hi = m - 1;
        else if (x > c->r[2 * m + 1]) lo = m + 1;
        else return 1;
    }
    return 0;
}

static int cs_invert(struct rp* p, struct cset* c, uint32_t maxv) {
    struct cset o = *c;
    c->r = 0; c->n = 0; c->cap = 0;
    uint32_t next = 0;
    for (int i = 0; i < o.n; i++) {
        if (o.r[2 * i] > next && cs_add(p, c, next, o.r[2 * i] - 1) < 0) return -1;
        next = o.r[2 * i + 1] + 1;
    }
    if (next <= maxv && cs_add(p, c, next, maxv) < 0) return -1;
    return 0;
}

static int cs_add_urange(struct rp* p, struct cset* c, const struct urange_set* u) {
    for (int i = 0; i < u->n; i++) if (cs_add(p, c, u->r[2 * i], u->r[2 * i + 1]) < 0) return -1;
    return 0;
}

static int cs_intersect(struct rp* p, struct cset* a, const struct cset* b) {
    struct cset o = *a;
    a->r = 0; a->n = 0; a->cap = 0;
    for (int i = 0; i < o.n; i++)
        for (int j = 0; j < b->n; j++) {
            uint32_t lo = o.r[2 * i] > b->r[2 * j] ? o.r[2 * i] : b->r[2 * j];
            uint32_t hi = o.r[2 * i + 1] < b->r[2 * j + 1] ? o.r[2 * i + 1] : b->r[2 * j + 1];
            if (lo <= hi && cs_add(p, a, lo, hi) < 0) return -1;
        }
    return 0;
}

static int cs_subtract(struct rp* p, struct cset* a, const struct cset* b) {
    struct cset nb;
    memset(&nb, 0, sizeof nb);
    if (cs_add_set(p, &nb, b) < 0 || cs_invert(p, &nb, MAXCP) < 0) return -1;
    nb.strs = 0;
    return cs_intersect(p, a, &nb);
}

// ---------------------------------------------------------------- case canonicalization

static uint32_t canon_u(uint32_t c) { return uni_simple_fold(c); }

static uint32_t canon_nu(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c > 0xFFFF) return c;
    uint32_t out[3];
    int k = uni_full_upper(c, out);
    if (k != 1) return c;
    if (out[0] < 128 || out[0] > 0xFFFF) return c;
    return out[0];
}

static uint32_t canon(int unicode, uint32_t c) { return unicode ? canon_u(c) : canon_nu(c); }

// S' = S ∪ { canon(x) : x in S } (matching tests canon(ch) in S')
static int cs_canonicalize(struct rp* p, struct cset* c, int unicode) {
    const struct upairs* t = unicode ? &uni_fold : &uni_upper;
    struct cset add;
    memset(&add, 0, sizeof add);
    for (int i = 0; i < t->n; i++) {
        uint32_t x = t->p[2 * i];
        if (!cs_has(c, x)) continue;
        uint32_t y = canon(unicode, x);
        if (y != x && cs_add(p, &add, y, y) < 0) return -1;
    }
    // non-unicode: lowercase letters whose uppercase is not in the table direction
    return cs_add_set(p, c, &add);
}

// CharacterComplement. With /v and /i the universe is the characters that are their
// own simple case folding and the operand is folded first: \P{Lower} then matches no
// letter at all (/u keeps the older code-point complement).
static int cs_complement(struct rp* p, struct cset* c) {
    if (!(p->vmode && (p->flags & RF_I))) return cs_invert(p, c, MAXCP);
    if (cs_canonicalize(p, c, 1) < 0 || cs_invert(p, c, MAXCP) < 0) return -1;
    struct cset folds;
    memset(&folds, 0, sizeof folds);
    for (int i = 0; i < uni_fold.n; i++)
        if (uni_fold.p[2 * i + 1] != uni_fold.p[2 * i] && cs_add(p, &folds, uni_fold.p[2 * i], uni_fold.p[2 * i]) < 0) return -1;
    return cs_subtract(p, c, &folds);
}

// ---------------------------------------------------------------- tree

enum { RN_EMPTY, RN_CHAR, RN_SET, RN_ANY, RN_BOL, RN_EOL, RN_WORDB, RN_SEQ, RN_ALT, RN_GROUP, RN_LOOK,
       RN_REPEAT, RN_BACKREF, RN_MODS };

struct rnode {
    uint8_t type;
    uint8_t neg;
    uint8_t behind;
    uint8_t greedy;
    uint32_t c;
    int cap;                // RN_GROUP: capture index (0 = non-capturing)
    uint32_t min, max;
    int cap_lo, cap_hi;     // capture groups inside [cap_lo, cap_hi)
    struct cset* set;
    struct rnode* kid;      // first child
    struct rnode* next;
    int add_flags, rm_flags;
    int* refs;
    int nrefs;
    char* refname;          // unresolved named backreference
    struct rnode* next_ref;
};

static struct rnode* node(struct rp* p, int type) {
    struct rnode* n = (struct rnode*)aalloc(p, sizeof *n);
    if (n) n->type = (uint8_t)type;
    return n;
}

static int peek(struct rp* p) { return p->i < p->n ? (int)p->s[p->i] : -1; }
static int peek2(struct rp* p, int k) { return p->i + (uint32_t)k < p->n ? (int)p->s[p->i + (uint32_t)k] : -1; }
static int eat(struct rp* p, uint32_t c) { if (p->i < p->n && p->s[p->i] == c) { p->i++; return 1; } return 0; }

static int is_syntax_char(uint32_t c) { return c && c < 128 && strchr("^$\\.*+?()[]{}|/", (int)c) != 0; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static struct rnode* parse_disjunction(struct rp* p);

// \uXXXX, \u{...} (unicode), surrogate pairs of \u escapes (unicode). -1: not an escape
static int parse_u_escape(struct rp* p, int allow_brace, uint32_t* out) {
    uint32_t save = p->i;
    if (peek(p) == '{' && allow_brace) {
        p->i++;
        uint32_t v = 0;
        int k = 0;
        while (hexv(peek(p)) >= 0) { v = v * 16 + (uint32_t)hexv(peek(p)); if (v > MAXCP) { fail(p, "invalid Unicode escape"); return -1; } p->i++; k++; }
        if (!k || !eat(p, '}')) { p->i = save; return -1; }
        *out = v;
        return 0;
    }
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexv(peek(p));
        if (h < 0) { p->i = save; return -1; }
        v = v * 16 + (uint32_t)h;
        p->i++;
    }
    if (allow_brace && v >= 0xD800 && v <= 0xDBFF && peek(p) == '\\' && peek2(p, 1) == 'u') {
        uint32_t s2 = p->i;
        p->i += 2;
        uint32_t w = 0;
        int ok = 1;
        for (int k = 0; k < 4; k++) {
            int h = hexv(peek(p));
            if (h < 0) { ok = 0; break; }
            w = w * 16 + (uint32_t)h;
            p->i++;
        }
        if (ok && w >= 0xDC00 && w <= 0xDFFF) v = 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00);
        else p->i = s2;
    }
    *out = v;
    return 0;
}

// group name (after "<" ... ">") as UTF-8
static char* parse_group_name(struct rp* p) {
    char buf[256];
    int len = 0;
    int first = 1;
    for (;;) {
        int c = peek(p);
        if (c < 0) { fail(p, "invalid capture group name"); return 0; }
        if (c == '>') { p->i++; break; }
        uint32_t cp;
        if (c == '\\') {
            p->i++;
            if (!eat(p, 'u') || parse_u_escape(p, 1, &cp) < 0) { fail(p, "invalid capture group name"); return 0; }
        } else {
            cp = (uint32_t)c;
            p->i++;
            // non-unicode patterns hold code units: join surrogate pairs here
            if (!p->unicode && cp >= 0xD800 && cp <= 0xDBFF && peek(p) >= 0xDC00 && peek(p) <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)peek(p) - 0xDC00);
                p->i++;
            }
        }
        int ok = first ? uni_id_start_cp(cp) : uni_id_continue_cp(cp);
        if (!ok) { fail(p, "invalid capture group name"); return 0; }
        first = 0;
        // UTF-8
        if (len > 240) { fail(p, "capture group name too long"); return 0; }
        if (cp < 0x80) buf[len++] = (char)cp;
        else if (cp < 0x800) { buf[len++] = (char)(0xC0 | (cp >> 6)); buf[len++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { buf[len++] = (char)(0xE0 | (cp >> 12)); buf[len++] = (char)(0x80 | ((cp >> 6) & 63)); buf[len++] = (char)(0x80 | (cp & 63)); }
        else { buf[len++] = (char)(0xF0 | (cp >> 18)); buf[len++] = (char)(0x80 | ((cp >> 12) & 63)); buf[len++] = (char)(0x80 | ((cp >> 6) & 63)); buf[len++] = (char)(0x80 | (cp & 63)); }
    }
    if (first) { fail(p, "invalid capture group name"); return 0; }
    char* r = (char*)aalloc(p, (size_t)len + 1);
    if (!r) return 0;
    memcpy(r, buf, (size_t)len);
    r[len] = 0;
    return r;
}

// class escapes: \d \D \s \S \w \W \p{..} \P{..}; returns 1 and fills set, 0 if not one
static int class_escape(struct rp* p, int c, struct cset* set) {
    int neg = 0;
    switch (c) {
    case 'D': neg = 1; /* fall through */
    case 'd': if (cs_add(p, set, '0', '9') < 0) return -1; break;
    case 'S': neg = 1; /* fall through */
    case 's': {
        static const uint32_t WS[] = { 9, 13, 32, 32, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A, 0x2028, 0x2029,
                                       0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000, 0xFEFF, 0xFEFF };
        for (unsigned i = 0; i < sizeof WS / sizeof WS[0]; i += 2) if (cs_add(p, set, WS[i], WS[i + 1]) < 0) return -1;
        break;
    }
    case 'W': neg = 1; /* fall through */
    case 'w':
        if (cs_add(p, set, '0', '9') < 0 || cs_add(p, set, 'A', 'Z') < 0 || cs_add(p, set, '_', '_') < 0 || cs_add(p, set, 'a', 'z') < 0) return -1;
        if (p->unicode && (p->flags & RF_I)) {
            if (cs_add(p, set, 0x17F, 0x17F) < 0 || cs_add(p, set, 0x212A, 0x212A) < 0) return -1;
        }
        break;
    case 'P': neg = 1; /* fall through */
    case 'p': {
        if (!p->unicode) return 0;
        if (!eat(p, '{')) { fail(p, "invalid property name"); return -1; }
        char name[96], value[96];
        int nl = 0, vl = 0, has_eq = 0;
        for (;;) {
            int ch = peek(p);
            if (ch == '}') { p->i++; break; }
            if (ch < 0 || !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '=')) {
                fail(p, "invalid property name");
                return -1;
            }
            p->i++;
            if (ch == '=') { if (has_eq) { fail(p, "invalid property name"); return -1; } has_eq = 1; continue; }
            if (has_eq) { if (vl < 95) value[vl++] = (char)ch; }
            else if (nl < 95) name[nl++] = (char)ch;
        }
        name[nl] = 0;
        value[vl] = 0;
        if (!nl || (has_eq && !vl)) { fail(p, "invalid property name"); return -1; }
        struct cset tmp;
        memset(&tmp, 0, sizeof tmp);
        int is_cn = 0;
        const struct urange_set* u = 0;
        if (has_eq) {
            int kind = (!strcmp(name, "General_Category") || !strcmp(name, "gc")) ? 0 :
                       (!strcmp(name, "Script") || !strcmp(name, "sc")) ? 2 :
                       (!strcmp(name, "Script_Extensions") || !strcmp(name, "scx")) ? 3 : -1;
            if (kind < 0) { fail(p, "invalid property name"); return -1; }
            u = uni_find_prop(kind, value, &is_cn);
            if (!u && !is_cn) { fail(p, "invalid property name"); return -1; }
        } else {
            if (!strcmp(name, "Any")) { if (cs_add(p, &tmp, 0, MAXCP) < 0) return -1; }
            else {
                u = uni_find_prop(0, name, &is_cn);
                if (!u && !is_cn) u = uni_find_prop(1, name, &is_cn);
                if (!u && !is_cn) { fail(p, "invalid property name"); return -1; }
            }
        }
        if (u && cs_add_urange(p, &tmp, u) < 0) return -1;
        if (is_cn) {
            // Cn = not Assigned; C = Cc|Cf|Cs|Co|Cn
            int dummy;
            const struct urange_set* as = uni_find_prop(1, "Assigned", &dummy);
            struct cset cn;
            memset(&cn, 0, sizeof cn);
            if (as && cs_add_urange(p, &cn, as) < 0) return -1;
            if (cs_invert(p, &cn, MAXCP) < 0 || cs_add_set(p, &tmp, &cn) < 0) return -1;
        }
        if (neg && cs_complement(p, &tmp) < 0) return -1;
        if (cs_add_set(p, set, &tmp) < 0) return -1;
        return 1;
    }
    default: return 0;
    }
    if (neg) {
        struct cset tmp;
        memset(&tmp, 0, sizeof tmp);
        if (cs_add_set(p, &tmp, set) < 0) return -1;
        set->n = 0;
        if ((p->unicode ? cs_complement(p, &tmp) : cs_invert(p, &tmp, 0xFFFF)) < 0) return -1;
        if (cs_add_set(p, set, &tmp) < 0) return -1;
    }
    return 1;
}

// character escape after '\' (not a class escape / backreference); -1 error
static int char_escape(struct rp* p, int in_class, uint32_t* out) {
    int c = peek(p);
    if (c < 0) { fail(p, "\\ at end of pattern"); return -1; }
    p->i++;
    switch (c) {
    case 't': *out = 9; return 0;
    case 'n': *out = 10; return 0;
    case 'v': *out = 11; return 0;
    case 'f': *out = 12; return 0;
    case 'r': *out = 13; return 0;
    case 'c': {
        int l = peek(p);
        if ((l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z')) { p->i++; *out = (uint32_t)(l % 32); return 0; }
        if (!p->unicode && in_class && (is_digit(l) || l == '_')) { p->i++; *out = (uint32_t)(l % 32); return 0; }
        if (p->unicode) { fail(p, "invalid unicode escape"); return -1; }
        p->i--;          // Annex B: "\c" is a backslash followed by 'c'
        *out = '\\';
        return 0;
    }
    case '0':
        if (!is_digit(peek(p))) { *out = 0; return 0; }
        if (p->unicode) { fail(p, "invalid decimal escape"); return -1; }
        /* fall through */
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        if (p->unicode) { fail(p, "invalid escape"); return -1; }
        if (c > '7') break;
        {
            uint32_t v = (uint32_t)(c - '0');
            if (peek(p) >= '0' && peek(p) <= '7') {
                v = v * 8 + (uint32_t)(peek(p) - '0');
                p->i++;
                if (c <= '3' && peek(p) >= '0' && peek(p) <= '7') { v = v * 8 + (uint32_t)(peek(p) - '0'); p->i++; }
            }
            *out = v;
            return 0;
        }
    case 'x': {
        int a = hexv(peek(p)), b = hexv(peek2(p, 1));
        if (a >= 0 && b >= 0) { p->i += 2; *out = (uint32_t)(a * 16 + b); return 0; }
        if (p->unicode) { fail(p, "invalid escape"); return -1; }
        *out = 'x';
        return 0;
    }
    case 'u': {
        uint32_t v;
        if (parse_u_escape(p, p->unicode, &v) == 0) { *out = v; return 0; }
        if (p->failed) return -1;
        if (p->unicode) { fail(p, "invalid Unicode escape"); return -1; }
        *out = 'u';
        return 0;
    }
    case '-':
        if (p->unicode && !in_class) { fail(p, "invalid escape"); return -1; }
        *out = '-';
        return 0;
    }
    if (c == '8' || c == '9') {
        if (p->unicode) { fail(p, "invalid escape"); return -1; }
        *out = (uint32_t)c;
        return 0;
    }
    if (p->unicode) {
        if (is_syntax_char((uint32_t)c) || c == '/') { *out = (uint32_t)c; return 0; }
        if (p->vmode && in_class && c && c < 128 && strchr("&-!#%,:;<=>@`~", c)) { *out = (uint32_t)c; return 0; }
        fail(p, "invalid escape");
        return -1;
    }
    // Annex B identity escape (but not \k when the pattern has named groups)
    if (c == 'k' && p->named) { fail(p, "invalid named reference"); return -1; }
    *out = (uint32_t)c;
    return 0;
}

// ---------------------------------------------------------------- character classes

// one ClassAtom: char (returns 0, *c set) or class escape (returns 1, set filled); -1 error
static int class_atom(struct rp* p, uint32_t* c, struct cset* set) {
    int ch = peek(p);
    if (ch == '\\') {
        p->i++;
        int e = peek(p);
        if (e < 0) { fail(p, "\\ at end of pattern"); return -1; }
        if (e == 'b') { p->i++; *c = 8; return 0; }
        if (p->unicode && e == '-') { p->i++; *c = '-'; return 0; }
        if (strchr("dDsSwWpP", e) && e) {
            p->i++;
            int r = class_escape(p, e, set);
            if (r < 0) return -1;
            if (r == 1) return 1;
            p->i--;
        }
        if (!p->unicode && e == 'k') { p->i++; *c = 'k'; return 0; }
        if (char_escape(p, 1, c) < 0) return -1;
        return 0;
    }
    p->i++;
    *c = (uint32_t)ch;
    return 0;
}

static int parse_class_v(struct rp* p, struct cset* out, int depth);

// [...] in u / non-u mode: returns the set (negation in *neg)
static int parse_class(struct rp* p, struct cset* set, int* neg) {
    // '[' consumed
    *neg = eat(p, '^');
    if (p->vmode) return parse_class_v(p, set, 0);
    for (;;) {
        int ch = peek(p);
        if (ch < 0) { fail(p, "missing ]"); return -1; }
        if (ch == ']') { p->i++; return 0; }
        uint32_t a, b;
        struct cset sa, sb;
        memset(&sa, 0, sizeof sa);
        memset(&sb, 0, sizeof sb);
        int ka = class_atom(p, &a, &sa);
        if (ka < 0) return -1;
        if (peek(p) == '-' && peek2(p, 1) != ']' && peek2(p, 1) >= 0) {
            p->i++;
            int kb = class_atom(p, &b, &sb);
            if (kb < 0) return -1;
            if (ka == 1 || kb == 1) {
                if (p->unicode) { fail(p, "invalid character class"); return -1; }
                // Annex B: \d-x is a union of the parts and '-'
                if (ka == 1) { if (cs_add_set(p, set, &sa) < 0) return -1; } else if (cs_add(p, set, a, a) < 0) return -1;
                if (cs_add(p, set, '-', '-') < 0) return -1;
                if (kb == 1) { if (cs_add_set(p, set, &sb) < 0) return -1; } else if (cs_add(p, set, b, b) < 0) return -1;
                continue;
            }
            if (a > b) { fail(p, "range out of order in character class"); return -1; }
            if (cs_add(p, set, a, b) < 0) return -1;
            continue;
        }
        if (ka == 1) { if (cs_add_set(p, set, &sa) < 0) return -1; }
        else if (cs_add(p, set, a, a) < 0) return -1;
    }
}

// v-mode ClassSetCharacter
static int v_set_char(struct rp* p, uint32_t* c) {
    int ch = peek(p);
    if (ch == '\\') {
        p->i++;
        int e = peek(p);
        if (e == 'b') { p->i++; *c = 8; return 0; }
        return char_escape(p, 1, c);
    }
    // syntax characters and double punctuators are not allowed unescaped
    if (ch < 0 || (ch < 128 && strchr("()[]{}/-|", ch) && ch)) { fail(p, "invalid set operation in character class"); return -1; }
    static const char DP[] = "&!#$%*+,.:;<=>?@^`~";
    if (ch < 128 && strchr(DP, ch) && ch && peek2(p, 1) == ch) { fail(p, "invalid set operation in character class"); return -1; }
    p->i++;
    *c = (uint32_t)ch;
    return 0;
}

// one v-mode ClassSetOperand into tmp: nested class, \q{...}, class escape, or character (range allowed)
static int v_operand(struct rp* p, struct cset* tmp, int allow_range, int depth) {
    int ch = peek(p);
    if (ch == '[') {
        p->i++;
        int neg = eat(p, '^');
        struct cset inner;
        memset(&inner, 0, sizeof inner);
        if (parse_class_v(p, &inner, depth + 1) < 0) return -1;
        if (neg) {
            if (inner.strs) { fail(p, "negated character class may contain strings"); return -1; }
            if (cs_complement(p, &inner) < 0) return -1;
        }
        return cs_add_set(p, tmp, &inner);
    }
    if (ch == '\\') {
        int e = peek2(p, 1);
        if (e == 'q') {
            p->i += 2;
            if (!eat(p, '{')) { fail(p, "invalid escape"); return -1; }
            for (;;) {
                uint32_t buf[256];
                int n = 0;
                while (peek(p) != '|' && peek(p) != '}') {
                    if (peek(p) < 0) { fail(p, "unterminated \\q"); return -1; }
                    uint32_t c;
                    if (peek(p) == '\\') { p->i++; if (char_escape(p, 1, &c) < 0) return -1; }
                    else { c = (uint32_t)peek(p); p->i++; }
                    if (n < 256) buf[n++] = c;
                }
                if (n == 1) { if (cs_add(p, tmp, buf[0], buf[0]) < 0) return -1; }
                else {
                    struct cstr* s = (struct cstr*)aalloc(p, sizeof *s);
                    if (!s) return -1;
                    s->cp = (uint32_t*)aalloc(p, (size_t)(n ? n : 1) * 4);
                    if (!s->cp) return -1;
                    memcpy(s->cp, buf, (size_t)n * 4);
                    s->n = n;
                    s->next = tmp->strs;
                    tmp->strs = s;
                }
                if (eat(p, '}')) break;
                p->i++;   // '|'
            }
            return 0;
        }
        if (e >= 0 && strchr("dDsSwWpP", e) && e) {
            p->i += 2;
            int r = class_escape(p, e, tmp);
            return r < 0 ? -1 : 0;
        }
    }
    uint32_t a;
    if (v_set_char(p, &a) < 0) return -1;
    if (allow_range && peek(p) == '-' && peek2(p, 1) != '-') {
        p->i++;
        uint32_t b;
        if (v_set_char(p, &b) < 0) return -1;
        if (a > b) { fail(p, "range out of order in character class"); return -1; }
        return cs_add(p, tmp, a, b);
    }
    return cs_add(p, tmp, a, a);
}

// /vi: set operations work on case-folded operands (MaybeSimpleCaseFolding): every
// member is replaced by its simple case folding, so [\q{A}--a] is empty
static int v_fold(struct rp* p, struct cset* c) {
    if (!(p->flags & RF_I)) return 0;
    for (struct cstr* s = c->strs; s; s = s->next)
        for (int i = 0; i < s->n; i++) s->cp[i] = canon(1, s->cp[i]);
    struct cset from, to;
    memset(&from, 0, sizeof from);
    memset(&to, 0, sizeof to);
    for (int i = 0; i < uni_fold.n; i++) {
        uint32_t x = uni_fold.p[2 * i], y = uni_fold.p[2 * i + 1];
        if (x == y || !cs_has(c, x)) continue;
        if (cs_add(p, &from, x, x) < 0 || cs_add(p, &to, y, y) < 0) return -1;
    }
    if (!from.n) return 0;
    struct cstr* strs = c->strs;
    if (cs_subtract(p, c, &from) < 0) return -1;
    c->strs = strs;
    return cs_add_set(p, c, &to);
}

static int parse_class_v(struct rp* p, struct cset* out, int depth) {
    if (depth > 64) { fail(p, "character class nested too deeply"); return -1; }
    // first operand, then either a union, or && / -- chains
    if (eat(p, ']')) return 0;
    struct cset acc;
    memset(&acc, 0, sizeof acc);
    if (v_operand(p, &acc, 1, depth) < 0 || v_fold(p, &acc) < 0) return -1;
    int op = 0;   // 1 intersection, 2 subtraction
    for (;;) {
        int ch = peek(p);
        if (ch < 0) { fail(p, "missing ]"); return -1; }
        if (ch == ']') { p->i++; break; }
        int this_op = 0;
        if (ch == '&' && peek2(p, 1) == '&') this_op = 1;
        else if (ch == '-' && peek2(p, 1) == '-') this_op = 2;
        if (this_op) {
            if (op && op != this_op) { fail(p, "invalid set operation in character class"); return -1; }
            op = this_op;
            p->i += 2;
            if (peek(p) == '&' && this_op == 1) { fail(p, "invalid set operation in character class"); return -1; }
            struct cset rhs;
            memset(&rhs, 0, sizeof rhs);
            if (v_operand(p, &rhs, 0, depth) < 0 || v_fold(p, &rhs) < 0) return -1;
            if (this_op == 1) {
                if (cs_intersect(p, &acc, &rhs) < 0) return -1;
                // strings: keep those present on both sides
                struct cstr* kept = 0;
                for (struct cstr* s = acc.strs; s; s = s->next)
                    for (struct cstr* t = rhs.strs; t; t = t->next)
                        if (s->n == t->n && !memcmp(s->cp, t->cp, (size_t)s->n * 4)) {
                            struct cstr* k = (struct cstr*)aalloc(p, sizeof *k);
                            if (!k) return -1;
                            *k = *s;
                            k->next = kept;
                            kept = k;
                            break;
                        }
                acc.strs = kept;
            } else {
                if (cs_subtract(p, &acc, &rhs) < 0) return -1;
                struct cstr** pp = &acc.strs;
                while (*pp) {
                    int drop = 0;
                    for (struct cstr* t = rhs.strs; t; t = t->next)
                        if ((*pp)->n == t->n && !memcmp((*pp)->cp, t->cp, (size_t)t->n * 4)) { drop = 1; break; }
                    if (drop) *pp = (*pp)->next; else pp = &(*pp)->next;
                }
            }
            continue;
        }
        if (op) { fail(p, "invalid set operation in character class"); return -1; }
        if (v_operand(p, &acc, 1, depth) < 0 || v_fold(p, &acc) < 0) return -1;
    }
    return cs_add_set(p, out, &acc);
}

// ---------------------------------------------------------------- atoms and terms

static int parse_quant_braces(struct rp* p, uint32_t* min, uint32_t* max) {
    // at '{': {n}, {n,}, {n,m}; returns 1 if a quantifier, 0 if not (Annex B literal)
    uint32_t save = p->i;
    p->i++;
    if (!is_digit(peek(p))) { p->i = save; return 0; }
    uint64_t a = 0;
    while (is_digit(peek(p))) { a = a * 10 + (uint64_t)(peek(p) - '0'); if (a > 0xFFFFFFFFu) a = 0xFFFFFFFFu; p->i++; }
    uint64_t b = a;
    if (eat(p, ',')) {
        if (is_digit(peek(p))) {
            b = 0;
            while (is_digit(peek(p))) { b = b * 10 + (uint64_t)(peek(p) - '0'); if (b > 0xFFFFFFFFu) b = 0xFFFFFFFFu; p->i++; }
        } else b = 0xFFFFFFFFu;
    }
    if (!eat(p, '}')) { p->i = save; return 0; }
    *min = (uint32_t)a;
    *max = (uint32_t)b;
    return 1;
}

static void count_groups(struct rp* p) {
    // pre-scan: number of capturing groups and whether names exist
    int in_class = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        uint32_t c = p->s[i];
        if (c == '\\') { i++; continue; }
        if (in_class) { if (c == ']') in_class = 0; continue; }
        if (c == '[') { in_class = 1; continue; }
        if (c == '(') {
            if (i + 1 < p->n && p->s[i + 1] == '?') {
                if (i + 2 < p->n && p->s[i + 2] == '<' && i + 3 < p->n && p->s[i + 3] != '=' && p->s[i + 3] != '!') {
                    p->total_caps++;
                    p->named = 1;
                }
            } else p->total_caps++;
        }
    }
}

static struct rnode* parse_atom(struct rp* p) {
    int c = peek(p);
    struct rnode* n;
    switch (c) {
    case '.':
        p->i++;
        return node(p, RN_ANY);
    case '(': {
        p->i++;
        if (eat(p, '?')) {
            int k = peek(p);
            if (k == ':') {
                p->i++;
                n = node(p, RN_GROUP);
                if (!n) return 0;
                n->kid = parse_disjunction(p);
                if (!eat(p, ')')) { fail(p, "unterminated group"); return 0; }
                return n;
            }
            if (k == '=' || k == '!' || (k == '<' && (peek2(p, 1) == '=' || peek2(p, 1) == '!'))) {
                n = node(p, RN_LOOK);
                if (!n) return 0;
                if (k == '<') { n->behind = 1; p->i++; }
                n->neg = peek(p) == '!';
                p->i++;
                n->cap_lo = p->ncaps + 1;
                n->kid = parse_disjunction(p);
                n->cap_hi = p->ncaps + 1;
                if (!eat(p, ')')) { fail(p, "unterminated group"); return 0; }
                return n;
            }
            if (k == '<') {
                p->i++;
                char* name = parse_group_name(p);
                if (!name) return 0;
                int idx = ++p->ncaps;
                if (idx >= p->names_cap) {
                    int nc = p->names_cap ? p->names_cap * 2 : 16;
                    while (nc <= idx) nc *= 2;
                    char** t = (char**)aalloc(p, (size_t)nc * sizeof(char*));
                    if (!t) return 0;
                    if (p->names_cap) memcpy(t, p->names, (size_t)p->names_cap * sizeof(char*));
                    p->names = t;
                    p->names_cap = nc;
                }
                p->names[idx] = name;
                n = node(p, RN_GROUP);
                if (!n) return 0;
                n->cap = idx;
                n->kid = parse_disjunction(p);
                if (!eat(p, ')')) { fail(p, "unterminated group"); return 0; }
                return n;
            }
            // modifiers (?ims-ims: ...)
            int add = 0, rm = 0, minus = 0;
            for (;;) {
                int f = peek(p);
                int bit = f == 'i' ? RF_I : f == 'm' ? RF_M : f == 's' ? RF_S : 0;
                if (f == '-') { if (minus) { fail(p, "invalid group"); return 0; } minus = 1; p->i++; continue; }
                if (f == ':') { p->i++; break; }
                if (!bit) { fail(p, "invalid group"); return 0; }
                if ((add | rm) & bit) { fail(p, "repeated flag in modifiers"); return 0; }
                if (minus) rm |= bit; else add |= bit;
                p->i++;
            }
            if (minus && !add && !rm) { fail(p, "invalid group"); return 0; }
            n = node(p, RN_MODS);
            if (!n) return 0;
            n->add_flags = add;
            n->rm_flags = rm;
            int saved = p->flags;
            p->flags = (p->flags | add) & ~rm;
            n->kid = parse_disjunction(p);
            p->flags = saved;
            if (!eat(p, ')')) { fail(p, "unterminated group"); return 0; }
            return n;
        }
        n = node(p, RN_GROUP);
        if (!n) return 0;
        n->cap = ++p->ncaps;
        n->kid = parse_disjunction(p);
        if (!eat(p, ')')) { fail(p, "unterminated group"); return 0; }
        return n;
    }
    case '[': {
        p->i++;
        n = node(p, RN_SET);
        if (!n) return 0;
        n->set = (struct cset*)aalloc(p, sizeof(struct cset));
        if (!n->set) return 0;
        int neg;
        if (parse_class(p, n->set, &neg) < 0) return 0;
        if (neg && n->set->strs) { fail(p, "negated character class may contain strings"); return 0; }
        n->neg = (uint8_t)neg;
        return n;
    }
    case '\\': {
        p->i++;
        int e = peek(p);
        if (e < 0) { fail(p, "\\ at end of pattern"); return 0; }
        if (e == 'b' || e == 'B') {
            p->i++;
            n = node(p, RN_WORDB);
            if (n) n->neg = e == 'B';
            return n;
        }
        if (e >= '1' && e <= '9') {
            uint32_t save = p->i;
            uint64_t v = 0;
            while (is_digit(peek(p))) { v = v * 10 + (uint64_t)(peek(p) - '0'); if (v > 100000) v = 100000; p->i++; }
            if (v <= (uint64_t)p->total_caps) {
                n = node(p, RN_BACKREF);
                if (!n) return 0;
                n->refs = (int*)aalloc(p, sizeof(int));
                if (!n->refs) return 0;
                n->refs[0] = (int)v;
                n->nrefs = 1;
                return n;
            }
            if (p->unicode) { fail(p, "invalid escape"); return 0; }
            p->i = save;
        }
        if (e == 'k' && (p->unicode || p->named)) {
            p->i++;
            if (!eat(p, '<')) { fail(p, "invalid named reference"); return 0; }
            char* name = parse_group_name(p);
            if (!name) return 0;
            n = node(p, RN_BACKREF);
            if (!n) return 0;
            n->refname = name;
            n->next_ref = p->backrefs;
            p->backrefs = n;
            return n;
        }
        n = node(p, RN_SET);
        if (!n) return 0;
        n->set = (struct cset*)aalloc(p, sizeof(struct cset));
        if (!n->set) return 0;
        if (e >= 0 && strchr("dDsSwWpP", e) && e) {
            p->i++;
            int r = class_escape(p, e, n->set);
            if (r < 0) return 0;
            if (r == 1) return n;
            p->i--;
        }
        uint32_t ch;
        if (char_escape(p, 0, &ch) < 0) return 0;
        n->type = RN_CHAR;
        n->c = ch;
        return n;
    }
    }
    // Annex B: lone ']' '{' '}' are literal outside unicode mode
    if (c == '*' || c == '+' || c == '?') { fail(p, "nothing to repeat"); return 0; }
    if (c == '{') {
        if (p->unicode) { fail(p, "nothing to repeat"); return 0; }
        uint32_t mn, mx;
        uint32_t save = p->i;
        if (parse_quant_braces(p, &mn, &mx)) { p->i = save; fail(p, "nothing to repeat"); return 0; }
    }
    if ((c == '}' || c == ']') && p->unicode) { fail(p, "lone quantifier brackets"); return 0; }
    p->i++;
    n = node(p, RN_CHAR);
    if (n) n->c = (uint32_t)c;
    return n;
}

static struct rnode* parse_term(struct rp* p) {
    int c = peek(p);
    struct rnode* n;
    if (c == '^') { p->i++; n = node(p, RN_BOL); if (n) n->neg = (p->flags & RF_M) != 0; return n; }
    if (c == '$') { p->i++; n = node(p, RN_EOL); if (n) n->neg = (p->flags & RF_M) != 0; return n; }
    int cap_before = p->ncaps;
    n = parse_atom(p);
    if (!n) return 0;
    // quantifier
    uint32_t min, max;
    c = peek(p);
    if (c == '*') { min = 0; max = 0xFFFFFFFFu; p->i++; }
    else if (c == '+') { min = 1; max = 0xFFFFFFFFu; p->i++; }
    else if (c == '?') { min = 0; max = 1; p->i++; }
    else if (c == '{') {
        if (!parse_quant_braces(p, &min, &max)) {
            if (p->unicode) { fail(p, "incomplete quantifier"); return 0; }
            return n;
        }
        if (min > max) { fail(p, "numbers out of order in {} quantifier"); return 0; }
    } else return n;
    if (n->type == RN_BOL || n->type == RN_EOL || n->type == RN_WORDB ||
        (n->type == RN_LOOK && (p->unicode || n->behind))) {
        fail(p, "nothing to repeat");
        return 0;
    }
    struct rnode* r = node(p, RN_REPEAT);
    if (!r) return 0;
    r->min = min;
    r->max = max;
    r->greedy = !eat(p, '?');
    r->kid = n;
    r->cap_lo = cap_before + 1;
    r->cap_hi = p->ncaps + 1;
    return r;
}

static struct rnode* parse_alternative(struct rp* p) {
    struct rnode* seq = node(p, RN_SEQ);
    if (!seq) return 0;
    struct rnode** tail = &seq->kid;
    while (!p->failed) {
        int c = peek(p);
        if (c < 0 || c == '|' || c == ')') break;
        struct rnode* t = parse_term(p);
        if (!t) return 0;
        *tail = t;
        tail = &t->next;
    }
    return p->failed ? 0 : seq;
}

static struct rnode* parse_disjunction(struct rp* p) {
    if (++p->depth > 400) { fail(p, "regular expression too large"); return 0; }
    struct rnode* first = parse_alternative(p);
    if (!first) return 0;
    if (peek(p) != '|') { p->depth--; return first; }
    struct rnode* alt = node(p, RN_ALT);
    if (!alt) return 0;
    alt->kid = first;
    struct rnode* last = first;
    while (eat(p, '|')) {
        struct rnode* a = parse_alternative(p);
        if (!a) return 0;
        last->next = a;
        last = a;
    }
    p->depth--;
    return alt;
}

// ---------------------------------------------------------------- code generation

enum {
    R_CHAR = 1, R_CHARI, R_ANY, R_SET, R_SETI, R_BOL, R_EOL, R_WORDB, R_SAVE, R_RESET, R_SPLIT, R_JMP,
    R_REG_ZERO, R_REG_POS, R_LOOP, R_PROGRESS, R_REG_INC, R_LOOK, R_LOOK_END, R_BACKREF, R_SIMPLE, R_MATCH,
    R_FAIL, R_STR,
};
#define R_BACK 0x80     // flag on character ops: match right-to-left

struct cg {
    struct rp* p;
    uint32_t* code;
    uint32_t len, cap;
    int nregs;
};

static int emit(struct cg* g, uint32_t w) {
    if (g->len >= g->cap) {
        uint32_t nc = g->cap ? g->cap * 2 : 256;
        uint32_t* t = (uint32_t*)ojs_sys_realloc(g->code, (size_t)nc * 4);
        if (!t) { g->p->failed = 1; ojs_snprintf(g->p->err, g->p->errcap, "out of memory"); return -1; }
        g->code = t;
        g->cap = nc;
    }
    g->code[g->len++] = w;
    return 0;
}

static int set_has(const uint32_t* r, uint32_t n, uint32_t c);

// R_SET / R_SETI: op neg n bitmap[4] ranges[2n]. The bitmap holds the membership of
// every ASCII input (after case folding for R_SETI), so most tests skip the search.
static int emit_set(struct cg* g, int op, const struct cset* s, int neg) {
    if (emit(g, (uint32_t)op) < 0 || emit(g, (uint32_t)neg) < 0 || emit(g, (uint32_t)s->n) < 0) return -1;
    uint32_t bm[4] = { 0, 0, 0, 0 };
    int icase = (op & 0x7F) == R_SETI;
    for (uint32_t c = 0; c < 128; c++) {
        uint32_t x = icase ? canon(g->p->unicode, c) : c;
        if (set_has(s->r, (uint32_t)s->n, x)) bm[c >> 5] |= 1u << (c & 31);
    }
    for (int i = 0; i < 4; i++) if (emit(g, bm[i]) < 0) return -1;
    for (int i = 0; i < s->n * 2; i++) if (emit(g, s->r[i]) < 0) return -1;
    return 0;
}

static int flags_icase(int f) { return (f & RF_I) != 0; }

// single-character atom (for the simple repeat); returns 1 if n is one
static int is_single_char(const struct rnode* n) {
    if (n->type == RN_CHAR || n->type == RN_ANY) return 1;
    if (n->type == RN_SET && !n->set->strs) return 1;
    if ((n->type == RN_GROUP && !n->cap) || n->type == RN_SEQ) {
        if (n->kid && !n->kid->next) return is_single_char(n->kid);
    }
    return 0;
}

static const struct rnode* single_of(const struct rnode* n) {
    while ((n->type == RN_GROUP || n->type == RN_SEQ) && n->kid) n = n->kid;
    return n;
}

static int gen(struct cg* g, const struct rnode* n, int flags, int back);

static int gen_char_atom(struct cg* g, const struct rnode* n, int flags, int back) {
    struct rp* p = g->p;
    uint32_t bflag = back ? R_BACK : 0;
    switch (n->type) {
    case RN_CHAR:
        if (flags_icase(flags)) return emit(g, R_CHARI | bflag) < 0 || emit(g, canon(p->unicode, n->c)) < 0 ? -1 : 0;
        return emit(g, R_CHAR | bflag) < 0 || emit(g, n->c) < 0 ? -1 : 0;
    case RN_ANY: return emit(g, R_ANY | bflag) < 0 || emit(g, (flags & RF_S) ? 1 : 0) < 0 ? -1 : 0;
    case RN_SET: {
        if (flags_icase(flags)) {
            struct cset c;
            memset(&c, 0, sizeof c);
            if (cs_add_set(p, &c, n->set) < 0 || cs_canonicalize(p, &c, p->unicode) < 0) return -1;
            return emit_set(g, R_SETI | bflag, &c, n->neg);
        }
        return emit_set(g, R_SET | bflag, n->set, n->neg);
    }
    }
    return -1;
}

// a literal character R_STR can hold: case-sensitive, one code unit, not a surrogate
static int str_char(const struct rnode* k, int flags) {
    return k && k->type == RN_CHAR && !flags_icase(flags) && (k->c < 0xD800 || (k->c >= 0xE000 && k->c <= 0xFFFF));
}

static int gen_seq(struct cg* g, const struct rnode* first, int flags, int back) {
    if (!back) {
        for (const struct rnode* k = first; k;) {
            // runs of literal characters: one R_STR n c1..cn compared in a tight loop
            if (str_char(k, flags) && str_char(k->next, flags)) {
                uint32_t n = 0;
                for (const struct rnode* x = k; str_char(x, flags); x = x->next) n++;
                if (emit(g, R_STR) < 0 || emit(g, n) < 0) return -1;
                for (; n; n--, k = k->next) if (emit(g, k->c) < 0) return -1;
                continue;
            }
            if (gen(g, k, flags, 0) < 0) return -1;
            k = k->next;
        }
        return 0;
    }
    // right to left: terms in reverse order
    int cnt = 0;
    for (const struct rnode* k = first; k; k = k->next) cnt++;
    const struct rnode** v = (const struct rnode**)ojs_sys_malloc(sizeof(void*) * (size_t)(cnt ? cnt : 1));
    if (!v) { g->p->failed = 1; return -1; }
    int i = 0;
    for (const struct rnode* k = first; k; k = k->next) v[i++] = k;
    int r = 0;
    for (i = cnt - 1; i >= 0 && r == 0; i--) r = gen(g, v[i], flags, 1);
    ojs_sys_free(v);
    return r;
}

static int gen(struct cg* g, const struct rnode* n, int flags, int back) {
    struct rp* p = g->p;
    uint32_t bflag = back ? R_BACK : 0;
    switch (n->type) {
    case RN_EMPTY: return 0;
    case RN_CHAR: case RN_ANY: return gen_char_atom(g, n, flags, back);
    case RN_SET:
        if (n->set->strs) {
            // a class with strings: (?:longest strings first|...|[single chars])
            int nstr = 0;
            for (struct cstr* s = n->set->strs; s; s = s->next) nstr++;
            struct cstr** v = (struct cstr**)ojs_sys_malloc(sizeof(void*) * (size_t)nstr);
            if (!v) { p->failed = 1; return -1; }
            int i = 0;
            for (struct cstr* s = n->set->strs; s; s = s->next) v[i++] = s;
            for (int a = 1; a < nstr; a++) {   // by length, descending
                struct cstr* x = v[a];
                int b = a;
                while (b > 0 && v[b - 1]->n < x->n) { v[b] = v[b - 1]; b--; }
                v[b] = x;
            }
            uint32_t* exits = (uint32_t*)ojs_sys_malloc(sizeof(uint32_t) * (size_t)(nstr + 1));
            if (!exits) { ojs_sys_free(v); p->failed = 1; return -1; }
            int ne = 0, rc = 0;
            for (i = 0; i < nstr && !rc; i++) {
                uint32_t sp = g->len;
                if (emit(g, R_SPLIT) < 0 || emit(g, 0) < 0 || emit(g, 0) < 0) { rc = -1; break; }
                g->code[sp + 1] = g->len;
                struct cstr* s = v[i];
                for (int k = 0; k < s->n && !rc; k++) {
                    int kk = back ? s->n - 1 - k : k;
                    if (flags_icase(flags)) rc = emit(g, R_CHARI | bflag) < 0 || emit(g, canon(p->unicode, s->cp[kk])) < 0;
                    else rc = emit(g, R_CHAR | bflag) < 0 || emit(g, s->cp[kk]) < 0;
                }
                if (emit(g, R_JMP) < 0) { rc = -1; break; }
                exits[ne++] = g->len;
                if (emit(g, 0) < 0) { rc = -1; break; }
                g->code[sp + 2] = g->len;
            }
            if (!rc) {
                struct rnode single = *n;
                struct cset cs = *n->set;
                cs.strs = 0;
                single.set = &cs;
                if (cs.n) rc = gen_char_atom(g, &single, flags, back);
                else rc = emit(g, R_FAIL);
                for (int k = 0; k < ne; k++) g->code[exits[k]] = g->len;
            }
            ojs_sys_free(v);
            ojs_sys_free(exits);
            return rc;
        }
        return gen_char_atom(g, n, flags, back);
    case RN_BOL: return emit(g, R_BOL) < 0 || emit(g, n->neg) < 0 ? -1 : 0;
    case RN_EOL: return emit(g, R_EOL) < 0 || emit(g, n->neg) < 0 ? -1 : 0;
    case RN_WORDB: return emit(g, R_WORDB) < 0 || emit(g, n->neg | ((p->unicode && flags_icase(flags)) ? 2 : 0)) < 0 ? -1 : 0;
    case RN_SEQ: return gen_seq(g, n->kid, flags, back);
    case RN_ALT: {
        // SPLIT a, next ; a ; JMP end ; next: ...
        uint32_t* exits = 0;
        int ne = 0, cap = 0;
        for (const struct rnode* k = n->kid; k; k = k->next) {
            uint32_t sp = 0;
            if (k->next) {
                sp = g->len;
                if (emit(g, R_SPLIT) < 0 || emit(g, 0) < 0 || emit(g, 0) < 0) return -1;
                g->code[sp + 1] = g->len;
            }
            if (gen(g, k, flags, back) < 0) { ojs_sys_free(exits); return -1; }
            if (k->next) {
                if (ne >= cap) {
                    cap = cap ? cap * 2 : 8;
                    uint32_t* t = (uint32_t*)ojs_sys_realloc(exits, sizeof(uint32_t) * (size_t)cap);
                    if (!t) { ojs_sys_free(exits); p->failed = 1; return -1; }
                    exits = t;
                }
                if (emit(g, R_JMP) < 0) { ojs_sys_free(exits); return -1; }
                exits[ne++] = g->len;
                if (emit(g, 0) < 0) { ojs_sys_free(exits); return -1; }
                g->code[sp + 2] = g->len;
            }
        }
        for (int k = 0; k < ne; k++) g->code[exits[k]] = g->len;
        ojs_sys_free(exits);
        return 0;
    }
    case RN_GROUP:
        if (!n->cap) return gen(g, n->kid, flags, back);
        if (emit(g, R_SAVE) < 0 || emit(g, (uint32_t)(n->cap * 2 + (back ? 1 : 0))) < 0) return -1;
        if (gen(g, n->kid, flags, back) < 0) return -1;
        return emit(g, R_SAVE) < 0 || emit(g, (uint32_t)(n->cap * 2 + (back ? 0 : 1))) < 0 ? -1 : 0;
    case RN_MODS: return gen(g, n->kid, (flags | n->add_flags) & ~n->rm_flags, back);
    case RN_LOOK: {
        uint32_t at = g->len;
        if (emit(g, R_LOOK) < 0 || emit(g, (uint32_t)(n->neg | (n->behind ? 2 : 0))) < 0 || emit(g, 0) < 0 ||
            emit(g, (uint32_t)n->cap_lo) < 0 || emit(g, (uint32_t)n->cap_hi) < 0) return -1;
        if (gen(g, n->kid, flags, n->behind) < 0) return -1;
        if (emit(g, R_LOOK_END) < 0) return -1;
        g->code[at + 2] = g->len;
        return 0;
    }
    case RN_BACKREF: {
        if (emit(g, R_BACKREF | bflag) < 0 || emit(g, (uint32_t)flags_icase(flags)) < 0 || emit(g, (uint32_t)n->nrefs) < 0) return -1;
        for (int i = 0; i < n->nrefs; i++) if (emit(g, (uint32_t)n->refs[i]) < 0) return -1;
        return 0;
    }
    case RN_REPEAT: {
        const struct rnode* k = n->kid;
        if (n->max == 0) return 0;
        if (is_single_char(k)) {
            // R_SIMPLE min max greedy atomlen <atom>
            uint32_t at = g->len;
            if (emit(g, R_SIMPLE | bflag) < 0 || emit(g, n->min) < 0 || emit(g, n->max) < 0 || emit(g, n->greedy) < 0 || emit(g, 0) < 0) return -1;
            uint32_t a0 = g->len;
            if (gen_char_atom(g, single_of(k), flags, 0) < 0) return -1;
            g->code[at + 4] = g->len - a0;
            return 0;
        }
        if (n->min == 1 && n->max == 1) return gen(g, k, flags, back);
        int r = g->nregs++;     // iteration counter
        int pr = g->nregs++;    // position at iteration start
        if (emit(g, R_REG_ZERO) < 0 || emit(g, (uint32_t)r) < 0) return -1;
        uint32_t loop = g->len;
        // R_LOOP r min max greedy body exit
        if (emit(g, R_LOOP) < 0 || emit(g, (uint32_t)r) < 0 || emit(g, n->min) < 0 || emit(g, n->max) < 0 ||
            emit(g, n->greedy) < 0 || emit(g, 0) < 0 || emit(g, 0) < 0) return -1;
        g->code[loop + 5] = g->len;
        if (n->cap_hi > n->cap_lo) {
            if (emit(g, R_RESET) < 0 || emit(g, (uint32_t)(n->cap_lo * 2)) < 0 || emit(g, (uint32_t)(n->cap_hi * 2)) < 0) return -1;
        }
        if (emit(g, R_REG_POS) < 0 || emit(g, (uint32_t)pr) < 0) return -1;
        if (gen(g, k, flags, back) < 0) return -1;
        if (emit(g, R_PROGRESS) < 0 || emit(g, (uint32_t)r) < 0 || emit(g, n->min) < 0 || emit(g, (uint32_t)pr) < 0) return -1;
        if (emit(g, R_REG_INC) < 0 || emit(g, (uint32_t)r) < 0) return -1;
        if (emit(g, R_JMP) < 0 || emit(g, loop) < 0) return -1;
        g->code[loop + 6] = g->len;
        return 0;
    }
    }
    return 0;
}

// ---------------------------------------------------------------- compile entry

int re_parse_flags(const struct str* f) {
    int fl = 0;
    for (uint32_t i = 0; i < str_len(f); i++) {
        uint32_t c = str_at(f, i);
        int b = c == 'g' ? RF_G : c == 'i' ? RF_I : c == 'm' ? RF_M : c == 's' ? RF_S : c == 'u' ? RF_U :
                c == 'y' ? RF_Y : c == 'd' ? RF_D : c == 'v' ? RF_V : 0;
        if (!b || (fl & b)) return -1;
        fl |= b;
    }
    if ((fl & RF_U) && (fl & RF_V)) return -1;
    return fl;
}

static void free_arena(struct rp* p) {
    for (struct arena* a = p->arena; a;) {
        struct arena* n = a->next;
        ojs_sys_free(a);
        a = n;
    }
    p->arena = 0;
}

// resolve named backrefs; check duplicate names (allowed only in different alternatives)
static int alt_disjoint(const struct rnode* root, int a, int b);
static int resolve_names(struct rp* p, struct rnode* root) {
    for (int i = 1; i <= p->ncaps; i++) {
        if (!p->names || i >= p->names_cap || !p->names[i]) continue;
        for (int j = i + 1; j <= p->ncaps; j++) {
            if (j >= p->names_cap || !p->names[j] || strcmp(p->names[i], p->names[j])) continue;
            if (!alt_disjoint(root, i, j)) { fail(p, "Duplicate capture group name"); return -1; }
            p->dup_names = 1;
        }
    }
    for (struct rnode* n = p->backrefs; n; n = n->next_ref) {
        int cnt = 0;
        for (int i = 1; i <= p->ncaps; i++) if (i < p->names_cap && p->names[i] && !strcmp(p->names[i], n->refname)) cnt++;
        if (!cnt) { fail(p, "Invalid named capture referenced"); return -1; }
        n->refs = (int*)aalloc(p, sizeof(int) * (size_t)cnt);
        if (!n->refs) return -1;
        for (int i = 1; i <= p->ncaps; i++) if (i < p->names_cap && p->names[i] && !strcmp(p->names[i], n->refname)) n->refs[n->nrefs++] = i;
    }
    return 0;
}

// do groups a and b sit in different alternatives of some disjunction?
static int contains_cap(const struct rnode* n, int c) {
    if (n->type == RN_GROUP && n->cap == c) return 1;
    for (const struct rnode* k = n->kid; k; k = k->next) if (contains_cap(k, c)) return 1;
    return 0;
}
static int alt_disjoint(const struct rnode* n, int a, int b) {
    if (n->type == RN_ALT) {
        const struct rnode* ka = 0;
        const struct rnode* kb = 0;
        for (const struct rnode* k = n->kid; k; k = k->next) {
            if (contains_cap(k, a)) ka = k;
            if (contains_cap(k, b)) kb = k;
        }
        if (ka && kb && ka != kb) return 1;
        if (ka && ka == kb) return alt_disjoint(ka, a, b);
        return 0;
    }
    for (const struct rnode* k = n->kid; k; k = k->next)
        if (contains_cap(k, a) && contains_cap(k, b)) return alt_disjoint(k, a, b);
    return 0;
}

struct re_prog* re_compile(ojs* J, const struct str* pat, int flags, char* err, int errcap) {
    struct rp P;
    memset(&P, 0, sizeof P);
    P.J = J;
    P.flags = flags;
    P.unicode = (flags & (RF_U | RF_V)) != 0;
    P.vmode = (flags & RF_V) != 0;
    P.err = err;
    P.errcap = errcap;
    err[0] = 0;
    // pattern as code points / units
    uint32_t n = str_len(pat), k = 0;
    uint32_t* s = (uint32_t*)ojs_sys_malloc(sizeof(uint32_t) * (n ? n : 1));
    if (!s) { ojs_snprintf(err, errcap, "out of memory"); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(pat, i);
        if (P.unicode && c >= 0xD800 && c <= 0xDBFF && i + 1 < n && str_at(pat, i + 1) >= 0xDC00 && str_at(pat, i + 1) <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (str_at(pat, i + 1) - 0xDC00);
            i++;
        }
        s[k++] = c;
    }
    P.s = s;
    P.n = k;
    count_groups(&P);
    if (P.unicode) P.named = P.named;   // \k is always special in unicode mode
    struct rnode* root = parse_disjunction(&P);
    if (!P.failed && P.i < P.n) fail(&P, peek(&P) == ')' ? "unmatched ')'" : "unexpected character");
    if (!P.failed) resolve_names(&P, root);
    struct re_prog* prog = 0;
    if (!P.failed) {
        struct cg G;
        memset(&G, 0, sizeof G);
        G.p = &P;
        if (emit(&G, R_SAVE) == 0 && emit(&G, 0) == 0 && gen(&G, root, flags, 0) == 0 &&
            emit(&G, R_SAVE) == 0 && emit(&G, 1) == 0 && emit(&G, R_MATCH) == 0) {
            // name table
            size_t names_len = 0;
            for (int i = 0; i <= P.ncaps; i++) {
                const char* nm = (P.names && i < P.names_cap) ? P.names[i] : 0;
                names_len += (nm ? strlen(nm) : 0) + 1;
            }
            size_t total = sizeof(struct re_prog) + (size_t)G.len * 4 + names_len;
            prog = (struct re_prog*)bytes_new(J, total);
            if (prog) {
                prog->flags = (uint32_t)flags;
                prog->ncaps = (uint32_t)P.ncaps + 1;
                prog->nregs = (uint32_t)G.nregs;
                prog->code_len = G.len;
                prog->names_len = (uint32_t)names_len;
                prog->has_dup_names = (uint32_t)P.dup_names;
                memcpy(prog->code, G.code, (size_t)G.len * 4);
                char* nt = (char*)(prog->code + G.len);
                for (int i = 0; i <= P.ncaps; i++) {
                    const char* nm = (P.names && i < P.names_cap) ? P.names[i] : 0;
                    size_t l = nm ? strlen(nm) : 0;
                    if (l) memcpy(nt, nm, l);
                    nt[l] = 0;
                    nt += l + 1;
                }
            } else {
                take_exc(J);
                ojs_snprintf(err, errcap, "out of memory");
            }
        }
        ojs_sys_free(G.code);
    }
    free_arena(&P);
    ojs_sys_free(s);
    return prog;
}

const char* re_group_name(const struct re_prog* p, uint32_t i) {
    const char* nt = (const char*)(p->code + p->code_len);
    for (uint32_t k = 0; k < i; k++) nt += strlen(nt) + 1;
    return *nt ? nt : 0;
}

int regexp_check_syntax(ojs* J, struct str* body, struct str* flags, char* err, int errcap) {
    int fl = re_parse_flags(flags);
    if (fl < 0) { ojs_snprintf(err, errcap, "Invalid regular expression flags"); return -1; }
    int saved = J->gc_disabled;
    struct re_prog* p = re_compile(J, body, fl, err, errcap);
    J->gc_disabled = saved;
    return p ? 0 : -1;
}

// ---------------------------------------------------------------- matcher

enum { BT_CHOICE, BT_CAP, BT_REG, BT_SIMPLE, BT_LAZY, BT_BARRIER };

struct bt {
    uint32_t kind;
    uint32_t pc;
    int32_t a, b, c;
};

struct m {
    ojs* J;
    const struct re_prog* p;
    const struct str* s;
    int wide;
    int32_t n;
    int unicode, sticky_dummy;
    int32_t* caps;
    int32_t* regs;
    struct bt* stk;
    uint32_t sp, cap;
    uint32_t steps;
    int error;
};

static inline uint32_t U(struct m* m, int32_t i) { return m->wide ? m->s->u.c16[i] : m->s->u.c8[i]; }

// code point forward from pos (or -1); *len units
static inline int32_t cp_fwd(struct m* m, int32_t pos, int* len) {
    if (pos >= m->n) return -1;
    uint32_t c = U(m, pos);
    *len = 1;
    if (m->unicode && c >= 0xD800 && c <= 0xDBFF && pos + 1 < m->n) {
        uint32_t d = U(m, pos + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) { *len = 2; return (int32_t)(0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00)); }
    }
    return (int32_t)c;
}

static inline int32_t cp_back(struct m* m, int32_t pos, int* len) {
    if (pos <= 0) return -1;
    uint32_t c = U(m, pos - 1);
    *len = 1;
    if (m->unicode && c >= 0xDC00 && c <= 0xDFFF && pos - 2 >= 0) {
        uint32_t h = U(m, pos - 2);
        if (h >= 0xD800 && h <= 0xDBFF) { *len = 2; return (int32_t)(0x10000 + ((h - 0xD800) << 10) + (c - 0xDC00)); }
    }
    return (int32_t)c;
}

static int push(struct m* m, uint32_t kind, uint32_t pc, int32_t a, int32_t b, int32_t c) {
    if (m->sp >= m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 256;
        if (nc > (64u << 20)) { throw_range(m->J, "Regular expression too complex"); m->error = 1; return -1; }
        struct bt* t = (struct bt*)ojs_sys_realloc(m->stk, (size_t)nc * sizeof(struct bt));
        if (!t) { throw_oom(m->J); m->error = 1; return -1; }
        m->stk = t;
        m->cap = nc;
    }
    struct bt* e = &m->stk[m->sp++];
    e->kind = kind;
    e->pc = pc;
    e->a = a;
    e->b = b;
    e->c = c;
    return 0;
}

static int set_cap(struct m* m, uint32_t slot, int32_t v) {
    if (push(m, BT_CAP, slot, m->caps[slot], 0, 0) < 0) return -1;
    m->caps[slot] = v;
    return 0;
}

static int set_reg(struct m* m, uint32_t r, int32_t v) {
    if (push(m, BT_REG, r, m->regs[r], 0, 0) < 0) return -1;
    m->regs[r] = v;
    return 0;
}

static int is_line_term(int32_t c) { return c == 10 || c == 13 || c == 0x2028 || c == 0x2029; }

static int is_word(struct m* m, int32_t pos, int ext) {
    if (pos < 0 || pos >= m->n) return 0;
    uint32_t c = U(m, pos);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return 1;
    return ext && (c == 0x17F || c == 0x212A);
}

static int set_has(const uint32_t* r, uint32_t n, uint32_t c) {
    int lo = 0, hi = (int)n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (c < r[2 * mid]) hi = mid - 1;
        else if (c > r[2 * mid + 1]) lo = mid + 1;
        else return 1;
    }
    return 0;
}

// one character-class atom at pos in direction; new position or -1
static int32_t atom_match(struct m* m, const uint32_t* a, int32_t pos, int back) {
    int len;
    int32_t c = back ? cp_back(m, pos, &len) : cp_fwd(m, pos, &len);
    if (c < 0) return -1;
    switch (a[0] & 0x7F) {
    case R_CHAR: if ((uint32_t)c != a[1]) return -1; break;
    case R_CHARI: if ((uint32_t)c != a[1] && canon(m->unicode, (uint32_t)c) != a[1]) return -1; break;
    case R_ANY: if (!a[1] && is_line_term(c)) return -1; break;
    case R_SET: case R_SETI: {
        int in = c < 128 ? (int)((a[3 + (c >> 5)] >> (c & 31)) & 1)
                         : set_has(a + 7, a[2], (a[0] & 0x7F) == R_SETI ? canon(m->unicode, (uint32_t)c) : (uint32_t)c);
        if (in == (int)a[1]) return -1;
        break;
    }
    default: return -1;
    }
    return back ? pos - len : pos + len;
}

static uint32_t atom_len(const uint32_t* a) {
    switch (a[0] & 0x7F) {
    case R_SET: case R_SETI: return 7 + a[2] * 2;
    default: return 2;
    }
}

static int backref(struct m* m, const uint32_t* code, uint32_t pc, int32_t* pos, int back) {
    int icase = (int)code[pc + 1];
    uint32_t nref = code[pc + 2];
    int32_t st = -1, en = -1;
    for (uint32_t i = 0; i < nref; i++) {
        uint32_t g = code[pc + 3 + i];
        if (m->caps[2 * g] >= 0 && m->caps[2 * g + 1] >= 0) { st = m->caps[2 * g]; en = m->caps[2 * g + 1]; break; }
    }
    if (st < 0) return 1;   // unmatched group: empty
    int32_t len = en - st;
    if (back) {
        if (*pos - len < 0) return 0;
        int32_t from = *pos - len;
        for (int32_t k = 0; k < len; k++) {
            uint32_t x = U(m, st + k), y = U(m, from + k);
            if (x != y && !(icase && canon(m->unicode, x) == canon(m->unicode, y))) return 0;
        }
        *pos = from;
        return 1;
    }
    if (*pos + len > m->n) return 0;
    if (!icase) {
        for (int32_t k = 0; k < len; k++) if (U(m, st + k) != U(m, *pos + k)) return 0;
    } else {
        // compare code point by code point
        int32_t i = st, j = *pos;
        while (i < en) {
            int l1, l2;
            int32_t x = cp_fwd(m, i, &l1), y = cp_fwd(m, j, &l2);
            if (y < 0) return 0;
            if (x != y && canon(m->unicode, (uint32_t)x) != canon(m->unicode, (uint32_t)y)) return 0;
            i += l1;
            j += l2;
        }
        *pos = j;
        return 1;
    }
    *pos += len;
    return 1;
}

static int interrupt(struct m* m) {
    ojs* J = m->J;
    if (!J->interrupt) return 0;
    if (J->interrupt(J, J->interrupt_op)) {
        throw_error(J, NE_RANGE, "interrupted");
        J->uncatchable = 1;
        m->error = 1;
        return -1;
    }
    return 0;
}

// run from pc at pos until R_MATCH / R_LOOK_END; backtracking stops at
// stack height `base`. 1 matched (*endpos), 0 failed, -1 error
static int run(struct m* m, uint32_t pc, int32_t pos, uint32_t base, int32_t* endpos) {
    const uint32_t* code = m->p->code;
    for (;;) {
        if (++m->steps >= 100000) {
            m->steps = 0;
            if (interrupt(m) < 0) return -1;
        }
        uint32_t op = code[pc];
        int back = (op & R_BACK) != 0;
        switch (op & 0x7F) {
        case R_CHAR: case R_CHARI: case R_ANY: case R_SET: case R_SETI: {
            int32_t np = atom_match(m, code + pc, pos, back);
            if (np < 0) goto fail;
            pos = np;
            pc += atom_len(code + pc);
            continue;
        }
        case R_STR: {
            uint32_t n = code[pc + 1];
            if ((uint32_t)pos + n > (uint32_t)m->n) goto fail;
            const uint32_t* cs = code + pc + 2;
            uint32_t i = 0;
            if (m->wide) { const uint16_t* u = m->s->u.c16 + pos; while (i < n && u[i] == cs[i]) i++; }
            else { const uint8_t* u = m->s->u.c8 + pos; while (i < n && u[i] == cs[i]) i++; }
            if (i < n) goto fail;
            pos += (int32_t)n;
            pc += 2 + n;
            continue;
        }
        case R_BOL:
            if (pos == 0 || (code[pc + 1] && is_line_term((int32_t)U(m, pos - 1)))) { pc += 2; continue; }
            goto fail;
        case R_EOL:
            if (pos == m->n || (code[pc + 1] && is_line_term((int32_t)U(m, pos)))) { pc += 2; continue; }
            goto fail;
        case R_WORDB: {
            int ext = (code[pc + 1] & 2) != 0;
            int a = is_word(m, pos - 1, ext), b = is_word(m, pos, ext);
            int at = a != b;
            if (at == !(code[pc + 1] & 1)) { pc += 2; continue; }
            goto fail;
        }
        case R_SAVE:
            if (set_cap(m, code[pc + 1], pos) < 0) return -1;
            pc += 2;
            continue;
        case R_RESET:
            for (uint32_t s = code[pc + 1]; s < code[pc + 2]; s++)
                if (m->caps[s] != -1 && set_cap(m, s, -1) < 0) return -1;
            pc += 3;
            continue;
        case R_SPLIT:
            if (push(m, BT_CHOICE, code[pc + 2], pos, 0, 0) < 0) return -1;
            pc = code[pc + 1];
            continue;
        case R_JMP: pc = code[pc + 1]; continue;
        case R_FAIL: goto fail;
        case R_REG_ZERO:
            if (set_reg(m, code[pc + 1], 0) < 0) return -1;
            pc += 2;
            continue;
        case R_REG_POS:
            if (set_reg(m, code[pc + 1], pos) < 0) return -1;
            pc += 2;
            continue;
        case R_REG_INC:
            if (set_reg(m, code[pc + 1], m->regs[code[pc + 1]] + 1) < 0) return -1;
            pc += 2;
            continue;
        case R_LOOP: {
            uint32_t cnt = (uint32_t)m->regs[code[pc + 1]];
            uint32_t mn = code[pc + 2], mx = code[pc + 3];
            uint32_t body = code[pc + 5], exitp = code[pc + 6];
            if (cnt < mn) { pc = body; continue; }
            if (mx != 0xFFFFFFFFu && cnt >= mx) { pc = exitp; continue; }
            if (code[pc + 4]) {   // greedy: body first
                if (push(m, BT_CHOICE, exitp, pos, 0, 0) < 0) return -1;
                pc = body;
            } else {
                if (push(m, BT_CHOICE, body, pos, 0, 0) < 0) return -1;
                pc = exitp;
            }
            continue;
        }
        case R_PROGRESS: {
            uint32_t cnt = (uint32_t)m->regs[code[pc + 1]];
            if (cnt >= code[pc + 2] && pos == m->regs[code[pc + 3]]) goto fail;
            pc += 4;
            continue;
        }
        case R_SIMPLE: {
            uint32_t mn = code[pc + 1], mx = code[pc + 2], greedy = code[pc + 3], alen = code[pc + 4];
            const uint32_t* atom = code + pc + 5;
            uint32_t next = pc + 5 + alen;
            int32_t p0 = pos;
            uint32_t cnt = 0;
            while (cnt < mn) {
                int32_t np = atom_match(m, atom, pos, back);
                if (np < 0) goto fail;
                pos = np;
                cnt++;
            }
            if (greedy) {
                int32_t minpos = pos;
                while (cnt < mx) {
                    int32_t np = atom_match(m, atom, pos, back);
                    if (np < 0) break;
                    pos = np;
                    cnt++;
                    if (!(cnt & 0xFFFF) && interrupt(m) < 0) return -1;
                }
                if (pos != minpos && push(m, BT_SIMPLE, pc, minpos, pos, 0) < 0) return -1;
            } else if (cnt < mx) {
                if (push(m, BT_LAZY, pc, pos, (int32_t)cnt, 0) < 0) return -1;
            }
            (void)p0;
            pc = next;
            continue;
        }
        case R_BACKREF:
            if (!backref(m, code, pc, &pos, back)) goto fail;
            pc += 3 + code[pc + 2];
            continue;
        case R_LOOK: {
            uint32_t kind = code[pc + 1], end = code[pc + 2];
            uint32_t lo = code[pc + 3] * 2, hi = code[pc + 4] * 2;
            uint32_t h = m->sp;
            int32_t ep;
            int r = run(m, pc + 5, pos, h, &ep);
            if (r < 0) return -1;
            int neg = kind & 1;
            if (r == 1) {
                // atomic: drop the body's choice points, keep its capture undo records
                uint32_t w = h;
                for (uint32_t i = h; i < m->sp; i++) {
                    uint32_t k = m->stk[i].kind;
                    if (k == BT_CAP || k == BT_REG) m->stk[w++] = m->stk[i];
                }
                m->sp = w;
                if (neg) {
                    // undo everything the body did, then fail
                    while (m->sp > h) {
                        struct bt* e = &m->stk[--m->sp];
                        if (e->kind == BT_CAP) m->caps[e->pc] = e->a;
                        else if (e->kind == BT_REG) m->regs[e->pc] = e->a;
                    }
                    goto fail;
                }
            } else {
                if (!neg) goto fail;
                // negative lookaround succeeded: its captures are undefined
                for (uint32_t s = lo; s < hi; s++) if (m->caps[s] != -1 && set_cap(m, s, -1) < 0) return -1;
            }
            pc = end;
            continue;
        }
        case R_LOOK_END:
        case R_MATCH:
            *endpos = pos;
            return 1;
        default:
            goto fail;
        }
    fail:
        for (;;) {
            if (m->sp <= base) return 0;
            struct bt* e = &m->stk[--m->sp];
            switch (e->kind) {
            case BT_CAP: m->caps[e->pc] = e->a; continue;
            case BT_REG: m->regs[e->pc] = e->a; continue;
            case BT_CHOICE: pc = e->pc; pos = e->a; goto resume;
            case BT_SIMPLE: {
                // give back one atom
                uint32_t spc = e->pc;
                int bk = (code[spc] & R_BACK) != 0;
                int32_t minpos = e->a, cur = e->b;
                int len = 0;
                if (bk) { cp_fwd(m, cur, &len); cur += len; }
                else { cp_back(m, cur, &len); cur -= len; }
                if ((!bk && cur > minpos) || (bk && cur < minpos)) {
                    e->b = cur;
                    m->sp++;   // keep the entry
                } else if (cur != minpos) continue;
                pc = spc + 5 + code[spc + 4];
                pos = cur;
                goto resume;
            }
            case BT_LAZY: {
                uint32_t spc = e->pc;
                int bk = (code[spc] & R_BACK) != 0;
                uint32_t mx = code[spc + 2];
                int32_t np = atom_match(m, code + spc + 5, e->a, bk);
                if (np < 0) continue;
                uint32_t cnt = (uint32_t)e->b + 1;
                if (cnt < mx) { e->a = np; e->b = (int32_t)cnt; m->sp++; }
                pc = spc + 5 + code[spc + 4];
                pos = np;
                goto resume;
            }
            default: continue;
            }
        }
    resume:;
    }
}

int re_exec(ojs* J, const struct re_prog* p, const struct str* s, uint32_t start, int32_t* caps, int sticky) {
    struct m M;
    memset(&M, 0, sizeof M);
    M.J = J;
    M.p = p;
    M.s = s;
    M.wide = str_wide(s);
    M.n = (int32_t)str_len(s);
    M.unicode = (p->flags & (RF_U | RF_V)) != 0;
    M.caps = caps;
    for (uint32_t i = 0; i < p->ncaps * 2; i++) caps[i] = -1;
    int32_t regs_small[32];
    M.regs = p->nregs <= 32 ? regs_small : (int32_t*)ojs_sys_malloc(sizeof(int32_t) * p->nregs);
    if (!M.regs) { throw_oom(J); return -1; }
    for (uint32_t i = 0; i < p->nregs; i++) M.regs[i] = 0;
    // /u and /v match code points: a start inside a surrogate pair is the pair's start
    if (M.unicode && start > 0 && (int32_t)start < M.n && (U(&M, (int32_t)start) & 0xFC00) == 0xDC00 &&
        (U(&M, (int32_t)start - 1) & 0xFC00) == 0xD800) start--;
    int32_t ep;
    int r = 0;
    // a leading literal character lets the scan skip impossible starts
    uint32_t lead = 0;
    int has_lead = 0;
    if (!sticky && p->code[2] == R_CHAR && !(p->flags & RF_I)) { lead = p->code[3]; has_lead = lead < 0xD800; }
    if (!sticky && p->code[2] == R_STR) { lead = p->code[4]; has_lead = 1; }   // (always case-sensitive, BMP)
    for (int32_t at = (int32_t)start; at <= M.n; ) {
        if (has_lead) {
            while (at < M.n && U(&M, at) != lead) at++;
            if (at >= M.n) break;
        }
        r = run(&M, 0, at, 0, &ep);
        if (r != 0 || sticky) break;
        // AdvanceStringIndex
        if (M.unicode && at + 1 < M.n && U(&M, at) >= 0xD800 && U(&M, at) <= 0xDBFF && U(&M, at + 1) >= 0xDC00 && U(&M, at + 1) <= 0xDFFF) at += 2;
        else at++;
        M.sp = 0;
    }
    if (M.regs != regs_small) ojs_sys_free(M.regs);
    ojs_sys_free(M.stk);
    if (r < 0 || M.error) return -1;
    return r;
}

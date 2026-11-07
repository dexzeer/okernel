// intl.c — runtime helpers for the Intl builtins (the JS surface lives in
// b_intl.c, the CLDR tables in intl_data.c). Everything here is ours: the
// BCP 47 grammar, canonicalization and matching of ECMA-402, plural-rule
// evaluation over the CLDR rule strings, decimal rounding on the exact
// expansion of a double (num.c), and the CLDR number/date pattern engines.

#include "ojs_int.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "intl.h"

extern int ojs_sys_tz_offset(double utc_ms, int is_local);   // embedder hook (weak default in b_date.c)
extern double km_floor(double);                              // src/kmath.c



// the engine builds freestanding (no stdio/stdlib): tiny local parsers
static int c_atoi(const char* s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static double c_atof(const char* s) {
    double v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double k = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * k; k *= 0.1; }
    }
    return neg ? -v : v;
}

static double intl_fmod(double a, double m) {
    double r = a - km_floor(a / m) * m;
    return r;
}

static int is_alpha(unsigned c) { return (c | 32) - 'a' < 26u; }
static int is_dig(unsigned c) { return c - '0' < 10u; }
static int is_alnum(unsigned c) { return is_alpha(c) || is_dig(c); }

jv intl_pool_str(ojs* J, unsigned off) {
    struct str* s = str_from_cstr(J, IP(off));
    return s ? jv_from_str(s) : JV_EXC;
}

static double p10(int e) {
    static const double t[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
                                  1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    return (e >= 0 && e < 23) ? t[e] : 1.0;
}

// ---------------------------------------------------------------- tag grammar

struct itag {
    char lang[9];
    char script[5];
    char reg[4];
    char var[6][9];
    unsigned char nvar;
    char uattr[5][9];
    unsigned char nuattr;
    char ukey[8][4];
    char uval[8][18];
    unsigned char nukw;
    char other[80];             // non-u extensions, canonical text incl. singleton
    unsigned char nother;
    unsigned char has_u;
};

// parse a language tag (the BCP 47 grammar ECMA-402 uses); 0 ok, -1 invalid
static int tag_parse(const char* s, unsigned n, struct itag* t) {
    memset(t, 0, sizeof *t);
    if (n > 128) return -1;
    char tok[9];
    unsigned tl = 0, ntok = 0;
    int done_lang = 0, done_script = 0, done_reg = 0;
    char singleton = 0;
    int curkw = -1;
    unsigned short keybits = 0;

    #define FAIL() return -1
    #define FLUSH() do { \
        if (tl == 0) FAIL(); \
        tok[tl] = 0; \
        if (ntok > 24) FAIL(); \
        if (!done_lang) { \
            if (tl < 2 || tl > 8) FAIL(); \
            for (unsigned i = 0; i < tl; i++) if (!is_alpha((unsigned char)tok[i])) FAIL(); \
            for (unsigned k = 0; k < tl; k++) t->lang[k] = (char)(tok[k] | 32); \
            t->lang[tl] = 0; done_lang = 1; \
        } else if (singleton == 'u') { \
            for (unsigned i = 0; i < tl; i++) if (!is_alnum((unsigned char)tok[i])) FAIL(); \
            if (tl == 2) { \
                if (t->nukw >= 8) FAIL(); \
                unsigned bit = (unsigned)(unsigned char)(tok[0] | 32) * 256u + (unsigned char)(tok[1] | 32); \
                if (keybits & (1u << (bit & 15))) FAIL();   /* duplicate keyword */ \
                keybits |= (unsigned short)(1u << (bit & 15)); \
                curkw = t->nukw++; \
                t->ukey[curkw][0] = (char)(tok[0] | 32); t->ukey[curkw][1] = (char)(tok[1] | 32); \
                t->ukey[curkw][2] = 0; t->uval[curkw][0] = 0; \
            } else if (curkw < 0) { \
                if (tl < 3 || tl > 8 || t->nuattr >= 5) FAIL(); \
                for (unsigned i = 0; i < tl; i++) t->uattr[t->nuattr][i] = (char)(tok[i] | 32); \
                t->uattr[t->nuattr][tl] = 0; t->nuattr++; \
            } else { \
                char* v = t->uval[curkw]; \
                unsigned vl = (unsigned)strlen(v); \
                if (tl < 3 || vl + 1 + tl >= 18) FAIL(); \
                if (vl) v[vl++] = '-'; \
                for (unsigned i = 0; i < tl; i++) v[vl + i] = (char)(tok[i] | 32); \
                v[vl + tl] = 0; \
            } \
        } else if (singleton) { \
            if (t->nother + tl + 3 > (int)sizeof t->other) FAIL(); \
            t->other[t->nother++] = '-'; t->other[t->nother++] = (char)(singleton | 32); \
            for (unsigned i = 0; i < tl; i++) t->other[t->nother++] = (char)(tok[i] | 32); \
        } else { \
            for (unsigned i = 0; i < tl; i++) if (!is_alnum((unsigned char)tok[i])) FAIL(); \
            int a4 = (tl == 4 && is_alpha((unsigned char)tok[0])); \
            int regish = (tl == 2 && is_alpha((unsigned char)tok[0])) || (tl == 3 && is_dig((unsigned char)tok[0])); \
            if (!done_script && a4) { \
                for (unsigned i = 0; i < 4; i++) t->script[i] = (char)(tok[i] | 32); \
                t->script[4] = 0; done_script = 1; \
            } else if (!done_reg && regish) { \
                for (unsigned i = 0; i < tl; i++) t->reg[i] = (char)(tok[i] | 32); \
                t->reg[tl] = 0; done_reg = 1; \
            } else if ((tl >= 5 && tl <= 8) || (tl == 4 && is_dig((unsigned char)tok[0]))) { \
                for (unsigned i = 0; i < tl; i++) tok[i] = (char)(tok[i] | 32); \
                for (unsigned i = 0; i < t->nvar; i++) if (strcmp(t->var[i], tok) == 0) FAIL(); \
                if (t->nvar >= 6) FAIL(); \
                memcpy(t->var[t->nvar++], tok, (size_t)tl + 1); \
            } else FAIL(); \
        } \
        tl = 0; ntok++; \
    } while (0)

    for (unsigned i = 0; i <= n; i++) {
        char c = i < n ? s[i] : '-';
        if (c == '_') c = '-';
        if (c == '-') {
            if (tl == 0) { if (i == n) break; FAIL(); }
            if (tl == 1) {
                char sc = (char)(tok[0] | 32);
                if (sc >= 'a' && sc <= 'z') {
                    if (singleton == sc) FAIL();          // duplicate singleton
                    singleton = sc;
                    if (sc == 'u') t->has_u = 1;
                    curkw = -1;
                    tl = 0; ntok++;
                    if (i == n) break;
                    continue;
                }
            }
            FLUSH();
            if (i == n) break;
        } else {
            if (tl >= 8) FAIL();
            if (!is_alnum((unsigned char)c)) FAIL();
            tok[tl++] = c;
        }
    }
    if (tl) { if (singleton) { singleton = 0; tl = 0; } else FLUSH(); }
    if (!done_lang) FAIL();
    return 0;
    #undef FAIL
    #undef FLUSH
}

static int alias_find(const struct intl_alias* tab, unsigned n, const char* k) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(tab[mid].from, k);
        if (c == 0) return (int)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

static void alias_apply(struct itag* t, const char* rep) {
    struct itag r;
    if (tag_parse(rep, (unsigned)strlen(rep), &r) < 0) return;
    strcpy(t->lang, r.lang);
    if (r.script[0]) strcpy(t->script, r.script);
    if (r.reg[0]) strcpy(t->reg, r.reg);
}

// serialize canonically: case fixes, CLDR aliases, sorted u keywords
static int tag_canonical(struct itag* t, char* out, unsigned cap) {
    char key[24];
    ojs_snprintf(key, sizeof key, "%s", t->lang);
    int a = alias_find(intl_alias_lang, intl_nalias_lang, key);
    if (a < 0 && t->nvar && strlen(t->var[0]) == 3) {
        // extlang form ("zh-hakka"): the alias table carries the replacement
        ojs_snprintf(key, sizeof key, "%s-%s", t->lang, t->var[0]);
        a = alias_find(intl_alias_lang, intl_nalias_lang, key);
        if (a >= 0) {
            for (unsigned i = 1; i < t->nvar; i++) memcpy(t->var[i - 1], t->var[i], sizeof t->var[0]);
            t->nvar--;
        }
    }
    if (a >= 0) alias_apply(t, intl_alias_lang[a].to);
    if (t->script[0]) {
        a = alias_find(intl_alias_scr, intl_nalias_scr, t->script);
        if (a >= 0) ojs_snprintf(t->script, sizeof t->script, "%s", intl_alias_scr[a].to);
    }
    if (t->reg[0]) {
        a = alias_find(intl_alias_terr, intl_nalias_terr, t->reg);
        if (a >= 0) {
            const char* rep = intl_alias_terr[a].to;
            const char* dash = strchr(rep, '-');
            size_t rl = dash ? (size_t)(dash - rep) : strlen(rep);
            if (rl >= sizeof t->reg) rl = sizeof t->reg - 1;
            memcpy(t->reg, rep, rl);
            t->reg[rl] = 0;
        }
    }
    for (char* p = t->lang; *p; p++) *p = (char)(*p | 32);
    if (t->script[0]) {
        t->script[0] = (char)(t->script[0] & ~32);
        for (char* p = t->script + 1; *p; p++) *p = (char)(*p | 32);
    }
    for (char* p = t->reg; *p; p++) *p = (char)(*p & ~32);

    char* o = out;
    char* end = out + cap - 1;
    #define PUTS(qs) do { for (const char* q = (qs); *q; q++) { if (o >= end) return -1; *o++ = *q; } } while (0)
    #define PUTC(cc) do { if (o >= end) return -1; *o++ = (char)(cc); } while (0)
    PUTS(t->lang);
    if (t->script[0]) { PUTC('-'); PUTS(t->script); }
    if (t->reg[0]) { PUTC('-'); PUTS(t->reg); }
    for (unsigned i = 0; i < t->nvar; i++) { PUTC('-'); PUTS(t->var[i]); }
    if (t->has_u) {
        PUTC('-'); PUTC('u');
        for (unsigned i = 0; i < t->nuattr; i++)
            for (unsigned j = i + 1; j < t->nuattr; j++)
                if (strcmp(t->uattr[i], t->uattr[j]) > 0) {
                    char tmp[9];
                    memcpy(tmp, t->uattr[i], 9);
                    memcpy(t->uattr[i], t->uattr[j], 9);
                    memcpy(t->uattr[j], tmp, 9);
                }
        for (unsigned i = 0; i < t->nuattr; i++) { PUTC('-'); PUTS(t->uattr[i]); }
        unsigned char used[8] = { 0 };
        for (unsigned k = 0; k < t->nukw; k++) {
            int best = -1;
            for (unsigned i = 0; i < t->nukw; i++)
                if (!used[i] && (best < 0 || strcmp(t->ukey[i], t->ukey[best]) < 0)) best = (int)i;
            if (best < 0) break;
            used[best] = 1;
            if (t->uval[best][0] && strcmp(t->uval[best], "true") == 0) continue;
            PUTC('-'); PUTS(t->ukey[best]);
            if (t->uval[best][0]) { PUTC('-'); PUTS(t->uval[best]); }
        }
    }
    if (t->nother) PUTS(t->other);
    *o = 0;
    return (int)(o - out);
    #undef PUTS
    #undef PUTC
}

int intl_canonicalize(const char* src, unsigned len, char* out, unsigned cap) {
    struct itag t;
    if (tag_parse(src, len, &t) < 0) return -1;
    return tag_canonical(&t, out, cap);
}

// read a u-keyword value from a canonical tag ("en-u-nu-arab" -> "arab");
// multi-subtag values ("islamic-civil") are collected until the next 2-letter key
int intl_tag_keyword(const char* tag, const char* key, char* out, unsigned cap) {
    out[0] = 0;
    size_t kl = strlen(key);
    const char* p = strstr(tag, "-u-");
    if (!p) return 0;
    p += 3;
    while (*p) {
        const char* q = strchr(p, '-');
        size_t l = q ? (size_t)(q - p) : strlen(p);
        if (l == kl && memcmp(p, key, kl) == 0) {
            size_t vl = 0;
            const char* w = q ? q + 1 : p + l;
            while (*w) {
                const char* wq = strchr(w, '-');
                size_t wl = wq ? (size_t)(wq - w) : strlen(w);
                if (wl == 2) break;   // next keyword
                if (vl + wl + 2 >= cap) return 0;
                if (vl) out[vl++] = '-';
                memcpy(out + vl, w, wl);
                vl += wl;
                out[vl] = 0;
                if (!wq) break;
                w = wq + 1;
            }
            return vl ? (int)vl : -1;   // -1: keyword present without a value
        }
        // skip this keyword and its value
        if (l == 2) {
            const char* w = q ? q + 1 : p + l;
            while (*w) {
                const char* wq = strchr(w, '-');
                size_t wl = wq ? (size_t)(wq - w) : strlen(w);
                if (wl == 2) { p = w; break; }
                if (!wq) { p = w + wl; break; }
                w = wq + 1;
            }
            if (*w && w == p) break;
            if (p == w) break;
            continue;
        }
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

// ---------------------------------------------------------------- matching

static int data_find(const char* tag, unsigned len) {
    char buf[24];
    if (len == 0 || len >= sizeof buf) return -1;
    memcpy(buf, tag, len);
    buf[len] = 0;
    unsigned lo = 0, hi = intl_nloc;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(intl_locales[mid].tag, buf);
        if (c == 0) return (int)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

int intl_lookup_locale(const char* tag, unsigned len) { return data_find(tag, len); }
const char* intl_loc_tag(unsigned li) { return intl_locales[li].tag; }

static int en_index(void) {
    static int eni = -2;
    if (eni == -2) eni = data_find("en", 2);
    return eni;
}

// BestAvailableLocale over the covered set; falls back to "en"
int intl_resolve(const char* tag, unsigned len) {
    char buf[128];
    if (len >= sizeof buf) return en_index();
    memcpy(buf, tag, len);
    buf[len] = 0;
    while (*buf) {
        int i = data_find(buf, (unsigned)strlen(buf));
        if (i >= 0) return i;
        char* cut = strrchr(buf, '-');
        if (!cut) break;
        *cut = 0;
    }
    return en_index();
}

int intl_maximize(const char* tag, unsigned len, char* out, unsigned cap) {
    char canon[128];
    if (intl_canonicalize(tag, len, canon, sizeof canon) < 0) return -1;
    struct itag c;
    if (tag_parse(canon, (unsigned)strlen(canon), &c) < 0) return -1;
    char key[24];
    const struct intl_likely* hit = 0;
    for (int pass = 0; pass < 3 && !hit; pass++) {
        if (pass == 0 && c.script[0] && c.reg[0]) ojs_snprintf(key, sizeof key, "%s-%s-%s", c.lang, c.script, c.reg);
        else if (pass == 1 && c.reg[0]) ojs_snprintf(key, sizeof key, "%s-%s", c.lang, c.reg);
        else if (pass == 2) ojs_snprintf(key, sizeof key, "%s", c.lang);
        else continue;
        for (unsigned i = 0; i < intl_nlikely; i++)
            if (strcmp(intl_likely[i].src, key) == 0) { hit = &intl_likely[i]; break; }
    }
    if (hit) {
        struct itag d;
        if (tag_parse(hit->dst, (unsigned)strlen(hit->dst), &d) == 0) {
            if (!c.script[0] && d.script[0]) strcpy(c.script, d.script);
            if (!c.reg[0] && d.reg[0]) strcpy(c.reg, d.reg);
        }
    }
    return tag_canonical(&c, out, cap);
}

// ---------------------------------------------------------------- plural rules

struct popnds {
    double n;
    unsigned long long i, f, t;
    int v, w;
};

// operands from the shortest round-trip decimal form (what V8 does; the
// exact expansion would make 0.1 non-"one")
static void plural_operands(double x, struct popnds* o) {
    memset(o, 0, sizeof *o);
    if (x != x) return;
    if (x < 0) x = -x;
    o->n = x;
    if (x == 0) return;
    if (x >= 1e21) { o->i = ~0ULL; return; }
    char buf[40];
    num_to_cstr(x, buf);
    // [-]D[.d][eE]: value = digits * 10^E, digits = mantissa without the dot
    int rawlen = (int)strlen(buf);
    int dot = -1;
    for (int i = 0; i < rawlen; i++) if (buf[i] == '.') { dot = i; break; }
    char* e = strchr(buf, 'e');
    int e10 = e ? c_atoi(e + 1) : 0;
    char dig[40];
    int nd = 0;
    for (int i = 0; i < rawlen && buf[i] != 'e'; i++) {
        char c = buf[i];
        if (c == '-' || c == '+' || c == '.') continue;
        dig[nd++] = c;
    }
    dig[nd] = 0;
    int E = e10 - (dot >= 0 ? (rawlen - dot - 1) : 0);
    unsigned long long iv = 0;
    int ip = nd + E;
    if (ip > 0) {
        for (int i = 0; i < ip && i < 19; i++) iv = iv * 10 + (unsigned long long)(dig[i] - '0');
        for (int i = 0; i < ip - nd && iv != ~0ULL; i++) {
            if (iv > 0x1999999999999999ULL) { iv = ~0ULL; break; }
            iv *= 10;
        }
    }
    o->i = iv;
    if (E < 0) {
        o->v = -E;
        unsigned long long fv = 0;
        int start = nd + E;
        int take = o->v > 19 ? 19 : o->v;
        for (int i = 0; i < take; i++) {
            int di = start + i;
            fv = fv * 10 + (unsigned long long)(di >= 0 && di < nd ? dig[di] - '0' : 0);
        }
        o->f = fv;
        // w: fraction digits after stripping trailing zeros (from the right)
        int w = o->v;
        for (int i = 1; i <= o->v; i++) {
            int di = nd + E + o->v - i;   // i-th fraction digit from the right
            if (di < 0 || di >= nd) { w = o->v - i; continue; }
            if (dig[di] != '0') break;
            w = o->v - i;
        }
        o->w = w;
        unsigned long long tv = o->f;
        while (tv && tv % 10 == 0) tv /= 10;
        o->t = tv;
    }
}

// rule interpreter over the CLDR rule strings (compiled lazily, cached)
struct rrange { double a, b; };
struct rrel { int opnd, mod, neg; int nr; struct rrange r[8]; };
struct rcond { int n; struct rrel rel[6]; };
struct rrule { int n; struct rcond c[6]; };

static struct rrule rs_cache[24][5];   // one rule per category (zero..many)
static unsigned char rs_done[24];

// "n % 10 = 2..4 and n % 100 != 12..14 or ..." — one ruleset = one rrule;
// the first matching relation-group names the category (manual tokenizing:
// the freestanding kernel build has no strtok)
static char* rs_tok(char** cur) {
    char* p = *cur;
    while (*p == ' ') p++;
    if (!*p) { *cur = p; return 0; }
    char* start = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    *cur = p;
    return start;
}

static int rs_parse_one(const char* src, struct rrule* out) {
    memset(out, 0, sizeof *out);
    char buf[160];
    ojs_snprintf(buf, sizeof buf, "%s", src);
    if (out->n >= 6) return -1;
    struct rcond* cur = &out->c[out->n++];
    cur->n = 0;
    char* cur2 = buf;
    char* tok;
    while ((tok = rs_tok(&cur2)) != 0) {
        if (strcmp(tok, "or") == 0) {
            if (out->n >= 6) return -1;
            cur = &out->c[out->n++];
            cur->n = 0;
            continue;
        }
        if (strcmp(tok, "and") == 0) continue;
        if (cur->n >= 6) return -1;
        struct rrel* r = &cur->rel[cur->n];
        memset(r, 0, sizeof *r);
        r->opnd = tok[0];
        char* pc = strchr(tok, '%');
        if (pc) {
            // inline form "n%100 = ..."
            *pc = 0;
            r->mod = c_atoi(pc + 1);
            tok = rs_tok(&cur2);
        } else {
            // CLDR's spaced form: "n % 100 = 3..10"
            tok = rs_tok(&cur2);
            if (tok && strcmp(tok, "%") == 0) {
                tok = rs_tok(&cur2);
                if (!tok) return -1;
                r->mod = c_atoi(tok);
                tok = rs_tok(&cur2);
            }
        }
        if (!tok) return -1;
        if (strcmp(tok, "!=") == 0) r->neg = 1;
        else if (strcmp(tok, "=") != 0) return -1;
        tok = rs_tok(&cur2);
        if (!tok) return -1;
        char list[80];
        ojs_snprintf(list, sizeof list, "%s", tok);
        size_t ll = strlen(list);
        while (ll && list[ll - 1] == ',') {
            char* nx = rs_tok(&cur2);
            if (!nx) break;
            size_t nl = strlen(nx);
            if (ll + nl + 1 >= sizeof list) break;
            memcpy(list + ll, nx, nl + 1);
            ll += nl;
        }
        char* piece = list;
        for (;;) {
            char* comma = strchr(piece, ',');
            if (comma) *comma = 0;
            if (*piece && r->nr < 8) {
                struct rrange* rg = &r->r[r->nr];
                char* dd = strstr(piece, "..");
                if (dd) {
                    *dd = 0;
                    rg->a = strcmp(piece, "inf") == 0 ? 1e308 : c_atof(piece);
                    rg->b = strcmp(dd + 2, "inf") == 0 ? 1e308 : c_atof(dd + 2);
                } else {
                    rg->a = rg->b = strcmp(piece, "inf") == 0 ? 1e308 : c_atof(piece);
                }
                r->nr++;
            }
            if (!comma) break;
            piece = comma + 1;
        }
        cur->n++;
    }
    return 0;
}

// compile every category rule of a ruleset; returns 0 when nothing parses
static struct rrule* rs_compile(unsigned idx) {
    if (idx >= intl_nrs_n || idx >= sizeof rs_cache / sizeof rs_cache[0]) return 0;
    if (rs_done[idx]) return rs_cache[idx];
    rs_done[idx] = 1;
    const struct intl_rset* rs = &intl_rs[idx];
    int any = 0;
    int nth = 0;
    for (int bit = 0; bit < 5; bit++) {
        if (!(rs->mask & (1u << bit))) continue;
        if (nth < rs->n) {
            if (rs_parse_one(IP(intl_rs_off[rs->off + nth]), &rs_cache[idx][bit]) == 0) any = 1;
        }
        nth++;
    }
    return any ? rs_cache[idx] : 0;
}

static double rel_operand(int opnd, const struct popnds* o) {
    switch (opnd) {
    case 'n': return o->n;
    case 'i': return (double)o->i;
    case 'f': return (double)o->f;
    case 't': return (double)o->t;
    case 'v': return o->v;
    case 'w': return o->w;
    default: return 0;   // 'e'/'c': no compact exponent in PluralRules
    }
}

static int rel_eval(const struct rrel* r, const struct popnds* o) {
    double v = rel_operand(r->opnd, o);
    if (r->mod) v = intl_fmod(v, (double)r->mod);
    int in = 0;
    for (int i = 0; i < r->nr; i++)
        if (v >= r->r[i].a && v <= r->r[i].b) { in = 1; break; }
    return r->neg ? !in : in;
}

int intl_plural_select(unsigned rs, double x) {
    struct rrule* rr = rs_compile(rs);
    if (!rr) return 5;
    struct popnds o;
    plural_operands(x, &o);
    for (int cat = 0; cat < 5; cat++) {
        const struct rrule* r = &rr[cat];
        for (int c = 0; c < r->n; c++) {
            int all = 1;
            for (int i = 0; i < r->c[c].n; i++)
                if (!rel_eval(&r->c[c].rel[i], &o)) { all = 0; break; }
            if (all) return cat;
        }
    }
    return 5;
}

const char* intl_plural_cat_name(int cat) {
    static const char* const N[6] = { "zero", "one", "two", "few", "many", "other" };
    return N[cat < 0 || cat > 5 ? 5 : cat];
}

int intl_plural_cat_index(const char* name) {
    static const char* const N[6] = { "zero", "one", "two", "few", "many", "other" };
    for (int i = 0; i < 6; i++) if (strcmp(name, N[i]) == 0) return i;
    return -1;
}

// ---------------------------------------------------------------- option helpers

int intl_opts_obj(ojs* J, jv opts, struct obj** out) {
    *out = 0;
    if (jv_is_undef(opts) || jv_is_nullish(opts)) return 0;
    if (!jv_is_obj(opts)) { throw_type(J, "options argument must be an object"); return -1; }
    *out = jv_obj(opts);
    return 0;
}

static int opt_get(ojs* J, struct obj* o, const char* name, jv* out) {
    *out = JV_UNDEFINED;
    if (!o) return 0;
    jv v = obj_get(J, o, bname_key(J, name), jv_from_obj(o));
    if (jv_is_exc(v)) return -1;
    *out = v;
    return 0;
}

int intl_opt_bool(ojs* J, struct obj* o, const char* name, int def, int* out) {
    jv v;
    if (opt_get(J, o, name, &v) < 0) return -1;
    if (jv_is_undef(v)) { *out = def; return 0; }
    *out = !(v == JV_FALSE || v == JV_UNDEFINED || v == JV_NULL || v == jv_from_int(0));
    return 0;
}

int intl_opt_num(ojs* J, struct obj* o, const char* name, double lo, double hi, int* out) {
    jv v;
    if (opt_get(J, o, name, &v) < 0) return -1;
    if (jv_is_undef(v)) { *out = -1; return 0; }
    double d;
    if (to_number_d(J, v, &d) < 0) return -1;
    if (d != d || d < lo || d > hi) { throw_range(J, "%s value is out of range", name); return -1; }
    *out = (int)d;
    return 0;
}

int intl_opt_enum(ojs* J, struct obj* o, const char* name, const char* const* vals, int nvals, int def, int* out) {
    jv v;
    if (opt_get(J, o, name, &v) < 0) return -1;
    if (jv_is_undef(v)) { *out = def; return 0; }
    jv s = to_string(J, v);
    if (jv_is_exc(s)) return -1;
    struct str* str = jv_str(s);
    char buf[24];
    unsigned n = str_len(str);
    unsigned b = 0;
    if (n < sizeof buf) {
        for (unsigned i = 0; i < n; i++) {
            uint32_t cp = str_at(str, i);
            if (cp >= 128) { b = 0; break; }
            buf[b++] = (char)cp;
        }
        if (b == n && b > 0) {
            buf[b] = 0;
            for (int i = 0; i < nvals; i++)
                if (strcmp(buf, vals[i]) == 0) { *out = i; return 0; }
        }
    }
    throw_range(J, "%s must be one of \"%s\"", name, vals[0]);
    return -1;
}

int intl_opt_str(ojs* J, struct obj* o, const char* name, jv* out) {
    jv v;
    if (opt_get(J, o, name, &v) < 0) return -1;
    if (jv_is_undef(v)) { *out = JV_UNDEFINED; return 0; }
    jv s = to_string(J, v);
    if (jv_is_exc(s)) return -1;
    *out = s;
    return 0;
}

int intl_opt_cstr(ojs* J, struct obj* o, const char* name, char* buf, unsigned cap) {
    jv v;
    if (intl_opt_str(J, o, name, &v) < 0) return -1;
    buf[0] = 0;
    if (jv_is_undef(v)) return 0;
    struct str* s = jv_str(v);
    unsigned n = str_len(s);
    if (n >= cap) { throw_range(J, "%s is too long", name); return -1; }
    for (unsigned i = 0; i < n; i++) {
        uint32_t cp = str_at(s, i);
        if (cp >= 128) { throw_range(J, "%s must be ASCII", name); return -1; }
        buf[i] = (char)cp;
    }
    buf[n] = 0;
    return 0;
}

// ---------------------------------------------------------------- decimal rounding

static int rest_nonzero(const char* d, int n, int from) {
    for (int i = from; i < n; i++) if (d[i] != '0') return 1;
    return 0;
}

// round the exact digits d[0..n) (value 0.d * 10^point) to `keep` digits.
// out receives the rounded digits, *on their count, *opoint the new point.
static void round_digits(const char* d, int n, int point, int keep, int mode, char* out, int* on, int* opoint) {
    if (keep < 0) keep = 0;
    if (keep > 780) keep = 780;
    int nd = keep;
    for (int i = 0; i < keep; i++) out[i] = i < n ? d[i] : '0';
    int up = 0;
    if (keep < n) {
        int tie = d[keep] == '5' && !rest_nonzero(d, n, keep + 1);
        int above = d[keep] > '5' || (d[keep] == '5' && rest_nonzero(d, n, keep + 1));
        int any = d[keep] != '0' || rest_nonzero(d, n, keep + 1);
        switch (mode) {
        case RM_TRUNC: case RM_FLOOR: up = 0; break;
        case RM_CEIL: case RM_EXPAND: up = any; break;
        case RM_HALF_TRUNC: case RM_HALF_FLOOR: up = above; break;
        case RM_HALF_CEIL: case RM_HALF_EXPAND: up = above || tie; break;
        case RM_HALF_EVEN: up = above || (tie && keep > 0 && (out[keep - 1] & 1)); break;
        }
    }
    if (up) {
        int i = keep - 1;
        while (i >= 0 && out[i] == '9') { out[i] = '0'; i--; }
        if (i >= 0) out[i]++;
        else {
            out[0] = '1';
            for (int j = 1; j < keep; j++) out[j] = '0';
            nd = keep + 1;
            point++;
        }
    }
    *on = nd;
    *opoint = point;
}

int intl_exact_digits(double x, char* out, int cap, int* point) {
    return num_exact_digits(x, out, cap, point);
}

// round x (halfExpand, as Intl defaults) to a fraction-digit window — used by
// Intl.PluralRules, whose select() rounds its input through the digit options
double intl_round_number(double x, int min_frac, int max_frac) {
    if (x != x || x == 0 || x > 1e21 || x < -1e21) return x;
    int neg = x < 0;
    double ax = neg ? -x : x;
    char ex[820];
    int pt;
    int n = intl_exact_digits(ax, ex, (int)sizeof ex - 4, &pt);
    char d[820];
    int nd, pt2;
    round_digits(ex, n, pt, pt + max_frac, RM_HALF_EXPAND, d, &nd, &pt2);
    int floord = pt2 + min_frac;
    while (nd > floord && d[nd - 1] == '0') nd--;
    double v = 0;
    for (int i = 0; i < nd; i++) v = v * 10 + (d[i] - '0');
    v *= p10(pt2 - nd);
    return neg ? -v : v;
}

// ---------------------------------------------------------------- parts

#define MAXPARTS 64
struct part { const char* type; char val[96]; const struct str* sval; };
struct plist { int n; struct part p[MAXPARTS]; int oom; };

static void pl_add(struct plist* L, const char* type, const char* val) {
    if (L->n >= MAXPARTS) { L->oom = 1; return; }
    struct part* q = &L->p[L->n++];
    q->type = type;
    q->sval = 0;
    unsigned l = 0;
    while (val[l] && l < sizeof q->val - 1) { q->val[l] = val[l]; l++; }
    q->val[l] = 0;
}

static jv plist_to_jv(ojs* J, struct plist* L, int want_parts, struct sbuf* sb) {
    if (L->oom) return throw_range(J, "too many format parts");
    if (!want_parts) {
        for (int i = 0; i < L->n; i++) {
            const struct part* q = &L->p[i];
            if (q->sval) sb_put_str(sb, q->sval);
            else sb_put_utf8(sb, q->val, strlen(q->val));   // values carry UTF-8 (month names, units...)
        }
        return sb_done(sb);
    }
    struct obj* a = obj_new_array(J, 0);
    if (!a) return JV_EXC;
    for (int i = 0; i < L->n; i++) {
        const struct part* q = &L->p[i];
        struct obj* o = obj_new_plain(J);
        if (!o) return JV_EXC;
        jv tv = str_value(J, q->type);
        if (obj_define_value(J, o, bname_key(J, "type"), tv, PA_DEFAULT) < 0) return JV_EXC;
        jv vv;
        if (q->sval) vv = jv_from_str((struct str*)q->sval);
        else {
            struct str* ss = str_from_cstr(J, q->val);
            if (!ss) return JV_EXC;
            vv = jv_from_str(ss);
        }
        if (obj_define_value(J, o, bname_key(J, "value"), vv, PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, a, PK_FROM_INDEX(i), jv_from_obj(o), PA_DEFAULT) < 0) return JV_EXC;
    }
    return jv_from_obj(a);
}

// ---------------------------------------------------------------- number patterns

struct npat {
    char pre[32];
    unsigned char prel;
    char suf[32];
    unsigned char sufl;
    short min_int, min_frac, max_frac;
    short grp1, grp2;           // 0 = no grouping
    unsigned char is_sci;
    short exp_min;
    short mult;                 // 1, 100 ('%'), 1000 (per mille)
    unsigned char neg_has_sign; // negative subpattern carries its own sign
    char npre[32];
    unsigned char nprel;
    char nsuf[32];
    unsigned char nsufl;
};

// copy a pattern affix up to a body char, handling quotes
static void affix_copy(const char* p, const char* end, char* out, unsigned char* ol) {
    unsigned l = 0;
    while (p < end && l < 30) {
        char c = *p;
        if (c == '\'') {
            const char* q = strchr(p + 1, '\'');
            if (!q) { while (p < end && l < 30) out[l++] = *p++; break; }
            for (const char* r = p + 1; r < q && l < 30; r++) out[l++] = *r;
            if (q + 1 < end && q[1] == '\'') out[l++] = '\'';
            p = (q + 1 < end && q[1] == '\'') ? q + 2 : q + 1;
            continue;
        }
        if (c == 0 || strchr("0#,.;@E", c)) break;
        out[l++] = c;
        p++;
    }
    out[l] = 0;
    *ol = l;
}

static int npat_parse(const char* pat, struct npat* o) {
    memset(o, 0, sizeof *o);
    char pos[80];
    const char* semi = strchr(pat, ';');
    unsigned pl = semi ? (unsigned)(semi - pat) : (unsigned)strlen(pat);
    if (pl >= sizeof pos) return -1;
    memcpy(pos, pat, pl);
    pos[pl] = 0;

    const char* body0 = 0;
    const char* body1 = pos + strlen(pos);
    int seen_e = 0;
    for (const char* p = pos; *p; p++) {
        if (strchr("0#,.", *p)) { if (!body0) body0 = p; body1 = p + 1; }
        else if (*p == 'E' && !seen_e) { seen_e = 1; o->is_sci = 1; if (!body0) body0 = p; body1 = p + 1; }
    }
    if (!body0) return -1;
    affix_copy(pos, body0, o->pre, &o->prel);

    const char* mant1 = seen_e ? strstr(pos, "E") : body1;
    const char* dec = 0;
    for (const char* p = body0; p < mant1; p++) if (*p == '.') { dec = p; break; }
    const char* iend = dec ? dec : mant1;
    int zeros = 0;
    const char* lastc = 0;
    for (const char* p = body0; p < iend; p++) {
        if (*p == '0') zeros++;
        else if (*p == ',') lastc = p;
    }
    o->min_int = zeros ? (short)zeros : 1;
    if (dec) {
        int fz = 0, fh = 0;
        for (const char* p = dec + 1; p < mant1; p++) {
            if (*p == '0') { fz++; fh++; }
            else if (*p == '#') fh++;
        }
        o->min_frac = (short)fz;
        o->max_frac = (short)fh;
    }
    if (lastc) {
        o->grp1 = (short)(iend - lastc - 1);
        const char* prevc = 0;
        for (const char* p = body0; p < lastc; p++) if (*p == ',') prevc = p;
        o->grp2 = prevc ? (short)(lastc - prevc - 1) : o->grp1;
        if (o->grp1 <= 0) o->grp1 = 0;
    }
    if (seen_e) {
        const char* e = strstr(pos, "E");
        int z = 0;
        for (const char* p = e + 1; *p == '0'; p++) z++;
        o->exp_min = (short)(z ? z : 1);
        affix_copy(e + (z ? z : 1), pos + strlen(pos), o->suf, &o->sufl);
    } else {
        affix_copy(body1, pos + strlen(pos), o->suf, &o->sufl);
    }
    if (memchr(o->pre, '%', o->prel) || memchr(o->suf, '%', o->sufl)) o->mult = 100;
    else if (strstr(o->pre, "\xE2\x80\xB0") || strstr(o->suf, "\xE2\x80\xB0")) o->mult = 1000;

    if (semi) {
        const char* neg = semi + 1;
        const char* nbody0 = 0;
        for (const char* p = neg; *p; p++) if (strchr("0#,.", *p)) { nbody0 = p; break; }
        if (nbody0) {
            affix_copy(neg, nbody0, o->npre, &o->nprel);
            const char* q = nbody0;
            while (*q && strchr("0#,.", *q)) q++;
            affix_copy(q, neg + strlen(neg), o->nsuf, &o->nsufl);
            int has = memchr(o->npre, '-', o->nprel) || memchr(o->npre, '+', o->nprel) ||
                      memchr(o->nsuf, '-', o->nsufl) || memchr(o->nsuf, '+', o->nsufl);
            if (!has && o->nprel + 1 < sizeof o->npre) {
                memmove(o->npre + 1, o->npre, o->nprel + 1);
                o->npre[0] = '-';
                o->nprel++;
            }
        }
    } else {
        // no negative subpattern: minus + the positive affixes
        if (o->prel + 1 < sizeof o->npre) {
            o->npre[0] = '-';
            memcpy(o->npre + 1, o->pre, o->prel);
            o->nprel = o->prel + 1;
            o->npre[o->nprel] = 0;
        }
        if (o->sufl < sizeof o->nsuf) {
            memcpy(o->nsuf, o->suf, o->sufl);
            o->nsufl = o->sufl;
            o->nsuf[o->nsufl] = 0;
        }
        o->neg_has_sign = 1;
    }
    return 0;
}

// ---------------------------------------------------------------- number formatting

// digits (ASCII 0-9) remapped into the numbering system, UTF-8 out; byte count
static int remap(char* out, const char* dig, int n, int nu) {
    const char* dz = intl_nsys[nu].digits;
    const unsigned char* u = (const unsigned char*)dz;
    uint32_t zero = u[0];
    if (zero >= 0x80) {
        if ((zero & 0xE0) == 0xC0) zero = ((zero & 0x1F) << 6) | (u[1] & 0x3F);
        else if ((zero & 0xF0) == 0xE0) zero = ((zero & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        else zero = ((zero & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    }
    int b = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = zero + (uint32_t)(dig[i] - '0');
        if (cp < 0x80) out[b++] = (char)cp;
        else if (cp < 0x800) { out[b++] = (char)(0xC0 | (cp >> 6)); out[b++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out[b++] = (char)(0xE0 | (cp >> 12));
            out[b++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[b++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[b++] = (char)(0xF0 | (cp >> 18));
            out[b++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[b++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[b++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[b] = 0;
    return b;
}

// integer + fraction with grouping/decimal symbols, as parts
static void emit_digits(struct plist* L, const struct intl_nfmt* f, const char* idig, int nid,
                        int grp1, int grp2, int use_grp, const char* fdig, int nfd) {
    char buf[960];
    if (use_grp && grp1 > 0 && nid > grp1) {
        int period = grp2 > 0 ? grp2 : grp1;
        int first = (nid - grp1) % period;
        if (first == 0) first = period;
        int i = 0;
        while (i < nid) {
            int run = (i == 0) ? first : period;
            if (i + run > nid) run = nid - i;
            remap(buf, idig + i, run, f->nu);
            pl_add(L, "integer", buf);
            i += run;
            if (i < nid) {
                const char* gsym = IP(f->sym[1]);
                size_t gl = strlen(gsym);
                if (gl > sizeof buf - 4) gl = sizeof buf - 4;
                memcpy(buf, gsym, gl);
                buf[gl] = 0;
                pl_add(L, "group", buf);
            }
        }
    } else {
        remap(buf, idig, nid, f->nu);
        pl_add(L, "integer", buf);
    }
    if (nfd > 0) {
        pl_add(L, "decimal", IP(f->sym[0]));
        remap(buf, fdig, nfd, f->nu);
        pl_add(L, "fraction", buf);
    }
}

// affix text with the ¤ runs replaced by `subst`; emitted as one part
static void emit_affix(struct plist* L, const char* aff, unsigned alen, const char* subst, const char* type) {
    char buf[96];
    unsigned b = 0;
    for (unsigned i = 0; i < alen && b < sizeof buf - 4;) {
        if ((unsigned char)aff[i] == 0xC2 && (unsigned char)aff[i + 1] == 0xA4) {
            if (subst) for (const char* q = subst; *q && b < sizeof buf - 4; q++) buf[b++] = *q;
            i += 2;
            continue;
        }
        buf[b++] = aff[i++];
    }
    buf[b] = 0;
    if (b) pl_add(L, type, buf);
}

// explicit sign policy; returns 1 when a sign part was added (the pattern's
// own negative subpattern must then be skipped)
static int nf_sign(struct plist* L, const struct intl_nfmt* f, int neg, int zero_result) {
    switch (f->sign_disp) {
    case 1: return 0;                                   // never
    case 2:                                             // always
        pl_add(L, neg ? "minusSign" : "plusSign", IP(neg ? f->sym[4] : f->sym[3]));
        return 1;
    case 3:                                             // exceptZero
        if (!zero_result) {
            pl_add(L, neg ? "minusSign" : "plusSign", IP(neg ? f->sym[4] : f->sym[3]));
            return 1;
        }
        return 0;
    case 4:                                             // negative: minus even on -0
        if (neg) { pl_add(L, "minusSign", IP(f->sym[4])); return 1; }
        return 0;
    default: return 0;                                  // auto: the pattern carries it
    }
}

// round |x| to the configured digits through the pattern's defaults
static void nf_round(const struct intl_nfmt* f, const struct npat* pat, double ax, char* d, int* nd, int* point) {
    if (!(ax > 0)) { d[0] = '0'; d[1] = 0; *nd = 1; *point = 1; return; }
    char ex[820];
    int pt;
    int n = intl_exact_digits(ax, ex, (int)sizeof ex - 4, &pt);
    if (f->max_sig >= 0) {
        round_digits(ex, n, pt, f->max_sig, f->mode, d, nd, point);
        while (*nd < f->min_sig && *nd < 800) d[(*nd)++] = '0';
        return;
    }
    int mf = f->max_frac >= 0 ? f->max_frac : pat->max_frac;
    int nf = f->min_frac >= 0 ? f->min_frac : pat->min_frac;
    if (mf < nf) mf = nf;
    int keep = pt + mf;
    round_digits(ex, n, pt, keep, f->mode, d, nd, point);
    int floor_digits = *point + nf;
    while (*nd > floor_digits && d[*nd - 1] == '0') (*nd)--;
    if (*nd == 0) { d[0] = '0'; *nd = 1; }
}

// standard (non-scientific) emission through the pattern
static void npat_emit(ojs* J, const struct intl_nfmt* f, const struct npat* pat,
                      const char* d, int nd, int point, int neg, const char* subst,
                      const char* affix_type, struct plist* L) {
    const char* pre = neg ? pat->npre : pat->pre;
    unsigned prel = neg ? pat->nprel : pat->prel;
    const char* suf = neg ? pat->nsuf : pat->suf;
    unsigned sufl = neg ? pat->nsufl : pat->sufl;
    emit_affix(L, pre, prel, subst, affix_type);
    int min_int = pat->min_int > f->min_int ? pat->min_int : f->min_int;
    char idig[820];
    int nid = 0;
    if (point <= 0) {
        int pad = (min_int > 0 ? min_int : 1);
        for (int i = 0; i < pad && nid < (int)sizeof idig; i++) idig[nid++] = '0';
    } else {
        for (int i = 0; i < point && nid < (int)sizeof idig; i++) idig[nid++] = i < nd ? d[i] : '0';
        while (nid < min_int && nid < (int)sizeof idig) {
            memmove(idig + 1, idig, (size_t)nid);
            idig[0] = '0';
            nid++;
        }
    }
    char fdig[820];
    int nfd = 0;
    if (f->max_sig >= 0) {
        // significant-digit rounding: the fraction covers everything past the point
        if (point < 0) {
            for (int i = 0; i < -point && nfd < (int)sizeof fdig; i++) fdig[nfd++] = '0';
        }
        for (int i = point > 0 ? point : 0; i < nd && nfd < (int)sizeof fdig; i++) fdig[nfd++] = d[i];
    } else {
        int max_frac = f->max_frac >= 0 ? f->max_frac : pat->max_frac;
        for (int i = 0; i < max_frac && nfd < (int)sizeof fdig; i++) {
            int di = point + i;
            fdig[nfd++] = (di >= 0 && di < nd) ? d[di] : '0';
        }
    }
    int strip_floor = f->min_frac >= 0 ? f->min_frac : pat->min_frac;
    while (nfd > strip_floor && nfd > 0 && fdig[nfd - 1] == '0') nfd--;
    emit_digits(L, f, idig, nid, pat->grp1, pat->grp2, f->use_grp == 1, fdig, nfd);
    emit_affix(L, suf, sufl, subst, affix_type);
}

// scientific emission (mantissa + exponent)
static void sci_emit(ojs* J, const struct intl_nfmt* f, const struct npat* pat,
                     const char* d, int nd, int point, int neg, const char* subst,
                     const char* affix_type, struct plist* L) {
    emit_affix(L, neg ? pat->npre : pat->pre, neg ? pat->nprel : pat->prel, subst, affix_type);
    // sci: 1 int digit, exponent = point-1; eng: the exponent is snapped to a
    // multiple of 3 and the mantissa carries point - e3 int digits
    int nid = pat->min_int > 0 ? pat->min_int : 1;
    int expv = point - nid;
    if (f->notation == 2) {
        int e3 = expv >= 0 ? (expv / 3) * 3 : -(((-expv) + 2) / 3) * 3;
        nid = point - e3;
        if (nid < 1) nid = 1;
        expv = e3;
    }
    if (nid > nd) nid = nd < 1 ? 1 : nd;
    char idig[900];
    for (int i = 0; i < nid; i++) idig[i] = i < nd ? d[i] : '0';
    char fdig[900];
    int nfd = 0;
    int max_frac = f->max_frac >= 0 ? f->max_frac : 3;
    for (int i = nid; i < nd && nfd < (int)sizeof fdig; i++) fdig[nfd++] = d[i];
    while (nfd > max_frac && nfd > 0 && fdig[nfd - 1] == '0') nfd--;
    int min_frac = f->min_frac >= 0 ? f->min_frac : pat->min_frac;
    while (nfd < min_frac && nfd < (int)sizeof fdig) fdig[nfd++] = '0';
    emit_digits(L, f, idig, nid, 0, 0, 0, nfd ? fdig : "", nfd);
    pl_add(L, "exponentSeparator", IP(f->sym[5]));

    if (expv < 0) pl_add(L, "exponentMinusSign", IP(f->sym[4]));
    char eb[16];
    int ev = expv < 0 ? -expv : expv;
    int en = 0;
    do { eb[en++] = (char)('0' + ev % 10); ev /= 10; } while (ev);
    while (en < pat->exp_min) eb[en++] = '0';
    char eb2[16];
    int ep = 0;
    if (expv < 0) eb2[ep++] = '-';
    while (en > 0) eb2[ep++] = eb[--en];
    eb2[ep] = 0;
    pl_add(L, "exponentInteger", eb2);
    emit_affix(L, neg ? pat->nsuf : pat->suf, neg ? pat->nsufl : pat->sufl, subst, affix_type);
}

// splice a "{0} …"-pattern: literal, the formatted number's parts, literal.
// returns 0 when the pattern has no {0}; JV_EXC propagates
static jv nf_splice(ojs* J, const struct intl_nfmt* f, const char* full, double x, int neg, struct plist* L) {
    const char* hole = strstr(full, "{0}");
    if (!hole) return JV_UNDEFINED;
    if (neg) pl_add(L, "minusSign", IP(f->sym[4]));
    char pre[96];
    unsigned prl = (unsigned)(hole - full);
    if (prl >= sizeof pre) prl = sizeof pre - 1;
    memcpy(pre, full, prl);
    pre[prl] = 0;
    if (prl) pl_add(L, "literal", pre);
    struct intl_nfmt f2 = *f;
    f2.style = 0;
    jv parts = intl_nf_format(J, &f2, neg ? -x : x, 1);
    if (jv_is_exc(parts)) return JV_EXC;
    struct obj* pa = jv_obj(parts);
    for (uint32_t i = 0;; i++) {
        jv ev = obj_get(J, pa, PK_FROM_INDEX(i), jv_from_obj(pa));
        if (jv_is_exc(ev) || !jv_is_obj(ev)) break;
        struct obj* po = jv_obj(ev);
        jv tv = obj_get(J, po, bname_key(J, "type"), jv_from_obj(po));
        jv vv2 = obj_get(J, po, bname_key(J, "value"), jv_from_obj(po));
        if (jv_is_exc(tv) || jv_is_exc(vv2) || !jv_is_str(vv2)) break;
        char tb[24], vb[96];
        size_t tl2 = str_to_utf8_buf(jv_str(tv), tb, sizeof tb - 1);
        tb[tl2] = 0;
        size_t vl2 = str_to_utf8_buf(jv_str(vv2), vb, sizeof vb - 1);
        vb[vl2] = 0;
        pl_add(L, tb, vb);
    }
    char post[96];
    ojs_snprintf(post, sizeof post, "%s", hole + 3);
    if (post[0]) pl_add(L, "literal", post);
    return jv_from_int(1);
}

// plural-selected pattern from a (cat, pattern) group
static unsigned grp_pat_for(const struct intl_cgrp* g, int cat) {
    unsigned poff = 0xFFFF;
    for (unsigned i = 0; i < g->n; i++) {
        const struct intl_cpair* q = &intl_cp[g->off + i];
        if (q->cat == cat) { poff = q->pat; break; }
    }
    if (poff == 0xFFFF && g->n) poff = intl_cp[g->off].pat;
    return poff;
}

jv intl_nf_format(ojs* J, const struct intl_nfmt* f, double x, int want_parts) {
    struct plist L;
    memset(&L, 0, sizeof L);
    struct sbuf sb;
    sb_init(J, &sb);

    if (x != x) { pl_add(&L, "nan", "NaN"); return plist_to_jv(J, &L, want_parts, &sb); }
    if (x > 1.7976931348623157e308 || x < -1.7976931348623157e308) {
        if (x < 0) pl_add(&L, "minusSign", IP(f->sym[4]));
        pl_add(&L, "infinity", "\xE2\x88\x9E");
        return plist_to_jv(J, &L, want_parts, &sb);
    }
    int neg = (x < 0) || (x == 0 && 1.0 / x < 0);
    double ax = neg ? -x : x;

    const char* pat_src = f->pat_dec;
    if (f->style == 1) pat_src = f->pat_pct;
    else if (f->style == 2) pat_src = f->pat_cur;

    struct npat pat;
    if (npat_parse(pat_src, &pat) < 0) return throw_type(J, "invalid number pattern");

    if (f->style == 1 && pat.mult) {
        // percent/per-mille: scale the shortest decimal form (multiplying the
        // double would keep 0.145's tail below 14.5 where ICU shows 15%)
        char cs[40];
        num_to_cstr(ax, cs);
        char* e = strchr(cs, 'e');
        int e10 = e ? c_atoi(e + 1) : 0;
        if (e) *e = 0;
        char dig[40];
        int nd2 = 0, dot = -1;
        for (char* q = cs; *q; q++) {
            if (*q >= '0' && *q <= '9') dig[nd2++] = *q;
            else if (*q == '.') dot = nd2;
        }
        dig[nd2] = 0;
        int shift = pat.mult == 1000 ? 3 : 2;
        ax = num_from_decimal(dig, nd2, e10 + shift - (dot >= 0 ? nd2 - dot : 0));
    }

    const char* subst = 0;
    const char* affix_type = "literal";
    const struct intl_lcur* lcur = (f->cur_entry != 0xFFFF && f->cur_entry < intl_nlcur) ? &intl_lcur[f->cur_entry] : 0;
    const struct intl_lunit* lunit = (f->unit_entry != 0xFFFF && f->unit_entry < intl_nlunit) ? &intl_lunit[f->unit_entry] : 0;

    if (f->style == 3) {
        // units: "{0} days" (or the short form "{0} days"/"{0} km/h") by plural
        unsigned ugrp = (f->unit_disp != 0 && lunit->sgrp != 0xFFFF) ? lunit->sgrp : lunit->grp;
        if (lunit && ugrp != 0xFFFF) {
            char d0[820];
            int nd0 = 0, pt0 = 0;
            nf_round(f, &pat, ax, d0, &nd0, &pt0);
            double rounded = 0;
            for (int i = 0; i < nd0; i++) rounded = rounded * 10 + (d0[i] - '0');
            rounded *= p10(pt0 - nd0);
            if (neg) rounded = -rounded;
            int cat = intl_plural_select(f->card, rounded);
            unsigned poff = grp_pat_for(&intl_cg[ugrp], cat);
            if (poff != 0xFFFF) {
                jv sp = nf_splice(J, f, IP(poff), x, neg, &L);
                if (jv_is_exc(sp)) return JV_EXC;
                if (!jv_is_undef(sp)) return plist_to_jv(J, &L, want_parts, &sb);
            }
        }
        if (lunit && lunit->disp != 0xFFFF) {
            char d0[820];
            int nd0 = 0, pt0 = 0;
            nf_round(f, &pat, ax, d0, &nd0, &pt0);
            double rounded = 0;
            for (int i = 0; i < nd0; i++) rounded = rounded * 10 + (d0[i] - '0');
            rounded *= p10(pt0 - nd0);
            int cat = intl_plural_select(f->card, rounded);
            (void)cat;
            char patbuf[128];
            ojs_snprintf(patbuf, sizeof patbuf, "{0} %s", IP(lunit->disp));
            jv sp = nf_splice(J, f, patbuf, x, neg, &L);
            if (jv_is_exc(sp)) return JV_EXC;
            if (!jv_is_undef(sp)) return plist_to_jv(J, &L, want_parts, &sb);
        }
        return throw_range(J, "unsupported unit");
    }

    if (f->style == 2) {
        if (f->cur_disp == 3 && lcur && lcur->grp != 0xFFFF) {
            // "{0} US dollars": literal prefix, plain number, literal suffix
            char d0[820];
            int nd0 = 0, pt0 = 0;
            nf_round(f, &pat, ax, d0, &nd0, &pt0);
            double rounded = 0;
            for (int i = 0; i < nd0; i++) rounded = rounded * 10 + (d0[i] - '0');
            rounded *= p10(pt0 - nd0);
            if (neg) rounded = -rounded;
            int cat = intl_plural_select(f->card, rounded);
            unsigned poff = grp_pat_for(&intl_cg[lcur->grp], cat);
            if (poff != 0xFFFF) {
                // CLDR displayName-count patterns are the bare name; the
                // number goes in front of it
                char patbuf[96];
                ojs_snprintf(patbuf, sizeof patbuf, "{0} %s", IP(poff));
                jv sp = nf_splice(J, f, patbuf, x, neg, &L);
                if (jv_is_exc(sp)) return JV_EXC;
                if (!jv_is_undef(sp)) return plist_to_jv(J, &L, want_parts, &sb);
            }
            subst = IP(lcur->sym);
            affix_type = "currency";
        } else if (f->cur_disp == 0) {
            // code display: ICU puts a no-break space between the code and the number
            static char codebuf[8];
            size_t cl = strlen(f->cur_code);
            if (cl > 3) cl = 3;
            memcpy(codebuf, f->cur_code, cl);
            memcpy(codebuf + cl, "\xC2\xA0", 3);
            subst = codebuf;
            affix_type = "currency";
        } else if (lcur) {
            subst = f->cur_disp == 2 ? IP(lcur->narrow) : IP(lcur->sym);
            affix_type = "currency";
        } else {
            subst = f->cur_code;
            affix_type = "currency";
        }
    }

    char d[820];
    int nd = 0, point = 0;

    if (f->notation == 3) {
        // compact: divisor from the table, pattern by the rounded plural
        int expv = 0;
        if (ax >= 10) {
            double t = ax;
            while (t >= 10 && expv < 22) { t /= 10; expv++; }
        }
        int gi = -1;
        if (expv >= 3 && expv <= 15) gi = f->comp_base + (expv - 3);
        else if (expv > 15) gi = f->comp_base + 12;
        int handled = 0;
        if (gi >= 0 && gi < (int)intl_ncg && intl_cg[gi].n) {
            const struct intl_cgrp* g = &intl_cg[gi];
            double comp = ax / p10(expv);
            // provisional rounding (fraction digits default to 0) for the plural
            struct intl_nfmt f3 = *f;
            if (f3.max_frac < 0) f3.max_frac = 0;
            if (f3.min_frac < 0) f3.min_frac = 0;
            struct npat neutral;
            memset(&neutral, 0, sizeof neutral);
            neutral.min_int = 1;
            char d2[820];
            int nd2 = 0, pt2 = 0;
            nf_round(&f3, &neutral, comp, d2, &nd2, &pt2);
            double rounded = 0;
            for (int i = 0; i < nd2; i++) rounded = rounded * 10 + (d2[i] - '0');
            rounded *= p10(pt2 - nd2);
            int cat = intl_plural_select(f->card, rounded);
            unsigned poff = 0xFFFF;
            for (unsigned i = 0; i < g->n; i++) {
                const struct intl_cpair* q = &intl_cp[g->off + i];
                if (q->cat == cat) { poff = q->pat; break; }
            }
            if (poff == 0xFFFF && g->n) poff = intl_cp[g->off].pat;
            if (poff != 0xFFFF && npat_parse(IP(poff), &pat) == 0) {
                // re-round the compacted value: compact shows one fraction
                // digit for single-digit quotients ("1.2M"), none otherwise
                struct intl_nfmt f2 = *f;
                if (f2.max_frac < 0) f2.max_frac = pat.min_int >= 2 ? 0 : 1;
                if (f2.min_frac < 0) f2.min_frac = 0;
                nf_round(&f2, &pat, comp, d, &nd, &point);
                int zc = 1;
                for (int i = 0; i < nd; i++) if (d[i] != '0') { zc = 0; break; }
                int ex = nf_sign(&L, f, neg, zc);
                npat_emit(J, &f2, &pat, d, nd, point, neg && !ex, subst, "compact", &L);
                return plist_to_jv(J, &L, want_parts, &sb);
            }
        }
        if (!handled) {
            nf_round(f, &pat, ax, d, &nd, &point);
            int zc = 1;
            for (int i = 0; i < nd; i++) if (d[i] != '0') { zc = 0; break; }
            int ex = nf_sign(&L, f, neg, zc);
            npat_emit(J, f, &pat, d, nd, point, neg && !ex, subst, affix_type, &L);
            return plist_to_jv(J, &L, want_parts, &sb);
        }
    }

    nf_round(f, &pat, ax, d, &nd, &point);

    // explicit sign policy (sign_display; "auto" relies on the pattern)
    int zero_result = 1;
    for (int i = 0; i < nd; i++) if (d[i] != '0') { zero_result = 0; break; }
    int ex = nf_sign(&L, f, neg, zero_result);

    if (f->notation == 1 || f->notation == 2) {
        if (f->max_sig < 0) {
            // scientific/engineering default: int digits + 3 fraction digits
            struct intl_nfmt f2 = *f;
            f2.max_sig = f->notation == 2 ? 6 : 4;
            f2.min_sig = 1;
            nf_round(&f2, &pat, ax, d, &nd, &point);
        }
        sci_emit(J, f, &pat, d, nd, point, neg && !ex, subst, affix_type, &L);
        return plist_to_jv(J, &L, want_parts, &sb);
    }
    // the effective negative: "never" and "exceptZero"-zero print unsigned,
    // and an explicitly emitted sign must not double with the subpattern's
    int neg_eff = neg && !ex && f->sign_disp != 1 && !(f->sign_disp == 3 && zero_result);
    npat_emit(J, f, &pat, d, nd, point, neg_eff, subst, affix_type, &L);
    return plist_to_jv(J, &L, want_parts, &sb);
}

// ---------------------------------------------------------------- date formatting

// nth NUL-terminated string of a consecutive run in the pool
static const char* pool_at(unsigned base, int i) {
    const char* p = IP(base);
    while (i-- > 0) p += strlen(p) + 1;
    return p;
}

// civil-date math (Howard Hinnant's algorithms, as in b_date.c)
static __attribute__((unused)) long long days_from_civil_ll(long long y, unsigned mo, unsigned d) {
    y -= mo <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days_ll(long long z, long long* y, unsigned* mo, unsigned* d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *mo = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*mo <= 2);
}

struct dtm { long long y; int mo, day, wd; int h, mi, s, ms; };

static void dt_split(double t, int is_local, struct dtm* m) {
    double tt = is_local ? t + (double)ojs_sys_tz_offset(t, 0) * 60000.0 : t;
    double days = km_floor(tt / 86400000.0);
    double rem = tt - days * 86400000.0;
    if (rem < 0) { rem += 86400000.0; days -= 1.0; }
    long long z = (long long)days;
    long long y;
    unsigned mo, dy;
    civil_from_days_ll(z, &y, &mo, &dy);
    m->y = y;
    m->mo = (int)mo;
    m->day = (int)dy;
    m->wd = (int)(((z % 7) + 11) % 7);   // 1970-01-01 is a Thursday (4), sun = 0
    m->h = (int)(rem / 3600000.0);
    m->mi = (int)(rem / 60000.0) % 60;
    m->s = (int)(rem / 1000.0) % 60;
    m->ms = (int)rem % 1000;
}

// format an integer with padding and numbering-system remap
static void num_part(struct plist* L, const struct intl_dfmt* f, const char* type, long long v, int pad) {
    char dig[24];
    int dn = 0;
    int negv = v < 0;
    if (negv) v = -v;
    if (v == 0) dig[dn++] = '0';
    while (v > 0) { dig[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn < pad && dn < 20) dig[dn++] = '0';
    char tmp[24];
    int n = 0;
    if (negv) tmp[n++] = '-';
    for (int i = dn - 1; i >= 0; i--) tmp[n++] = dig[i];
    tmp[n] = 0;
    char out[48];
    remap(out, tmp, n, f->nu);
    pl_add(L, type, out);
}

static void name_part(struct plist* L, const char* type, const char* name) {
    char out[96];
    unsigned l = 0;
    for (const char* q = name; *q && l < sizeof out - 1; q++) out[l++] = *q;
    out[l] = 0;
    pl_add(L, type, out);
}

// one date pattern symbol
static void dt_field(struct intl_dfmt* f, const struct intl_loc* loc,
                     const struct dtm* m, double t, char sym, int len, int era_mode, int standalone,
                     struct plist* L) {
    char buf[96];
    switch (sym) {
    case 'y': case 'Y': case 'u': {
        long long y = m->y;
        if (era_mode && y <= 0) y = 1 - y;
        if (len >= 2) {
            long long yy = y % 100;
            if (yy < 0) yy += 100;
            num_part(L, f, "year", yy, len);
        } else {
            num_part(L, f, "year", y, 1);
        }
        break;
    }
    case 'G':
        if (len >= 4) name_part(L, "era", pool_at(loc->era, m->y <= 0 ? 2 : 3));
        else name_part(L, "era", pool_at(loc->era, m->y <= 0 ? 0 : 1));
        break;
    case 'M': case 'L': {
        if (len <= 2) { num_part(L, f, "month", m->mo, len); break; }
        int w = len == 3 ? 1 : (len == 4 ? 0 : 2);
        name_part(L, "month", pool_at(loc->mo[standalone ? w + 3 : w], m->mo - 1));
        break;
    }
    case 'd':
        num_part(L, f, "day", m->day, len);
        break;
    case 'E': case 'e': case 'c':
        if (sym != 'E' && len <= 2) { num_part(L, f, "weekday", ((m->wd + 6) % 7) + 1, len); break; }
        if (f->wk_len > len) len = f->wk_len;   // the requested width wins
        if (len >= 5) name_part(L, "weekday", pool_at(loc->dy[2], m->wd));
        else if (len == 4) name_part(L, "weekday", pool_at(loc->dy[0], m->wd));
        else name_part(L, "weekday", pool_at(loc->dy[1], m->wd));
        break;
    case 'h': num_part(L, f, "hour", m->h % 12 ? m->h % 12 : 12, len); break;
    case 'K': num_part(L, f, "hour", m->h % 12, len); break;
    case 'H': num_part(L, f, "hour", m->h, len); break;
    case 'k': num_part(L, f, "hour", m->h ? m->h : 24, len); break;
    case 'm': num_part(L, f, "minute", m->mi, len); break;
    case 's':
        num_part(L, f, "second", m->s, len);
        if (f->frac_add) {
            // the matched skeleton lacked fractional seconds; insert them here
            int dn = f->frac_add > 3 ? 3 : f->frac_add;
            int v2 = m->ms;
            if (dn == 1) v2 /= 100;
            else if (dn == 2) v2 /= 10;
            char dg[8];
            dg[0] = (char)('0' + (v2 / 100) % 10);
            dg[1] = (char)('0' + (v2 / 10) % 10);
            dg[2] = (char)('0' + v2 % 10);
            dg[dn] = 0;
            char buf[16];
            remap(buf, dg, dn, f->nu);
            pl_add(L, "decimal", IP(loc->sym[0]));
            pl_add(L, "fractionalSecond", buf);
            f->frac_add = 0;
        }
        break;
    case 'S': {
        int v2 = m->ms;
        if (len == 1) v2 /= 100;
        else if (len == 2) v2 /= 10;
        else if (len > 3) v2 = v2;              // SSSS+ appends zeros below
        char dg[8];
        int dn = len > 3 ? 3 : len;
        dg[0] = (char)('0' + (v2 / 100) % 10);
        dg[1] = (char)('0' + (v2 / 10) % 10);
        dg[2] = (char)('0' + v2 % 10);
        while (dn < len && dn < 7) dg[dn++] = '0';
        dg[dn] = 0;
        remap(buf, dg, dn, f->nu);
        pl_add(L, "fractionalSecond", buf);
        break;
    }
    case 'a':
        name_part(L, "dayPeriod", pool_at(loc->per, m->h < 12 ? 0 : 1));
        break;
    case 'b': {
        int idx;
        if (m->h == 0 && m->mi == 0 && m->s == 0) idx = 2;
        else if (m->h == 12 && m->mi == 0 && m->s == 0) idx = 3;
        else idx = m->h < 12 ? 0 : 1;
        name_part(L, "dayPeriod", pool_at(loc->per, idx));
        break;
    }
    case 'z': {
        if (f->tz == 0) {
            pl_add(L, "timeZoneName", len >= 4 ? "Coordinated Universal Time" : "UTC");
            break;
        }
        int off = ojs_sys_tz_offset(t, 0);
        int sign = off < 0 ? -1 : 1;
        int a = off * sign;
        if (len >= 4) ojs_snprintf(buf, sizeof buf, "GMT%c%02d:%02d", sign < 0 ? '-' : '+', a / 60, a % 60);
        else if (a % 60) ojs_snprintf(buf, sizeof buf, "GMT%c%d:%02d", sign < 0 ? '-' : '+', a / 60, a % 60);
        else ojs_snprintf(buf, sizeof buf, "GMT%c%d", sign < 0 ? '-' : '+', a / 60);
        pl_add(L, "timeZoneName", buf);
        break;
    }
    default:
        break;
    }
}

static int sym_is_date(char c) { return c == 'y' || c == 'Y' || c == 'u' || c == 'M' || c == 'L' || c == 'd' || c == 'G' || c == 'E' || c == 'e' || c == 'c'; }
static int sym_is_time(char c) { return c == 'h' || c == 'H' || c == 'K' || c == 'k' || c == 'm' || c == 's' || c == 'S' || c == 'a' || c == 'b'; }

static void dt_pattern(ojs* J, struct intl_dfmt* f, const struct intl_loc* loc,
                       const struct dtm* m, double t, const char* p, struct plist* L) {
    (void)J;
    int era_mode = strchr(p, 'G') != 0;
    int standalone = 0;
    {
        int has_month = 0, has_other = 0;
        for (const char* q = p; *q; q++) {
            if (*q == '\'') {
                const char* e2 = strchr(q + 1, '\'');
                if (!e2) break;
                q = e2;
                continue;
            }
            if (is_alpha((unsigned char)*q)) {
                if (*q == 'M' || *q == 'L') has_month = 1;
                else has_other = 1;
            }
        }
        standalone = has_month && !has_other;
    }
    char lit[96];
    while (*p) {
        if (*p == '\'') {
            if (p[1] == '\'') { pl_add(L, "literal", "'"); p += 2; continue; }
            const char* q = strchr(p + 1, '\'');
            if (!q) { pl_add(L, "literal", p + 1); break; }
            unsigned l = (unsigned)(q - p - 1);
            if (l >= sizeof lit) l = sizeof lit - 1;
            memcpy(lit, p + 1, l);
            lit[l] = 0;
            pl_add(L, "literal", lit);
            p = q + 1;
            continue;
        }
        if (is_alpha((unsigned char)*p)) {
            char sym = *p;
            int len = 1;
            while (p[len] == sym) len++;
            p += len;
            dt_field(f, loc, m, t, sym, len, era_mode, standalone, L);
            continue;
        }
        unsigned l = 0;
        while (*p && !is_alpha((unsigned char)*p) && *p != '\'' && l < sizeof lit - 1) lit[l++] = *p++;
        lit[l] = 0;
        if (l) pl_add(L, "literal", lit);
    }
}

jv intl_df_format(ojs* J, struct intl_dfmt* f, double t, int want_parts) {
    if (t != t || t > 8.64e15 || t < -8.64e15) return throw_range(J, "Invalid time value");
    struct plist L;
    memset(&L, 0, sizeof L);
    struct sbuf sb;
    sb_init(J, &sb);
    const struct intl_loc* loc = &intl_locales[f->li];
    struct dtm m;
    dt_split(t, f->tz == 1, &m);
    if (f->glue && f->date_pat && f->time_pat) {
        // glue "{1} {0}": {1} = date, {0} = time
        const char* p = f->glue;
        char lit[96];
        while (*p) {
            if (*p == '{' && (p[1] == '0' || p[1] == '1') && p[2] == '}') {
                if (p[1] == '1') dt_pattern(J, f, loc, &m, t, f->date_pat, &L);
                else dt_pattern(J, f, loc, &m, t, f->time_pat, &L);
                p += 3;
                continue;
            }
            unsigned l = 0;
            while (*p && *p != '{' && l < sizeof lit - 1) lit[l++] = *p++;
            lit[l] = 0;
            if (l) pl_add(&L, "literal", lit);
        }
    } else if (f->date_pat) {
        dt_pattern(J, f, loc, &m, t, f->date_pat, &L);
    } else if (f->time_pat) {
        dt_pattern(J, f, loc, &m, t, f->time_pat, &L);
    }
    // the matched skeleton may lack fractional seconds / the zone name even
    // though they were requested — append them (ICU does the same)
    if (f->tz_add && (f->time_pat || f->glue)) {
        pl_add(&L, "literal", " ");
        dt_field(f, loc, &m, t, 'z', f->tz_len, 0, 0, &L);
    }
    return plist_to_jv(J, &L, want_parts, &sb);
}

// ---------------------------------------------------------------- pattern selection

static int skel_scan(const char* s, char* syms, int* lens, int max) {
    int n = 0;
    while (*s && n < max) {
        if (!is_alpha((unsigned char)*s)) { s++; continue; }
        char c = *s;
        int l = 1;
        while (s[l] == c) l++;
        syms[n] = c;
        lens[n] = l;
        n++;
        s += l;
    }
    return n;
}

static int width_dist(char sym, int a, int b) {
    static const char NUMSYMS[] = "ydDhHkKmsS";
    if (strchr(NUMSYMS, sym)) { int d = a - b; if (d < 0) d = -d; return d > 2 ? 2 : d; }
    int oa = a >= 5 ? 4 : a - 1;   // 1,2 numeric; 3 abbrev; 4 wide; 5 narrow
    int ob = b >= 5 ? 4 : b - 1;
    int d = oa - ob;
    if (d < 0) d = -d;
    return d * 3;
}

static int rn_has_time(const char* syms, int n) {
    for (int i = 0; i < n; i++) if (sym_is_time(syms[i])) return 1;
    return 0;
}

static int rn_has_date(const char* syms, int n) {
    for (int i = 0; i < n; i++) if (sym_is_date(syms[i])) return 1;
    return 0;
}

int intl_dt_pick(unsigned short li, int date_style, int time_style,
                 const struct intl_dtcomp* c, const char** dp, const char** tp, const char** glue) {
    const struct intl_loc* loc = &intl_locales[li];
    *dp = 0;
    *tp = 0;
    *glue = 0;
    if (date_style >= 0) *dp = pool_at(loc->df, date_style);
    if (time_style >= 0) *tp = pool_at(loc->tf, time_style);
    if (date_style >= 0 && time_style >= 0) {
        *glue = pool_at(loc->glue, date_style);
        return 0;
    }
    if (!c) return 0;

    // requested skeleton
    char sk[40];
    int sl = 0;
    #define APP(sym, n) do { for (int i = 0; i < (n) && sl < (int)sizeof sk - 1; i++) sk[sl++] = (sym); } while (0)
    if (c->era) APP('G', c->era == 3 ? 4 : (c->era == 4 ? 1 : 5));
    if (c->year) APP('y', c->year == 2 ? 2 : 1);
    if (c->month) APP('M', c->month == 1 ? 1 : c->month == 2 ? 2 : c->month == 3 ? 4 : c->month == 4 ? 3 : 5);
    if (c->day) APP('d', c->day == 2 ? 2 : 1);
    if (c->weekday) APP('E', c->weekday == 3 ? 4 : c->weekday == 4 ? 1 : 5);
    if (c->dayperiod) APP('b', c->dayperiod == 3 ? 4 : c->dayperiod == 4 ? 2 : 1);
    if (c->hour) {
        char hs = c->hc == 0 ? 'K' : c->hc == 1 ? 'h' : c->hc == 2 ? 'H' : 'k';
        APP(hs, c->hour == 2 ? 2 : 1);
    }
    if (c->minute) APP('m', c->minute == 2 ? 2 : 1);
    if (c->second) APP('s', c->second == 2 ? 2 : 1);
    if (c->frac) APP('S', c->frac);
    if (c->tzname) APP('z', (c->tzname == 2 || c->tzname == 4 || c->tzname == 6) ? 4 : 1);
    #undef APP
    sk[sl] = 0;
    if (!sl) return 0;

    char rsyms[12];
    int rlens[12];
    int rn = skel_scan(sk, rsyms, rlens, 12);
    int want_date = rn_has_date(rsyms, rn);
    int want_time = rn_has_time(rsyms, rn);

    int best = -1, best_score = 1 << 30;
    int best_date = -1, best_date_score = 1 << 30;
    int best_time = -1, best_time_score = 1 << 30;
    for (unsigned i = 0; i < loc->af_n; i++) {
        const char* key = intl_skel[intl_af[loc->af_off + 2 * i]];
        char ks[12];
        int klens[12];
        int kn = skel_scan(key, ks, klens, 12);
        int used[12] = { 0 };
        int extra = 0, score = 0;
        for (int a = 0; a < rn; a++) {
            int found = -1;
            for (int b = 0; b < kn; b++) {
                if (!used[b] && ks[b] == rsyms[a]) { found = b; break; }
            }
            if (found < 0) { score += 1000; continue; }   // dropped field: near-disqualifying
            used[found] = 1;
            score += width_dist(rsyms[a], rlens[a], klens[found]);
        }
        for (int b = 0; b < kn; b++) if (!used[b]) extra++;
        int has_date = 0, has_time = 0;
        for (int b = 0; b < kn; b++) {
            if (sym_is_date(ks[b])) has_date = 1;
            if (sym_is_time(ks[b])) has_time = 1;
        }
        int total = score + extra * 25;
        if (extra == 0 && score < 1000 && total < best_score) { best_score = total; best = (int)i; }
        if (has_date && !has_time && total < best_date_score) { best_date_score = total; best_date = (int)i; }
        if (has_time && !has_date && total < best_time_score) { best_time_score = total; best_time = (int)i; }
    }
    if (best >= 0) {
        const char* pat = IP(intl_af[loc->af_off + 2 * best + 1]);
        int has_date = 0, has_time = 0;
        for (const char* q = pat; *q; q++) {
            if (*q == '\'') { const char* e2 = strchr(q + 1, '\''); if (!e2) break; q = e2; continue; }
            if (sym_is_date(*q)) has_date = 1;
            else if (sym_is_time(*q)) has_time = 1;
        }
        if (has_date) *dp = pat;
        if (has_time) *tp = pat;
        if (has_date && has_time) return 0;
        if (has_date && want_time) { *tp = pool_at(loc->tf, 2); *glue = pool_at(loc->glue, 2); }
        if (has_time && want_date) { *dp = pool_at(loc->df, 2); *glue = pool_at(loc->glue, 2); }
        return 0;
    }
    // no exact-set entry: best date-only + best time-only with the glue
    // (only the halves that were actually requested)
    if (want_date && best_date >= 0) *dp = IP(intl_af[loc->af_off + 2 * best_date + 1]);
    if (want_time && best_time >= 0) *tp = IP(intl_af[loc->af_off + 2 * best_time + 1]);
    if (!*dp && want_date) *dp = pool_at(loc->df, 2);
    if (!*tp && want_time) *tp = pool_at(loc->tf, 2);
    if (*dp && *tp) *glue = pool_at(loc->glue, 2);
    return 0;
}

// ---------------------------------------------------------------- minimize

int intl_minimize(const char* tag, unsigned len, char* out, unsigned cap) {
    char max[128];
    if (intl_maximize(tag, len, max, sizeof max) < 0) return -1;
    struct itag c;
    if (tag_parse(max, (unsigned)strlen(max), &c) < 0) return -1;
    // drop the region when it matches the likely region of the language, then
    // drop the script when it matches the likely script (CLDR 49 carries only
    // language-level likely entries, so one lookup drives both)
    const struct intl_likely* hit = 0;
    char key[24];
    ojs_snprintf(key, sizeof key, "%s", c.lang);
    for (unsigned i = 0; i < intl_nlikely; i++) {
        if (strcmp(intl_likely[i].src, key) == 0) { hit = &intl_likely[i]; break; }
    }
    if (hit) {
        struct itag d;
        if (tag_parse(hit->dst, (unsigned)strlen(hit->dst), &d) == 0) {
            if (c.reg[0] && d.reg[0] && strcmp(d.reg, c.reg) == 0) c.reg[0] = 0;
            if (c.script[0] && d.script[0] && strcmp(d.script, c.script) == 0) c.script[0] = 0;
        }
    }
    return tag_canonical(&c, out, cap);
}

// ---------------------------------------------------------------- relative time

jv intl_rtf_format(ojs* J, unsigned short li, int field, int width, int numeric,
                   double value, int want_parts) {
    const struct intl_loc* loc = &intl_locales[li];
    if (loc->fld == 0xFFFF) return throw_range(J, "unsupported unit");
    if (loc->fld + field * 3 + width >= intl_nfg) return throw_range(J, "unsupported unit");
    const struct intl_fgrp* g = &intl_fg[loc->fld + field * 3 + width];
    struct plist L;
    memset(&L, 0, sizeof L);
    struct sbuf sb;
    sb_init(J, &sb);

    const char* text = 0;
    if (!numeric) {
        long long iv = (long long)value;
        if ((double)iv == value && iv >= -1 && iv <= 1) {
            unsigned rel = g->rel[iv + 1];
            if (rel != 0xFFFF) text = IP(rel);
        }
    }
    if (!text) {
        // numeric: "in {0} days" / "{0} days ago" (fg stores the cpair run)
        double count = value < 0 ? -value : value;
        double rc = km_floor(count + 0.5);
        int cat = intl_plural_select(loc->card, rc);
        unsigned goff = value < 0 ? g->past : g->fut;
        unsigned gn = value < 0 ? g->past_n : g->fut_n;
        if (goff == 0xFFFF || gn == 0) return throw_range(J, "unsupported unit");
        unsigned poff = 0xFFFF;
        for (unsigned i = 0; i < gn; i++) {
            const struct intl_cpair* q = &intl_cp[goff + i];
            if (q->cat == cat) { poff = q->pat; break; }
        }
        if (poff == 0xFFFF) poff = intl_cp[goff].pat;
        if (poff == 0xFFFF) return throw_range(J, "unsupported unit");
        const char* full = IP(poff);
        const char* hole = strstr(full, "{0}");
        if (!hole) return throw_range(J, "unsupported unit");
        char pre[96];
        unsigned prl = (unsigned)(hole - full);
        if (prl >= sizeof pre) prl = sizeof pre - 1;
        memcpy(pre, full, prl);
        pre[prl] = 0;
        if (prl) pl_add(&L, "literal", pre);
        // the count formatted with grouping (a plain decimal format)
        struct intl_nfmt nf;
        memset(&nf, 0, sizeof nf);
        nf.pat_dec = IP(intl_locales[li].dec);
        nf.pat_pct = IP(intl_locales[li].pct);
        nf.pat_cur = IP(intl_locales[li].cur);
        for (int i = 0; i < 8; i++) nf.sym[i] = intl_locales[li].sym[i];
        nf.min_int = 1;
        nf.min_frac = 0;
        nf.max_frac = 0;
        nf.min_sig = -1;
        nf.max_sig = -1;
        nf.mode = RM_HALF_EXPAND;
        nf.use_grp = 1;
        nf.nu = intl_locales[li].nu;
        nf.card = intl_locales[li].card;
        jv parts = intl_nf_format(J, &nf, rc, 1);
        if (jv_is_exc(parts)) return JV_EXC;
        struct obj* pa = jv_obj(parts);
        for (uint32_t i = 0;; i++) {
            jv ev = obj_get(J, pa, PK_FROM_INDEX(i), jv_from_obj(pa));
            if (jv_is_exc(ev) || !jv_is_obj(ev)) break;
            struct obj* po = jv_obj(ev);
            jv tv = obj_get(J, po, bname_key(J, "type"), jv_from_obj(po));
            jv vv2 = obj_get(J, po, bname_key(J, "value"), jv_from_obj(po));
            if (jv_is_exc(tv) || jv_is_exc(vv2) || !jv_is_str(vv2)) break;
            char tb[24], vb[96];
            size_t tl2 = str_to_utf8_buf(jv_str(tv), tb, sizeof tb - 1);
            tb[tl2] = 0;
            size_t vl2 = str_to_utf8_buf(jv_str(vv2), vb, sizeof vb - 1);
            vb[vl2] = 0;
            pl_add(&L, tb, vb);
        }
        char post[96];
        ojs_snprintf(post, sizeof post, "%s", hole + 3);
        if (post[0]) pl_add(&L, "literal", post);
        return plist_to_jv(J, &L, want_parts, &sb);
    }
    pl_add(&L, "literal", text);
    return plist_to_jv(J, &L, want_parts, &sb);
}

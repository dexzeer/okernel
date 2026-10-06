// Stylesheet parser: rule structure, at-rules, selectors, declarations.
#include "css_int.h"

// ---- storage helpers ------------------------------------------------------------

#define GROW(arr, n, cap, type, init) do { \
    if ((n) >= (cap)) { \
        int nc_ = (cap) ? (cap) * 2 : (init); \
        type* na_ = (type*)w_realloc((arr), (size_t)nc_ * sizeof(type)); \
        if (!na_) return -1; \
        (arr) = na_; (cap) = nc_; \
    } } while (0)

static int add_decl(struct wsheet* sh, const struct cdecl* d) {
    GROW(sh->decls, sh->ndecl, sh->capdecl, struct cdecl, 256);
    sh->decls[sh->ndecl] = *d;
    return sh->ndecl++;
}

static int add_part(struct wsheet* sh, const struct cpart* p) {
    GROW(sh->parts, sh->npart, sh->cappart, struct cpart, 256);
    sh->parts[sh->npart] = *p;
    return sh->npart++;
}

static int add_sel(struct wsheet* sh, const struct csel* s) {
    GROW(sh->sels, sh->nsel, sh->capsel, struct csel, 128);
    sh->sels[sh->nsel] = *s;
    return sh->nsel++;
}

static int add_rule(struct wsheet* sh, const struct crule* r) {
    GROW(sh->rules, sh->nrule, sh->caprule, struct crule, 128);
    sh->rules[sh->nrule] = *r;
    return sh->nrule++;
}

static int add_mq(struct wsheet* sh, uint32_t raw, uint32_t rawlen) {
    GROW(sh->mqs, sh->nmq, sh->capmq, struct cmq, 16);
    sh->mqs[sh->nmq].raw = raw;
    sh->mqs[sh->nmq].rawlen = rawlen;
    return sh->nmq++;
}

static uint32_t pool_put(struct wsheet* sh, const char* s, int len) {
    uint32_t off = (uint32_t)sh->pool.len;
    wbuf_put(&sh->pool, s, len);
    return off;
}

#define POOL(sh, off) ((sh)->pool.p + (off))

// Stable pool reference for text p[0..len): text inside the current parse
// source maps to its pool copy; anything else is copied in. Never pass a
// pointer into sh->pool here (the append could realloc it).
static uint32_t ref_of(struct wsheet* sh, const char* p, int len) {
    if (sh->src && p >= sh->src && p + len <= sh->src + sh->src_len)
        return sh->src_off + (uint32_t)(p - sh->src);
    return pool_put(sh, p, len);
}

void csheet_free(struct wsheet* sh) {
    wbuf_free(&sh->pool);
    wbuf_free(&sh->imports);
    w_free(sh->decls); w_free(sh->rules); w_free(sh->sels); w_free(sh->parts);
    w_free(sh->calcs); w_free(sh->mqs);
    memset(sh, 0, sizeof(*sh));
}

// ---- comment stripping -------------------------------------------------------------

// Copy text into the pool with /* comments */ and top-level <!-- --> removed.
static uint32_t strip_into_pool(struct wsheet* sh, const char* s, int len, int* out_len) {
    uint32_t start = (uint32_t)sh->pool.len;
    wbuf_reserve(&sh->pool, len + 1);
    int i = 0;
    while (i < len) {
        // copy a run of ordinary characters in one go
        int j = i;
        while (j < len) {
            char cc = s[j];
            if (cc == '/' || cc == '"' || cc == 0x27 || cc == '<' || cc == '-' || cc == 0x5C) break;
            j++;
        }
        if (j > i) { wbuf_put(&sh->pool, s + i, j - i); i = j; if (i >= len) break; }
        char c = s[i];
        if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            wbuf_putc(&sh->pool, ' ');
            continue;
        }
        if (c == '"' || c == '\'') {
            int st = i;
            i++;
            while (i < len && s[i] != c && s[i] != '\n') { if (s[i] == '\\') i++; i++; }
            if (i < len) i++;
            if (i > len) i = len;
            wbuf_put(&sh->pool, s + st, i - st);
            continue;
        }
        if (c == '<' && i + 3 < len && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') { i += 4; continue; }
        if (c == '-' && i + 2 < len && s[i + 1] == '-' && s[i + 2] == '>') { i += 3; continue; }
        if (c == '\\' && i + 1 < len) { wbuf_put(&sh->pool, s + i, 2); i += 2; continue; }
        wbuf_putc(&sh->pool, c);
        i++;
    }
    *out_len = sh->pool.len - (int)start;
    wbuf_putc(&sh->pool, 0);
    return start;
}

// Find the matching close brace for the block starting after '{' at i.
// Returns the index of '}' (or len).
static int block_end(const char* s, int len, int i) {
    int depth = 1;
    while (i < len) {
        char c = s[i];
        if (c == '\\') { i += 2; continue; }
        if (c == '"' || c == '\'') {
            i++;
            while (i < len && s[i] != c) { if (s[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return i; }
        i++;
    }
    return len;
}

// Scan to the next top-level stop char (';' or '{' or '}'), honoring
// strings, escapes and (...)/[...] nesting.
static int scan_to(const char* s, int len, int i, const char* stops) {
    int depth = 0;
    while (i < len) {
        char c = s[i];
        if (c == '\\') { i += 2; continue; }
        if (c == '"' || c == '\'') {
            i++;
            while (i < len && s[i] != c) { if (s[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth) depth--;
        else if (!depth) {
            for (const char* p = stops; *p; p++) if (c == *p) return i;
        }
        i++;
    }
    return len;
}

// ---- identifiers with escapes --------------------------------------------------------

static int is_name_char(unsigned char c) {
    return w_isalnum(c) || c == '-' || c == '_' || c >= 0x80;
}

// Read an identifier at s[*i] (handles \-escapes); writes UTF-8 into out.
static int read_ident(const char* s, int len, int* i, char* out, int cap) {
    int o = 0, j = *i;
    while (j < len) {
        unsigned char c = (unsigned char)s[j];
        if (c == '\\' && j + 1 < len) {
            j++;
            if (w_ishex((unsigned char)s[j])) {
                uint32_t cp = 0;
                int k = 0;
                while (k < 6 && j < len && w_ishex((unsigned char)s[j])) { cp = cp * 16 + w_hexval((unsigned char)s[j]); j++; k++; }
                if (j < len && w_isspace((unsigned char)s[j])) j++;
                char u[4];
                int ul = w_utf8_enc(cp ? cp : 0xFFFD, u);
                for (int q = 0; q < ul && o < cap - 1; q++) out[o++] = u[q];
            } else {
                if (o < cap - 1) out[o++] = s[j];
                j++;
            }
            continue;
        }
        if (!is_name_char(c)) break;
        if (o < cap - 1) out[o++] = (char)c;
        j++;
    }
    out[o] = 0;
    *i = j;
    return o;
}

// ---- selectors ------------------------------------------------------------------

struct selp {
    struct wsheet* sh;
    struct watoms* atoms;   // NULL: static UA mode (known names only)
    const char* s;
    int len, i;
    int bad;
};

static int atom_of(struct selp* p, const char* name, int n, int lower) {
    char low[256];
    if (n > 255) n = 255;
    for (int k = 0; k < n; k++) low[k] = lower ? (char)w_lower((unsigned char)name[k]) : name[k];
    if (!p->atoms) {
        for (int a = 1; a < ATOM_KNOWN_COUNT; a++)
            if ((int)strlen(atom_known_names[a]) == n && !memcmp(atom_known_names[a], low, n)) return a;
        p->bad = 1; // UA sheet uses an unknown name: programming error
        return 0;
    }
    return watom_intern(p->atoms, low, n);
}

static void skip_ws(struct selp* p) { while (p->i < p->len && w_isspace((unsigned char)p->s[p->i])) p->i++; }

static int parse_sel_list(struct selp* p, int forgiving, int relative, int* first, int* count,
                          uint32_t* max_spec);

// Parse "An+B" (or odd/even) from text; returns 1 on success.
static int parse_nth(const char* s, int len, int32_t* A, int32_t* B) {
    cv_trim(&s, &len);
    if (w_ieq(s, len, "odd")) { *A = 2; *B = 1; return 1; }
    if (w_ieq(s, len, "even")) { *A = 2; *B = 0; return 1; }
    char buf[64];
    int n = 0;
    for (int k = 0; k < len && n < 63; k++) if (!w_isspace((unsigned char)s[k])) buf[n++] = (char)w_lower((unsigned char)s[k]);
    buf[n] = 0;
    int npos = -1;
    for (int k = 0; k < n; k++) if (buf[k] == 'n') { npos = k; break; }
    if (npos < 0) {
        int32_t m; int u;
        if (!cv_number(buf, n, &m, &u) || u != n) return 0;
        *A = 0; *B = m / 1000;
        return 1;
    }
    int32_t a = 1;
    if (npos == 0) a = 1;
    else if (npos == 1 && buf[0] == '-') a = -1;
    else if (npos == 1 && buf[0] == '+') a = 1;
    else {
        int32_t m; int u;
        if (!cv_number(buf, npos, &m, &u) || u != npos) return 0;
        a = m / 1000;
    }
    int32_t b = 0;
    if (npos + 1 < n) {
        int32_t m; int u;
        if (!cv_number(buf + npos + 1, n - npos - 1, &m, &u) || u != n - npos - 1) return 0;
        b = m / 1000;
    }
    *A = a; *B = b;
    return 1;
}

struct pcname { const char* n; int id; };
static const struct pcname PCLASSES[] = {
    {"first-child", PC_FIRST_CHILD}, {"last-child", PC_LAST_CHILD}, {"only-child", PC_ONLY_CHILD},
    {"first-of-type", PC_FIRST_OF_TYPE}, {"last-of-type", PC_LAST_OF_TYPE},
    {"only-of-type", PC_ONLY_OF_TYPE}, {"root", PC_ROOT}, {"empty", PC_EMPTY},
    {"link", PC_LINK}, {"any-link", PC_LINK}, {"-webkit-any-link", PC_LINK},
    {"visited", PC_VISITED}, {"hover", PC_HOVER}, {"active", PC_ACTIVE}, {"focus", PC_FOCUS},
    {"focus-visible", PC_FOCUS_VISIBLE}, {"focus-within", PC_FOCUS_WITHIN},
    {"checked", PC_CHECKED}, {"disabled", PC_DISABLED}, {"enabled", PC_ENABLED},
    {"required", PC_REQUIRED}, {"optional", PC_OPTIONAL}, {"read-only", PC_READ_ONLY},
    {"read-write", PC_READ_WRITE}, {"placeholder-shown", PC_PLACEHOLDER_SHOWN},
    {"target", PC_TARGET}, {"default", PC_DEFAULT}, {"indeterminate", PC_INDETERMINATE},
    {"defined", PC_ALWAYS}, {"scope", PC_ROOT}, {"open", PC_OPEN},
    // states that never apply in this browser (no hover / validation / media)
    {"invalid", PC_NEVER}, {"valid", PC_NEVER}, {"user-invalid", PC_NEVER},
    {"user-valid", PC_NEVER}, {"fullscreen", PC_NEVER}, {"modal", PC_NEVER},
    {"popover-open", PC_NEVER}, {"autofill", PC_NEVER}, {"-webkit-autofill", PC_NEVER},
    {"playing", PC_NEVER}, {"paused", PC_NEVER}, {"target-within", PC_NEVER},
    {"in-range", PC_ALWAYS}, {"out-of-range", PC_NEVER}, {"blank", PC_NEVER},
    {"local-link", PC_NEVER}, {"current", PC_NEVER}, {"past", PC_NEVER}, {"future", PC_NEVER},
    {"host", PC_NEVER}, {"focus-ring", PC_NEVER}, {"-moz-focusring", PC_NEVER},
    {"-moz-ui-invalid", PC_NEVER}, {"-ms-input-placeholder", PC_NEVER},
    {"-webkit-full-screen", PC_NEVER}, {"-moz-full-screen", PC_NEVER},
    {"first", PC_NEVER}, {"left", PC_NEVER}, {"right", PC_NEVER},
    {0, 0}
};

static const char* const PELEMS_OTHER[] = {
    "first-line", "first-letter", "selection", "backdrop", "file-selector-button",
    "-webkit-scrollbar", "-webkit-scrollbar-thumb", "-webkit-scrollbar-track",
    "-webkit-scrollbar-corner", "-webkit-scrollbar-button", "-webkit-scrollbar-track-piece",
    "-webkit-resizer", "-webkit-input-placeholder", "-moz-placeholder", "-moz-selection",
    "-webkit-search-cancel-button", "-webkit-search-decoration", "-webkit-details-marker",
    "-webkit-inner-spin-button", "-webkit-outer-spin-button", "-moz-focus-inner",
    "-webkit-file-upload-button", "-ms-clear", "-ms-reveal", "-ms-expand", "cue",
    "-webkit-calendar-picker-indicator", "slotted", "part", "view-transition",
    "view-transition-old", "view-transition-new", "view-transition-group", "highlight",
    "spelling-error", "grammar-error", "target-text", "-webkit-media-controls", 0
};

// Parse one compound + following combinator chain into parts; returns spec.
static int parse_complex(struct selp* p, int relative, uint32_t* spec_out, uint8_t* pe_out,
                         int* first_part) {
    struct wsheet* sh = p->sh;
    uint32_t a = 0, b = 0, c = 0;
    *pe_out = PE_NONE;
    // Parts are buffered locally and appended contiguously at the end:
    // nested lists (:is/:not/:has/nth-of) append their own selectors while
    // this one is being parsed and must not interleave with it.
    struct cpart lb[96];
    int nlb = 0;
#define LPUSH(x) do { if (nlb < 96) lb[nlb++] = (x); else { p->bad = 1; return 0; } } while (0)
    skip_ws(p);
    if (relative && p->i < p->len && (p->s[p->i] == '>' || p->s[p->i] == '+' || p->s[p->i] == '~')) {
        // leading combinator of a :has() relative selector: implicit scope
        struct cpart cp; memset(&cp, 0, sizeof cp);
        cp.kind = SK_COMB;
        cp.op = p->s[p->i] == '>' ? CB_CHILD : p->s[p->i] == '+' ? CB_ADJ : CB_SIB;
        cp.sub = -1;
        LPUSH(cp);
        p->i++;
        skip_ws(p);
    } else if (relative) {
        struct cpart cp; memset(&cp, 0, sizeof cp);
        cp.kind = SK_COMB; cp.op = CB_DESC; cp.sub = -1;
        LPUSH(cp);
    }
    int compounds = 0;
    for (;;) {
        // one compound
        int any = 0;
        while (p->i < p->len) {
            char ch = p->s[p->i];
            struct cpart cp;
            memset(&cp, 0, sizeof cp);
            cp.sub = -1;
            if (ch == '*') {
                p->i++;
                if (p->i < p->len && p->s[p->i] == '|') { p->i++; continue; } // *|x
                cp.kind = SK_UNIV;
                LPUSH(cp);
                any = 1;
                continue;
            }
            if (ch == '|') { p->i++; continue; } // |x (no namespace)
            if (is_name_char((unsigned char)ch) || ch == '\\') {
                if (any) { p->bad = 1; return 0; }
                char nm[128];
                int n = read_ident(p->s, p->len, &p->i, nm, sizeof nm);
                if (p->i < p->len && p->s[p->i] == '|') { p->i++; continue; } // ns|tag
                cp.kind = SK_TAG;
                cp.atom = (uint16_t)atom_of(p, nm, n, 1);
                LPUSH(cp);
                c++;
                any = 1;
                continue;
            }
            if (ch == '#') {
                p->i++;
                char nm[256];
                int n = read_ident(p->s, p->len, &p->i, nm, sizeof nm);
                if (!n) { p->bad = 1; return 0; }
                cp.kind = SK_ID;
                cp.atom = (uint16_t)atom_of(p, nm, n, 0);
                LPUSH(cp);
                a++;
                any = 1;
                continue;
            }
            if (ch == '.') {
                p->i++;
                char nm[256];
                int n = read_ident(p->s, p->len, &p->i, nm, sizeof nm);
                if (!n) { p->bad = 1; return 0; }
                cp.kind = SK_CLASS;
                cp.atom = (uint16_t)atom_of(p, nm, n, 0);
                LPUSH(cp);
                b++;
                any = 1;
                continue;
            }
            if (ch == '[') {
                p->i++;
                skip_ws(p);
                char nm[128];
                int n = read_ident(p->s, p->len, &p->i, nm, sizeof nm);
                if (p->i < p->len && p->s[p->i] == '|' && (p->i + 1 >= p->len || p->s[p->i + 1] != '=')) {
                    p->i++;
                    n = read_ident(p->s, p->len, &p->i, nm, sizeof nm);
                }
                if (!n) { p->bad = 1; return 0; }
                cp.kind = SK_ATTR;
                cp.atom = (uint16_t)atom_of(p, nm, n, 1);
                cp.op = AO_EXISTS;
                skip_ws(p);
                if (p->i < p->len && p->s[p->i] != ']') {
                    char o = p->s[p->i];
                    if (o == '=') { cp.op = AO_EQ; p->i++; }
                    else if (p->i + 1 < p->len && p->s[p->i + 1] == '=') {
                        cp.op = o == '~' ? AO_INCLUDES : o == '|' ? AO_DASH : o == '^' ? AO_PREFIX :
                                o == '$' ? AO_SUFFIX : o == '*' ? AO_SUBSTR : 255;
                        if (cp.op == 255) { p->bad = 1; return 0; }
                        p->i += 2;
                    } else { p->bad = 1; return 0; }
                    skip_ws(p);
                    char val[256];
                    int vl = 0;
                    if (p->i < p->len && (p->s[p->i] == '"' || p->s[p->i] == '\'')) {
                        char q = p->s[p->i++];
                        while (p->i < p->len && p->s[p->i] != q) {
                            if (p->s[p->i] == '\\' && p->i + 1 < p->len) p->i++;
                            if (vl < 255) val[vl++] = p->s[p->i];
                            p->i++;
                        }
                        p->i++;
                    } else {
                        vl = read_ident(p->s, p->len, &p->i, val, sizeof val);
                    }
                    cp.a = (int32_t)pool_put(sh, val, vl);
                    cp.b = vl;
                    skip_ws(p);
                    if (p->i < p->len && (p->s[p->i] == 'i' || p->s[p->i] == 'I')) { cp.ci = 1; p->i++; skip_ws(p); }
                    else if (p->i < p->len && (p->s[p->i] == 's' || p->s[p->i] == 'S')) { p->i++; skip_ws(p); }
                }
                if (p->i >= p->len || p->s[p->i] != ']') { p->bad = 1; return 0; }
                p->i++;
                // type="..." on HTML elements matches case-insensitively
                if (cp.atom == A_type) cp.ci = 1;
                LPUSH(cp);
                b++;
                any = 1;
                continue;
            }
            if (ch == ':') {
                p->i++;
                int is_pe = 0;
                if (p->i < p->len && p->s[p->i] == ':') { is_pe = 1; p->i++; }
                char nm[64];
                int n = read_ident(p->s, p->len, &p->i, nm, sizeof nm);
                for (int k = 0; k < n; k++) nm[k] = (char)w_lower((unsigned char)nm[k]);
                int has_arg = p->i < p->len && p->s[p->i] == '(';
                int arg_s = 0, arg_e = 0;
                if (has_arg) {
                    int depth = 0, j = p->i;
                    for (; j < p->len; j++) {
                        char x = p->s[j];
                        if (x == '"' || x == '\'') { j++; while (j < p->len && p->s[j] != x) j++; continue; }
                        if (x == '(') depth++;
                        else if (x == ')') { if (--depth == 0) break; }
                    }
                    arg_s = p->i + 1;
                    arg_e = j;
                    p->i = j < p->len ? j + 1 : p->len;
                }
                // pseudo-elements (incl. legacy single-colon forms)
                if (is_pe || (!has_arg && (w_ieq(nm, n, "before") || w_ieq(nm, n, "after") ||
                                           w_ieq(nm, n, "first-line") || w_ieq(nm, n, "first-letter")))) {
                    if (w_ieq(nm, n, "before")) *pe_out = PE_BEFORE;
                    else if (w_ieq(nm, n, "after")) *pe_out = PE_AFTER;
                    else if (w_ieq(nm, n, "marker")) *pe_out = PE_MARKER;
                    else if (w_ieq(nm, n, "placeholder")) *pe_out = PE_PLACEHOLDER;
                    else {
                        int known = 0;
                        for (int k = 0; PELEMS_OTHER[k]; k++) if (w_ieq(nm, n, PELEMS_OTHER[k])) known = 1;
                        if (!known && !(n > 8 && nm[0] == '-')) { p->bad = 1; return 0; }
                        *pe_out = PE_OTHER;
                    }
                    c++;
                    any = 1;
                    continue;
                }
                cp.kind = SK_PSEUDO;
                if (has_arg) {
                    const char* arg = p->s + arg_s;
                    int alen = arg_e - arg_s;
                    if (w_ieq(nm, n, "not") || w_ieq(nm, n, "is") || w_ieq(nm, n, "where") ||
                        w_ieq(nm, n, "matches") || w_ieq(nm, n, "-webkit-any") ||
                        w_ieq(nm, n, "-moz-any") || w_ieq(nm, n, "has")) {
                        struct selp sub = *p;
                        sub.s = arg; sub.len = alen; sub.i = 0; sub.bad = 0;
                        int is_where = w_ieq(nm, n, "where");
                        int is_has = w_ieq(nm, n, "has");
                        int is_not = w_ieq(nm, n, "not");
                        int f, cnt;
                        uint32_t ms = 0;
                        if (parse_sel_list(&sub, !is_not, is_has, &f, &cnt, &ms) < 0 || sub.bad || !cnt) {
                            p->bad = 1;
                            return 0;
                        }
                        cp.op = is_not ? PC_NOT : is_has ? PC_HAS : is_where ? PC_WHERE : PC_IS;
                        cp.sub = f;
                        cp.nsub = cnt;
                        if (!is_where) { a += ms >> 20; b += (ms >> 10) & 1023; c += ms & 1023; }
                    } else if (w_ieq(nm, n, "nth-child") || w_ieq(nm, n, "nth-last-child") ||
                               w_ieq(nm, n, "nth-of-type") || w_ieq(nm, n, "nth-last-of-type")) {
                        // optional "of S"
                        int of = -1;
                        for (int k = 0; k + 3 < alen; k++)
                            if ((k == 0 || w_isspace((unsigned char)arg[k - 1])) &&
                                w_ieq_prefix(arg + k, alen - k, "of") && w_isspace((unsigned char)arg[k + 2])) { of = k; break; }
                        int32_t A, B;
                        if (!parse_nth(arg, of >= 0 ? of : alen, &A, &B)) { p->bad = 1; return 0; }
                        cp.op = w_ieq(nm, n, "nth-child") ? PC_NTH_CHILD :
                                w_ieq(nm, n, "nth-last-child") ? PC_NTH_LAST_CHILD :
                                w_ieq(nm, n, "nth-of-type") ? PC_NTH_OF_TYPE : PC_NTH_LAST_OF_TYPE;
                        cp.a = A; cp.b = B;
                        if (of >= 0) {
                            struct selp sub = *p;
                            sub.s = arg + of + 3; sub.len = alen - of - 3; sub.i = 0; sub.bad = 0;
                            int f, cnt; uint32_t ms = 0;
                            if (parse_sel_list(&sub, 0, 0, &f, &cnt, &ms) < 0 || sub.bad) { p->bad = 1; return 0; }
                            cp.sub = f; cp.nsub = cnt;
                            a += ms >> 20; b += (ms >> 10) & 1023; c += ms & 1023;
                        }
                    } else if (w_ieq(nm, n, "lang")) {
                        const char* v = arg; int vl = alen;
                        cv_trim(&v, &vl);
                        if (vl && (v[0] == '"' || v[0] == '\'')) { v++; vl -= 2; }
                        if (vl < 0) vl = 0;
                        cp.op = PC_LANG;
                        cp.a = (int32_t)pool_put(sh, v, vl);
                        cp.b = vl;
                    } else if (w_ieq(nm, n, "dir")) {
                        const char* v = arg; int vl = alen;
                        cv_trim(&v, &vl);
                        cp.op = w_ieq(v, vl, "ltr") ? PC_ALWAYS : PC_NEVER;
                    } else if (w_ieq(nm, n, "host") || w_ieq(nm, n, "host-context") ||
                               w_ieq(nm, n, "state") || w_ieq(nm, n, "-moz-locale-dir") ||
                               w_ieq(nm, n, "active-view-transition-type")) {
                        cp.op = PC_NEVER;
                    } else { p->bad = 1; return 0; }
                } else {
                    cp.op = 0;
                    for (int k = 0; PCLASSES[k].n; k++)
                        if (w_ieq(nm, n, PCLASSES[k].n)) { cp.op = (uint8_t)PCLASSES[k].id; break; }
                    if (!cp.op) { p->bad = 1; return 0; }
                }
                LPUSH(cp);
                b++;
                any = 1;
                continue;
            }
            break;
        }
        if (!any) { p->bad = 1; return 0; }
        compounds++;
        // combinator?
        int had_ws = 0;
        while (p->i < p->len && w_isspace((unsigned char)p->s[p->i])) { p->i++; had_ws = 1; }
        if (p->i >= p->len || p->s[p->i] == ',' || p->s[p->i] == ')') break;
        struct cpart cp;
        memset(&cp, 0, sizeof cp);
        cp.kind = SK_COMB;
        cp.sub = -1;
        char ch = p->s[p->i];
        if (ch == '>' || ch == '+' || ch == '~') {
            cp.op = ch == '>' ? CB_CHILD : ch == '+' ? CB_ADJ : CB_SIB;
            p->i++;
            skip_ws(p);
        } else if (had_ws) {
            cp.op = CB_DESC;
        } else { p->bad = 1; return 0; }
        if (*pe_out != PE_NONE) { p->bad = 1; return 0; } // pseudo-element must be last
        LPUSH(cp);
    }
    struct cpart end;
    memset(&end, 0, sizeof end);
    end.kind = SK_END;
    end.sub = -1;
    LPUSH(end);
#undef LPUSH
    *first_part = sh->npart;
    for (int k = 0; k < nlb; k++) if (add_part(sh, &lb[k]) < 0) { p->bad = 1; return 0; }
    if (a > 1023) a = 1023;
    if (b > 1023) b = 1023;
    if (c > 1023) c = 1023;
    *spec_out = (a << 20) | (b << 10) | c;
    return compounds;
}

// Parse a comma-separated selector list into consecutive csel entries.
// forgiving: invalid entries are dropped instead of failing the list.
static int parse_sel_list(struct selp* p, int forgiving, int relative, int* first, int* count,
                          uint32_t* max_spec) {
    // reserve consecutive selector slots: parse into a temporary list first
    // because nested lists append their own selectors in between.
    struct csel tmp[64];
    int nt = 0;
    *max_spec = 0;
    while (p->i <= p->len) {
        skip_ws(p);
        struct selp q = *p;
        q.bad = 0;
        uint32_t spec;
        uint8_t pe;
        int fp;
        int npart0 = p->sh->npart;
        int ok = parse_complex(&q, relative, &spec, &pe, &fp) > 0 && !q.bad;
        // must end at ',' or end
        skip_ws(&q);
        if (ok && q.i < q.len && q.s[q.i] != ',') ok = 0;
        if (!ok) {
            if (!forgiving) { p->bad = 1; return -1; }
            p->sh->npart = npart0;
            // skip to the next top-level comma
            int depth = 0;
            while (q.i < q.len) {
                char c = q.s[q.i];
                if (c == '(') depth++;
                else if (c == ')') depth--;
                else if (c == ',' && depth <= 0) break;
                q.i++;
            }
        } else if (nt < 64) {
            tmp[nt].first = fp;
            tmp[nt].spec = spec;
            tmp[nt].pseudo = pe;
            tmp[nt].has_rel = (uint8_t)relative;
            if (spec > *max_spec) *max_spec = spec;
            nt++;
        }
        p->i = q.i;
        if (p->i < p->len && p->s[p->i] == ',') { p->i++; continue; }
        break;
    }
    *first = p->sh->nsel;
    for (int k = 0; k < nt; k++) if (add_sel(p->sh, &tmp[k]) < 0) return -1;
    *count = nt;
    return 0;
}

// ---- declarations ---------------------------------------------------------------

// keyword tables: "name", value pairs terminated by {0,0}
struct kw { const char* n; int v; };
static const struct kw KW_DISPLAY[] = {
    {"none", D_NONE}, {"inline", D_INLINE}, {"block", D_BLOCK}, {"inline-block", D_INLINE_BLOCK},
    {"list-item", D_LIST_ITEM}, {"flex", D_FLEX}, {"inline-flex", D_INLINE_FLEX},
    {"grid", D_GRID}, {"inline-grid", D_INLINE_GRID}, {"table", D_TABLE},
    {"inline-table", D_INLINE_TABLE}, {"table-row-group", D_TABLE_ROW_GROUP},
    {"table-header-group", D_TABLE_HEADER_GROUP}, {"table-footer-group", D_TABLE_FOOTER_GROUP},
    {"table-row", D_TABLE_ROW}, {"table-cell", D_TABLE_CELL}, {"table-column", D_TABLE_COLUMN},
    {"table-column-group", D_TABLE_COLUMN_GROUP}, {"table-caption", D_TABLE_CAPTION},
    {"contents", D_CONTENTS}, {"flow-root", D_FLOW_ROOT}, {"-webkit-box", D_BLOCK},
    {"-webkit-inline-box", D_INLINE_BLOCK}, {"-ms-flexbox", D_FLEX}, {"-webkit-flex", D_FLEX},
    {"-ms-inline-flexbox", D_INLINE_FLEX}, {"-webkit-inline-flex", D_INLINE_FLEX},
    {"-ms-grid", D_GRID}, {"run-in", D_BLOCK}, {"ruby", D_INLINE}, {"ruby-text", D_INLINE},
    {"ruby-base", D_INLINE}, {"block flow", D_BLOCK}, {"inline flow", D_INLINE},
    {"inline flow-root", D_INLINE_BLOCK}, {"block flex", D_FLEX}, {"inline flex", D_INLINE_FLEX},
    {"block grid", D_GRID}, {"inline grid", D_INLINE_GRID}, {"block flow-root", D_FLOW_ROOT},
    {"list-item block", D_LIST_ITEM}, {"block list-item", D_LIST_ITEM}, {0, 0}
};
static const struct kw KW_POSITION[] = {
    {"static", POS_STATIC}, {"relative", POS_RELATIVE}, {"absolute", POS_ABSOLUTE},
    {"fixed", POS_FIXED}, {"sticky", POS_STICKY}, {"-webkit-sticky", POS_STICKY}, {0, 0}
};
static const struct kw KW_FLOAT[] = {
    {"none", FL_NONE}, {"left", FL_LEFT}, {"right", FL_RIGHT}, {"inline-start", FL_LEFT},
    {"inline-end", FL_RIGHT}, {0, 0}
};
static const struct kw KW_CLEAR[] = {
    {"none", CLR_NONE}, {"left", CLR_LEFT}, {"right", CLR_RIGHT}, {"both", CLR_BOTH},
    {"inline-start", CLR_LEFT}, {"inline-end", CLR_RIGHT}, {"all", CLR_BOTH}, {0, 0}
};
static const struct kw KW_BOXSIZ[] = { {"content-box", BX_CONTENT}, {"border-box", BX_BORDER}, {0, 0} };
static const struct kw KW_VIS[] = { {"visible", 0}, {"hidden", 1}, {"collapse", 2}, {0, 0} };
static const struct kw KW_OVERFLOW[] = {
    {"visible", OV_VISIBLE}, {"hidden", OV_HIDDEN}, {"scroll", OV_SCROLL}, {"auto", OV_AUTO},
    {"clip", OV_CLIP}, {"overlay", OV_AUTO}, {"-webkit-paged-x", OV_AUTO}, {0, 0}
};
static const struct kw KW_BSTYLE[] = {
    {"none", BS_NONE}, {"hidden", BS_HIDDEN}, {"solid", BS_SOLID}, {"dashed", BS_DASHED},
    {"dotted", BS_DOTTED}, {"double", BS_DOUBLE}, {"groove", BS_GROOVE}, {"ridge", BS_RIDGE},
    {"inset", BS_INSET}, {"outset", BS_OUTSET}, {0, 0}
};
static const struct kw KW_TALIGN[] = {
    {"start", TA_START}, {"left", TA_LEFT}, {"right", TA_RIGHT}, {"center", TA_CENTER},
    {"justify", TA_JUSTIFY}, {"end", TA_END}, {"-webkit-center", TA_WEBKIT_CENTER},
    {"-moz-center", TA_WEBKIT_CENTER}, {"-webkit-left", TA_LEFT}, {"-webkit-right", TA_RIGHT},
    {"-moz-left", TA_LEFT}, {"-moz-right", TA_RIGHT}, {"match-parent", TA_START},
    {"-webkit-match-parent", TA_START}, {"justify-all", TA_JUSTIFY}, {0, 0}
};
static const struct kw KW_TTRANS[] = {
    {"none", TT_NONE}, {"uppercase", TT_UPPER}, {"lowercase", TT_LOWER},
    {"capitalize", TT_CAPITALIZE}, {"full-width", TT_NONE}, {0, 0}
};
static const struct kw KW_WS[] = {
    {"normal", WS_NORMAL}, {"nowrap", WS_NOWRAP}, {"pre", WS_PRE}, {"pre-wrap", WS_PRE_WRAP},
    {"pre-line", WS_PRE_LINE}, {"break-spaces", WS_BREAK_SPACES},
    {"collapse", WS_NORMAL}, {"preserve", WS_PRE_WRAP}, {"preserve nowrap", WS_PRE},
    {"collapse nowrap", WS_NOWRAP}, {"-moz-pre-wrap", WS_PRE_WRAP}, {0, 0}
};
static const struct kw KW_WBREAK[] = {
    {"normal", WB_NORMAL}, {"break-all", WB_BREAK_ALL}, {"keep-all", WB_KEEP_ALL},
    {"break-word", WB_BREAK_WORD}, {"auto-phrase", WB_NORMAL}, {0, 0}
};
static const struct kw KW_OWRAP[] = {
    {"normal", 0}, {"break-word", 1}, {"anywhere", 1}, {0, 0}
};
static const struct kw KW_VALIGN[] = {
    {"baseline", VA_BASELINE}, {"top", VA_TOP}, {"middle", VA_MIDDLE}, {"bottom", VA_BOTTOM},
    {"sub", VA_SUB}, {"super", VA_SUPER}, {"text-top", VA_TEXT_TOP},
    {"text-bottom", VA_TEXT_BOTTOM}, {0, 0}
};
static const struct kw KW_LSTYPE[] = {
    {"disc", LS_DISC}, {"circle", LS_CIRCLE}, {"square", LS_SQUARE}, {"decimal", LS_DECIMAL},
    {"decimal-leading-zero", LS_DECIMAL_LZ}, {"lower-alpha", LS_LOWER_ALPHA},
    {"lower-latin", LS_LOWER_ALPHA}, {"upper-alpha", LS_UPPER_ALPHA},
    {"upper-latin", LS_UPPER_ALPHA}, {"lower-roman", LS_LOWER_ROMAN},
    {"upper-roman", LS_UPPER_ROMAN}, {"lower-greek", LS_LOWER_GREEK}, {"none", LS_NONE},
    {"disclosure-open", LS_DISCLOSURE_OPEN}, {"disclosure-closed", LS_DISCLOSURE_CLOSED},
    {"armenian", LS_DECIMAL}, {"georgian", LS_DECIMAL}, {"cjk-decimal", LS_DECIMAL},
    {"hebrew", LS_DECIMAL}, {"arabic-indic", LS_DECIMAL}, {"cjk-ideographic", LS_DECIMAL},
    {0, 0}
};
static const struct kw KW_LSPOS[] = { {"outside", 0}, {"inside", 1}, {0, 0} };
static const struct kw KW_FDIR[] = {
    {"row", FD_ROW}, {"row-reverse", FD_ROW_REV}, {"column", FD_COL},
    {"column-reverse", FD_COL_REV}, {0, 0}
};
static const struct kw KW_FWRAP[] = {
    {"nowrap", FW_NOWRAP}, {"wrap", FW_WRAP}, {"wrap-reverse", FW_WRAP_REV}, {0, 0}
};
static const struct kw KW_ALIGN[] = {
    {"normal", AL_NORMAL}, {"start", AL_START}, {"end", AL_END}, {"flex-start", AL_START},
    {"flex-end", AL_END}, {"self-start", AL_START}, {"self-end", AL_END},
    {"center", AL_CENTER}, {"stretch", AL_STRETCH}, {"baseline", AL_BASELINE},
    {"first baseline", AL_BASELINE}, {"last baseline", AL_BASELINE},
    {"space-between", AL_SPACE_BETWEEN}, {"space-around", AL_SPACE_AROUND},
    {"space-evenly", AL_SPACE_EVENLY}, {"auto", AL_AUTO}, {"left", AL_LEFT}, {"right", AL_RIGHT},
    {"safe center", AL_CENTER}, {"unsafe center", AL_CENTER}, {"safe end", AL_END},
    {"safe flex-end", AL_END}, {"legacy", AL_NORMAL}, {"legacy center", AL_CENTER},
    {"anchor-center", AL_CENTER}, {"start safe", AL_START}, {0, 0}
};
static const struct kw KW_FSTYLE[] = { {"normal", 0}, {"italic", 1}, {"oblique", 1}, {0, 0} };
static const struct kw KW_FVAR[] = { {"normal", 0}, {"small-caps", 1}, {"none", 0}, {0, 0} };
static const struct kw KW_DSTYLE[] = {
    {"solid", 0}, {"double", 1}, {"dotted", 2}, {"dashed", 3}, {"wavy", 4}, {0, 0}
};
static const struct kw KW_BCOLL[] = { {"separate", 0}, {"collapse", 1}, {0, 0} };
static const struct kw KW_TLAYOUT[] = { {"auto", 0}, {"fixed", 1}, {0, 0} };
static const struct kw KW_CAPSIDE[] = { {"top", 0}, {"bottom", 1}, {"block-start", 0}, {"block-end", 1}, {0, 0} };
static const struct kw KW_OFIT[] = {
    {"fill", OF_FILL}, {"contain", OF_CONTAIN}, {"cover", OF_COVER}, {"none", OF_NONE},
    {"scale-down", OF_SCALE_DOWN}, {0, 0}
};
static const struct kw KW_PEVENTS[] = { {"auto", 0}, {"none", 1}, {"all", 0}, {"visiblepainted", 0}, {0, 0} };
static const struct kw KW_DIR[] = { {"ltr", 0}, {"rtl", 1}, {0, 0} };
static const struct kw KW_TOVER[] = { {"clip", 0}, {"ellipsis", 1}, {0, 0} };
static const struct kw KW_BGREP[] = {
    {"repeat", BG_REPEAT}, {"repeat-x", BG_REPEAT_X}, {"repeat-y", BG_REPEAT_Y},
    {"no-repeat", BG_NO_REPEAT}, {"space", BG_REPEAT}, {"round", BG_REPEAT},
    {"repeat repeat", BG_REPEAT}, {"no-repeat no-repeat", BG_NO_REPEAT},
    {"repeat no-repeat", BG_REPEAT_X}, {"no-repeat repeat", BG_REPEAT_Y}, {0, 0}
};

// value with whitespace runs collapsed, lowercased into buf (for multi-word keywords)
static int norm_kw(const char* s, int len, char* buf, int cap) {
    int o = 0, sp = 0;
    cv_trim(&s, &len);
    for (int i = 0; i < len; i++) {
        if (w_isspace((unsigned char)s[i])) { sp = 1; continue; }
        if (o + (sp && o ? 2 : 1) > cap - 1) break;
        if (sp && o) buf[o++] = ' ';
        sp = 0;
        buf[o++] = (char)w_lower((unsigned char)s[i]);
    }
    buf[o] = 0;
    return o;
}

static int kw_lookup(const struct kw* t, const char* s, int len) {
    char buf[64];
    int n = norm_kw(s, len, buf, sizeof buf);
    for (; t->n; t++) if ((int)strlen(t->n) == n && !memcmp(t->n, buf, n)) return t->v;
    return -1;
}

struct dctx {
    struct wsheet* sh;
    int important;
    uint32_t raw, rawlen;   // the full declaration value (pool)
};

static void emit_kw(struct dctx* c, int prop, int v) {
    struct cdecl d; memset(&d, 0, sizeof d);
    d.prop = (uint16_t)prop; d.important = (uint8_t)c->important; d.kind = VK_KW; d.a = v;
    d.raw = c->raw; d.rawlen = c->rawlen;
    add_decl(c->sh, &d);
}

static void emit_raw(struct dctx* c, int prop, const char* s, int len) {
    struct cdecl d; memset(&d, 0, sizeof d);
    d.prop = (uint16_t)prop; d.important = (uint8_t)c->important; d.kind = VK_RAW;
    cv_trim(&s, &len);
    d.raw = ref_of(c->sh, s, len);
    d.rawlen = (uint32_t)len;
    add_decl(c->sh, &d);
}

static int global_kw(const char* s, int len) {
    char b[16];
    int n = norm_kw(s, len, b, sizeof b);
    if (n == 7 && !memcmp(b, "inherit", 7)) return KW_INHERIT;
    if (n == 7 && !memcmp(b, "initial", 7)) return KW_INITIAL;
    if (n == 5 && !memcmp(b, "unset", 5)) return KW_UNSET;
    if (n == 6 && !memcmp(b, "revert", 6)) return KW_REVERT;
    if (n == 12 && !memcmp(b, "revert-layer", 12)) return KW_REVERT;
    return 0;
}

// Parse a <length-percentage> (+auto/none/min-content...) into d.
// flags: 1 allow auto, 2 allow none, 4 allow negative, 8 allow keywords
// (min/max/fit-content), 16 unitless numbers allowed (as px)
static int parse_len(const char* s, int len, int flags, struct cdecl* d) {
    cv_trim(&s, &len);
    if (len <= 0) return 0;
    if ((flags & 1) && w_ieq(s, len, "auto")) { d->kind = VK_AUTO; return 1; }
    if ((flags & 2) && w_ieq(s, len, "none")) { d->kind = VK_KW; d->a = WL_NONE; return 1; }
    if (flags & 8) {
        if (w_ieq(s, len, "min-content") || w_ieq(s, len, "-webkit-min-content")) { d->kind = VK_KW; d->a = WL_MIN; return 1; }
        if (w_ieq(s, len, "max-content") || w_ieq(s, len, "-webkit-max-content")) { d->kind = VK_KW; d->a = WL_MAX; return 1; }
        if (w_ieq(s, len, "fit-content") || w_ieq(s, len, "-webkit-fit-content") ||
            w_ieq(s, len, "-moz-fit-content") || w_ieq_prefix(s, len, "fit-content(")) { d->kind = VK_KW; d->a = WL_FIT; return 1; }
        if (w_ieq(s, len, "stretch") || w_ieq(s, len, "-webkit-fill-available") ||
            w_ieq(s, len, "-moz-available")) { d->kind = VK_AUTO; return 1; }
    }
    if (w_ieq_prefix(s, len, "calc(") || w_ieq_prefix(s, len, "min(") || w_ieq_prefix(s, len, "max(") ||
        w_ieq_prefix(s, len, "clamp(") || w_ieq_prefix(s, len, "-webkit-calc(")) {
        d->kind = VK_CALC;
        return 1;
    }
    int32_t m; int u;
    if (!cv_number(s, len, &m, &u)) return 0;
    if (m < 0 && !(flags & 4)) return 0;
    if (u == len) {
        if (m == 0 || (flags & 16)) { d->kind = VK_LEN; d->a = m; d->unit = U_PX; return 1; }
        return 0;
    }
    if (s[u] == '%' && u + 1 == len) { d->kind = VK_PCT; d->a = m; return 1; }
    int un = cv_unit(s + u, len - u);
    if (un < 0 || un >= U_DEG) return 0;
    d->kind = VK_LEN; d->a = m; d->unit = (uint8_t)un;
    return 1;
}

static void emit_len(struct dctx* c, int prop, const char* s, int len, int flags) {
    struct cdecl d; memset(&d, 0, sizeof d);
    d.prop = (uint16_t)prop; d.important = (uint8_t)c->important;
    cv_trim(&s, &len);
    if (!parse_len(s, len, flags, &d)) return;
    if (d.kind == VK_CALC) {
        d.raw = ref_of(c->sh, s, len);
        d.rawlen = (uint32_t)len;
    } else { d.raw = c->raw; d.rawlen = c->rawlen; }
    add_decl(c->sh, &d);
}

static int emit_color(struct dctx* c, int prop, const char* s, int len) {
    uint32_t col; int cur;
    if (!cv_color(s, len, &col, &cur)) return 0;
    struct cdecl d; memset(&d, 0, sizeof d);
    d.prop = (uint16_t)prop; d.important = (uint8_t)c->important; d.kind = VK_COLOR;
    d.a = (int32_t)col; d.b = cur;
    add_decl(c->sh, &d);
    return 1;
}

static void emit_num(struct dctx* c, int prop, const char* s, int len, int allow_pct) {
    int32_t m; int u;
    cv_trim(&s, &len);
    if (!cv_number(s, len, &m, &u)) return;
    if (u < len) {
        if (allow_pct && s[u] == '%' && u + 1 == len) m /= 100;
        else return;
    }
    struct cdecl d; memset(&d, 0, sizeof d);
    d.prop = (uint16_t)prop; d.important = (uint8_t)c->important; d.kind = VK_NUM; d.a = m;
    add_decl(c->sh, &d);
}

static void emit_kwt(struct dctx* c, int prop, const struct kw* t, const char* s, int len) {
    int v = kw_lookup(t, s, len);
    if (v >= 0) emit_kw(c, prop, v);
}

// 1-4 value box shorthand (margin/padding/inset/border-width/style/color/radius)
static void box4(struct dctx* c, const int props[4], const char* s, int len, int kind,
                 int flags, const struct kw* t) {
    const char* v[4]; int vl[4]; int n = 0, pos = 0;
    const char* cs; int cl;
    while (n < 4 && cv_next(s, len, &pos, &cs, &cl)) { v[n] = cs; vl[n] = cl; n++; }
    if (!n) return;
    if (cv_next(s, len, &pos, &cs, &cl)) return; // too many components
    int idx[4][4] = { {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3} };
    for (int k = 0; k < 4; k++) {
        int j = idx[n - 1][k];
        if (kind == 0) emit_len(c, props[k], v[j], vl[j], flags);
        else if (kind == 1) emit_kwt(c, props[k], t, v[j], vl[j]);
        else if (kind == 2) emit_color(c, props[k], v[j], vl[j]);
    }
}

static const int P_MARGIN4[4] = { P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM, P_MARGIN_LEFT };
static const int P_PADDING4[4] = { P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT };
static const int P_INSET4[4] = { P_TOP, P_RIGHT, P_BOTTOM, P_LEFT };
static const int P_BW4[4] = { P_BTW, P_BRW, P_BBW, P_BLW };
static const int P_BS4[4] = { P_BTS, P_BRS, P_BBS, P_BLS };
static const int P_BC4[4] = { P_BTC, P_BRC, P_BBC, P_BLC };
static const int P_RAD4[4] = { P_RTL, P_RTR, P_RBR, P_RBL };

static int is_bwidth(const char* s, int len, struct cdecl* d) {
    if (w_ieq(s, len, "thin")) { d->kind = VK_LEN; d->a = 1000; d->unit = U_PX; return 1; }
    if (w_ieq(s, len, "medium")) { d->kind = VK_LEN; d->a = 3000; d->unit = U_PX; return 1; }
    if (w_ieq(s, len, "thick")) { d->kind = VK_LEN; d->a = 5000; d->unit = U_PX; return 1; }
    return parse_len(s, len, 0, d) && d->kind != VK_PCT;
}

static void emit_bwidth(struct dctx* c, int prop, const char* s, int len) {
    struct cdecl d; memset(&d, 0, sizeof d);
    cv_trim(&s, &len);
    if (!is_bwidth(s, len, &d)) return;
    d.prop = (uint16_t)prop; d.important = (uint8_t)c->important;
    if (d.kind == VK_CALC) { d.raw = ref_of(c->sh, s, len); d.rawlen = (uint32_t)len; }
    add_decl(c->sh, &d);
}

// border / border-top ... / outline shorthand: width style color in any order
static void border_sh(struct dctx* c, const int* wp, const int* sp, const int* cp, int nsides,
                      const char* s, int len) {
    int pos = 0; const char* cs; int cl;
    struct cdecl w; memset(&w, 0, sizeof w);
    w.kind = VK_LEN; w.a = 3000; w.unit = U_PX; // medium
    int style = BS_NONE;
    uint32_t col = 0xFF000000; int cur = 1;
    if (w_ieq(s, len, "none") || w_ieq(s, len, "0")) { style = BS_NONE; }
    while (cv_next(s, len, &pos, &cs, &cl)) {
        struct cdecl t; memset(&t, 0, sizeof t);
        int k = kw_lookup(KW_BSTYLE, cs, cl);
        if (k >= 0) { style = k; continue; }
        if (is_bwidth(cs, cl, &t)) { w = t; if (t.kind == VK_CALC) { w.raw = ref_of(c->sh, cs, cl); w.rawlen = (uint32_t)cl; } continue; }
        uint32_t cc; int ccur;
        if (cv_color(cs, cl, &cc, &ccur)) { col = cc; cur = ccur; continue; }
        return; // invalid component: whole declaration invalid
    }
    for (int i = 0; i < nsides; i++) {
        struct cdecl d = w;
        d.prop = (uint16_t)wp[i]; d.important = (uint8_t)c->important;
        add_decl(c->sh, &d);
        emit_kw(c, sp[i], style);
        struct cdecl e; memset(&e, 0, sizeof e);
        e.prop = (uint16_t)cp[i]; e.important = (uint8_t)c->important; e.kind = VK_COLOR;
        e.a = (int32_t)col; e.b = cur;
        add_decl(c->sh, &e);
    }
}

static void font_weight(struct dctx* c, const char* s, int len) {
    cv_trim(&s, &len);
    int v = -1;
    if (w_ieq(s, len, "normal")) v = 400;
    else if (w_ieq(s, len, "bold")) v = 700;
    else if (w_ieq(s, len, "bolder")) v = -2;
    else if (w_ieq(s, len, "lighter")) v = -3;
    else {
        int32_t m; int u;
        if (cv_number(s, len, &m, &u) && u == len && m >= 1000 && m <= 1000000) v = m / 1000;
    }
    if (v == -1) return;
    emit_kw(c, P_FONT_WEIGHT, v);
}

static int is_font_size_kw(const char* s, int len) {
    static const char* const K[] = { "xx-small", "x-small", "small", "medium", "large",
                                     "x-large", "xx-large", "xxx-large", "smaller", "larger", 0 };
    for (int i = 0; K[i]; i++) if (w_ieq(s, len, K[i])) return i + 1;
    return 0;
}

static void font_size(struct dctx* c, const char* s, int len) {
    cv_trim(&s, &len);
    int k = is_font_size_kw(s, len);
    if (k) { emit_kw(c, P_FONT_SIZE, k); return; }
    emit_len(c, P_FONT_SIZE, s, len, 0);
}

static void line_height(struct dctx* c, const char* s, int len) {
    cv_trim(&s, &len);
    if (w_ieq(s, len, "normal")) { emit_kw(c, P_LINE_HEIGHT, 0); return; }
    int32_t m; int u;
    if (cv_number(s, len, &m, &u) && u == len) {
        struct cdecl d; memset(&d, 0, sizeof d);
        d.prop = P_LINE_HEIGHT; d.important = (uint8_t)c->important; d.kind = VK_NUM; d.a = m;
        add_decl(c->sh, &d);
        return;
    }
    emit_len(c, P_LINE_HEIGHT, s, len, 0);
}

static void font_sh(struct dctx* c, const char* s, int len) {
    cv_trim(&s, &len);
    // system font keywords: treat as the default UI font
    if (w_ieq(s, len, "caption") || w_ieq(s, len, "icon") || w_ieq(s, len, "menu") ||
        w_ieq(s, len, "message-box") || w_ieq(s, len, "small-caption") ||
        w_ieq(s, len, "status-bar") || w_ieq(s, len, "-webkit-control")) {
        emit_kw(c, P_FONT_STYLE, 0);
        emit_kw(c, P_FONT_WEIGHT, 400);
        emit_raw(c, P_FONT_FAMILY, "sans-serif", 10);
        return;
    }
    int pos = 0; const char* cs; int cl;
    int style = 0, weight = 400, variant = 0;
    while (cv_next(s, len, &pos, &cs, &cl)) {
        int k;
        if (w_ieq(cs, cl, "normal")) continue;
        if ((k = kw_lookup(KW_FSTYLE, cs, cl)) >= 0) { style = k; continue; }
        if (w_ieq(cs, cl, "small-caps")) { variant = 1; continue; }
        if (w_ieq(cs, cl, "bold")) { weight = 700; continue; }
        if (w_ieq(cs, cl, "bolder")) { weight = 700; continue; }
        if (w_ieq(cs, cl, "lighter")) { weight = 300; continue; }
        int32_t m; int u;
        if (cv_number(cs, cl, &m, &u) && u == cl && m >= 1000 && m <= 1000000 && m % 1000 == 0 &&
            m / 1000 % 100 == 0) { weight = m / 1000; continue; }
        if (w_ieq(cs, cl, "condensed") || w_ieq(cs, cl, "expanded") ||
            w_ieq(cs, cl, "semi-condensed") || w_ieq(cs, cl, "ultra-condensed")) continue;
        // size[/line-height]
        const char* sz = cs; int szl = cl;
        int slash = -1;
        for (int i = 0; i < cl; i++) if (cs[i] == '/') { slash = i; break; }
        if (slash >= 0) szl = slash;
        struct cdecl t; memset(&t, 0, sizeof t);
        if (!is_font_size_kw(sz, szl) && !parse_len(sz, szl, 0, &t)) return; // invalid
        emit_kw(c, P_FONT_STYLE, style);
        emit_kw(c, P_FONT_WEIGHT, weight);
        emit_kw(c, P_FONT_VARIANT, variant);
        font_size(c, sz, szl);
        // line-height: after '/' in this component or as next "/ x"
        if (slash >= 0 && slash + 1 < cl) line_height(c, cs + slash + 1, cl - slash - 1);
        else {
            int save = pos;
            const char* n1; int n1l;
            if (cv_next(s, len, &pos, &n1, &n1l) && n1l == 1 && n1[0] == '/') {
                const char* lh; int lhl;
                if (cv_next(s, len, &pos, &lh, &lhl)) line_height(c, lh, lhl);
            } else if (slash >= 0) {
                const char* lh; int lhl;
                pos = save;
                if (cv_next(s, len, &pos, &lh, &lhl)) line_height(c, lh, lhl);
            } else {
                pos = save;
                emit_kw(c, P_LINE_HEIGHT, 0);
            }
        }
        // the rest is the family list
        while (pos < len && w_isspace((unsigned char)s[pos])) pos++;
        if (pos < len) emit_raw(c, P_FONT_FAMILY, s + pos, len - pos);
        return;
    }
}

static void flex_sh(struct dctx* c, const char* s, int len) {
    cv_trim(&s, &len);
    struct cdecl g, sh, b;
    memset(&g, 0, sizeof g); memset(&sh, 0, sizeof sh); memset(&b, 0, sizeof b);
    g.kind = sh.kind = VK_NUM;
    if (w_ieq(s, len, "none")) { g.a = 0; sh.a = 0; b.kind = VK_AUTO; }
    else if (w_ieq(s, len, "auto")) { g.a = 1000; sh.a = 1000; b.kind = VK_AUTO; }
    else if (w_ieq(s, len, "initial")) { g.a = 0; sh.a = 1000; b.kind = VK_AUTO; }
    else {
        int pos = 0, nn = 0; const char* cs; int cl;
        g.a = 1000; sh.a = 1000; b.kind = VK_LEN; b.a = 0; b.unit = U_PX; // flex: 1 => 1 1 0%
        b.kind = VK_PCT; b.a = 0;
        int got_basis = 0;
        while (cv_next(s, len, &pos, &cs, &cl)) {
            int32_t m; int u;
            if (cv_number(cs, cl, &m, &u) && u == cl && !got_basis) {
                if (nn == 0) g.a = m; else if (nn == 1) sh.a = m; else return;
                nn++;
                continue;
            }
            if (got_basis) return;
            struct cdecl t; memset(&t, 0, sizeof t);
            if (w_ieq(cs, cl, "content")) { t.kind = VK_KW; t.a = WL_CONTENT; }
            else if (!parse_len(cs, cl, 1 | 8, &t)) return;
            if (t.kind == VK_CALC) { t.raw = ref_of(c->sh, cs, cl); t.rawlen = (uint32_t)cl; }
            b = t;
            got_basis = 1;
            if (nn == 0) nn = 0;
        }
    }
    g.prop = P_FLEX_GROW; sh.prop = P_FLEX_SHRINK; b.prop = P_FLEX_BASIS;
    g.important = sh.important = b.important = (uint8_t)c->important;
    add_decl(c->sh, &g);
    add_decl(c->sh, &sh);
    add_decl(c->sh, &b);
}

static void grid_line_pair(struct dctx* c, int p0, int p1, const char* s, int len) {
    int slash = -1;
    for (int i = 0; i < len; i++) if (s[i] == '/') { slash = i; break; }
    if (slash < 0) {
        emit_raw(c, p0, s, len);
        // single custom ident applies to both ends; otherwise end = auto
        const char* t = s; int tl = len; cv_trim(&t, &tl);
        int ident = tl > 0 && !w_isdigit((unsigned char)t[0]) && t[0] != '-' && !w_ieq_prefix(t, tl, "span") &&
                    !w_ieq(t, tl, "auto");
        if (ident) emit_raw(c, p1, s, len);
        else emit_raw(c, p1, "auto", 4);
        return;
    }
    emit_raw(c, p0, s, slash);
    emit_raw(c, p1, s + slash + 1, len - slash - 1);
}

static void grid_area_sh(struct dctx* c, const char* s, int len) {
    const char* v[4]; int vl[4]; int n = 0;
    int st = 0;
    for (int i = 0; i <= len && n < 4; i++) {
        if (i == len || s[i] == '/') { v[n] = s + st; vl[n] = i - st; n++; st = i + 1; }
    }
    // grid-area: row-start / col-start / row-end / col-end
    const char* t = v[0]; int tl = vl[0]; cv_trim(&t, &tl);
    int ident = tl > 0 && !w_isdigit((unsigned char)t[0]) && t[0] != '-' && !w_ieq_prefix(t, tl, "span") &&
                !w_ieq(t, tl, "auto");
    emit_raw(c, P_GRID_ROW_START, v[0], vl[0]);
    emit_raw(c, P_GRID_COLUMN_START, n > 1 ? v[1] : (ident ? v[0] : "auto"), n > 1 ? vl[1] : (ident ? vl[0] : 4));
    emit_raw(c, P_GRID_ROW_END, n > 2 ? v[2] : (ident ? v[0] : "auto"), n > 2 ? vl[2] : (ident ? vl[0] : 4));
    emit_raw(c, P_GRID_COLUMN_END, n > 3 ? v[3] : (ident ? v[0] : "auto"), n > 3 ? vl[3] : (ident ? vl[0] : 4));
}

static int is_image_value(const char* s, int len) {
    return w_ieq_prefix(s, len, "url(") || w_ieq_prefix(s, len, "linear-gradient(") ||
           w_ieq_prefix(s, len, "-webkit-linear-gradient(") ||
           w_ieq_prefix(s, len, "repeating-linear-gradient(") ||
           w_ieq_prefix(s, len, "radial-gradient(") || w_ieq_prefix(s, len, "-webkit-gradient(") ||
           w_ieq_prefix(s, len, "image-set(") || w_ieq_prefix(s, len, "-webkit-image-set(") ||
           w_ieq_prefix(s, len, "repeating-radial-gradient(") || w_ieq_prefix(s, len, "conic-gradient(");
}

static void background_sh(struct dctx* c, const char* s, int len) {
    // layers: take the first layer with an image; color from the final layer
    int pos = 0; const char* layer; int ll;
    const char* layers[8]; int lls[8]; int nl = 0;
    while (nl < 8 && cv_next_comma(s, len, &pos, &layer, &ll)) { layers[nl] = layer; lls[nl] = ll; nl++; }
    if (!nl) return;
    int img_done = 0;
    uint32_t col = 0; int cur = 0;
    int rep = BG_REPEAT;
    const char* img = 0; int imgl = 0;
    const char* posx = 0; int posxl = 0;
    const char* posy = 0; int posyl = 0;
    const char* size = 0; int sizel = 0;
    for (int L = 0; L < nl; L++) {
        int p2 = 0; const char* cs; int cl;
        int after_slash = 0;
        int this_has_img = 0;
        const char* lx = 0; int lxl = 0; const char* ly = 0; int lyl = 0;
        const char* lsz = 0; int lszl = 0;
        int lrep = BG_REPEAT;
        while (cv_next(layers[L], lls[L], &p2, &cs, &cl)) {
            if (cl == 1 && cs[0] == '/') { after_slash = 1; continue; }
            if (after_slash) {
                if (!lsz) { lsz = cs; lszl = cl; }
                else lszl = (int)(cs + cl - lsz);
                continue;
            }
            if (is_image_value(cs, cl)) { if (!this_has_img) { this_has_img = 1; if (!img_done) { img = cs; imgl = cl; } } continue; }
            if (w_ieq(cs, cl, "none")) continue;
            int k = kw_lookup(KW_BGREP, cs, cl);
            if (k >= 0) { lrep = k; continue; }
            if (w_ieq(cs, cl, "scroll") || w_ieq(cs, cl, "fixed") || w_ieq(cs, cl, "local") ||
                w_ieq(cs, cl, "border-box") || w_ieq(cs, cl, "padding-box") ||
                w_ieq(cs, cl, "content-box") || w_ieq(cs, cl, "text")) continue;
            uint32_t cc; int ccur;
            if (L == nl - 1 && cv_color(cs, cl, &cc, &ccur)) { col = cc; cur = ccur; continue; }
            // position component
            if (!lx) { lx = cs; lxl = cl; }
            else if (!ly) { ly = cs; lyl = cl; }
        }
        if (this_has_img && !img_done) {
            img_done = 1;
            rep = lrep;
            posx = lx; posxl = lxl; posy = ly; posyl = lyl;
            size = lsz; sizel = lszl;
        }
    }
    struct cdecl d; memset(&d, 0, sizeof d);
    d.prop = P_BG_COLOR; d.important = (uint8_t)c->important; d.kind = VK_COLOR; d.a = (int32_t)col; d.b = cur;
    add_decl(c->sh, &d);
    if (img) emit_raw(c, P_BG_IMAGE, img, imgl);
    else emit_raw(c, P_BG_IMAGE, "none", 4);
    emit_kw(c, P_BG_REPEAT, rep);
    if (size) emit_raw(c, P_BG_SIZE, size, sizel); else emit_raw(c, P_BG_SIZE, "auto", 4);
    // keywords may come in either order ("top left")
    if (posx && posy && (w_ieq(posx, posxl, "top") || w_ieq(posx, posxl, "bottom") ||
                         w_ieq(posy, posyl, "left") || w_ieq(posy, posyl, "right"))) {
        const char* t = posx; int tl = posxl; posx = posy; posxl = posyl; posy = t; posyl = tl;
    }
    if (posx && !posy && (w_ieq(posx, posxl, "top") || w_ieq(posx, posxl, "bottom"))) {
        posy = posx; posyl = posxl; posx = "center"; posxl = 6;
    }
    emit_raw(c, P_BG_POS_X, posx ? posx : "0%", posx ? posxl : 2);
    emit_raw(c, P_BG_POS_Y, posy ? posy : (posx ? "center" : "0%"), posy ? posyl : (posx ? 6 : 2));
}

static void text_deco_sh(struct dctx* c, const char* s, int len) {
    int pos = 0; const char* cs; int cl;
    int line = 0, style = 0;
    uint32_t col = 0; int cur = 1, have_col = 0;
    while (cv_next(s, len, &pos, &cs, &cl)) {
        if (w_ieq(cs, cl, "none")) continue;
        if (w_ieq(cs, cl, "underline")) { line |= DECO_UNDERLINE; continue; }
        if (w_ieq(cs, cl, "overline")) { line |= DECO_OVERLINE; continue; }
        if (w_ieq(cs, cl, "line-through")) { line |= DECO_LINE_THROUGH; continue; }
        if (w_ieq(cs, cl, "blink")) continue;
        int k = kw_lookup(KW_DSTYLE, cs, cl);
        if (k >= 0) { style = k; continue; }
        uint32_t cc; int ccur;
        if (cv_color(cs, cl, &cc, &ccur)) { col = cc; cur = ccur; have_col = 1; continue; }
        // thickness / auto etc. ignored
    }
    emit_kw(c, P_DECO_LINE, line);
    emit_kw(c, P_DECO_STYLE, style);
    struct cdecl d; memset(&d, 0, sizeof d);
    d.prop = P_DECO_COLOR; d.important = (uint8_t)c->important; d.kind = VK_COLOR;
    d.a = (int32_t)col; d.b = have_col ? cur : 1;
    add_decl(c->sh, &d);
}

static void deco_line(struct dctx* c, const char* s, int len) {
    int pos = 0; const char* cs; int cl, line = 0;
    while (cv_next(s, len, &pos, &cs, &cl)) {
        if (w_ieq(cs, cl, "underline")) line |= DECO_UNDERLINE;
        else if (w_ieq(cs, cl, "overline")) line |= DECO_OVERLINE;
        else if (w_ieq(cs, cl, "line-through")) line |= DECO_LINE_THROUGH;
        else if (!w_ieq(cs, cl, "none") && !w_ieq(cs, cl, "blink")) return;
    }
    emit_kw(c, P_DECO_LINE, line);
}

static void list_style_sh(struct dctx* c, const char* s, int len) {
    int pos = 0; const char* cs; int cl;
    int type = -1, posn = 0, none_count = 0;
    while (cv_next(s, len, &pos, &cs, &cl)) {
        int k;
        if (w_ieq(cs, cl, "none")) { none_count++; continue; }
        if ((k = kw_lookup(KW_LSPOS, cs, cl)) >= 0) { posn = k; continue; }
        if ((k = kw_lookup(KW_LSTYPE, cs, cl)) >= 0) { type = k; continue; }
        if (cs[0] == '"' || cs[0] == '\'') { emit_raw(c, P_LIST_STYLE_TYPE, cs, cl); type = -2; continue; }
        // url(): image markers unsupported -> keep default type
    }
    if (type == -1 && none_count) type = LS_NONE;
    if (type == -1) type = LS_DISC;
    if (type >= 0) emit_kw(c, P_LIST_STYLE_TYPE, type);
    emit_kw(c, P_LIST_STYLE_POSITION, posn);
}

static void gap_sh(struct dctx* c, const char* s, int len) {
    int pos = 0; const char* a; int al; const char* b; int bl;
    if (!cv_next(s, len, &pos, &a, &al)) return;
    if (!cv_next(s, len, &pos, &b, &bl)) { b = a; bl = al; }
    emit_len(c, P_ROW_GAP, a, al, 0);
    emit_len(c, P_COLUMN_GAP, b, bl, 0);
}

static void place_sh(struct dctx* c, int p_align, int p_justify, const char* s, int len) {
    int pos = 0; const char* a; int al; const char* b; int bl;
    if (!cv_next(s, len, &pos, &a, &al)) return;
    int va = kw_lookup(KW_ALIGN, a, al);
    if (va < 0) return;
    int vb = va;
    if (cv_next(s, len, &pos, &b, &bl)) { vb = kw_lookup(KW_ALIGN, b, bl); if (vb < 0) return; }
    emit_kw(c, p_align, va);
    emit_kw(c, p_justify, vb);
}

static void overflow_sh(struct dctx* c, const char* s, int len) {
    int pos = 0; const char* a; int al; const char* b; int bl;
    if (!cv_next(s, len, &pos, &a, &al)) return;
    int va = kw_lookup(KW_OVERFLOW, a, al);
    if (va < 0) return;
    int vb = va;
    if (cv_next(s, len, &pos, &b, &bl)) { vb = kw_lookup(KW_OVERFLOW, b, bl); if (vb < 0) return; }
    emit_kw(c, P_OVERFLOW_X, va);
    emit_kw(c, P_OVERFLOW_Y, vb);
}

static void radius_sh(struct dctx* c, const char* s, int len) {
    // horizontal radii only (before '/')
    int slash = -1;
    for (int i = 0; i < len; i++) if (s[i] == '/') { slash = i; break; }
    box4(c, P_RAD4, s, slash >= 0 ? slash : len, 0, 0, 0);
}

static void radius_one(struct dctx* c, int prop, const char* s, int len) {
    int pos = 0; const char* a; int al;
    if (!cv_next(s, len, &pos, &a, &al)) return;
    emit_len(c, prop, a, al, 0);
}

static void border_spacing(struct dctx* c, const char* s, int len) {
    int pos = 0; const char* a; int al; const char* b; int bl;
    struct cdecl d; memset(&d, 0, sizeof d);
    if (!cv_next(s, len, &pos, &a, &al)) return;
    struct cdecl t1, t2; memset(&t1, 0, sizeof t1); memset(&t2, 0, sizeof t2);
    if (!parse_len(a, al, 0, &t1) || t1.kind != VK_LEN) return;
    if (cv_next(s, len, &pos, &b, &bl)) { if (!parse_len(b, bl, 0, &t2) || t2.kind != VK_LEN) return; }
    else t2 = t1;
    d.prop = P_BORDER_SPACING; d.important = (uint8_t)c->important; d.kind = VK_LEN2;
    d.a = t1.a; d.unit = t1.unit; d.b = t2.a; d.unit2 = t2.unit;
    add_decl(c->sh, &d);
}

static void white_space(struct dctx* c, const char* s, int len) {
    int v = kw_lookup(KW_WS, s, len);
    if (v >= 0) emit_kw(c, P_WHITE_SPACE, v);
}

// logical -> physical (horizontal-tb, ltr)
static void logical_pair(struct dctx* c, int p_start, int p_end, const char* s, int len, int flags) {
    int pos = 0; const char* a; int al; const char* b; int bl;
    if (!cv_next(s, len, &pos, &a, &al)) return;
    if (!cv_next(s, len, &pos, &b, &bl)) { b = a; bl = al; }
    emit_len(c, p_start, a, al, flags);
    emit_len(c, p_end, b, bl, flags);
}

// grid-template / grid: "<rows> / <columns>" or the ASCII-art form
// "'a a' 40px 'b c' 1fr / 1fr 2fr"; "none" resets all three.
static void grid_template_sh(struct dctx* c, const char* s, int len) {
    const char* t = s; int tl = len;
    cv_trim(&t, &tl);
    if (w_ieq(t, tl, "none")) {
        emit_raw(c, P_GRID_TEMPLATE_ROWS, "none", 4);
        emit_raw(c, P_GRID_TEMPLATE_COLUMNS, "none", 4);
        emit_raw(c, P_GRID_TEMPLATE_AREAS, "none", 4);
        return;
    }
    // top-level slash
    int slash = -1, depth = 0;
    for (int i = 0; i < tl; i++) {
        char ch = t[i];
        if (ch == '"' || ch == '\'') { char q = ch; i++; while (i < tl && t[i] != q) i++; continue; }
        if (ch == '(') depth++;
        else if (ch == ')') depth--;
        else if (ch == '/' && !depth) { slash = i; break; }
    }
    const char* rows = t; int rl = slash >= 0 ? slash : tl;
    const char* cols = slash >= 0 ? t + slash + 1 : "none"; int cl = slash >= 0 ? tl - slash - 1 : 4;
    // auto-flow forms of the "grid" shorthand: ignore the flow keyword part
    if (w_ieq_prefix(rows, rl, "auto-flow") || w_ieq_prefix(cols, cl, " auto-flow")) return;
    int has_str = 0;
    for (int i = 0; i < rl; i++) if (rows[i] == '"' || rows[i] == '\'') has_str = 1;
    if (has_str) {
        // split strings (areas) from row sizes
        char areas[1024], sizes[512];
        int na = 0, ns = 0;
        int i = 0;
        while (i < rl) {
            if (rows[i] == '"' || rows[i] == '\'') {
                char q = rows[i];
                int st = i;
                i++;
                while (i < rl && rows[i] != q) i++;
                i++;
                if (na + (i - st) + 1 < (int)sizeof areas) { memcpy(areas + na, rows + st, i - st); na += i - st; areas[na++] = ' '; }
                continue;
            }
            if (ns < (int)sizeof sizes - 1) sizes[ns++] = rows[i];
            i++;
        }
        emit_raw(c, P_GRID_TEMPLATE_AREAS, areas, na);
        int all_ws = 1;
        for (int k = 0; k < ns; k++) if (!w_isspace((unsigned char)sizes[k])) all_ws = 0;
        if (all_ws) emit_raw(c, P_GRID_TEMPLATE_ROWS, "none", 4);
        else emit_raw(c, P_GRID_TEMPLATE_ROWS, sizes, ns);
    } else {
        emit_raw(c, P_GRID_TEMPLATE_ROWS, rows, rl);
        emit_raw(c, P_GRID_TEMPLATE_AREAS, "none", 4);
    }
    emit_raw(c, P_GRID_TEMPLATE_COLUMNS, cols, cl);
}

static int has_var(const char* s, int len) {
    for (int i = 0; i + 4 <= len; i++)
        if ((s[i] == 'v' || s[i] == 'V') && w_ieq_prefix(s + i, len - i, "var(")) return 1;
    for (int i = 0; i + 4 <= len; i++)
        if ((s[i] == 'e' || s[i] == 'E') && w_ieq_prefix(s + i, len - i, "env(")) return 1;
    return 0;
}

// The property table: name -> handler id.
enum {
    H_NONE, H_KW, H_LEN, H_COLOR, H_NUM, H_RAW, H_BOX4LEN, H_BOX4KW, H_BOX4COLOR, H_BORDER,
    H_BORDER_SIDE, H_BWIDTH, H_FONT, H_FWEIGHT, H_FSIZE, H_LHEIGHT, H_FLEX, H_GRIDPAIR,
    H_GRIDAREA, H_BG, H_DECO, H_DECOLINE, H_LIST, H_GAP, H_PLACE, H_OVERFLOW, H_RADIUS,
    H_RADIUS1, H_BSPACING, H_WS, H_LOGICAL, H_ZINDEX, H_ORDER, H_BGCOLOR, H_OUTLINE,
    H_FLEXFLOW, H_INSET, H_LSTYPE, H_IGNORE, H_GRIDTEMPLATE
};

struct propdef {
    const char* name;
    int h;          // handler
    int prop;       // primary longhand
    int p2;         // secondary
    int flags;      // parse_len flags
    const struct kw* kw;
};

static const struct propdef PROPS[] = {
    {"display", H_KW, P_DISPLAY, 0, 0, KW_DISPLAY},
    {"position", H_KW, P_POSITION, 0, 0, KW_POSITION},
    {"float", H_KW, P_FLOAT, 0, 0, KW_FLOAT},
    {"clear", H_KW, P_CLEAR, 0, 0, KW_CLEAR},
    {"box-sizing", H_KW, P_BOX_SIZING, 0, 0, KW_BOXSIZ},
    {"-webkit-box-sizing", H_KW, P_BOX_SIZING, 0, 0, KW_BOXSIZ},
    {"-moz-box-sizing", H_KW, P_BOX_SIZING, 0, 0, KW_BOXSIZ},
    {"visibility", H_KW, P_VISIBILITY, 0, 0, KW_VIS},
    {"overflow", H_OVERFLOW, 0, 0, 0, 0},
    {"overflow-x", H_KW, P_OVERFLOW_X, 0, 0, KW_OVERFLOW},
    {"overflow-y", H_KW, P_OVERFLOW_Y, 0, 0, KW_OVERFLOW},
    {"overflow-inline", H_KW, P_OVERFLOW_X, 0, 0, KW_OVERFLOW},
    {"overflow-block", H_KW, P_OVERFLOW_Y, 0, 0, KW_OVERFLOW},
    {"z-index", H_ZINDEX, P_Z_INDEX, 0, 0, 0},
    {"opacity", H_NUM, P_OPACITY, 1, 0, 0},
    {"width", H_LEN, P_WIDTH, 0, 1 | 8, 0},
    {"height", H_LEN, P_HEIGHT, 0, 1 | 8, 0},
    {"inline-size", H_LEN, P_WIDTH, 0, 1 | 8, 0},
    {"block-size", H_LEN, P_HEIGHT, 0, 1 | 8, 0},
    {"min-width", H_LEN, P_MIN_WIDTH, 0, 1 | 8, 0},
    {"min-height", H_LEN, P_MIN_HEIGHT, 0, 1 | 8, 0},
    {"min-inline-size", H_LEN, P_MIN_WIDTH, 0, 1 | 8, 0},
    {"min-block-size", H_LEN, P_MIN_HEIGHT, 0, 1 | 8, 0},
    {"max-width", H_LEN, P_MAX_WIDTH, 0, 2 | 8, 0},
    {"max-height", H_LEN, P_MAX_HEIGHT, 0, 2 | 8, 0},
    {"max-inline-size", H_LEN, P_MAX_WIDTH, 0, 2 | 8, 0},
    {"max-block-size", H_LEN, P_MAX_HEIGHT, 0, 2 | 8, 0},
    {"margin", H_BOX4LEN, 0, 0, 1 | 4, 0},
    {"margin-top", H_LEN, P_MARGIN_TOP, 0, 1 | 4, 0},
    {"margin-right", H_LEN, P_MARGIN_RIGHT, 0, 1 | 4, 0},
    {"margin-bottom", H_LEN, P_MARGIN_BOTTOM, 0, 1 | 4, 0},
    {"margin-left", H_LEN, P_MARGIN_LEFT, 0, 1 | 4, 0},
    {"margin-block-start", H_LEN, P_MARGIN_TOP, 0, 1 | 4, 0},
    {"margin-block-end", H_LEN, P_MARGIN_BOTTOM, 0, 1 | 4, 0},
    {"margin-inline-start", H_LEN, P_MARGIN_LEFT, 0, 1 | 4, 0},
    {"margin-inline-end", H_LEN, P_MARGIN_RIGHT, 0, 1 | 4, 0},
    {"-webkit-margin-start", H_LEN, P_MARGIN_LEFT, 0, 1 | 4, 0},
    {"-webkit-margin-end", H_LEN, P_MARGIN_RIGHT, 0, 1 | 4, 0},
    {"margin-block", H_LOGICAL, P_MARGIN_TOP, P_MARGIN_BOTTOM, 1 | 4, 0},
    {"margin-inline", H_LOGICAL, P_MARGIN_LEFT, P_MARGIN_RIGHT, 1 | 4, 0},
    {"padding", H_BOX4LEN, 1, 0, 0, 0},
    {"padding-top", H_LEN, P_PADDING_TOP, 0, 0, 0},
    {"padding-right", H_LEN, P_PADDING_RIGHT, 0, 0, 0},
    {"padding-bottom", H_LEN, P_PADDING_BOTTOM, 0, 0, 0},
    {"padding-left", H_LEN, P_PADDING_LEFT, 0, 0, 0},
    {"padding-block-start", H_LEN, P_PADDING_TOP, 0, 0, 0},
    {"padding-block-end", H_LEN, P_PADDING_BOTTOM, 0, 0, 0},
    {"padding-inline-start", H_LEN, P_PADDING_LEFT, 0, 0, 0},
    {"padding-inline-end", H_LEN, P_PADDING_RIGHT, 0, 0, 0},
    {"-webkit-padding-start", H_LEN, P_PADDING_LEFT, 0, 0, 0},
    {"padding-block", H_LOGICAL, P_PADDING_TOP, P_PADDING_BOTTOM, 0, 0},
    {"padding-inline", H_LOGICAL, P_PADDING_LEFT, P_PADDING_RIGHT, 0, 0},
    {"inset", H_INSET, 0, 0, 1 | 4, 0},
    {"top", H_LEN, P_TOP, 0, 1 | 4, 0},
    {"right", H_LEN, P_RIGHT, 0, 1 | 4, 0},
    {"bottom", H_LEN, P_BOTTOM, 0, 1 | 4, 0},
    {"left", H_LEN, P_LEFT, 0, 1 | 4, 0},
    {"inset-block-start", H_LEN, P_TOP, 0, 1 | 4, 0},
    {"inset-block-end", H_LEN, P_BOTTOM, 0, 1 | 4, 0},
    {"inset-inline-start", H_LEN, P_LEFT, 0, 1 | 4, 0},
    {"inset-inline-end", H_LEN, P_RIGHT, 0, 1 | 4, 0},
    {"inset-block", H_LOGICAL, P_TOP, P_BOTTOM, 1 | 4, 0},
    {"inset-inline", H_LOGICAL, P_LEFT, P_RIGHT, 1 | 4, 0},
    {"border", H_BORDER, 0, 0, 0, 0},
    {"border-top", H_BORDER_SIDE, 0, 0, 0, 0},
    {"border-right", H_BORDER_SIDE, 1, 0, 0, 0},
    {"border-bottom", H_BORDER_SIDE, 2, 0, 0, 0},
    {"border-left", H_BORDER_SIDE, 3, 0, 0, 0},
    {"border-block-start", H_BORDER_SIDE, 0, 0, 0, 0},
    {"border-block-end", H_BORDER_SIDE, 2, 0, 0, 0},
    {"border-inline-start", H_BORDER_SIDE, 3, 0, 0, 0},
    {"border-inline-end", H_BORDER_SIDE, 1, 0, 0, 0},
    {"border-block", H_BORDER_SIDE, 4, 0, 0, 0},
    {"border-inline", H_BORDER_SIDE, 5, 0, 0, 0},
    {"border-width", H_BOX4LEN, 2, 0, 0, 0},
    {"border-style", H_BOX4KW, 0, 0, 0, KW_BSTYLE},
    {"border-color", H_BOX4COLOR, 0, 0, 0, 0},
    {"border-top-width", H_BWIDTH, P_BTW, 0, 0, 0},
    {"border-right-width", H_BWIDTH, P_BRW, 0, 0, 0},
    {"border-bottom-width", H_BWIDTH, P_BBW, 0, 0, 0},
    {"border-left-width", H_BWIDTH, P_BLW, 0, 0, 0},
    {"border-top-style", H_KW, P_BTS, 0, 0, KW_BSTYLE},
    {"border-right-style", H_KW, P_BRS, 0, 0, KW_BSTYLE},
    {"border-bottom-style", H_KW, P_BBS, 0, 0, KW_BSTYLE},
    {"border-left-style", H_KW, P_BLS, 0, 0, KW_BSTYLE},
    {"border-top-color", H_COLOR, P_BTC, 0, 0, 0},
    {"border-right-color", H_COLOR, P_BRC, 0, 0, 0},
    {"border-bottom-color", H_COLOR, P_BBC, 0, 0, 0},
    {"border-left-color", H_COLOR, P_BLC, 0, 0, 0},
    {"border-block-start-color", H_COLOR, P_BTC, 0, 0, 0},
    {"border-block-end-color", H_COLOR, P_BBC, 0, 0, 0},
    {"border-inline-start-color", H_COLOR, P_BLC, 0, 0, 0},
    {"border-inline-end-color", H_COLOR, P_BRC, 0, 0, 0},
    {"border-block-start-width", H_BWIDTH, P_BTW, 0, 0, 0},
    {"border-block-end-width", H_BWIDTH, P_BBW, 0, 0, 0},
    {"border-inline-start-width", H_BWIDTH, P_BLW, 0, 0, 0},
    {"border-inline-end-width", H_BWIDTH, P_BRW, 0, 0, 0},
    {"border-radius", H_RADIUS, 0, 0, 0, 0},
    {"-webkit-border-radius", H_RADIUS, 0, 0, 0, 0},
    {"-moz-border-radius", H_RADIUS, 0, 0, 0, 0},
    {"border-top-left-radius", H_RADIUS1, P_RTL, 0, 0, 0},
    {"border-top-right-radius", H_RADIUS1, P_RTR, 0, 0, 0},
    {"border-bottom-right-radius", H_RADIUS1, P_RBR, 0, 0, 0},
    {"border-bottom-left-radius", H_RADIUS1, P_RBL, 0, 0, 0},
    {"border-start-start-radius", H_RADIUS1, P_RTL, 0, 0, 0},
    {"border-start-end-radius", H_RADIUS1, P_RTR, 0, 0, 0},
    {"border-end-end-radius", H_RADIUS1, P_RBR, 0, 0, 0},
    {"border-end-start-radius", H_RADIUS1, P_RBL, 0, 0, 0},
    {"color", H_COLOR, P_COLOR, 0, 0, 0},
    {"background", H_BG, 0, 0, 0, 0},
    {"background-color", H_BGCOLOR, P_BG_COLOR, 0, 0, 0},
    {"background-image", H_RAW, P_BG_IMAGE, 0, 0, 0},
    {"background-repeat", H_KW, P_BG_REPEAT, 0, 0, KW_BGREP},
    {"background-size", H_RAW, P_BG_SIZE, 0, 0, 0},
    {"-webkit-background-size", H_RAW, P_BG_SIZE, 0, 0, 0},
    {"background-position", H_RAW, P_BG_POS_X, 0, 0, 0},
    {"background-position-x", H_RAW, P_BG_POS_X, 0, 0, 0},
    {"background-position-y", H_RAW, P_BG_POS_Y, 0, 0, 0},
    {"font", H_FONT, 0, 0, 0, 0},
    {"font-family", H_RAW, P_FONT_FAMILY, 0, 0, 0},
    {"font-size", H_FSIZE, P_FONT_SIZE, 0, 0, 0},
    {"font-weight", H_FWEIGHT, P_FONT_WEIGHT, 0, 0, 0},
    {"font-style", H_KW, P_FONT_STYLE, 0, 0, KW_FSTYLE},
    {"font-variant", H_KW, P_FONT_VARIANT, 0, 0, KW_FVAR},
    {"font-variant-caps", H_KW, P_FONT_VARIANT, 0, 0, KW_FVAR},
    {"line-height", H_LHEIGHT, P_LINE_HEIGHT, 0, 0, 0},
    {"text-align", H_KW, P_TEXT_ALIGN, 0, 0, KW_TALIGN},
    {"text-decoration", H_DECO, 0, 0, 0, 0},
    {"text-decoration-line", H_DECOLINE, 0, 0, 0, 0},
    {"-webkit-text-decoration-line", H_DECOLINE, 0, 0, 0, 0},
    {"text-decoration-color", H_COLOR, P_DECO_COLOR, 0, 0, 0},
    {"-webkit-text-decoration-color", H_COLOR, P_DECO_COLOR, 0, 0, 0},
    {"text-decoration-style", H_KW, P_DECO_STYLE, 0, 0, KW_DSTYLE},
    {"text-transform", H_KW, P_TEXT_TRANSFORM, 0, 0, KW_TTRANS},
    {"text-indent", H_LEN, P_TEXT_INDENT, 0, 4, 0},
    {"text-overflow", H_KW, P_TEXT_OVERFLOW, 0, 0, KW_TOVER},
    {"letter-spacing", H_LEN, P_LETTER_SPACING, 0, 4, 0},
    {"word-spacing", H_LEN, P_WORD_SPACING, 0, 4, 0},
    {"white-space", H_WS, 0, 0, 0, 0},
    {"white-space-collapse", H_WS, 0, 0, 0, 0},
    {"text-wrap-mode", H_IGNORE, 0, 0, 0, 0},
    {"word-break", H_KW, P_WORD_BREAK, 0, 0, KW_WBREAK},
    {"overflow-wrap", H_KW, P_OVERFLOW_WRAP, 0, 0, KW_OWRAP},
    {"word-wrap", H_KW, P_OVERFLOW_WRAP, 0, 0, KW_OWRAP},
    {"vertical-align", H_LEN, P_VERTICAL_ALIGN, 0, 4, KW_VALIGN},
    {"direction", H_KW, P_DIRECTION, 0, 0, KW_DIR},
    {"list-style", H_LIST, 0, 0, 0, 0},
    {"list-style-type", H_LSTYPE, P_LIST_STYLE_TYPE, 0, 0, KW_LSTYPE},
    {"list-style-position", H_KW, P_LIST_STYLE_POSITION, 0, 0, KW_LSPOS},
    {"flex", H_FLEX, 0, 0, 0, 0},
    {"-webkit-flex", H_FLEX, 0, 0, 0, 0},
    {"-ms-flex", H_FLEX, 0, 0, 0, 0},
    {"flex-flow", H_FLEXFLOW, 0, 0, 0, 0},
    {"flex-direction", H_KW, P_FLEX_DIRECTION, 0, 0, KW_FDIR},
    {"-webkit-flex-direction", H_KW, P_FLEX_DIRECTION, 0, 0, KW_FDIR},
    {"flex-wrap", H_KW, P_FLEX_WRAP, 0, 0, KW_FWRAP},
    {"-webkit-flex-wrap", H_KW, P_FLEX_WRAP, 0, 0, KW_FWRAP},
    {"justify-content", H_KW, P_JUSTIFY_CONTENT, 0, 0, KW_ALIGN},
    {"-webkit-justify-content", H_KW, P_JUSTIFY_CONTENT, 0, 0, KW_ALIGN},
    {"align-items", H_KW, P_ALIGN_ITEMS, 0, 0, KW_ALIGN},
    {"-webkit-align-items", H_KW, P_ALIGN_ITEMS, 0, 0, KW_ALIGN},
    {"align-self", H_KW, P_ALIGN_SELF, 0, 0, KW_ALIGN},
    {"align-content", H_KW, P_ALIGN_CONTENT, 0, 0, KW_ALIGN},
    {"justify-items", H_KW, P_JUSTIFY_ITEMS, 0, 0, KW_ALIGN},
    {"justify-self", H_KW, P_JUSTIFY_SELF, 0, 0, KW_ALIGN},
    {"place-items", H_PLACE, P_ALIGN_ITEMS, P_JUSTIFY_ITEMS, 0, 0},
    {"place-content", H_PLACE, P_ALIGN_CONTENT, P_JUSTIFY_CONTENT, 0, 0},
    {"place-self", H_PLACE, P_ALIGN_SELF, P_JUSTIFY_SELF, 0, 0},
    {"flex-grow", H_NUM, P_FLEX_GROW, 0, 0, 0},
    {"-webkit-flex-grow", H_NUM, P_FLEX_GROW, 0, 0, 0},
    {"flex-shrink", H_NUM, P_FLEX_SHRINK, 0, 0, 0},
    {"-webkit-flex-shrink", H_NUM, P_FLEX_SHRINK, 0, 0, 0},
    {"flex-basis", H_LEN, P_FLEX_BASIS, 0, 1 | 8, 0},
    {"order", H_ORDER, P_ORDER, 0, 0, 0},
    {"gap", H_GAP, 0, 0, 0, 0},
    {"grid-gap", H_GAP, 0, 0, 0, 0},
    {"row-gap", H_LEN, P_ROW_GAP, 0, 0, 0},
    {"column-gap", H_LEN, P_COLUMN_GAP, 0, 0, 0},
    {"grid-row-gap", H_LEN, P_ROW_GAP, 0, 0, 0},
    {"grid-column-gap", H_LEN, P_COLUMN_GAP, 0, 0, 0},
    {"grid-template-columns", H_RAW, P_GRID_TEMPLATE_COLUMNS, 0, 0, 0},
    {"grid-template-rows", H_RAW, P_GRID_TEMPLATE_ROWS, 0, 0, 0},
    {"grid-template-areas", H_RAW, P_GRID_TEMPLATE_AREAS, 0, 0, 0},
    {"grid-auto-flow", H_RAW, P_GRID_AUTO_FLOW, 0, 0, 0},
    {"grid-auto-columns", H_RAW, P_GRID_AUTO_COLUMNS, 0, 0, 0},
    {"grid-auto-rows", H_RAW, P_GRID_AUTO_ROWS, 0, 0, 0},
    {"grid-column", H_GRIDPAIR, P_GRID_COLUMN_START, P_GRID_COLUMN_END, 0, 0},
    {"grid-row", H_GRIDPAIR, P_GRID_ROW_START, P_GRID_ROW_END, 0, 0},
    {"grid-column-start", H_RAW, P_GRID_COLUMN_START, 0, 0, 0},
    {"grid-column-end", H_RAW, P_GRID_COLUMN_END, 0, 0, 0},
    {"grid-row-start", H_RAW, P_GRID_ROW_START, 0, 0, 0},
    {"grid-row-end", H_RAW, P_GRID_ROW_END, 0, 0, 0},
    {"grid-area", H_GRIDAREA, 0, 0, 0, 0},
    {"border-collapse", H_KW, P_BORDER_COLLAPSE, 0, 0, KW_BCOLL},
    {"border-spacing", H_BSPACING, 0, 0, 0, 0},
    {"table-layout", H_KW, P_TABLE_LAYOUT, 0, 0, KW_TLAYOUT},
    {"caption-side", H_KW, P_CAPTION_SIDE, 0, 0, KW_CAPSIDE},
    {"content", H_RAW, P_CONTENT, 0, 0, 0},
    {"box-shadow", H_RAW, P_BOX_SHADOW, 0, 0, 0},
    {"-webkit-box-shadow", H_RAW, P_BOX_SHADOW, 0, 0, 0},
    {"transform", H_RAW, P_TRANSFORM, 0, 0, 0},
    {"-webkit-transform", H_RAW, P_TRANSFORM, 0, 0, 0},
    {"-ms-transform", H_RAW, P_TRANSFORM, 0, 0, 0},
    {"translate", H_RAW, P_TRANSFORM, 0, 0, 0},
    {"aspect-ratio", H_RAW, P_ASPECT_RATIO, 0, 0, 0},
    {"object-fit", H_KW, P_OBJECT_FIT, 0, 0, KW_OFIT},
    {"pointer-events", H_KW, P_POINTER_EVENTS, 0, 0, KW_PEVENTS},
    {"cursor", H_RAW, P_CURSOR, 0, 0, 0},
    {"outline", H_OUTLINE, 0, 0, 0, 0},
    {"outline-width", H_BWIDTH, P_OUTLINE_WIDTH, 0, 0, 0},
    {"outline-style", H_KW, P_OUTLINE_STYLE, 0, 0, KW_BSTYLE},
    {"outline-color", H_COLOR, P_OUTLINE_COLOR, 0, 0, 0},
    {"fill", H_RAW, P_FILL, 0, 0, 0},
    {"stroke", H_RAW, P_STROKE, 0, 0, 0},
    {"stroke-width", H_LEN, P_STROKE_WIDTH, 0, 16, 0},
    {"mask-image", H_RAW, P_MASK, 0, 0, 0},
    {"-webkit-mask-image", H_RAW, P_MASK, 0, 0, 0},
    {"mask", H_RAW, P_MASK, 0, 0, 0},
    {"-webkit-mask", H_RAW, P_MASK, 0, 0, 0},
    {"-webkit-mask-box-image", H_RAW, P_MASK, 0, 0, 0},
    {"grid-template", H_GRIDTEMPLATE, 0, 0, 0, 0},
    {"grid", H_GRIDTEMPLATE, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0}
};

#define PHASH 512
static int16_t phash[PHASH];
static int phash_ready;

static uint32_t name_hash(const char* s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)w_lower((unsigned char)s[i]); h *= 16777619u; }
    return h;
}

static void phash_init(void) {
    for (int i = 0; i < PHASH; i++) phash[i] = -1;
    for (int i = 0; PROPS[i].name; i++) {
        uint32_t h = name_hash(PROPS[i].name, (int)strlen(PROPS[i].name)) & (PHASH - 1);
        while (phash[h] >= 0) h = (h + 1) & (PHASH - 1);
        phash[h] = (int16_t)i;
    }
    phash_ready = 1;
}

static const struct propdef* prop_find(const char* name, int len) {
    if (!phash_ready) phash_init();
    uint32_t h = name_hash(name, len) & (PHASH - 1);
    while (phash[h] >= 0) {
        const struct propdef* p = &PROPS[phash[h]];
        if (w_ieq(name, len, p->name)) return p;
        h = (h + 1) & (PHASH - 1);
    }
    return 0;
}

// Dispatch one property value. `val` may live in the pool.
static int apply_prop(struct dctx* c, const struct propdef* pd, const char* v, int vl) {
    int before = c->sh->ndecl;
    switch (pd->h) {
    case H_KW: {
        if (pd->prop == P_VERTICAL_ALIGN) break;
        emit_kwt(c, pd->prop, pd->kw, v, vl);
        break;
    }
    case H_LEN:
        if (pd->prop == P_VERTICAL_ALIGN) {
            int k = kw_lookup(KW_VALIGN, v, vl);
            if (k >= 0) emit_kw(c, P_VERTICAL_ALIGN, k);
            else emit_len(c, P_VERTICAL_ALIGN, v, vl, 4);
        } else emit_len(c, pd->prop, v, vl, pd->flags);
        break;
    case H_COLOR: emit_color(c, pd->prop, v, vl); break;
    case H_BGCOLOR: emit_color(c, P_BG_COLOR, v, vl); break;
    case H_NUM: emit_num(c, pd->prop, v, vl, pd->p2); break;
    case H_RAW: emit_raw(c, pd->prop, v, vl); break;
    case H_ZINDEX: {
        const char* s = v; int l = vl; cv_trim(&s, &l);
        if (w_ieq(s, l, "auto")) { emit_kw(c, P_Z_INDEX, 0); c->sh->decls[c->sh->ndecl - 1].kind = VK_AUTO; }
        else emit_num(c, P_Z_INDEX, v, vl, 0);
        break;
    }
    case H_ORDER: emit_num(c, P_ORDER, v, vl, 0); break;
    case H_BOX4LEN:
        if (pd->prop == 0) box4(c, P_MARGIN4, v, vl, 0, pd->flags, 0);
        else if (pd->prop == 1) box4(c, P_PADDING4, v, vl, 0, 0, 0);
        else {
            // border-width: thin/medium/thick allowed
            const char* cv[4]; int cvl[4]; int n = 0, pos = 0; const char* cs; int cl;
            while (n < 4 && cv_next(v, vl, &pos, &cs, &cl)) { cv[n] = cs; cvl[n] = cl; n++; }
            if (!n) break;
            int idx[4][4] = { {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3} };
            for (int k = 0; k < 4; k++) emit_bwidth(c, P_BW4[k], cv[idx[n - 1][k]], cvl[idx[n - 1][k]]);
        }
        break;
    case H_INSET: box4(c, P_INSET4, v, vl, 0, pd->flags, 0); break;
    case H_BOX4KW: box4(c, P_BS4, v, vl, 1, 0, pd->kw); break;
    case H_BOX4COLOR: box4(c, P_BC4, v, vl, 2, 0, 0); break;
    case H_BORDER: border_sh(c, P_BW4, P_BS4, P_BC4, 4, v, vl); break;
    case H_BORDER_SIDE: {
        int sd = pd->prop;
        if (sd < 4) {
            int wp[1] = { P_BW4[sd] }, sp[1] = { P_BS4[sd] }, cp[1] = { P_BC4[sd] };
            border_sh(c, wp, sp, cp, 1, v, vl);
        } else if (sd == 4) { // block: top + bottom
            int wp[2] = { P_BTW, P_BBW }, sp[2] = { P_BTS, P_BBS }, cp[2] = { P_BTC, P_BBC };
            border_sh(c, wp, sp, cp, 2, v, vl);
        } else {
            int wp[2] = { P_BLW, P_BRW }, sp[2] = { P_BLS, P_BRS }, cp[2] = { P_BLC, P_BRC };
            border_sh(c, wp, sp, cp, 2, v, vl);
        }
        break;
    }
    case H_OUTLINE: {
        int wp[1] = { P_OUTLINE_WIDTH }, sp[1] = { P_OUTLINE_STYLE }, cp[1] = { P_OUTLINE_COLOR };
        border_sh(c, wp, sp, cp, 1, v, vl);
        break;
    }
    case H_BWIDTH: emit_bwidth(c, pd->prop, v, vl); break;
    case H_FONT: font_sh(c, v, vl); break;
    case H_FWEIGHT: font_weight(c, v, vl); break;
    case H_FSIZE: font_size(c, v, vl); break;
    case H_LHEIGHT: line_height(c, v, vl); break;
    case H_FLEX: flex_sh(c, v, vl); break;
    case H_FLEXFLOW: {
        int pos = 0; const char* cs; int cl;
        while (cv_next(v, vl, &pos, &cs, &cl)) {
            int k;
            if ((k = kw_lookup(KW_FDIR, cs, cl)) >= 0) emit_kw(c, P_FLEX_DIRECTION, k);
            else if ((k = kw_lookup(KW_FWRAP, cs, cl)) >= 0) emit_kw(c, P_FLEX_WRAP, k);
        }
        break;
    }
    case H_GRIDPAIR: grid_line_pair(c, pd->prop, pd->p2, v, vl); break;
    case H_GRIDAREA: grid_area_sh(c, v, vl); break;
    case H_BG: background_sh(c, v, vl); break;
    case H_DECO: text_deco_sh(c, v, vl); break;
    case H_DECOLINE: deco_line(c, v, vl); break;
    case H_LIST: list_style_sh(c, v, vl); break;
    case H_LSTYPE: {
        const char* s = v; int l = vl; cv_trim(&s, &l);
        if (l && (s[0] == '"' || s[0] == '\'')) emit_raw(c, P_LIST_STYLE_TYPE, s, l);
        else emit_kwt(c, P_LIST_STYLE_TYPE, KW_LSTYPE, s, l);
        break;
    }
    case H_GAP: gap_sh(c, v, vl); break;
    case H_PLACE: place_sh(c, pd->prop, pd->p2, v, vl); break;
    case H_OVERFLOW: overflow_sh(c, v, vl); break;
    case H_RADIUS: radius_sh(c, v, vl); break;
    case H_RADIUS1: radius_one(c, pd->prop, v, vl); break;
    case H_BSPACING: border_spacing(c, v, vl); break;
    case H_WS: white_space(c, v, vl); break;
    case H_LOGICAL: logical_pair(c, pd->prop, pd->p2, v, vl, pd->flags); break;
    case H_IGNORE: break;
    case H_GRIDTEMPLATE: grid_template_sh(c, v, vl); break;
    }
    return c->sh->ndecl - before;
}

// Shorthand longhand lists used when a var() shorthand / global keyword
// must cover every longhand.
static int longhands_of(const struct propdef* pd, int* out) {
    int n = 0;
    switch (pd->h) {
    case H_BOX4LEN:
        if (pd->prop == 0) for (int k = 0; k < 4; k++) out[n++] = P_MARGIN4[k];
        else if (pd->prop == 1) for (int k = 0; k < 4; k++) out[n++] = P_PADDING4[k];
        else for (int k = 0; k < 4; k++) out[n++] = P_BW4[k];
        break;
    case H_INSET: for (int k = 0; k < 4; k++) out[n++] = P_INSET4[k]; break;
    case H_BOX4KW: for (int k = 0; k < 4; k++) out[n++] = P_BS4[k]; break;
    case H_BOX4COLOR: for (int k = 0; k < 4; k++) out[n++] = P_BC4[k]; break;
    case H_BORDER:
        for (int k = 0; k < 4; k++) { out[n++] = P_BW4[k]; out[n++] = P_BS4[k]; out[n++] = P_BC4[k]; }
        break;
    case H_BORDER_SIDE: {
        int sd = pd->prop;
        if (sd < 4) { out[n++] = P_BW4[sd]; out[n++] = P_BS4[sd]; out[n++] = P_BC4[sd]; }
        break;
    }
    case H_OUTLINE: out[n++] = P_OUTLINE_WIDTH; out[n++] = P_OUTLINE_STYLE; out[n++] = P_OUTLINE_COLOR; break;
    case H_RADIUS: for (int k = 0; k < 4; k++) out[n++] = P_RAD4[k]; break;
    case H_FONT:
        out[n++] = P_FONT_STYLE; out[n++] = P_FONT_WEIGHT; out[n++] = P_FONT_VARIANT;
        out[n++] = P_FONT_SIZE; out[n++] = P_LINE_HEIGHT; out[n++] = P_FONT_FAMILY;
        break;
    case H_FLEX: out[n++] = P_FLEX_GROW; out[n++] = P_FLEX_SHRINK; out[n++] = P_FLEX_BASIS; break;
    case H_FLEXFLOW: out[n++] = P_FLEX_DIRECTION; out[n++] = P_FLEX_WRAP; break;
    case H_GRIDPAIR: out[n++] = pd->prop; out[n++] = pd->p2; break;
    case H_GRIDAREA:
        out[n++] = P_GRID_ROW_START; out[n++] = P_GRID_COLUMN_START;
        out[n++] = P_GRID_ROW_END; out[n++] = P_GRID_COLUMN_END;
        break;
    case H_BG:
        out[n++] = P_BG_COLOR; out[n++] = P_BG_IMAGE; out[n++] = P_BG_REPEAT;
        out[n++] = P_BG_SIZE; out[n++] = P_BG_POS_X; out[n++] = P_BG_POS_Y;
        break;
    case H_DECO: out[n++] = P_DECO_LINE; out[n++] = P_DECO_STYLE; out[n++] = P_DECO_COLOR; break;
    case H_DECOLINE: out[n++] = P_DECO_LINE; break;
    case H_LIST: out[n++] = P_LIST_STYLE_TYPE; out[n++] = P_LIST_STYLE_POSITION; break;
    case H_GAP: out[n++] = P_ROW_GAP; out[n++] = P_COLUMN_GAP; break;
    case H_PLACE: case H_LOGICAL: out[n++] = pd->prop; out[n++] = pd->p2; break;
    case H_OVERFLOW: out[n++] = P_OVERFLOW_X; out[n++] = P_OVERFLOW_Y; break;
    case H_BSPACING: out[n++] = P_BORDER_SPACING; break;
    case H_WS: out[n++] = P_WHITE_SPACE; break;
    case H_IGNORE: break;
    case H_GRIDTEMPLATE:
        out[n++] = P_GRID_TEMPLATE_ROWS; out[n++] = P_GRID_TEMPLATE_COLUMNS; out[n++] = P_GRID_TEMPLATE_AREAS;
        break;
    default: out[n++] = pd->prop; break;
    }
    return n;
}

// One declaration "name: value [!important]".
static void parse_decl(struct wsheet* sh, const char* s, int len) {
    int colon = -1;
    for (int i = 0; i < len; i++) if (s[i] == ':') { colon = i; break; }
    if (colon <= 0) return;
    const char* name = s; int nl = colon;
    cv_trim(&name, &nl);
    const char* val = s + colon + 1; int vl = len - colon - 1;
    cv_trim(&val, &vl);
    int important = 0;
    // !important (possibly "! important")
    for (int i = vl - 1; i >= 0; i--) {
        if (val[i] == '!') {
            const char* t = val + i + 1; int tl = vl - i - 1;
            cv_trim(&t, &tl);
            if (w_ieq(t, tl, "important")) { important = 1; vl = i; cv_trim(&val, &vl); }
            break;
        }
        if (!w_isalpha((unsigned char)val[i]) && !w_isspace((unsigned char)val[i])) break;
    }
    struct dctx c;
    c.sh = sh;
    c.important = important;
    c.raw = ref_of(sh, val, vl);
    c.rawlen = (uint32_t)vl;
    if (nl > 2 && name[0] == '-' && name[1] == '-') {
        struct cdecl d; memset(&d, 0, sizeof d);
        d.prop = P_CUSTOM; d.important = (uint8_t)important; d.kind = VK_RAW;
        d.raw = c.raw; d.rawlen = c.rawlen;
        d.name = ref_of(sh, name, nl); d.namelen = (uint32_t)nl;
        add_decl(sh, &d);
        return;
    }
    const struct propdef* pd = prop_find(name, nl);
    if (!pd) return;
    int gk = global_kw(val, vl);
    if (gk || has_var(val, vl)) {
        int lh[16];
        int n = longhands_of(pd, lh);
        for (int k = 0; k < n; k++) {
            struct cdecl d; memset(&d, 0, sizeof d);
            d.prop = (uint16_t)lh[k]; d.important = (uint8_t)important;
            d.raw = c.raw; d.rawlen = c.rawlen;
            if (gk) { d.kind = VK_KW; d.a = gk; }
            else {
                d.kind = VK_VAR;
                d.name = ref_of(sh, name, nl); d.namelen = (uint32_t)nl;
                d.b = k; // index of this longhand within the shorthand expansion
            }
            add_decl(sh, &d);
        }
        return;
    }
    apply_prop(&c, pd, val, vl);
}

int cparse_value(struct wsheet* sh, const char* name, int nlen, const char* val, int vlen,
                 int important) {
    const struct propdef* pd = prop_find(name, nlen);
    if (!pd) return 0;
    // val is external (never the pool): copy it in and parse from val itself,
    // mapping pointers back to the copy.
    uint32_t off = pool_put(sh, val, vlen);
    const char* osrc = sh->src; uint32_t ooff = sh->src_off; int olen = sh->src_len;
    sh->src = val; sh->src_off = off; sh->src_len = vlen;
    struct dctx c;
    c.sh = sh;
    c.important = important;
    c.raw = off;
    c.rawlen = (uint32_t)vlen;
    int gk = global_kw(val, vlen);
    int n;
    if (gk) {
        int lh[16];
        n = longhands_of(pd, lh);
        for (int k = 0; k < n; k++) emit_kw(&c, lh[k], gk);
    } else n = apply_prop(&c, pd, val, vlen);
    sh->src = osrc; sh->src_off = ooff; sh->src_len = olen;
    return n;
}

// ---- rule structure ----------------------------------------------------------------

struct pctx {
    struct wsheet* sh;
    struct watoms* atoms;
    uint32_t order;
};

static void parse_block_items(struct pctx* pc, const char* s, int len, int mq,
                              const char* parent_sel, int parent_len);

static void add_style_rule(struct pctx* pc, const char* sel, int sl, int decl_first,
                           int decl_count, int mq) {
    struct selp p;
    memset(&p, 0, sizeof p);
    p.sh = pc->sh;
    p.atoms = pc->atoms;
    p.s = sel;
    p.len = sl;
    int first, count;
    uint32_t ms;
    if (parse_sel_list(&p, 0, 0, &first, &count, &ms) < 0 || p.bad) return;
    for (int k = 0; k < count; k++) {
        struct crule r;
        memset(&r, 0, sizeof r);
        r.sel = first + k;
        r.spec = pc->sh->sels[first + k].spec;
        r.pseudo = pc->sh->sels[first + k].pseudo;
        r.decl = decl_first;
        r.ndecl = decl_count;
        r.mq = mq;
        r.order = pc->order++;
        add_rule(pc->sh, &r);
    }
}

// Build the nested selector text: '&' -> :is(parent); no '&' -> ":is(parent) X".
static int nest_selector(const char* parent, int plen, const char* sel, int sl, char* out, int cap) {
    int o = 0;
    #define PUT(str, n) do { int n_ = (n); if (o + n_ < cap) { memcpy(out + o, (str), n_); o += n_; } } while (0)
    int has_amp = 0;
    for (int i = 0; i < sl; i++) if (sel[i] == '&') has_amp = 1;
    // split the nested list on top-level commas
    int pos = 0; const char* item; int il;
    int firstitem = 1;
    while (cv_next_comma(sel, sl, &pos, &item, &il)) {
        if (!firstitem) PUT(",", 1);
        firstitem = 0;
        int amp = 0;
        for (int i = 0; i < il; i++) if (item[i] == '&') amp = 1;
        if (!amp) {
            PUT(":is(", 4); PUT(parent, plen); PUT(") ", 2);
            PUT(item, il);
        } else {
            for (int i = 0; i < il; i++) {
                if (item[i] == '&') { PUT(":is(", 4); PUT(parent, plen); PUT(")", 1); }
                else PUT(item + i, 1);
            }
        }
    }
    (void)has_amp;
    #undef PUT
    out[o] = 0;
    return o;
}

static int supports_cond(struct pctx* pc, const char* s, int len);

static int supports_cond(struct pctx* pc, const char* s, int len) {
    cv_trim(&s, &len);
    if (len <= 0) return 0;
    if (w_ieq_prefix(s, len, "not ")) return !supports_cond(pc, s + 4, len - 4);
    // split on top-level " and " / " or "
    int depth = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (!depth && i > 0 && w_isspace((unsigned char)s[i - 1])) {
            if (w_ieq_prefix(s + i, len - i, "and ")) return supports_cond(pc, s, i) && supports_cond(pc, s + i + 4, len - i - 4);
            if (w_ieq_prefix(s + i, len - i, "or ")) return supports_cond(pc, s, i) || supports_cond(pc, s + i + 3, len - i - 3);
        }
    }
    if (w_ieq_prefix(s, len, "selector(") && s[len - 1] == ')') {
        struct selp p; memset(&p, 0, sizeof p);
        p.sh = pc->sh; p.atoms = pc->atoms; p.s = s + 9; p.len = len - 10;
        int f, cnt; uint32_t ms;
        int npart = pc->sh->npart, nsel = pc->sh->nsel;
        int ok = parse_sel_list(&p, 0, 0, &f, &cnt, &ms) == 0 && !p.bad;
        pc->sh->npart = npart; pc->sh->nsel = nsel;
        return ok;
    }
    if (s[0] == '(' && s[len - 1] == ')') {
        const char* in = s + 1; int il = len - 2;
        cv_trim(&in, &il);
        if (il && in[0] == '(') return supports_cond(pc, in, il);
        int colon = -1;
        for (int i = 0; i < il; i++) if (in[i] == ':') { colon = i; break; }
        if (colon < 0) return supports_cond(pc, in, il);
        const char* nm = in; int nl = colon; cv_trim(&nm, &nl);
        const char* v = in + colon + 1; int vl = il - colon - 1; cv_trim(&v, &vl);
        if (nl > 2 && nm[0] == '-' && nm[1] == '-') return 1;
        const struct propdef* pd = prop_find(nm, nl);
        if (!pd) return 0;
        // trial parse: does the value produce declarations?
        int nd = pc->sh->ndecl;
        int pl = pc->sh->pool.len;
        int got = cparse_value(pc->sh, nm, nl, v, vl, 0);
        pc->sh->ndecl = nd;
        pc->sh->pool.len = pl;
        if (got > 0) return 1;
        // values our parser keeps raw are "supported" if the property is known,
        // except for obvious unsupported functions
        return 0;
    }
    return 0;
}

// Parse a run of top-level rules (sheet body or an at-rule block).
static void parse_rules(struct pctx* pc, const char* s, int len, int mq) {
    int i = 0;
    while (i < len) {
        while (i < len && (w_isspace((unsigned char)s[i]) || s[i] == ';')) i++;
        if (i >= len) break;
        if (s[i] == '}') { i++; continue; }
        if (s[i] == '@') {
            int ns = i + 1, ne = ns;
            while (ne < len && (is_name_char((unsigned char)s[ne]))) ne++;
            const char* name = s + ns; int nl = ne - ns;
            int stop = scan_to(s, len, ne, ";{}");
            const char* pre = s + ne; int pl = stop - ne;
            cv_trim(&pre, &pl);
            if (stop >= len || s[stop] == ';' || s[stop] == '}') {
                // statement at-rule
                if (w_ieq(name, nl, "import")) {
                    // url("x") or "x"
                    const char* u = pre; int ul = pl;
                    if (w_ieq_prefix(u, ul, "url(")) {
                        u += 4;
                        int k = 0;
                        while (k < ul - 4 && u[k] != ')') k++;
                        ul = k;
                    } else {
                        int k = 0;
                        if (ul && (u[0] == '"' || u[0] == '\'')) {
                            char q = u[0]; u++; ul--;
                            while (k < ul && u[k] != q) k++;
                            ul = k;
                        }
                    }
                    cv_trim(&u, &ul);
                    if (ul && (u[0] == '"' || u[0] == '\'')) { u++; ul -= 2; }
                    if (ul > 0) {
                        char abs[256];
                        css_resolve_url(pc->sh->base, u, ul, abs, sizeof abs);
                        wbuf_put(&pc->sh->imports, abs, (int)strlen(abs) + 1);
                        pc->sh->nimports++;
                    }
                }
                i = stop < len ? stop + 1 : len;
                continue;
            }
            int be = block_end(s, len, stop + 1);
            const char* body = s + stop + 1; int bl = be - stop - 1;
            if (w_ieq(name, nl, "media") || w_ieq(name, nl, "container")) {
                const char* cond = pre; int cl = pl;
                if (w_ieq(name, nl, "container")) {
                    // optional container name before the condition
                    int k = 0;
                    while (k < cl && cond[k] != '(' && !w_ieq_prefix(cond + k, cl - k, "not ")) k++;
                    cond += k; cl -= k;
                }
                uint32_t off = pool_put(pc->sh, cond, cl);
                int m = add_mq(pc->sh, off, (uint32_t)cl);
                if (m >= 0 && mq >= 0) {
                    // nested media: AND with the outer condition, stored as
                    // "outer\x01inner" (evaluated as both)
                    int ol = (int)pc->sh->mqs[mq].rawlen;
                    char* tmp = (char*)w_malloc(ol + cl + 1);
                    if (!tmp) { i = be < len ? be + 1 : len; continue; }
                    memcpy(tmp, pc->sh->pool.p + pc->sh->mqs[mq].raw, ol);
                    tmp[ol] = 1;
                    memcpy(tmp + ol + 1, pc->sh->pool.p + off, cl);
                    uint32_t o2 = pool_put(pc->sh, tmp, ol + cl + 1);
                    w_free(tmp);
                    pc->sh->mqs[m].raw = o2;
                    pc->sh->mqs[m].rawlen = (uint32_t)(pc->sh->pool.len - (int)o2);
                }
                parse_rules(pc, body, bl, m);
            } else if (w_ieq(name, nl, "supports")) {
                if (supports_cond(pc, pre, pl)) parse_rules(pc, body, bl, mq);
            } else if (w_ieq(name, nl, "layer") || w_ieq(name, nl, "scope") ||
                       w_ieq(name, nl, "document") || w_ieq(name, nl, "-moz-document")) {
                parse_rules(pc, body, bl, mq);
            }
            // @font-face, @keyframes, @page, @font-feature-values, @counter-style,
            // @property, @starting-style...: skipped
            i = be < len ? be + 1 : len;
            continue;
        }
        // qualified rule
        int stop = scan_to(s, len, i, "{;}");
        if (stop >= len) break;
        if (s[stop] != '{') { i = stop + 1; continue; }
        int be = block_end(s, len, stop + 1);
        const char* sel = s + i; int sl = stop - i;
        cv_trim(&sel, &sl);
        parse_block_items(pc, s + stop + 1, be - stop - 1, mq, sel, sl);
        i = be < len ? be + 1 : len;
    }
}

// Declarations + nested rules inside a style rule's block.
static void parse_block_items(struct pctx* pc, const char* s, int len, int mq,
                              const char* sel, int sl) {
    int dfirst = pc->sh->ndecl;
    // first pass: declarations (nested rules are parsed after, so their
    // decls stay out of this rule's contiguous range)
    struct { int s, e, be; } nested[64];
    int nn = 0;
    int i = 0;
    while (i < len) {
        while (i < len && (w_isspace((unsigned char)s[i]) || s[i] == ';')) i++;
        if (i >= len) break;
        int stop = scan_to(s, len, i, ";{}");
        if (stop < len && s[stop] == '{') {
            int be = block_end(s, len, stop + 1);
            if (nn < 64) { nested[nn].s = i; nested[nn].e = stop; nested[nn].be = be; nn++; }
            i = be < len ? be + 1 : len;
            continue;
        }
        parse_decl(pc->sh, s + i, stop - i);
        i = stop < len ? stop + 1 : len;
    }
    int dcount = pc->sh->ndecl - dfirst;
    if (sl > 0 && dcount > 0) add_style_rule(pc, sel, sl, dfirst, dcount, mq);
    for (int k = 0; k < nn; k++) {
        const char* ns = s + nested[k].s; int nsl = nested[k].e - nested[k].s;
        cv_trim(&ns, &nsl);
        const char* body = s + nested[k].e + 1; int bl = nested[k].be - nested[k].e - 1;
        if (nsl > 0 && ns[0] == '@') {
            // nested conditional group rule: its declarations apply to the parent selector
            int ne = 1;
            while (ne < nsl && is_name_char((unsigned char)ns[ne])) ne++;
            const char* pre = ns + ne; int pl = nsl - ne;
            cv_trim(&pre, &pl);
            if (w_ieq(ns + 1, ne - 1, "media") || w_ieq(ns + 1, ne - 1, "container")) {
                uint32_t off = pool_put(pc->sh, pre, pl);
                int m = add_mq(pc->sh, off, (uint32_t)pl);
                parse_block_items(pc, body, bl, m, sel, sl);
            } else if (w_ieq(ns + 1, ne - 1, "supports")) {
                if (supports_cond(pc, pre, pl)) parse_block_items(pc, body, bl, mq, sel, sl);
            } else if (w_ieq(ns + 1, ne - 1, "layer")) {
                parse_block_items(pc, body, bl, mq, sel, sl);
            }
            continue;
        }
        char buf[2048];
        int n = nest_selector(sel, sl, ns, nsl, buf, sizeof buf);
        // the nested block's selector text must live in the pool (selectors
        // are compiled immediately, so a stack buffer is fine)
        parse_block_items(pc, body, bl, mq, buf, n);
    }
}

void csheet_parse(struct wsheet* sh, struct watoms* atoms, const char* text, int len) {
    int sl;
    uint32_t off = strip_into_pool(sh, text, len, &sl);
    char* dup = (char*)w_malloc(sl + 1);
    if (!dup) return;
    memcpy(dup, sh->pool.p + off, sl);
    dup[sl] = 0;
    struct pctx pc;
    pc.sh = sh;
    pc.atoms = atoms;
    pc.order = (uint32_t)sh->nrule;
    sh->src = dup; sh->src_off = off; sh->src_len = sl;
    parse_rules(&pc, dup, sl, sh->outer_mq);
    sh->src = 0; sh->src_len = 0;
    w_free(dup);
}

void csheet_parse_decls(struct wsheet* sh, struct watoms* atoms, const char* text, int len,
                        int* first, int* count) {
    (void)atoms;
    int sl;
    uint32_t off = strip_into_pool(sh, text, len, &sl);
    char* dup = (char*)w_malloc(sl + 1);
    *first = sh->ndecl;
    *count = 0;
    if (!dup) return;
    memcpy(dup, sh->pool.p + off, sl);
    sh->src = dup; sh->src_off = off; sh->src_len = sl;
    int i = 0;
    while (i < sl) {
        int stop = scan_to(dup, sl, i, ";");
        parse_decl(sh, dup + i, stop - i);
        i = stop + 1;
    }
    sh->src = 0; sh->src_len = 0;
    w_free(dup);
    *count = sh->ndecl - *first;
}

// Attach a media condition (the media="" attribute of <link>/<style>) to a
// sheet before parsing; every rule of the sheet is gated by it.
void csheet_set_media(struct wsheet* sh, const char* media, int len) {
    sh->outer_mq = -1;
    const char* m = media;
    int ml = len;
    cv_trim(&m, &ml);
    if (ml <= 0 || w_ieq(m, ml, "all") || w_ieq(m, ml, "screen")) return;
    uint32_t off = pool_put(sh, m, ml);
    sh->outer_mq = add_mq(sh, off, (uint32_t)ml);
}

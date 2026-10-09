// lex.c — ECMAScript lexical grammar (ECMA-262 §12, Annex B.1 comments
// and legacy literals).

#include "lex.h"
#include "unicode.h"
#include "atoms.h"

double num_from_decimal(const char* digits, int nd, int exp10);   // num.c

static const char* const KEYWORDS[] = {
    "await", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do",
    "else", "enum", "export", "extends", "false", "finally", "for", "function", "if", "import", "in",
    "instanceof", "new", "null", "return", "super", "switch", "this", "throw", "true", "try", "typeof",
    "var", "void", "while", "with", 0
};

int lex_is_keyword(struct str* s) {
    uint32_t n = str_len(s);
    if (n < 2 || n > 10 || str_wide(s)) return 0;
    for (int i = 1; KEYWORDS[i]; i++)   // "await" is contextual (index 0 skipped)
        if (strlen(KEYWORDS[i]) == n && !memcmp(KEYWORDS[i], s->u.c8, n)) return T_BREAK + i - 1;
    return 0;
}

const char* tok_name(int t) {
    static const char* const P[] = { "end of input", "error", "identifier", "private name", "number", "bigint",
        "string", "template", "regular expression", "{", "}", "(", ")", "[", "]", ".", "...", ";", ",", "<", ">",
        "<=", ">=", "==", "!=", "===", "!==", "+", "-", "*", "%", "**", "++", "--", "<<", ">>", ">>>", "&", "|", "^",
        "!", "~", "&&", "||", "??", "?", "?.", ":", "=", "+=", "-=", "*=", "%=", "**=", "<<=", ">>=", ">>>=", "&=",
        "|=", "^=", "&&=", "||=", "?\?=", "=>", "/", "/=", "@" };
    if (t >= 0 && t < (int)(sizeof P / sizeof P[0])) return P[t];
    if (t >= T_BREAK && t < T_COUNT) return KEYWORDS[t - T_BREAK + 1];
    return "token";
}

void lex_init(struct lexer* L, ojs* J, struct str* src, int module) {
    memset(L, 0, sizeof *L);
    L->J = J;
    L->src = src;
    L->len = str_len(src);
    L->module = module;
    L->strict = module;
    // hashbang comment
    if (L->len >= 2 && str_at(src, 0) == '#' && str_at(src, 1) == '!') {
        uint32_t i = 2;
        while (i < L->len) {
            uint32_t c = str_at(src, i);
            if (c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029) break;
            i++;
        }
        L->pos = i;
    }
}

void lex_free(struct lexer* L) { ojs_sys_free(L->line_starts); L->line_starts = 0; }

static int lerr(struct lexer* L, uint32_t pos, const char* msg) {
    if (!L->err[0]) {
        size_t n = strlen(msg);
        if (n >= sizeof L->err) n = sizeof L->err - 1;
        memcpy(L->err, msg, n);
        L->err[n] = 0;
        L->err_pos = pos;
    }
    L->t.type = T_ERROR;
    return T_ERROR;
}

static int is_lt(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
static int is_ws_cu(uint32_t c) { return uni_is_white_space(c); }

void lex_line_col(struct lexer* L, uint32_t pos, int* line, int* col) {
    if (!L->line_starts) {
        uint32_t cap = 256, n = 0;
        uint32_t* t = (uint32_t*)ojs_sys_malloc(cap * 4);
        if (!t) { *line = 1; *col = (int)pos + 1; return; }
        t[n++] = 0;
        for (uint32_t i = 0; i < L->len; i++) {
            uint32_t c = str_at(L->src, i);
            if (c == '\r' && i + 1 < L->len && str_at(L->src, i + 1) == '\n') continue;
            if (is_lt(c)) {
                if (n >= cap) {
                    cap *= 2;
                    uint32_t* nt = (uint32_t*)ojs_sys_realloc(t, cap * 4);
                    if (!nt) break;
                    t = nt;
                }
                t[n++] = i + 1;
            }
        }
        L->line_starts = t;
        L->nlines = n;
    }
    int lo = 0, hi = (int)L->nlines - 1;
    while (lo < hi) {
        int m = (lo + hi + 1) >> 1;
        if (L->line_starts[m] <= pos) lo = m; else hi = m - 1;
    }
    *line = lo + 1;
    *col = (int)(pos - L->line_starts[lo]) + 1;
}

// skip white space and comments; returns 1 if a line terminator was crossed, -1 on error
static int skip_trivia(struct lexer* L) {
    int nl = L->seen_token ? 0 : 1;   // the start of input counts as a line start for `-->`
    int real_nl = 0;
    for (;;) {
        uint32_t c = lex_cu(L, L->pos);
        if (c == 0xFFFFFFFFu) return real_nl;
        if (is_lt(c)) { nl = 1; real_nl = 1; L->pos++; continue; }
        if (is_ws_cu(c)) { L->pos++; continue; }
        if (c == '/') {
            uint32_t d = lex_cu(L, L->pos + 1);
            if (d == '/') {
                L->pos += 2;
                while (L->pos < L->len && !is_lt(str_at(L->src, L->pos))) L->pos++;
                continue;
            }
            if (d == '*') {
                uint32_t st = L->pos;
                L->pos += 2;
                for (;;) {
                    if (L->pos >= L->len) { lerr(L, st, "unterminated comment"); return -1; }
                    uint32_t e = str_at(L->src, L->pos);
                    if (e == '*' && lex_cu(L, L->pos + 1) == '/') { L->pos += 2; break; }
                    if (is_lt(e)) { nl = 1; real_nl = 1; }
                    L->pos++;
                }
                continue;
            }
        }
        if (!L->module) {
            // <!-- single-line HTML open comment
            if (c == '<' && lex_cu(L, L->pos + 1) == '!' && lex_cu(L, L->pos + 2) == '-' && lex_cu(L, L->pos + 3) == '-') {
                while (L->pos < L->len && !is_lt(str_at(L->src, L->pos))) L->pos++;
                continue;
            }
            // --> at the start of a line (only white space / comments before it on the line)
            if (c == '-' && lex_cu(L, L->pos + 1) == '-' && lex_cu(L, L->pos + 2) == '>' && nl) {
                while (L->pos < L->len && !is_lt(str_at(L->src, L->pos))) L->pos++;
                continue;
            }
        }
        return real_nl;
    }
}

static int hexval(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

// \uXXXX or \u{X..} at L->pos (after the backslash): code point or -1
static int32_t read_unicode_escape(struct lexer* L) {
    if (lex_cu(L, L->pos) != 'u') return -1;
    L->pos++;
    if (lex_cu(L, L->pos) == '{') {
        L->pos++;
        uint32_t v = 0;
        int n = 0;
        for (;;) {
            uint32_t c = lex_cu(L, L->pos);
            if (c == '}') { L->pos++; break; }
            int h = hexval(c);
            if (h < 0) return -1;
            v = v * 16 + (uint32_t)h;
            if (v > 0x10FFFF) return -1;
            n++;
            L->pos++;
        }
        return n ? (int32_t)v : -1;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(lex_cu(L, L->pos));
        if (h < 0) return -1;
        v = v * 16 + (uint32_t)h;
        L->pos++;
    }
    return (int32_t)v;
}

// code point at pos (surrogate pairs joined); *n = units used
static uint32_t cp_at_pos(struct lexer* L, uint32_t pos, int* n) {
    uint32_t c = lex_cu(L, pos);
    *n = 1;
    if (c >= 0xD800 && c <= 0xDBFF) {
        uint32_t d = lex_cu(L, pos + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) { *n = 2; return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); }
    }
    return c;
}

static int lex_ident(struct lexer* L, int priv) {
    struct sbuf b;
    sb_init(L->J, &b);
    int first = 1, esc = 0;
    for (;;) {
        uint32_t c = lex_cu(L, L->pos);
        uint32_t cp;
        if (c == '\\') {
            L->pos++;
            int32_t e = read_unicode_escape(L);
            if (e < 0) { sb_free(&b); return lerr(L, L->pos, "invalid Unicode escape in identifier"); }
            cp = (uint32_t)e;
            esc = 1;
            if (first ? !uni_id_start_cp(cp) : !uni_id_continue_cp(cp)) { sb_free(&b); return lerr(L, L->pos, "invalid identifier character"); }
        } else {
            int n;
            cp = cp_at_pos(L, L->pos, &n);
            if (c == 0xFFFFFFFFu) break;
            if (first ? !uni_id_start_cp(cp) : !uni_id_continue_cp(cp)) break;
            L->pos += (uint32_t)n;
        }
        sb_put_cp(&b, cp);
        first = 0;
    }
    if (first) { sb_free(&b); return lerr(L, L->pos, priv ? "invalid private name" : "invalid identifier"); }
    jv s = sb_done(&b);
    if (s == JV_EXC) return lerr(L, L->pos, "out of memory");
    struct str* a = atom_str(L->J, jv_str(s));
    if (!a) return lerr(L, L->pos, "out of memory");
    L->t.str = a;
    L->t.escaped = esc;
    if (priv) return L->t.type = T_PRIVATE;
    int kw = lex_is_keyword(a);
    // an escaped reserved word is never the keyword: it is an identifier
    // token the parser rejects wherever an identifier is not allowed
    return L->t.type = (kw && !esc) ? kw : T_IDENT;
}

static int lex_number(struct lexer* L) {
    uint32_t st = L->pos;
    uint32_t c = lex_cu(L, L->pos);
    char stackbuf[128];
    char* dig = stackbuf;
    int cap = (int)sizeof stackbuf, nd = 0, exp10 = 0;
    int radix = 10, bigint = 0, legacy = 0;
    if (c == '0') {
        uint32_t d = lex_cu(L, L->pos + 1) | 0x20;
        if (d == 'x') radix = 16;
        else if (d == 'o') radix = 8;
        else if (d == 'b') radix = 2;
        else if (lex_cu(L, L->pos + 1) >= '0' && lex_cu(L, L->pos + 1) <= '9') legacy = 1;
    }
    if (radix != 10) {
        L->pos += 2;
        double v = 0;
        int n = 0, last_sep = 1;
        uint32_t ds = L->pos;
        for (;;) {
            c = lex_cu(L, L->pos);
            if (c == '_') {
                if (last_sep) return lerr(L, L->pos, "invalid numeric separator");
                last_sep = 1;
                L->pos++;
                continue;
            }
            int h = hexval(c);
            if (h < 0 || h >= radix) break;
            v = v * radix + h;
            n++;
            last_sep = 0;
            L->pos++;
        }
        if (!n || last_sep) return lerr(L, L->pos, "invalid number");
        if (lex_cu(L, L->pos) == 'n') { L->pos++; bigint = 1; }
        // exact value for long literals (more than 53 bits)
        if (!bigint && n * (radix == 16 ? 4 : radix == 8 ? 3 : 1) > 53) {
            struct sbuf b;
            sb_init(L->J, &b);
            sb_putc(&b, '0'); sb_putc(&b, radix == 16 ? 'x' : radix == 8 ? 'o' : 'b');
            for (uint32_t i = ds; i < L->pos; i++) if (str_at(L->src, i) != '_') sb_putc(&b, str_at(L->src, i));
            jv sv = sb_done(&b);
            if (sv != JV_EXC) v = num_from_str(jv_str(sv), 0);
        }
        L->t.num = v;
        goto done;
    }
    if (legacy) {
        // 0777 (octal) or 0889 (decimal with leading zero): both legacy
        int octal = 1;
        uint32_t i = L->pos + 1;
        while (lex_cu(L, i) >= '0' && lex_cu(L, i) <= '9') { if (lex_cu(L, i) >= '8') octal = 0; i++; }
        L->t.legacy_octal = 1;
        if (octal) {
            double v = 0;
            for (uint32_t k = L->pos + 1; k < i; k++) v = v * 8 + (str_at(L->src, k) - '0');
            L->pos = i;
            L->t.num = v;
            goto done;
        }
        // decimal-with-leading-zero continues below (no separators, no bigint)
    }
    {
        int last_sep = 0, seen_dot = 0, any = 0;
        for (;;) {
            c = lex_cu(L, L->pos);
            if (c >= '0' && c <= '9') {
                if (nd >= cap) {
                    int nc = cap * 2;
                    char* t = (char*)ojs_sys_malloc((size_t)nc);
                    if (!t) { if (dig != stackbuf) ojs_sys_free(dig); return lerr(L, L->pos, "out of memory"); }
                    memcpy(t, dig, (size_t)nd);
                    if (dig != stackbuf) ojs_sys_free(dig);
                    dig = t;
                    cap = nc;
                }
                dig[nd++] = (char)c;
                if (seen_dot) exp10--;
                last_sep = 0;
                any = 1;
                L->pos++;
            } else if (c == '_' && !legacy) {
                int lead0 = !seen_dot && str_at(L->src, st) == '0' && L->pos == st + 1;
                if (!any || last_sep || lead0) { if (dig != stackbuf) ojs_sys_free(dig); return lerr(L, L->pos, "invalid numeric separator"); }
                uint32_t nx = lex_cu(L, L->pos + 1);
                if (nx < '0' || nx > '9') { if (dig != stackbuf) ojs_sys_free(dig); return lerr(L, L->pos, "invalid numeric separator"); }
                last_sep = 1;
                L->pos++;
            } else if (c == '.' && !seen_dot) {
                if (last_sep) break;
                seen_dot = 1;
                any = 0;   // a separator may not follow the dot
                L->pos++;
                if (lex_cu(L, L->pos) == '_') { if (dig != stackbuf) ojs_sys_free(dig); return lerr(L, L->pos, "invalid numeric separator"); }
                any = 0;
            } else break;
        }
        if (!seen_dot && !legacy && lex_cu(L, L->pos) == 'n') {
            L->pos++;
            bigint = 1;
        } else {
            c = lex_cu(L, L->pos);
            if ((c | 0x20) == 'e') {
                uint32_t save = L->pos;
                L->pos++;
                int neg = 0;
                c = lex_cu(L, L->pos);
                if (c == '+' || c == '-') { neg = c == '-'; L->pos++; }
                int ev = 0, en = 0, ls = 1;
                for (;;) {
                    c = lex_cu(L, L->pos);
                    if (c >= '0' && c <= '9') { if (ev < 100000) ev = ev * 10 + (int)(c - '0'); en++; ls = 0; L->pos++; }
                    else if (c == '_' && !ls) { uint32_t nx = lex_cu(L, L->pos + 1); if (nx < '0' || nx > '9') break; ls = 1; L->pos++; }
                    else break;
                }
                if (!en) { (void)save; if (dig != stackbuf) ojs_sys_free(dig); return lerr(L, L->pos, "invalid number"); }
                exp10 += neg ? -ev : ev;
            }
        }
        L->t.num = bigint ? 0 : num_from_decimal(dig, nd, exp10);
        if (dig != stackbuf) ojs_sys_free(dig);
    }
done:
    {
        // the number must not be followed by an identifier start or digit
        int n;
        uint32_t cp = cp_at_pos(L, L->pos, &n);
        if (L->pos < L->len && (uni_id_start_cp(cp) || (cp >= '0' && cp <= '9') || cp == '\\'))
            return lerr(L, L->pos, "invalid number");
    }
    if (bigint) {
        // the literal's text (without separators / suffix) for BigInt parsing
        struct sbuf b;
        sb_init(L->J, &b);
        for (uint32_t i = st; i < L->pos - 1; i++) if (str_at(L->src, i) != '_') sb_putc(&b, str_at(L->src, i));
        jv s = sb_done(&b);
        if (s == JV_EXC) return lerr(L, L->pos, "out of memory");
        L->t.str = jv_str(s);
        return L->t.type = T_BIGINT;
    }
    return L->t.type = T_NUM;
}

// escape sequence inside a string or template (L->pos at the char after '\').
// Appends to b. Returns 0 ok, -1 error (msg set), -2 invalid in a template (cooked undefined)
static int lex_escape(struct lexer* L, struct sbuf* b, int tmpl) {
    uint32_t c = lex_cu(L, L->pos);
    switch (c) {
    case 'n': sb_putc(b, '\n'); L->pos++; return 0;
    case 't': sb_putc(b, '\t'); L->pos++; return 0;
    case 'r': sb_putc(b, '\r'); L->pos++; return 0;
    case 'b': sb_putc(b, '\b'); L->pos++; return 0;
    case 'f': sb_putc(b, '\f'); L->pos++; return 0;
    case 'v': sb_putc(b, '\v'); L->pos++; return 0;
    case '\r':
        L->pos++;
        if (lex_cu(L, L->pos) == '\n') L->pos++;
        return 0;
    case '\n': case 0x2028: case 0x2029: L->pos++; return 0;   // line continuation
    case 'x': {
        int h1 = hexval(lex_cu(L, L->pos + 1)), h2 = hexval(lex_cu(L, L->pos + 2));
        if (h1 < 0 || h2 < 0) { if (tmpl) return -2; lerr(L, L->pos, "invalid hexadecimal escape"); return -1; }
        sb_putc(b, (uint32_t)(h1 * 16 + h2));
        L->pos += 3;
        return 0;
    }
    case 'u': {
        uint32_t save = L->pos;
        int32_t cp = read_unicode_escape(L);
        if (cp < 0) { if (tmpl) { L->pos = save + 1; return -2; } lerr(L, save, "invalid Unicode escape"); return -1; }
        sb_put_cp(b, (uint32_t)cp);
        return 0;
    }
    default:
        if (c >= '0' && c <= '7') {
            uint32_t d = lex_cu(L, L->pos + 1);
            if (c == '0' && !(d >= '0' && d <= '9')) { sb_putc(b, 0); L->pos++; return 0; }
            if (tmpl) return -2;
            // legacy octal escape
            L->t.bad_escape = 1;
            uint32_t v = c - '0';
            L->pos++;
            if (lex_cu(L, L->pos) >= '0' && lex_cu(L, L->pos) <= '7') {
                v = v * 8 + (lex_cu(L, L->pos) - '0');
                L->pos++;
                if (c <= '3' && lex_cu(L, L->pos) >= '0' && lex_cu(L, L->pos) <= '7') {
                    v = v * 8 + (lex_cu(L, L->pos) - '0');
                    L->pos++;
                }
            }
            sb_putc(b, v);
            return 0;
        }
        if (c == '8' || c == '9') {
            if (tmpl) return -2;
            L->t.bad_escape = 1;
            sb_putc(b, c);
            L->pos++;
            return 0;
        }
        if (c == 0xFFFFFFFFu) { lerr(L, L->pos, "unterminated string"); return -1; }
        {
            int n;
            uint32_t cp = cp_at_pos(L, L->pos, &n);
            sb_put_cp(b, cp);
            L->pos += (uint32_t)n;
        }
        return 0;
    }
}

static int lex_string(struct lexer* L, uint32_t q) {
    struct sbuf b;
    sb_init(L->J, &b);
    L->pos++;
    for (;;) {
        uint32_t c = lex_cu(L, L->pos);
        if (c == 0xFFFFFFFFu || c == '\n' || c == '\r') { sb_free(&b); return lerr(L, L->pos, "unterminated string literal"); }
        if (c == q) { L->pos++; break; }
        if (c == '\\') {
            L->pos++;
            if (lex_escape(L, &b, 0) < 0) { sb_free(&b); return T_ERROR; }
            continue;
        }
        sb_putc(&b, c);
        L->pos++;
    }
    jv s = sb_done(&b);
    if (s == JV_EXC) return lerr(L, L->pos, "out of memory");
    L->t.str = jv_str(s);
    return L->t.type = T_STRING;
}

// template characters from L->pos up to '`' or '${'
static int lex_template_part(struct lexer* L) {
    struct sbuf cooked, raw;
    sb_init(L->J, &cooked);
    sb_init(L->J, &raw);
    int bad = 0;
    for (;;) {
        uint32_t c = lex_cu(L, L->pos);
        if (c == 0xFFFFFFFFu) { sb_free(&cooked); sb_free(&raw); return lerr(L, L->pos, "unterminated template literal"); }
        if (c == '`') { L->pos++; L->t.template_tail = 1; break; }
        if (c == '$' && lex_cu(L, L->pos + 1) == '{') { L->pos += 2; L->t.template_tail = 0; break; }
        if (c == '\\') {
            uint32_t es = L->pos;
            L->pos++;
            int r = lex_escape(L, &cooked, 1);
            if (r == -1) { sb_free(&cooked); sb_free(&raw); return T_ERROR; }
            if (r == -2) {
                bad = 1;
                // skip the rest of the bad escape: one char (the next char after '\' already consumed by -2 paths)
                if (L->pos == es + 1) L->pos++;
            }
            // raw text: line terminators normalized
            for (uint32_t i = es; i < L->pos; i++) {
                uint32_t r2 = str_at(L->src, i);
                if (r2 == '\r') { sb_putc(&raw, '\n'); if (i + 1 < L->pos && str_at(L->src, i + 1) == '\n') i++; }
                else sb_putc(&raw, r2);
            }
            continue;
        }
        if (c == '\r') {   // CR and CRLF become LF in both cooked and raw
            L->pos++;
            if (lex_cu(L, L->pos) == '\n') L->pos++;
            sb_putc(&cooked, '\n');
            sb_putc(&raw, '\n');
            continue;
        }
        sb_putc(&cooked, c);
        sb_putc(&raw, c);
        L->pos++;
    }
    jv cs = sb_done(&cooked);
    jv rs = sb_done(&raw);
    if (cs == JV_EXC || rs == JV_EXC) return lerr(L, L->pos, "out of memory");
    L->t.str = bad ? 0 : jv_str(cs);
    L->t.raw = jv_str(rs);
    L->t.bad_escape = bad;
    return L->t.type = T_TEMPLATE;
}

int lex_template_continue(struct lexer* L) {
    // current token is '}' : the template resumes right after it
    L->pos = L->t.end;
    L->t.start = L->pos - 1;
    L->t.bad_escape = 0;
    int r = lex_template_part(L);
    L->t.end = L->pos;
    return r;
}

void lex_rescan_regex(struct lexer* L) {
    uint32_t st = L->t.start;
    L->pos = st + 1;
    int in_class = 0;
    for (;;) {
        uint32_t c = lex_cu(L, L->pos);
        if (c == 0xFFFFFFFFu || is_lt(c)) { lerr(L, st, "unterminated regular expression"); return; }
        if (c == '\\') {
            uint32_t d = lex_cu(L, L->pos + 1);
            if (d == 0xFFFFFFFFu || is_lt(d)) { lerr(L, st, "unterminated regular expression"); return; }
            L->pos += 2;
            continue;
        }
        if (c == '[') in_class = 1;
        else if (c == ']') in_class = 0;
        else if (c == '/' && !in_class) break;
        L->pos++;
    }
    uint32_t body_end = L->pos;
    L->pos++;
    uint32_t fs = L->pos;
    for (;;) {
        int n;
        uint32_t cp = cp_at_pos(L, L->pos, &n);
        if (L->pos >= L->len) break;
        if (cp == '\\') { lerr(L, L->pos, "invalid regular expression flags"); return; }
        if (!uni_id_continue_cp(cp)) break;
        L->pos += (uint32_t)n;
    }
    jv body = jstr_sub(L->J, jv_from_str(L->src), st + 1, body_end);
    jv flags = jstr_sub(L->J, jv_from_str(L->src), fs, L->pos);
    if (body == JV_EXC || flags == JV_EXC) { lerr(L, st, "out of memory"); return; }
    L->t.raw = str_flat(L->J, body);
    L->t.flags = str_flat(L->J, flags);
    L->t.type = T_REGEXP;
    L->t.end = L->pos;
}

int lex_next(struct lexer* L, int regex_ok) {
    struct token* t = &L->t;
    t->str = 0; t->raw = 0; t->flags = 0; t->escaped = 0; t->legacy_octal = 0; t->bad_escape = 0;
    t->template_tail = 0;
    int nl = skip_trivia(L);
    if (nl < 0) return t->type = T_ERROR;
    t->nl_before = nl;
    t->start = L->pos;
    if (L->pos >= L->len) { t->end = L->pos; return t->type = T_EOF; }
    uint32_t c = str_at(L->src, L->pos);
    uint32_t c1 = lex_cu(L, L->pos + 1), c2 = lex_cu(L, L->pos + 2);
    int r;
#define P1(tok) do { L->pos += 1; r = (tok); goto out; } while (0)
#define P2(tok) do { L->pos += 2; r = (tok); goto out; } while (0)
#define P3(tok) do { L->pos += 3; r = (tok); goto out; } while (0)
#define P4(tok) do { L->pos += 4; r = (tok); goto out; } while (0)
    switch (c) {
    case '{': P1(T_LBRACE);
    case '}': P1(T_RBRACE);
    case '(': P1(T_LPAREN);
    case ')': P1(T_RPAREN);
    case '[': P1(T_LBRACK);
    case ']': P1(T_RBRACK);
    case ';': P1(T_SEMI);
    case ',': P1(T_COMMA);
    case ':': P1(T_COLON);
    case '~': P1(T_TILDE);
    case '@': P1(T_AT);
    case '.':
        if (c1 >= '0' && c1 <= '9') { r = lex_number(L); goto out; }
        if (c1 == '.' && c2 == '.') P3(T_ELLIPSIS);
        P1(T_DOT);
    case '<':
        if (c1 == '<') { if (c2 == '=') P3(T_SHL_ASSIGN); P2(T_SHL); }
        if (c1 == '=') P2(T_LE);
        P1(T_LT);
    case '>':
        if (c1 == '>') {
            if (c2 == '>') { if (lex_cu(L, L->pos + 3) == '=') P4(T_SHR_ASSIGN); P3(T_SHR); }
            if (c2 == '=') P3(T_SAR_ASSIGN);
            P2(T_SAR);
        }
        if (c1 == '=') P2(T_GE);
        P1(T_GT);
    case '=':
        if (c1 == '=') { if (c2 == '=') P3(T_SEQ); P2(T_EQ); }
        if (c1 == '>') P2(T_ARROW);
        P1(T_ASSIGN);
    case '!':
        if (c1 == '=') { if (c2 == '=') P3(T_SNE); P2(T_NE); }
        P1(T_NOT);
    case '+':
        if (c1 == '+') P2(T_INC);
        if (c1 == '=') P2(T_PLUS_ASSIGN);
        P1(T_PLUS);
    case '-':
        if (c1 == '-') P2(T_DEC);
        if (c1 == '=') P2(T_MINUS_ASSIGN);
        P1(T_MINUS);
    case '*':
        if (c1 == '*') { if (c2 == '=') P3(T_STARSTAR_ASSIGN); P2(T_STARSTAR); }
        if (c1 == '=') P2(T_STAR_ASSIGN);
        P1(T_STAR);
    case '%':
        if (c1 == '=') P2(T_PERCENT_ASSIGN);
        P1(T_PERCENT);
    case '&':
        if (c1 == '&') { if (c2 == '=') P3(T_AND_ASSIGN); P2(T_AND); }
        if (c1 == '=') P2(T_AMP_ASSIGN);
        P1(T_AMP);
    case '|':
        if (c1 == '|') { if (c2 == '=') P3(T_OR_ASSIGN); P2(T_OR); }
        if (c1 == '=') P2(T_PIPE_ASSIGN);
        P1(T_PIPE);
    case '^':
        if (c1 == '=') P2(T_CARET_ASSIGN);
        P1(T_CARET);
    case '?':
        if (c1 == '?') { if (c2 == '=') P3(T_NULLISH_ASSIGN); P2(T_NULLISH); }
        if (c1 == '.' && !(c2 >= '0' && c2 <= '9')) P2(T_OPTCHAIN);
        P1(T_QUESTION);
    case '/':
        if (regex_ok) {
            t->type = T_SLASH;
            t->end = L->pos + 1;
            lex_rescan_regex(L);
            return t->type;
        }
        if (c1 == '=') P2(T_SLASH_ASSIGN);
        P1(T_SLASH);
    case '"': case '\'': r = lex_string(L, c); goto out;
    case '`': L->pos++; r = lex_template_part(L); goto out;
    case '#': L->pos++; r = lex_ident(L, 1); goto out;
    default:
        if (c >= '0' && c <= '9') { r = lex_number(L); goto out; }
        {
            int n;
            uint32_t cp = cp_at_pos(L, L->pos, &n);
            if (c == '\\' || uni_id_start_cp(cp)) { r = lex_ident(L, 0); goto out; }
        }
        r = lerr(L, L->pos, "unexpected character");
        goto out;
    }
out:
    t->end = L->pos;
    t->type = r;
    L->seen_token = 1;
    return r;
}

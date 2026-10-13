// dmccl lexer: source text -> a flat list of tokens (with line numbers).
//
// Newlines are tokens (statements end at them), runs of newlines collapse into one, `#` starts a comment, and a number may
// carry a unit: 5s = 5, 500ms = 0.5 (seconds are what `waitfor` and friends want).
#include "dm_internal.h"

static void fail(char* err, size_t cap, int line, const char* what, char c) {
    size_t n = 0;
    const char* head = "line ";
    while (*head && n + 1 < cap) err[n++] = *head++;
    char num[12];
    int k = 0, v = line;
    do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    while (k && n + 1 < cap) err[n++] = num[--k];
    if (n + 2 < cap) { err[n++] = ':'; err[n++] = ' '; }
    while (*what && n + 1 < cap) err[n++] = *what++;
    if (c && n + 4 < cap) { err[n++] = ' '; err[n++] = '\''; err[n++] = c; err[n++] = '\''; }
    err[n] = 0;
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int push_token(struct dm_tokens* t, int kind, int line, double num, const char* text, size_t text_len) {
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 256;
        struct dm_token* grown = (struct dm_token*)DM_REALLOC(t->t, (size_t)cap * sizeof *grown);
        if (!grown) return -1;
        t->t = grown;
        t->cap = cap;
    }
    struct dm_token* k = &t->t[t->n];
    k->kind = (uint8_t)kind;
    k->line = (uint16_t)(line > 65535 ? 65535 : line);
    k->num = num;
    k->text = 0;
    if (text) {
        if (t->pool_n + text_len + 1 > t->pool_cap) {
            // the pool may move: remember offsets, not pointers, until the end (see dm_lex)
            size_t cap = t->pool_cap ? t->pool_cap * 2 : 2048;
            while (cap < t->pool_n + text_len + 1) cap *= 2;
            char* grown = (char*)DM_REALLOC(t->pool, cap);
            if (!grown) return -1;
            t->pool = grown;
            t->pool_cap = cap;
        }
        memcpy(t->pool + t->pool_n, text, text_len);
        t->pool[t->pool_n + text_len] = 0;
        k->text = (const char*)(size_t)(t->pool_n + 1);    // offset + 1 for now; fixed up at the end
        t->pool_n += text_len + 1;
    }
    t->n++;
    return 0;
}

static int keyword(const char* w, size_t n) {
    static const struct { const char* s; int k; } K[] = {
        { "when", T_WHEN }, { "do", T_DO }, { "endifchain", T_ENDIFCHAIN }, { "from", T_FROM }, { "import", T_IMPORT },
        { "if", T_IF }, { "elseif", T_ELSEIF }, { "else", T_ELSE }, { "then", T_THEN }, { "also", T_THEN },
        { "suicide", T_SUICIDE }, { "not", T_NOT }, { "waitfor", T_WAITFOR }, { "above", T_ABOVE },
        { "checkfor", T_CHECKFOR }, { "repeat", T_REPEAT }, { "as", T_AS }, { "forever", T_FOREVER }, { "stop", T_STOP },
        { "thing", T_THING }, { "is", T_IS }, { "and", T_AND }, { "or", T_OR },
    };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (strlen(K[i].s) == n && !memcmp(K[i].s, w, n)) return K[i].k;
    return T_ID;
}

int dm_lex(const char* src, size_t len, struct dm_tokens* out, char* err, size_t err_cap) {
    memset(out, 0, sizeof *out);
    int line = 1;
    size_t i = 0;
    int oom = 0;
    while (i < len && !oom) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '\n') {
            i++;
            if (out->n && out->t[out->n - 1].kind != T_NL) oom |= push_token(out, T_NL, line, 0, 0, 0);
            line++;
            continue;
        }
        if (c == '#') { while (i < len && src[i] != '\n') i++; continue; }
        if (c == '"') {
            // string with \n \t \\ \" escapes
            char buf[512];
            size_t n = 0;
            i++;
            for (;;) {
                if (i >= len || src[i] == '\n') { fail(err, err_cap, line, "unterminated string", 0); dm_tokens_free(out); return -1; }
                char s = src[i++];
                if (s == '"') break;
                if (s == '\\') {
                    if (i >= len) { fail(err, err_cap, line, "unterminated string", 0); dm_tokens_free(out); return -1; }
                    char e = src[i++];
                    if (e == 'n') s = '\n';
                    else if (e == 't') s = '\t';
                    else if (e == '\\' || e == '"') s = e;
                    else { fail(err, err_cap, line, "invalid escape \\", e); dm_tokens_free(out); return -1; }
                }
                if (n + 1 >= sizeof buf) { fail(err, err_cap, line, "string longer than 500 characters", 0); dm_tokens_free(out); return -1; }
                buf[n++] = s;
            }
            oom |= push_token(out, T_STR, line, 0, buf, n);
            continue;
        }
        if (is_digit(c)) {
            double v = 0, frac = 0, scale = 1;
            int dot = 0;
            while (i < len && (is_digit(src[i]) || (src[i] == '.' && !dot && i + 1 < len && is_digit(src[i + 1])))) {
                if (src[i] == '.') dot = 1;
                else if (!dot) v = v * 10 + (src[i] - '0');
                else { scale /= 10; frac += (src[i] - '0') * scale; }
                i++;
            }
            v += frac;
            // unit suffix: `ms` divides by 1000, a lone `s` is seconds (a no-op)
            if (i < len && src[i] == 'm' && i + 1 < len && src[i + 1] == 's' && !(i + 2 < len && (is_alpha(src[i + 2]) || is_digit(src[i + 2])))) { v /= 1000; i += 2; }
            else if (i < len && src[i] == 's' && !(i + 1 < len && (is_alpha(src[i + 1]) || is_digit(src[i + 1])))) i++;
            oom |= push_token(out, T_NUM, line, v, 0, 0);
            continue;
        }
        if (is_alpha(c)) {
            size_t s = i;
            while (i < len && (is_alpha(src[i]) || is_digit(src[i]))) i++;
            int k = keyword(src + s, i - s);
            oom |= push_token(out, k, line, 0, src + s, i - s);   // keywords keep their text: `guy.stop` is a fine member name
            continue;
        }
        i++;
        char d = i < len ? src[i] : 0;
        int k = -1;
        switch (c) {
        case '=': if (d == '=') { i++; k = T_EQ; } else k = T_ASSIGN; break;
        case '!': if (d == '=') { i++; k = T_NE; } break;
        case '<': if (d == '=') { i++; k = T_LE; } else k = T_LT; break;
        case '>': if (d == '=') { i++; k = T_GE; } else k = T_GT; break;
        case '+': if (d == '=') { i++; k = T_PLUSEQ; } else k = T_PLUS; break;
        case '-': if (d == '=') { i++; k = T_MINUSEQ; } else k = T_MINUS; break;
        case '*': k = T_STAR; break;
        case '/': k = T_SLASH; break;
        case '%': k = T_PCT; break;
        case '&': k = T_AND; break;
        case '|': k = T_OR; break;
        case '(': k = T_LPAREN; break;
        case ')': k = T_RPAREN; break;
        case ',': k = T_COMMA; break;
        case ':': k = T_COLON; break;
        case '.': k = T_DOT; break;
        default: break;
        }
        if (k < 0) { fail(err, err_cap, line, "unexpected character", c); dm_tokens_free(out); return -1; }
        oom |= push_token(out, k, line, 0, 0, 0);
    }
    oom |= push_token(out, T_NL, line, 0, 0, 0);
    oom |= push_token(out, T_EOF, line, 0, 0, 0);
    if (oom) { fail(err, err_cap, line, "out of memory", 0); dm_tokens_free(out); return -1; }
    // the pool stopped moving: turn the stored offsets into pointers
    for (int n = 0; n < out->n; n++)
        if (out->t[n].text) out->t[n].text = out->pool + ((size_t)out->t[n].text - 1);
    return 0;
}

void dm_tokens_free(struct dm_tokens* t) {
    DM_FREE(t->t);
    DM_FREE(t->pool);
    memset(t, 0, sizeof *t);
}

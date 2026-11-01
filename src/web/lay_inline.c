// Inline formatting context: text processing, line breaking, line boxes.
#include "lay_int.h"
#include "../cjk.h"
#include "wcase.h"
#include "bidi.h"

enum { IT_TEXT, IT_OPEN, IT_CLOSE, IT_ATOMIC, IT_BR, IT_WBR, IT_FLOAT, IT_ABS, IT_RT };

struct iitem {
    uint8_t kind;
    int32_t box;                 // text/inline/atomic/float/abs box
    uint32_t t0, tl;             // processed text (IT_TEXT)
    const struct wstyle* st;     // style of the text / box
    int32_t shift;               // baseline shift of the enclosing inline (LU, +down)
    int32_t ibox;                // enclosing inline box (-1 = root)
};

enum { P_TEXT, P_SPACE, P_OPEN, P_CLOSE, P_ATOMIC, P_RT };
struct piece {
    uint8_t kind, brk;           // brk: a line break is allowed after this piece
    uint8_t trim;                // trailing collapsible space (hangs, not drawn)
    uint8_t hang;                // starts with an ideographic space: preserved in every white-space mode, never trimmed
    int32_t item;
    uint32_t t0, tl;
    int32_t w;
    int32_t nsp;                 // spaces (justification)
    int32_t rw;                  // P_RT: the annotation's own width (w stays 0: it consumes no line width)
};

// scratch (single-threaded engine)
static struct iitem* IT;
static int nit, capit;
static struct piece* PC;
static int npc, cappc;

static int it_push(void) {
    if (nit >= capit) {
        int nc = capit ? capit * 2 : 256;
        struct iitem* n = (struct iitem*)w_realloc(IT, nc * sizeof(struct iitem));
        if (!n) return -1;
        IT = n;
        capit = nc;
    }
    memset(&IT[nit], 0, sizeof IT[nit]);
    return nit++;
}

static int pc_push(void) {
    if (npc >= cappc) {
        int nc = cappc ? cappc * 2 : 256;
        struct piece* n = (struct piece*)w_realloc(PC, nc * sizeof(struct piece));
        if (!n) return -1;
        PC = n;
        cappc = nc;
    }
    memset(&PC[npc], 0, sizeof PC[npc]);
    return npc++;
}

static int add_frag(struct wlayout* L) {
    if (L->nfr >= L->capfr) {
        int nc = L->capfr ? L->capfr * 2 : 1024;
        struct lfrag* n = (struct lfrag*)w_realloc(L->fr, nc * sizeof(struct lfrag));
        if (!n) return -1;
        L->fr = n;
        L->capfr = nc;
    }
    memset(&L->fr[L->nfr], 0, sizeof L->fr[0]);
    return L->nfr++;
}

// ---- text transform ----------------------------------------------------------------

static uint32_t to_upper(uint32_t c) { return c < 0x80 ? ((c >= 'a' && c <= 'z') ? c - 32 : c) : wcase_upper(c); }
static uint32_t to_lower(uint32_t c) { return c < 0x80 ? ((c >= 'A' && c <= 'Z') ? c + 32 : c) : wcase_lower(c); }

static int is_letter_cp(uint32_t c) {
    return (c < 0x80) ? w_isalnum((int)c) : (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}

// ---- text processing ---------------------------------------------------------------

struct tstate { int last_space; int line_start; int cap_next; };

static void put_cp(struct wbuf* b, uint32_t cp) {
    char u[4];
    wbuf_put(b, u, w_utf8_enc(cp, u));
}

// East Asian Wide / Fullwidth characters that are not Hangul: a collapsed segment break between two of them leaves no space
static int is_ea_wide(uint32_t c) {
    return (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) ||
           (c >= 0xFF01 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x3FFFD);
}

static uint32_t last_text_cp(struct wlayout* L) {
    int n = L->text.len;
    if (n <= 0) return 0;
    int k = n - 1;
    while (k > 0 && ((unsigned char)L->text.p[k] & 0xC0) == 0x80) k--;
    uint32_t cp = 0;
    w_utf8_dec(L->text.p + k, n - k, &cp);
    return cp;
}

// i = index just after the newline in raw
static int seg_break_removed(struct wlayout* L, const char* raw, int i, int rl) {
    uint32_t prev = last_text_cp(L);
    if (!is_ea_wide(prev)) return 0;
    while (i < rl && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n')) i++;
    if (i >= rl) return 0;
    uint32_t next = 0;
    w_utf8_dec(raw + i, rl - i, &next);
    return is_ea_wide(next);
}

// Does the text node of box tb live under lang="tr" / "az" (Turkic casing of i / I)?
static int lang_turkic(struct wlayout* L, int tb) {
    int n = L->b[tb].node;
    for (int guard = 0; n >= 0 && guard < 200; guard++, n = L->d->n[n].parent) {
        if (L->d->n[n].type != WN_ELEM) continue;
        int vl;
        const char* v = wdom_attr(L->d, n, A_lang, &vl);
        if (!v) continue;
        return vl >= 2 && (w_lower((unsigned char)v[0]) == 't' && w_lower((unsigned char)v[1]) == 'r' || w_lower((unsigned char)v[0]) == 'a' && w_lower((unsigned char)v[1]) == 'z') && (vl == 2 || v[2] == '-');
    }
    return 0;
}

// Process raw text of a text box into IT_TEXT / IT_BR items.
static void process_text(struct wlayout* L, int tb, const struct wstyle* st, int32_t shift,
                         int ibox, struct tstate* ts) {
    int rl;
    const char* raw = lay_box_raw(L, tb, &rl);
    int ws = st->white_space;
    int keep_spaces = ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_BREAK_SPACES;
    int keep_nl = keep_spaces || ws == WS_PRE_LINE;
    int tt = st->text_transform;
    int turkic = tt != TT_NONE && lang_turkic(L, tb);
    uint32_t seg_start = (uint32_t)L->text.len;
    #define FLUSH() do { \
        uint32_t e_ = (uint32_t)L->text.len; \
        if (e_ > seg_start) { int k_ = it_push(); if (k_ >= 0) { \
            IT[k_].kind = IT_TEXT; IT[k_].box = tb; IT[k_].t0 = seg_start; IT[k_].tl = e_ - seg_start; \
            IT[k_].st = st; IT[k_].shift = shift; IT[k_].ibox = ibox; } } \
        seg_start = (uint32_t)L->text.len; } while (0)
    int col = 0;
    for (int i = 0; i < rl;) {
        uint32_t cp;
        int n = w_utf8_dec(raw + i, rl - i, &cp);
        i += n;
        if (cp == '\r') continue;
        if (cp == '\n' && keep_nl) {
            FLUSH();
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_BR; IT[k].box = tb; IT[k].st = st; IT[k].shift = shift; IT[k].ibox = ibox; }
            ts->last_space = 0;
            ts->line_start = 1;
            col = 0;
            continue;
        }
        if (cp == ' ' || cp == '\t' || cp == '\n') {
            if (keep_spaces) {
                if (cp == '\t') {
                    // tab-size: a number of space widths, or a length turned into that many spaces
                    int tabn = 8;
                    if (st->tab_size > 0) tabn = (st->tab_size + 500) / 1000;
                    else if (st->tab_size < 0) {
                        struct wfont tf; lb_font(st, &tf);
                        int32_t sw = wfont_advance(&tf, ' ');
                        int32_t tl = -st->tab_size - 1;
                        tabn = (sw > 0 && tl > 0) ? (int)((tl + sw / 2) / sw) : 1;
                    }
                    if (tabn < 1) tabn = 1;
                    int nsp = tabn - (col % tabn);
                    for (int k = 0; k < nsp; k++) wbuf_putc(&L->text, ' ');
                    col += nsp;
                }
                else { wbuf_putc(&L->text, ' '); col++; }
                ts->last_space = 0;
                ts->line_start = 0;
                ts->cap_next = 1;
                continue;
            }
            if (ts->last_space || ts->line_start) continue;
            if (cp == '\n' && seg_break_removed(L, raw, i, rl)) continue;       // CSS Text 3: no space between two East Asian wide characters
            wbuf_putc(&L->text, ' ');
            ts->last_space = 1;
            ts->cap_next = 1;
            col++;
            continue;
        }
        if (cp == 0x200B && st->wst) {      // word-space-transform: a soft wrap opportunity becomes a space
            ts->last_space = 0; ts->line_start = 0;
            if (st->wst == 1) wbuf_putc(&L->text, ' '); else put_cp(&L->text, 0x3000);
            col++;
            continue;
        }
        if (cp == 0xAD && st->hyphens == HY_NONE) continue;   // soft hyphen: a break opportunity unless hyphens: none (drawn as a hyphen when the line breaks there)
        ts->last_space = 0;
        ts->line_start = 0;
        if (turkic && (tt == TT_UPPER || tt == TT_CAPITALIZE && ts->cap_next) && cp == 'i') cp = 0x130;
        else if (turkic && tt == TT_LOWER && (cp == 'I' || cp == 0x130)) cp = cp == 'I' ? 0x131 : 'i';
        else if (turkic && tt == TT_UPPER && cp == 0x131) cp = 'I';
        if (tt == TT_UPPER && !(turkic && cp == 0x130)) {
            uint32_t mc[3];
            int nm = wcase_upper_multi(cp, mc);
            if (nm) { for (int k = 0; k < nm - 1; k++) { put_cp(&L->text, mc[k]); col++; } cp = mc[nm - 1]; }
            else cp = to_upper(cp);
        }
        else if (tt == TT_LOWER) cp = to_lower(cp);
        else if (tt == TT_CAPITALIZE) {
            if (ts->cap_next && is_letter_cp(cp)) cp = to_upper(cp);
            ts->cap_next = !is_letter_cp(cp) && cp != '\'' && cp != 0x2019;
        }
        if (cp == 0) continue;
        put_cp(&L->text, cp);
        col++;
    }
    FLUSH();
    #undef FLUSH
}

// baseline shift of an inline box relative to its parent's baseline
static int32_t va_shift(const struct wstyle* s, const struct wstyle* ps) {
    struct wfmetrics m, pm;
    switch (s->vertical_align) {
    case VA_SUB: return ps->font_size / 5;
    case VA_SUPER: return -(ps->font_size * 34 / 100);
    case VA_LEN: return -s->va_len;
    case VA_MIDDLE:
        lb_font_metrics(s, &m);
        lb_font_metrics(ps, &pm);
        return (m.ascent - m.descent) / 2 - pm.xheight / 2;
    case VA_TEXT_TOP:
        lb_font_metrics(s, &m);
        lb_font_metrics(ps, &pm);
        return m.ascent - pm.ascent;
    case VA_TEXT_BOTTOM:
        lb_font_metrics(s, &m);
        lb_font_metrics(ps, &pm);
        return pm.descent - m.descent;
    }
    return 0;
}

static void flatten(struct wlayout* L, int box, const struct wstyle* pst, int32_t shift, int ibox,
                    struct tstate* ts, int32_t content_w, int depth) {
    for (int c = L->b[box].first; c >= 0; c = L->b[c].next) {
        struct lbox* b = &L->b[c];
        if (b->flags & BF_ABS) {
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_ABS; IT[k].box = c; IT[k].ibox = ibox; }
            continue;
        }
        if (b->flags & BF_FLOAT) {
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_FLOAT; IT[k].box = c; IT[k].ibox = ibox; }
            continue;
        }
        switch (b->kind) {
        case LB_TEXT:
            process_text(L, c, b->st, shift, ibox, ts);
            break;
        case LB_MARKER: {
            int k = it_push();
            if (k >= 0) {
                IT[k].kind = IT_TEXT; IT[k].box = c; IT[k].t0 = b->t0; IT[k].tl = b->tl;
                IT[k].st = b->st; IT[k].shift = shift; IT[k].ibox = ibox;
            }
            ts->line_start = 0;
            ts->last_space = 1;
            break;
        }
        case LB_BR: {
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_BR; IT[k].box = c; IT[k].st = b->st; IT[k].shift = shift; IT[k].ibox = ibox; }
            ts->last_space = 0;
            ts->line_start = 1;
            break;
        }
        case LB_WBR: {
            if (b->st && b->st->wst) {          // word-space-transform: <wbr> becomes a space
                uint32_t t0 = (uint32_t)L->text.len;
                if (b->st->wst == 1) wbuf_putc(&L->text, ' '); else put_cp(&L->text, 0x3000);
                int k = it_push();
                if (k >= 0) { IT[k].kind = IT_TEXT; IT[k].box = c; IT[k].t0 = t0; IT[k].tl = (uint32_t)L->text.len - t0; IT[k].st = b->st; IT[k].shift = shift; IT[k].ibox = ibox; }
                ts->line_start = 0; ts->last_space = 0;
                break;
            }
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_WBR; IT[k].box = c; IT[k].ibox = ibox; }
            break;
        }
        case LB_INLINE: {
            if (b->st->display == D_RUBY_TEXT) {
                // <rt>: its text becomes a ruby annotation (zero width, placed
                // above the base run at line finalization). The raw text is
                // copied to the IFC scratch buffer here (the text box's slice
                // is only filled by process_text, which annotations skip).
                for (int t = b->first; t >= 0; t = L->b[t].next) {
                    struct lbox* tb = &L->b[t];
                    if (tb->kind != LB_TEXT) continue;
                    int rl;
                    const char* raw = lay_box_raw(L, t, &rl);
                    if (!raw || rl <= 0) continue;
                    uint32_t t0 = (uint32_t)L->text.len;
                    wbuf_put(&L->text, raw, (uint32_t)rl);
                    int k = it_push();
                    if (k >= 0) { IT[k].kind = IT_RT; IT[k].box = t; IT[k].t0 = t0; IT[k].tl = (uint32_t)rl; IT[k].st = tb->st; IT[k].ibox = ibox; }
                }
                break;
            }
            lb_resolve_box_sides(L, c, content_w);
            b = &L->b[c];
            int32_t sh = shift + va_shift(b->st, pst);
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_OPEN; IT[k].box = c; IT[k].st = b->st; IT[k].shift = sh; IT[k].ibox = ibox; }
            if (b->m[3] + b->b[3] + b->p[3] > 0) ts->line_start = 0, ts->last_space = 0;
            if (depth < 200) flatten(L, c, b->st, sh, c, ts, content_w, depth + 1);
            k = it_push();
            if (k >= 0) { IT[k].kind = IT_CLOSE; IT[k].box = c; IT[k].st = L->b[c].st; IT[k].shift = sh; IT[k].ibox = ibox; }
            b = &L->b[c];
            if (b->m[1] + b->b[1] + b->p[1] > 0) ts->last_space = 0;
            break;
        }
        default: { // atomic inline (inline-block, replaced, inline-flex/grid/table)
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_ATOMIC; IT[k].box = c; IT[k].st = b->st; IT[k].shift = shift; IT[k].ibox = ibox; }
            ts->line_start = 0;
            ts->last_space = 0;
            break;
        }
        }
    }
}

// ---- measurement -----------------------------------------------------------------

static int32_t measure(const struct wstyle* st, const char* s, int len, int* nspaces) {
    struct wfont f;
    lb_font(st, &f);
    int32_t w = wfont_measure(&f, s, len);
    int chars = 0, sp = 0;
    for (int i = 0; i < len;) {
        uint32_t cp;
        i += w_utf8_dec(s + i, len - i, &cp);
        chars++;
        if (cp == ' ') sp++;
    }
    w += st->letter_spacing * chars + st->word_spacing * sp;
    if (nspaces) *nspaces = sp;
    return w;
}

static int is_cjk_cp(uint32_t cp) {
    return cjk_is_passthrough(cp) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF) ||
           (cp >= 0x2E80 && cp <= 0x2FDF) || (cp >= 0x3100 && cp <= 0x31FF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

// Scripts written without spaces that need a dictionary to break words (Thai, Lao, Myanmar, Khmer, Balinese, Javanese): with no
// dictionary the fallback is a break opportunity between every character cluster (never inside one, never overflowing).
static int is_nodict_cp(uint32_t c) {
    return (c >= 0x0E01 && c <= 0x0E5B) || (c >= 0x0E81 && c <= 0x0EDF) || (c >= 0x1000 && c <= 0x109F) ||
           (c >= 0x1780 && c <= 0x17FF) || (c >= 0x1B05 && c <= 0x1B7F) || (c >= 0xA980 && c <= 0xA9DF) || (c >= 0xAA60 && c <= 0xAA7F);
}
// combining marks / dependent vowels / signs of those scripts: they stay with the cluster before them
static int is_nodict_mark(uint32_t c) {
    return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) || (c >= 0x0E47 && c <= 0x0E4E) ||
           c == 0x0EB1 || (c >= 0x0EB4 && c <= 0x0EBC) || (c >= 0x0EC8 && c <= 0x0ECD) ||
           (c >= 0x102B && c <= 0x103E) || (c >= 0x1056 && c <= 0x1059) || (c >= 0x105E && c <= 0x1060) || (c >= 0x1062 && c <= 0x1064) ||
           (c >= 0x1067 && c <= 0x106D) || (c >= 0x1071 && c <= 0x1074) || (c >= 0x1082 && c <= 0x108D) || c == 0x108F || (c >= 0x109A && c <= 0x109D) ||
           (c >= 0x17B4 && c <= 0x17D3) || c == 0x17DD ||
           (c >= 0x1B00 && c <= 0x1B04) || (c >= 0x1B34 && c <= 0x1B44) || (c >= 0x1B6B && c <= 0x1B73) ||
           (c >= 0xA980 && c <= 0xA983) || c == 0xA9B3 || (c >= 0xA9B4 && c <= 0xA9C0) ||
           c == 0x0E2F || c == 0x0E46 || c == 0x0EC6 || c == 0x17D7;
}

// UAX 14 (line breaking): characters before which a break is never allowed (closing punctuation CL/CP, exclamation EX, infix
// separators IS/SY, non-starters NS) and after which none is allowed (opening punctuation OP, word joiner / no-break GL)
static int lb_range(uint32_t c, const uint32_t* r, int n) {
    for (int i = 0; i < n; i += 2) if (c >= r[i] && c <= r[i + 1]) return 1;
    return 0;
}

// Ps/Pe pairs that alternate (odd offset = closing)
static int lb_alt_open(uint32_t c) {
    static const uint32_t R[] = { 0x2308, 0x230B, 0x2768, 0x2775, 0x27E6, 0x27EF, 0x2983, 0x2998, 0x29D8, 0x29DB, 0x2E22, 0x2E29 };
    for (int i = 0; i < (int)(sizeof R / sizeof R[0]); i += 2) if (c >= R[i] && c <= R[i + 1]) return ((c - R[i]) & 1) ? 2 : 1;
    return 0;
}

static int lb_no_before(uint32_t c) {
    if (lb_alt_open(c) == 2) return 1;
    static const uint32_t R[] = {
        0x21, 0x21, 0x29, 0x29, 0x2C, 0x2C, 0x2E, 0x2F, 0x3A, 0x3B, 0x3F, 0x3F, 0x5D, 0x5D, 0x7D, 0x7D,   // ASCII ! ) , . / : ; ? ] }
        0xA0, 0xA0, 0x2060, 0x2060, 0x202F, 0x202F, 0x2007, 0x2007, 0x2011, 0x2011,                          // GL, WJ
        0x203C, 0x203D, 0x2047, 0x2049, 0x2024, 0x2024,                                                       // double ! ? and friends
        0x0F3B, 0x0F3B, 0x0F3D, 0x0F3D, 0x169C, 0x169C, 0x2046, 0x2046, 0x207E, 0x207E, 0x208E, 0x208E, 0x232A, 0x232A,
        0x27C6, 0x27C6, 0x29FD, 0x29FD, 0xFD3E, 0xFD3E, 0xFE18, 0xFE18, 0x2E23, 0x2E23, 0x2E42, 0x2E42,
        0x0E5A, 0x0E5B, 0x17D4, 0x17D6, 0x17D8, 0x17DA, 0x0F34, 0x0F34, 0x0F3A, 0x0F3A, 0x0F3C, 0x0F3C,      // Thai / Khmer / Tibetan marks
        0x3001, 0x3002, 0x3005, 0x3005, 0x3009, 0x3009, 0x300B, 0x300B, 0x300D, 0x300D, 0x300F, 0x300F,
        0x3011, 0x3011, 0x3015, 0x3015, 0x3017, 0x3017, 0x3019, 0x3019, 0x301B, 0x301C, 0x301E, 0x301F,
        0x3030, 0x3030, 0x303B, 0x303C, 0x309B, 0x309E, 0x30A0, 0x30A0, 0x30FB, 0x30FB, 0x30FD, 0x30FE,
        0xFE10, 0xFE16, 0xFE18, 0xFE18, 0xFE30, 0xFE30, 0xFE36, 0xFE36, 0xFE38, 0xFE38, 0xFE3A, 0xFE3A,
        0xFE3C, 0xFE3C, 0xFE3E, 0xFE3E, 0xFE40, 0xFE40, 0xFE42, 0xFE42, 0xFE44, 0xFE44, 0xFE48, 0xFE48,
        0xFE50, 0xFE52, 0xFE54, 0xFE57, 0xFE5A, 0xFE5A, 0xFE5C, 0xFE5C, 0xFE5E, 0xFE5E,
        0xFF01, 0xFF01, 0xFF09, 0xFF09, 0xFF0C, 0xFF0C, 0xFF0E, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF1F,
        0xFF3D, 0xFF3D, 0xFF5D, 0xFF5D, 0xFF60, 0xFF61, 0xFF63, 0xFF65, 0xFF9E, 0xFF9F,
    };
    return lb_range(c, R, (int)(sizeof R / sizeof R[0]));
}

static int lb_no_after(uint32_t c) {
    if (lb_alt_open(c) == 1) return 1;
    static const uint32_t R[] = {
        0x28, 0x28, 0x5B, 0x5B, 0x7B, 0x7B, 0x0F3A, 0x0F3A, 0x0F3C, 0x0F3C, 0x169B, 0x169B, 0x201A, 0x201A, 0x201E, 0x201E,
        0x2045, 0x2045, 0x207D, 0x207D, 0x208D, 0x208D, 0x2329, 0x2329, 0x27C5, 0x27C5, 0x29FC, 0x29FC, 0xFD3F, 0xFD3F, 0xFE17, 0xFE17,
        0xA0, 0xA0, 0x2060, 0x2060, 0x202F, 0x202F, 0x2007, 0x2007, 0x2011, 0x2011,
        0x3008, 0x3008, 0x300A, 0x300A, 0x300C, 0x300C, 0x300E, 0x300E, 0x3010, 0x3010, 0x3014, 0x3014,
        0x3016, 0x3016, 0x3018, 0x3018, 0x301A, 0x301A, 0x301D, 0x301D,
        0xFE35, 0xFE35, 0xFE37, 0xFE37, 0xFE39, 0xFE39, 0xFE3B, 0xFE3B, 0xFE3D, 0xFE3D, 0xFE3F, 0xFE3F,
        0xFE41, 0xFE41, 0xFE43, 0xFE43, 0xFE47, 0xFE47, 0xFE59, 0xFE59, 0xFE5B, 0xFE5B, 0xFE5D, 0xFE5D,
        0xFF08, 0xFF08, 0xFF3B, 0xFF3B, 0xFF5B, 0xFF5B, 0xFF5F, 0xFF5F, 0xFF62, 0xFF62,
    };
    return lb_range(c, R, (int)(sizeof R / sizeof R[0]));
}

// Characters that forbid a soft wrap opportunity between them and a neighbouring atomic inline (GL/WJ/ZWJ/CM; NBSP is not one of them)
static int atomic_nobrk(uint32_t c) {
    return c == 0x202F || c == 0x2060 || c == 0x200D || c == 0xFEFF || c == 0x180E || c == 0x2007 || c == 0x2011 ||
           c == 0x0F08 || c == 0x0F0C || c == 0x0F12 || (c >= 0x0300 && c <= 0x036F);
}

// First (dir > 0) or last (dir < 0) code point of the text next to item i, looking through inline box edges; 0 = none / not text
static uint32_t it_edge_cp(struct wlayout* L, const struct iitem* IT, int nit, int i, int dir) {
    for (int j = i + dir; j >= 0 && j < nit; j += dir) {
        if (IT[j].kind == IT_OPEN || IT[j].kind == IT_CLOSE) continue;
        if (IT[j].kind != IT_TEXT || IT[j].tl == 0) return 0;
        const char* t = L->text.p + IT[j].t0;
        int n = (int)IT[j].tl;
        uint32_t cp = 0;
        if (dir > 0) { w_utf8_dec(t, n, &cp); return cp; }
        int k = n - 1;
        while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
        w_utf8_dec(t + k, n - k, &cp);
        return cp;
    }
    return 0;
}

static int wraps(const struct wstyle* s) {
    return s->white_space != WS_NOWRAP && s->white_space != WS_PRE && !s->tw_nowrap;
}

// Split an IT_TEXT item into word/space pieces appended to *out callback.
// We generate pieces lazily per item inside the line builder.
struct tok { uint32_t t0, tl; int space; int brk; int hang; };

// next token from text [p, e): a run of spaces, or a word (with internal
// break opportunities at hyphens and CJK characters ending the token)
static int next_tok(const char* text, uint32_t p, uint32_t e, const struct wstyle* st, struct tok* t) {
    if (p >= e) return 0;
    int keep = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP || st->white_space == WS_BREAK_SPACES;
    t->t0 = p;
    int anywhere = st->line_break == LB_ANYWHERE;
    // preserved wrapping white space: an ideographic space (U+3000) behaves like a space (it hangs at the line end)
    int ideo = 1;
#define IDEO_AT(i) (ideo && (i) + 2 < e && (unsigned char)text[i] == 0xE3 && (unsigned char)text[(i) + 1] == 0x80 && (unsigned char)text[(i) + 2] == 0x80)
    if (text[p] == ' ' || IDEO_AT(p)) {
        uint32_t q = p;
        while (q < e && (text[q] == ' ' || IDEO_AT(q))) { q += text[q] == ' ' ? 1 : 3; if (st->white_space == WS_BREAK_SPACES || anywhere) break; }   // break-spaces: a break after every space
        t->tl = q - p;
        t->space = 1;
        t->brk = 1;
        t->hang = text[p] != ' ';
        (void)keep;
        return 1;
    }
    uint32_t q = p, lastcp = 0;
    int break_all = st->word_break == WB_BREAK_ALL || anywhere;
    while (q < e && text[q] != ' ') {
        uint32_t cp;
        int n = w_utf8_dec(text + q, (int)(e - q), &cp);
        if (is_cjk_cp(cp) && st->word_break != WB_KEEP_ALL) {
            if (q == p) { lastcp = cp; q += n; break; }  // a CJK char is its own token
            break;
        }
        if (q > p && is_nodict_cp(cp) && !is_nodict_mark(cp) && is_nodict_cp(lastcp) && st->word_break != WB_KEEP_ALL && st->word_break != WB_MANUAL) break;   // fallback: between clusters
        lastcp = cp;
        q += n;
        if (cp == 0x200B || cp == 0xAD) break;       // zero width space, soft hyphen: a break opportunity after it
        if (cp == 0x1680 || (cp >= 0x2000 && cp <= 0x2006) || (cp >= 0x2008 && cp <= 0x200A) || cp == 0x205F || cp == 0x3000) break;   // UAX 14 BA: break after
        if (anywhere) {      // a typographic character unit: the base with the combining marks and joiners after it
            while (q < e && text[q] != ' ') {
                uint32_t cm;
                int nn = w_utf8_dec(text + q, (int)(e - q), &cm);
                if (!((cm >= 0x300 && cm <= 0x36F) || cm == 0x200D || cm == 0xFE0F || cm == 0xFEFF || cm == 0x2060 || cm == 0x200C)) break;
                q += nn;
            }
        }
        if (break_all) break;
        // break opportunity after a hyphen between letters
        if ((cp == '-' || cp == 0x2010 || cp == 0x2013) && q < e && q - p > 1) {
            uint32_t nx;
            w_utf8_dec(text + q, (int)(e - q), &nx);
            if (is_letter_cp(nx)) break;
        }
        // after '/' or '?' inside long URL-like words keep going (no break)
    }
    t->tl = q - p;
    t->space = 0;
    t->hang = 0;
    t->brk = lastcp == 0x200B || lastcp == 0xAD || lastcp == 0x1680 || (lastcp >= 0x2000 && lastcp <= 0x2006) || (lastcp >= 0x2008 && lastcp <= 0x200A) || lastcp == 0x205F || lastcp == 0x3000;
    if (q < e && text[q] != ' ') {
        t->brk = 1; // internal opportunity (hyphen / CJK / break-all)
        uint32_t nx;
        w_utf8_dec(text + q, (int)(e - q), &nx);
        if (!anywhere && (lb_no_before(nx) || lb_no_after(lastcp))) t->brk = 0;
    }
    return 1;
}

// ---- line building ----------------------------------------------------------------

struct lrec { int fr0; int32_t y, x0, avail, base; };

struct ifc {
    struct wlayout* L;
    int root;
    const struct wstyle* rst;
    int32_t content_w;
    int32_t abs_left, abs_top;     // root content box (provisional absolute)
    int32_t y;                     // current line top (relative to content top)
    int32_t line_x0, avail;        // current line box horizontal extent
    int32_t avail_full;            // the extent before text-wrap: balance narrowed the measure (alignment is relative to it)
    int32_t used;                  // width of pieces on the line
    int last_brk;                  // index of last piece allowing a break after
    int first_line;
    int lines;
    int32_t strut_above, strut_below;
    int32_t baseline_first, baseline_last;
    // per line, for line-clamp: first fragment, top y, left edge, available width, baseline
    struct lrec* lr;
    int nlr, caplr;
    // open inline boxes at line start
    int32_t open[128];
    int nopen;
    uint8_t needs_bidi;            // any rtl content, formatting chars or unicode-bidi elements
};

static void line_bounds(struct ifc* c) {
    struct wlayout* L = c->L;
    int32_t l = c->abs_left, r = c->abs_left + c->content_w;
    if (L->nfl > L->fl_base)
        fl_avail(L, c->abs_top + c->y, c->strut_above + c->strut_below, l, r, &l, &r);
    c->line_x0 = l - c->abs_left;
    c->avail = r - l;
    if (c->first_line) {
        int32_t ind = wl_resolve(c->rst->text_indent, c->content_w, 0);
        c->line_x0 += ind;
        c->avail -= ind;
    }
    if (c->avail < 0) c->avail = 0;
    c->avail_full = c->avail;
    if (L->ifc_balancing && L->bal_seg < 16 && L->bal_w[L->bal_seg] > 0 && L->bal_w[L->bal_seg] < c->avail) c->avail = L->bal_w[L->bal_seg];
}

static int line_has_floats_narrowing(struct ifc* c) {
    return c->line_x0 > (c->first_line ? wl_resolve(c->rst->text_indent, c->content_w, 0) : 0) ||
           c->avail < c->content_w - (c->first_line ? wl_resolve(c->rst->text_indent, c->content_w, 0) : 0);
}

static void text_metrics(const struct wstyle* s, int32_t* above, int32_t* below) {
    struct wfmetrics m;
    lb_font_metrics(s, &m);
    int32_t ad = m.ascent + m.descent;
    int32_t lh = s->line_height > 0 ? s->line_height : ad + m.line_gap;
    int32_t half = (lh - ad) / 2;
    *above = m.ascent + half;
    *below = lh - *above;
}

static void ellipsize_line(struct ifc* c, int fr0);

// Finalize pieces [0, n) as one line; forced = ended by a forced break.
// Bidi line emission: resolve the line's codepoints with UAX #9 and emit the
// fragments in visual order (rtl runs get their characters reversed and
// mirrored; the text is appended to the scratch buffer). Ruby annotations are
// skipped on bidi lines; span backgrounds keep logical positions.
static void emit_line_bidi(struct ifc* c, int n, int32_t x, int32_t base, int32_t off, int32_t extra,
                           int32_t lh, int32_t anno_base_over, int32_t anno_base_under) {
    struct wlayout* L = c->L;
    static uint32_t bcp[8192];
    static int bown[8192];
    static uint8_t bctrl[8192];
    static uint8_t blvl[8192];
    static int border[8192];
    int nb = 0;
    int opensyn[64]; int nopens = 0;
    // span (inline box) ranges in stream indexes, for background fragments;
    // boxes still open from the previous line cover the whole line
    struct { int box, a, b; } spans[64];
    int nspans = 0;
    for (int k = 0; k < c->nopen && nspans < 64; k++) { spans[nspans].box = c->open[k]; spans[nspans].a = 0; spans[nspans].b = -1; nspans++; }

    // paragraph direction (P3; unicode-bidi: plaintext resolves first-strong)
    int bbase = c->rst->direction_rtl ? 1 : 0;
    if (c->rst->ubidi == 5) {
        // first strong of the whole paragraph
        int found = -1;
        for (int i = 0; i < n && found < 0; i++) {
            struct piece* p = &PC[i];
            if (p->kind != P_TEXT) continue;
            struct iitem* it = &IT[p->item];
            for (uint32_t q = p->t0; q < p->t0 + p->tl && found < 0; ) {
                uint32_t cp; int nn = w_utf8_dec(L->text.p + q, (int)(p->t0 + p->tl - q), &cp);
                if (!nn) break;
                int cl = bdi_class(cp);
                if (cl == BCL_L) found = 0;
                else if (cl == BCL_R || cl == BCL_AL) found = 1;
                q += (uint32_t)nn;
            }
        }
        if (found >= 0) bbase = found;
    }
    for (int i = 0; i < n; i++) {
        struct piece* p = &PC[i];
        struct iitem* it = &IT[p->item];
        if (p->kind == P_OPEN) {
            if (nspans < 64) { spans[nspans].box = it->box; spans[nspans].a = nb; spans[nspans].b = -1; nspans++; }
            const struct wstyle* s = it->st;
            if (s && s->ubidi) {
                int dir = s->direction_rtl;
                uint32_t sc = s->ubidi == 1 ? (dir ? 0x202B : 0x202A)   // embed
                            : s->ubidi == 3 ? (dir ? 0x202E : 0x202D)   // bidi-override
                            : s->ubidi == 5 ? 0x2068                    // plaintext: first-strong isolate
                            : (dir ? 0x2067 : 0x2066);                  // isolate / isolate-override
                if (nb < 8192 && nopens < 64) {
                    bcp[nb] = sc; bown[nb] = i; bctrl[nb] = 1; nb++;
                    opensyn[nopens++] = nb - 1;
                }
            }
            continue;
        }
        if (p->kind == P_CLOSE) {
            for (int q = nspans - 1; q >= 0; q--)
                if (spans[q].box == it->box && spans[q].b < 0) { spans[q].b = nb; break; }
            if (nopens > 0) {
                int oi = opensyn[--nopens];
                uint32_t cl = bcp[oi] <= 0x202E ? 0x202C : 0x2069;
                if (nb < 8192) { bcp[nb] = cl; bown[nb] = i; bctrl[nb] = 1; nb++; }
            }
            continue;
        }
        if (p->kind == P_RT || p->trim) continue;
        if (p->kind == P_ATOMIC) {
            if (nb < 8192) { bcp[nb] = 0xFFFC; bown[nb] = i; bctrl[nb] = 0; nb++; }
            continue;
        }
        // P_TEXT / P_SPACE
        for (uint32_t q = p->t0; q < p->t0 + p->tl && nb < 8192; ) {
            uint32_t cp; int nn = w_utf8_dec(L->text.p + q, (int)(p->t0 + p->tl - q), &cp);
            if (!nn) break;
            bcp[nb] = cp; bown[nb] = i; bctrl[nb] = 0; nb++;
            q += (uint32_t)nn;
        }
    }
    bidi_levels(bcp, nb, bbase, blvl, bctrl);
    nb = bidi_reorder(blvl, bctrl, nb, border);
    for (int q = 0; q < nspans; q++)
        if (spans[q].b < 0) spans[q].b = nb;
    // walk 1: per-entry x positions (advances only), then span backgrounds
    {
        static int32_t bx[8192];
        static int32_t bw[8192];
        int32_t wx = x;
        for (int k = 0; k < nb; k++) {
            int idx = border[k];
            if (bctrl[idx] & 1) { bx[idx] = wx; bw[idx] = 0; continue; }
            struct piece* p = &PC[bown[idx]];
            struct iitem* it = &IT[p->item];
            if (p->kind == P_ATOMIC) { bx[idx] = wx; bw[idx] = p->w; wx += p->w; continue; }
            struct wfont f;
            lb_font(it->st, &f);
            int32_t adv = wfont_advance(&f, bcp[idx]) + it->st->letter_spacing;
            bx[idx] = wx; bw[idx] = adv;
            wx += adv;
        }
        for (int q = 0; q < nspans; q++) {
            int32_t xa = 0, xb = 0;
            int any = 0;
            for (int e = spans[q].a; e < spans[q].b && e < nb; e++) {
                if (bctrl[e] & 1) continue;
                if (!any) { xa = bx[e]; xb = bx[e] + bw[e]; any = 1; }
                else { if (bx[e] < xa) xa = bx[e]; if (bx[e] + bw[e] > xb) xb = bx[e] + bw[e]; }
            }
            if (!any) continue;
            int f2 = add_frag(L);
            if (f2 < 0) break;
            struct lbox* ib = &L->b[spans[q].box];
            struct wfmetrics m;
            lb_font_metrics(ib->st, &m);
            int32_t sh = 0;
            for (int qq = 0; qq < nit; qq++) if (IT[qq].kind == IT_OPEN && IT[qq].box == spans[q].box) { sh = IT[qq].shift; break; }
            struct lfrag* fr = &L->fr[f2];
            fr->kind = FR_IBOX;
            fr->box = spans[q].box;
            fr->first = fr->last = 0;
            fr->x = xa;
            fr->w = xb - xa;
            fr->y = base + sh - m.ascent - ib->p[0] - ib->b[0];
            fr->h = m.ascent + m.descent + ib->p[0] + ib->p[2] + ib->b[0] + ib->b[2];
        }
    }
    int32_t px = x;
    int k = 0;
    while (k < nb) {
        int idx = border[k];
        if (bctrl[idx] & 1) { k++; continue; }
        struct piece* p = &PC[bown[idx]];
        struct iitem* it = &IT[p->item];
        if (p->kind == P_ATOMIC) {
            struct lbox* ab = &L->b[it->box];
            int32_t mh = ab->h + ab->m[0] + ab->m[2];
            int32_t bl2 = ab->baseline >= 0 ? ab->m[0] + ab->baseline : mh;
            if (ab->kind == LB_BLOCK && (ab->st->overflow_y != OV_VISIBLE || ab->baseline < 0)) bl2 = mh;
            int va = ab->st->vertical_align;
            int32_t top;
            if (va == VA_TOP) top = c->y;
            else if (va == VA_BOTTOM) top = c->y + lh - mh;
            else if (va == VA_MIDDLE) {
                struct wfmetrics pm;
                lb_font_metrics(c->rst, &pm);
                top = base - pm.xheight / 2 - mh / 2;
            } else {
                int32_t sh = it->shift;
                if (va == VA_LEN || va == VA_SUB || va == VA_SUPER) sh += va_shift(ab->st, c->rst);
                top = base + sh - bl2;
            }
            ab->x = L->b[c->root].b[3] + L->b[c->root].p[3] + px + ab->m[3];
            ab->y = L->b[c->root].b[0] + L->b[c->root].p[0] + top + ab->m[0];
            ab->lparent = c->root;
            int f2 = add_frag(L);
            if (f2 >= 0) {
                struct lfrag* fr = &L->fr[f2];
                fr->kind = FR_ATOMIC;
                fr->box = it->box;
                fr->x = px;
                fr->y = top;
                fr->w = p->w;
                fr->h = mh;
            }
            px += p->w;
            k++;
            continue;
        }
        // a text run: consecutive visual entries of the same piece
        int k2 = k + 1;
        while (k2 < nb && bown[border[k2]] == bown[idx] && !(bctrl[border[k2]] & 1)) k2++;
        int rtl = blvl[idx] & 1;
        const struct wstyle* s = it->st;
        uint32_t t0 = (uint32_t)L->text.len;
        int spaces = 0;
        for (int q = k; q < k2; q++) {
            int e = border[q];
            uint32_t cp = bcp[e];
            if (rtl) { uint32_t m2 = bdi_mirror(cp); if (m2) cp = m2; }
            if (cp == ' ') spaces++;
            char u[4]; int ul = w_utf8_enc(cp, u);
            wbuf_put(&L->text, u, (uint32_t)ul);
        }
        uint32_t tl2 = (uint32_t)L->text.len - t0;
        struct wfont f;
        lb_font(s, &f);
        int32_t w = tl2 ? wfont_measure(&f, L->text.p + t0, (int)tl2) : 0;
        int sp_extra = spaces ? extra * spaces : 0;
        w += sp_extra;
        int f2 = add_frag(L);
        if (f2 >= 0) {
            struct lfrag* fr = &L->fr[f2];
            fr->kind = FR_TEXT;
            fr->box = it->box;
            fr->x = px;
            fr->y = base + it->shift;
            fr->w = w;
            fr->t0 = t0;
            fr->tl = tl2;
            fr->word_extra = spaces ? extra : 0;
            struct wfmetrics m;
            lb_font_metrics(s, &m);
            fr->h = m.ascent + m.descent;
            if (s->emph_cp) {
                // emphasis marks over the (possibly reordered) run
                struct wfont f50;
                lb_font(s, &f50);
                f50.px = (uint16_t)(f50.px / 2);
                struct wfmetrics m5;
                wfont_metrics(&f50, &m5);
                const struct wstyle* es = s;
                int mover = 1;
                if (es->wmode == WM_VRL) mover = !(es->emph_left || es->emph_under);
                else if (es->wmode == WM_VLR || es->wmode == WM_SLR) mover = !es->emph_under || es->emph_left;
                else if (es->emph_under) mover = 0;
                int mf = add_frag(L);
                if (mf >= 0) {
                    struct lfrag* mr = &L->fr[mf];
                    mr->kind = FR_MARKS;
                    mr->box = it->box;
                    mr->x = px;
                    mr->y = mover ? anno_base_over - m5.descent : anno_base_under + m5.ascent;
                    mr->w = w;
                    mr->t0 = t0;
                    mr->tl = tl2;
                    mr->h = m5.ascent + m5.descent;
                }
            }
        }
        px += w;
        k = k2;
    }
}

static void finish_line(struct ifc* c, int n, int forced) {
    struct wlayout* L = c->L;
    int lr_fr0 = L->nfr;
    int32_t lr_y = c->y, lr_x0 = c->line_x0, lr_avail = c->avail;
    // trailing collapsible spaces hang (not counted), even when followed by
    // closing inline boxes
    for (int i = 0; i < n; i++) PC[i].trim = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (PC[i].kind == P_OPEN || PC[i].kind == P_CLOSE) continue;
        if (PC[i].kind != P_SPACE || PC[i].hang) break;
        const struct wstyle* s = IT[PC[i].item].st;
        if (s->white_space == WS_PRE || s->white_space == WS_PRE_WRAP || s->white_space == WS_BREAK_SPACES) break;
        PC[i].trim = 1;
    }
    int end = n;
    int32_t used = 0;
    int has_content = forced == 2; // <br> lines always exist
    int nsp = 0;
    for (int i = 0; i < n; i++) {
        if (!PC[i].trim) used += PC[i].w;
        if (PC[i].kind == P_TEXT || PC[i].kind == P_ATOMIC) has_content = 1;
        if ((PC[i].kind == P_OPEN || PC[i].kind == P_CLOSE) && PC[i].w) has_content = 1;
        if (PC[i].kind == P_SPACE && !PC[i].trim) {
            const struct wstyle* s = IT[PC[i].item].st;
            if (PC[i].hang || s->white_space == WS_PRE || s->white_space == WS_PRE_WRAP || s->white_space == WS_BREAK_SPACES) has_content = 1;
            nsp += PC[i].nsp;
        }
    }
    if (!has_content) {
        // keep open-box state flowing through, but no line box
        for (int i = 0; i < n; i++) {
            if (PC[i].kind == P_OPEN && c->nopen < 128) c->open[c->nopen++] = IT[PC[i].item].box;
            else if (PC[i].kind == P_CLOSE && c->nopen) c->nopen--;
        }
        return;
    }
    // alignment
    int ta = c->rst->text_align;
    int last_line = forced != 0;                  // the last line of the block, or the one before a forced break
    if (last_line && c->rst->text_align_last) ta = c->rst->text_align_last - 1;
    if (ta == TA_START) ta = c->rst->direction_rtl ? TA_RIGHT : TA_LEFT;
    if (ta == TA_END) ta = c->rst->direction_rtl ? TA_LEFT : TA_RIGHT;
    if (ta == TA_WEBKIT_CENTER) ta = TA_CENTER;
    int justify_last = ta == TA_JUSTIFY_ALL || (last_line && c->rst->text_align_last == TA_JUSTIFY + 1);
    if (ta == TA_JUSTIFY_ALL) ta = TA_JUSTIFY;
    int32_t off = 0, extra = 0;
    int32_t freew = c->avail - used;
    int32_t alignw = freew;                   // (text-wrap: balance aligns within the narrowed measure here)
    if (ta == TA_RIGHT && alignw > 0) off = alignw;
    else if (ta == TA_CENTER && alignw > 0) off = alignw / 2;
    else if (ta == TA_JUSTIFY && (!forced || justify_last) && freew > 0 && nsp > 0) extra = freew / nsp;
    // vertical metrics
    int32_t above = c->strut_above, below = c->strut_below;
    int32_t atop_extra = 0; // for top/bottom aligned atomics
    for (int i = 0; i < n; i++) {
        struct iitem* it = &IT[PC[i].item];
        int32_t a, b;
        if (PC[i].kind == P_TEXT || PC[i].kind == P_SPACE || PC[i].kind == P_OPEN || PC[i].kind == P_CLOSE) {
            if (!it->st) continue;
            text_metrics(it->st, &a, &b);
            if (-it->shift + a > above) above = -it->shift + a;
            if (it->shift + b > below) below = it->shift + b;
        } else if (PC[i].kind == P_ATOMIC) {
            struct lbox* ab = &L->b[it->box];
            int32_t mh = ab->h + ab->m[0] + ab->m[2];
            int32_t bl = ab->baseline >= 0 ? ab->m[0] + ab->baseline : mh;
            if (ab->kind == LB_BLOCK && (ab->st->overflow_y != OV_VISIBLE || ab->baseline < 0)) bl = mh;
            int va = ab->st->vertical_align;
            if (va == VA_TOP || va == VA_BOTTOM) { if (mh > atop_extra) atop_extra = mh; continue; }
            int32_t sh = it->shift;
            if (va == VA_MIDDLE) {
                struct wfmetrics pm;
                lb_font_metrics(c->rst, &pm);
                sh += mh / 2 - bl - pm.xheight / 2 + (bl - mh / 2) * 0;
                // center of the box at parent baseline - x-height/2
                a = mh / 2 + pm.xheight / 2;
                b = mh - a;
                if (a > above) above = a;
                if (b > below) below = b;
                continue;
            }
            if (va == VA_LEN || va == VA_SUB || va == VA_SUPER) sh += va_shift(ab->st, c->rst);
            a = bl - sh;
            b = mh - bl + sh;
            if (a > above) above = a;
            if (b > below) below = b;
        }
    }
    if (above + below < atop_extra) below = atop_extra - above;
    // ruby annotations and text-emphasis marks extend the line box above (or
    // below) the text: the extent is the annotation font's line (ascent+descent).
    // In a vertical box's logical frame the physical side maps over/under:
    // logical-over is the physical right in vertical-rl, the left in vertical-lr;
    // an explicit left/right (or under) picks the other logical side.
    int32_t anno_over = 0, anno_under = 0;
    for (int i = 0; i < n; i++) {
        struct piece* p = &PC[i];
        struct iitem* it = &IT[p->item];
        if (p->kind == P_RT) {
            struct wfmetrics m;
            lb_font_metrics(L->b[it->box].st, &m);
            const struct wstyle* rs = L->b[it->box].st;
            int over = 1;
            if (rs->wmode == WM_VRL) over = !(rs->ruby_left || rs->ruby_under);           // right = logical over; left or under = logical under
            else if (rs->wmode == WM_VLR || rs->wmode == WM_SLR) over = !rs->ruby_under || rs->ruby_left; // left = logical over; right or under = logical under
            else if (rs->ruby_under) over = 0;
            int32_t* target = over ? &anno_over : &anno_under;
            if (m.ascent + m.descent > *target) *target = m.ascent + m.descent;
        } else if (p->kind == P_TEXT && it->st && it->st->emph_cp) {
            struct wfont f;
            lb_font(it->st, &f);
            f.px = (uint16_t)(f.px / 2);
            struct wfmetrics m;
            wfont_metrics(&f, &m);
            const struct wstyle* es = it->st;
            int over = 1;
            if (es->wmode == WM_VRL) over = !(es->emph_left || es->emph_under);           // right = logical over; left or under = logical under
            else if (es->wmode == WM_VLR || es->wmode == WM_SLR) over = !es->emph_under || es->emph_left; // left = logical over; right or under = logical under
            else if (es->emph_under) over = 0;
            int32_t* target = over ? &anno_over : &anno_under;
            if (m.ascent + m.descent > *target) *target = m.ascent + m.descent;
        }
    }
    int32_t above0 = above, below0 = below;
    above += anno_over;
    below += anno_under;
    int32_t lh = above + below;
    int32_t base = c->y + above;
    int32_t anno_base_over = base - above0;       // annotation descent line (over)
    int32_t anno_base_under = base + below0;      // annotation ascent line (under)
    // emit fragments
    int32_t x = c->line_x0 + off;
    if (c->needs_bidi) {
        emit_line_bidi(c, n, x, base, off, extra, lh, anno_base_over, anno_base_under);
        goto line_tail;
    }
    int32_t ostack[128];
    int32_t ostart[128];
    uint8_t ofirst[128];
    int no = 0;
    for (int k = 0; k < c->nopen && no < 128; k++) { ostack[no] = c->open[k]; ostart[no] = x; ofirst[no] = 0; no++; }
    // inline box backgrounds are emitted before the text so they paint under
    int frag_first = L->nfr;
    // pass 1: positions; record ibox spans
    struct span { int32_t box, x0, x1; uint8_t first, last; } spans[256];
    int nspan = 0;
    int32_t px = x;
    int cur_item = -1;
    int fr_text = -1;
    // reserve: we emit text frags into a temp list after ibox frags; simplest
    // is two passes over the pieces
    for (int i = 0; i < n; i++) {
        struct piece* p = &PC[i];
        struct iitem* it = &IT[p->item];
        if (p->kind == P_OPEN) {
            struct lbox* ib = &L->b[it->box];
            if (no < 128) { ostack[no] = it->box; ostart[no] = px + ib->m[3]; ofirst[no] = 1; no++; }
            px += p->w;
            continue;
        }
        if (p->kind == P_CLOSE) {
            struct lbox* ib = &L->b[it->box];
            int32_t xe = px + p->w - ib->m[1];
            if (no) {
                no--;
                if (nspan < 256) {
                    spans[nspan].box = ostack[no]; spans[nspan].x0 = ostart[no]; spans[nspan].x1 = xe;
                    spans[nspan].first = ofirst[no]; spans[nspan].last = 1; nspan++;
                }
            }
            px += p->w;
            continue;
        }
        if (p->trim) continue;
        px += p->w + (p->kind == P_SPACE ? extra * p->nsp : 0);
    }
    int32_t line_end = px;
    for (int k = no - 1; k >= 0; k--) {
        if (nspan < 256) {
            spans[nspan].box = ostack[k]; spans[nspan].x0 = ostart[k]; spans[nspan].x1 = line_end;
            spans[nspan].first = ofirst[k]; spans[nspan].last = 0; nspan++;
        }
    }
    // ibox fragments: outer boxes first (spans were closed inner-first)
    for (int k = nspan - 1; k >= 0; k--) {
        int f = add_frag(L);
        if (f < 0) break;
        struct lbox* ib = &L->b[spans[k].box];
        struct wfmetrics m;
        lb_font_metrics(ib->st, &m);
        int32_t sh = 0;
        for (int q = 0; q < nit; q++) if (IT[q].kind == IT_OPEN && IT[q].box == spans[k].box) { sh = IT[q].shift; break; }
        struct lfrag* fr = &L->fr[f];
        fr->kind = FR_IBOX;
        fr->box = spans[k].box;
        fr->first = spans[k].first;
        fr->last = spans[k].last;
        // span edges are border-box edges already (open/close pieces carry
        // margin+border+padding; ostart/xe step over the margin only)
        fr->x = spans[k].x0;
        fr->w = spans[k].x1 - spans[k].x0;
        fr->y = base + sh - m.ascent - ib->p[0] - ib->b[0];
        fr->h = m.ascent + m.descent + ib->p[0] + ib->p[2] + ib->b[0] + ib->b[2];
    }
    // pass 2: text + atomic fragments
    px = x;
    no = 0;
    int32_t rt_x0 = -1;           // start of the base run the next <rt> centers over
    for (int i = 0; i < n; i++) {
        struct piece* p = &PC[i];
        struct iitem* it = &IT[p->item];
        if (p->kind == P_OPEN || p->kind == P_CLOSE) { px += p->w; cur_item = -1; if (rt_x0 < 0) rt_x0 = px; continue; }
        if (p->trim) continue;
        if (p->kind == P_RT) {
            // a ruby annotation: centered over the base run that precedes it
            struct lbox* tb = &L->b[it->box];
            struct wfmetrics m;
            lb_font_metrics(tb->st, &m);
            int32_t cx = rt_x0 >= 0 ? rt_x0 + (px - rt_x0) / 2 : px;
            int f = add_frag(L);
            if (f >= 0) {
                struct lfrag* fr = &L->fr[f];
                fr->kind = FR_TEXT;
                fr->box = it->box;
                fr->x = cx - p->rw / 2;
                fr->y = tb->st->ruby_under ? anno_base_under + m.ascent : anno_base_over - m.descent;
                fr->w = p->rw;
                fr->t0 = it->t0;
                fr->tl = it->tl;
                fr->h = m.ascent + m.descent;
            }
            rt_x0 = -1;
            cur_item = -1;
            continue;
        }
        if (rt_x0 < 0) rt_x0 = px;
        if (p->kind == P_ATOMIC) {
            struct lbox* ab = &L->b[it->box];
            int32_t mh = ab->h + ab->m[0] + ab->m[2];
            int32_t bl = ab->baseline >= 0 ? ab->m[0] + ab->baseline : mh;
            if (ab->kind == LB_BLOCK && (ab->st->overflow_y != OV_VISIBLE || ab->baseline < 0)) bl = mh;
            int va = ab->st->vertical_align;
            int32_t top;
            if (va == VA_TOP) top = c->y;
            else if (va == VA_BOTTOM) top = c->y + lh - mh;
            else if (va == VA_MIDDLE) {
                struct wfmetrics pm;
                lb_font_metrics(c->rst, &pm);
                top = base - pm.xheight / 2 - mh / 2;
            } else {
                int32_t sh = it->shift;
                if (va == VA_LEN || va == VA_SUB || va == VA_SUPER) sh += va_shift(ab->st, c->rst);
                top = base + sh - bl;
            }
            ab->x = L->b[c->root].b[3] + L->b[c->root].p[3] + px + ab->m[3];
            ab->y = L->b[c->root].b[0] + L->b[c->root].p[0] + top + ab->m[0];
            ab->lparent = c->root;
            int f = add_frag(L);
            if (f >= 0) {
                struct lfrag* fr = &L->fr[f];
                fr->kind = FR_ATOMIC;
                fr->box = it->box;
                fr->x = px;
                fr->y = top;
                fr->w = p->w;
                fr->h = mh;
            }
            px += p->w;
            cur_item = -1;
            continue;
        }
        int32_t adv = p->w + (p->kind == P_SPACE ? extra * p->nsp : 0);
        if (cur_item == p->item && fr_text >= 0 && L->fr[fr_text].t0 + L->fr[fr_text].tl == p->t0) {
            L->fr[fr_text].tl += p->tl;
            L->fr[fr_text].w += adv;
        } else {
            int f = add_frag(L);
            if (f < 0) break;
            struct lfrag* fr = &L->fr[f];
            fr->kind = it->box >= 0 && L->b[it->box].kind == LB_MARKER ? FR_MARKER : FR_TEXT;
            fr->box = it->box;
            fr->x = px;
            fr->y = base + it->shift;
            fr->w = adv;
            fr->t0 = p->t0;
            fr->tl = p->tl;
            fr->word_extra = extra;
            struct wfmetrics m;
            lb_font_metrics(it->st, &m);
            fr->h = m.ascent + m.descent;
            fr_text = f;
            cur_item = p->item;
        }
        if (it->st && it->st->emph_cp) {
            // text-emphasis: one marks fragment per piece (paint walks the glyphs)
            struct wfont f50;
            lb_font(it->st, &f50);
            f50.px = (uint16_t)(f50.px / 2);
            struct wfmetrics m5;
            wfont_metrics(&f50, &m5);
            const struct wstyle* es = it->st;
            int mover = 1;
            if (es->wmode == WM_VRL) mover = !(es->emph_left || es->emph_under);
            else if (es->wmode == WM_VLR || es->wmode == WM_SLR) mover = !es->emph_under || es->emph_left;
            else if (es->emph_under) mover = 0;
            int mf = add_frag(L);
            if (mf >= 0) {
                struct lfrag* mr = &L->fr[mf];
                mr->kind = FR_MARKS;
                mr->box = it->box;
                mr->x = px;
                mr->y = mover ? anno_base_over - m5.descent : anno_base_under + m5.ascent;
                mr->w = adv;
                mr->t0 = p->t0;
                mr->tl = p->tl;
                mr->h = m5.ascent + m5.descent;
            }
        }
        px += adv;
    }
    (void)frag_first;
    (void)end;
line_tail:
    // open-box state after this line = state at line start + this line's
    // opens/closes
    for (int i = 0; i < n; i++) {
        if (PC[i].kind == P_OPEN && c->nopen < 128) c->open[c->nopen++] = IT[PC[i].item].box;
        else if (PC[i].kind == P_CLOSE && c->nopen) c->nopen--;
    }
    // note: boxes open before this line stay open unless closed above
    ellipsize_line(c, lr_fr0);
    if (c->lines == 0) c->baseline_first = base;
    c->baseline_last = base;
    if (c->nlr >= c->caplr) {
        int nc = c->caplr ? c->caplr * 2 : 16;
        struct lrec* nl = (struct lrec*)w_realloc(c->lr, (size_t)nc * sizeof(struct lrec));
        if (nl) { c->lr = nl; c->caplr = nc; }
    }
    if (c->nlr < c->caplr) {
        struct lrec* lr = &c->lr[c->nlr++];
        lr->fr0 = lr_fr0; lr->y = lr_y; lr->x0 = lr_x0; lr->avail = lr_avail; lr->base = base;
    }
    c->lines++;
    L->nlines++;
    if (L->ifc_balancing) L->seg_lines[L->bal_seg < 16 ? L->bal_seg : 15]++;
    c->y += lh;
    c->first_line = 0;
    line_bounds(c);
}

// Fix: pieces store the open-box state at line start. finish_line rebuilds
// the continuing-open list from the line's own pieces, so boxes opened on
// earlier lines must be re-seeded. We keep them in c->open before the call.

static void break_at(struct ifc* c, int k, int forced) {
    struct wlayout* L0 = c->L;
    // a break after a soft hyphen shows the hyphen
    if (k >= 0 && PC[k].kind == P_TEXT && PC[k].tl >= 2 && !forced &&
        (unsigned char)L0->text.p[PC[k].t0 + PC[k].tl - 2] == 0xC2 && (unsigned char)L0->text.p[PC[k].t0 + PC[k].tl - 1] == 0xAD) {
        int hp = pc_push();
        if (hp >= 0) {
            struct piece hy = PC[k];
            uint32_t off = (uint32_t)L0->text.len;
            wbuf_putc(&L0->text, '-');
            hy.t0 = off; hy.tl = 1; hy.brk = 1; hy.nsp = 0;
            struct wfont f;
            lb_font(IT[hy.item].st, &f);
            hy.w = wfont_measure(&f, "-", 1);
            for (int i = hp; i > k + 1; i--) PC[i] = PC[i - 1];
            PC[k + 1] = hy;
            PC[k].brk = 0;
            k++;
        }
    }
    // pieces [0..k] form the line; the rest carries over
    int carry = npc - (k + 1);
    finish_line(c, k + 1, forced);
    // move the carried pieces to the front
    for (int i = 0; i < carry; i++) PC[i] = PC[k + 1 + i];
    npc = carry;
    // drop leading collapsible spaces on the new line
    while (npc > 0 && PC[0].kind == P_SPACE && !PC[0].hang) {
        const struct wstyle* s = IT[PC[0].item].st;
        if (s->white_space == WS_PRE || s->white_space == WS_PRE_WRAP || s->white_space == WS_BREAK_SPACES) break;
        for (int i = 1; i < npc; i++) PC[i - 1] = PC[i];
        npc--;
    }
    c->used = 0;
    c->last_brk = -1;
    for (int i = 0; i < npc; i++) {
        c->used += PC[i].w;
        if (PC[i].brk) c->last_brk = i;
    }
}

// Add a piece; may trigger line breaks.
static void add_piece(struct ifc* c, const struct piece* np, int breakable_before) {
    // does it fit?
    int is_space = np->kind == P_SPACE && IT[np->item].st->white_space != WS_BREAK_SPACES;   // break-spaces: spaces do not hang
    for (int guard = 0; guard < 1000; guard++) {
        if (is_space || c->used + np->w <= c->avail) break;
        if (c->last_brk >= 0 && c->last_brk < npc - 0) {
            // break at the last opportunity (but never leave an empty line)
            int k = c->last_brk;
            break_at(c, k, 0);
            continue;
        }
        if (npc == 0 || (breakable_before && c->used > 0)) {
            if (npc > 0 && breakable_before) { break_at(c, npc - 1, 0); continue; }
            // empty line and the piece does not fit: move below floats if any
            if (line_has_floats_narrowing(c) && c->L->nfl > c->L->fl_base) {
                int32_t nb = INT32_MAX;
                struct wlayout* L = c->L;
                for (int i = L->fl_base; i < L->nfl; i++) {
                    struct bfloat* f = &L->fl[i];
                    int32_t fb = f->y + f->h - c->abs_top;
                    if (fb > c->y && fb < nb) nb = fb;
                }
                if (nb != INT32_MAX) { c->y = nb; line_bounds(c); continue; }
            }
        }
        break;
    }
    int k = pc_push();
    if (k < 0) return;
    PC[k] = *np;
    c->used += np->w;
    if (np->brk) c->last_brk = k;
}

static void mark_break_before(struct ifc* c) {
    if (npc > 0) { PC[npc - 1].brk = 1; c->last_brk = npc - 1; }
}

// text-overflow: a line wider than its scroll container ends in an ellipsis (or the given string): what does not fit with it is cut
static void ellipsize_line(struct ifc* c, int fr0) {
    const struct wstyle* st = c->rst;
    if (!st->text_overflow_ellipsis || st->overflow_x == OV_VISIBLE) return;
    struct wlayout* L = c->L;
    int32_t limit = c->line_x0 + c->avail, right = 0;
    for (int i = fr0; i < L->nfr; i++) {
        const struct lfrag* f = &L->fr[i];
        if ((f->kind == FR_TEXT || f->kind == FR_MARKER || f->kind == FR_ATOMIC) && f->x + f->w > right) right = f->x + f->w;
    }
    if (right <= limit) return;
    struct wfont rf;
    lb_font(st, &rf);
    const char* es = "\xE2\x80\xA6"; int el = 3;
    if (st->text_overflow_ellipsis == 2) { es = css_str(L->ss, st->text_overflow_str, st->text_overflow_len); el = st->text_overflow_len; }
    int32_t ew = el ? wfont_measure(&rf, es, el) : 0;
    int32_t cut = limit - ew, kept = c->line_x0;
    int32_t ey = 0, eh = 0;
    for (int i = fr0; i < L->nfr; i++) {
        struct lfrag* f = &L->fr[i];
        if (f->kind == FR_ATOMIC) { if (f->x + f->w > cut) { f->w = 0; f->h = 0; if (f->box >= 0) L->b[f->box].flags |= BF_CLAMPED; } else if (f->x + f->w > kept) kept = f->x + f->w; continue; }
        if (f->kind != FR_TEXT && f->kind != FR_MARKER) continue;
        const struct lbox* tb = &L->b[f->box >= 0 ? f->box : c->root];
        struct wfont wf;
        lb_font(tb->st, &wf);
        if (!eh) { ey = f->y; eh = f->h; }
        while (f->tl > 0 && f->x + f->w > cut) {                         // drop characters from the end
            uint32_t k = f->tl - 1;
            while (k > 0 && ((unsigned char)L->text.p[f->t0 + k] & 0xC0) == 0x80) k--;
            f->tl = k;
            f->w = k ? wfont_measure(&wf, L->text.p + f->t0, (int)k) : 0;
        }
        if (f->tl > 0 && f->x + f->w > kept) kept = f->x + f->w;
    }
    if (!el) return;
    int e = add_frag(L);
    if (e < 0) return;
    struct lfrag* ef = &L->fr[e];
    ef->kind = FR_TEXT;
    ef->box = c->root;
    ef->x = kept;
    ef->y = ey;
    ef->w = ew;
    ef->h = eh;
    ef->t0 = (uint32_t)L->text.len;
    ef->tl = (uint32_t)el;
    wbuf_put(&L->text, es, el);
}

// ---- line-clamp --------------------------------------------------------------------------------------------------------
// A line-clamp container (lay_box) sets L->clamp_left to its N; every inline formatting context below it takes its lines off that
// count in document order. The one holding line N+1 is cut there and its Nth line ends in an ellipsis (as much text is dropped as it
// takes to make room); when line N is the last line of a context, an ellipsis is reserved (an empty fragment) that the next content
// below the clamp point switches on. Boxes after the clamp point are not laid out at all.

static const char ELL[] = "\xE2\x80\xA6";

// N lines the box clamps to (0 = it does not)
int lay_clamp_lines(const struct wlayout* L, const struct lbox* b) {
    const struct wstyle* st = b->st;
    int n = st->multicol ? 0 : st->line_clamp;
    if (n > 0 && st->clamp_webkit && !(st->webkit_box && st->webkit_orient_v)) n = 0;    // the legacy spelling needs display: -webkit-box + vertical orient
    if (n < 0) {          // auto: as many lines as the larger of min-height and max-height holds
        n = 0;
        if (st->max_h.t == WL_LEN && !st->max_h.pct) {
            int32_t mh = st->max_h.px;
            if (st->min_h.t == WL_LEN && !st->min_h.pct && st->min_h.px > mh) mh = st->min_h.px;
            int32_t lh = lb_line_height(st);
            if (lh > 0 && mh > 0) { n = mh / lh; if (n < 1) n = 1; }
        }
    }
    (void)L;
    return n;
}

// switch on the ellipsis reserved on the last line before the clamp point
void lay_clamp_activate(struct wlayout* L) {
    if (L->pend_ef < 0 || L->clamp_skip) return;
    struct lfrag* ef = &L->fr[L->pend_ef];
    struct lfrag* f = &L->fr[L->pend_last];
    const struct lbox* rb = &L->b[L->pend_root];
    struct wfont wf;
    lb_font(rb->st, &wf);
    const char* ell = ELL; int ell_len = 3;
    if (L->clamp_st && L->clamp_st->clamp_ell) {
        ell_len = L->clamp_st->clamp_ell_len;
        ell = ell_len ? css_str(L->ss, L->clamp_st->clamp_ell_str, ell_len) : ELL;
    }
    int32_t ew = ell_len ? wfont_measure(&wf, ell, ell_len) : 0;
    const struct lbox* tb = &L->b[f->box >= 0 ? f->box : L->pend_root];
    struct wfont tf;
    lb_font(tb->st, &tf);
    while (f->tl > 0 && f->x + f->w + ew > L->pend_limit) {           // drop words (then characters) until the ellipsis fits
        uint32_t k = f->tl;
        while (k > 0 && L->text.p[f->t0 + k - 1] == ' ') k--;
        if (k == f->tl) {
            uint32_t q = k;
            while (q > 0 && L->text.p[f->t0 + q - 1] != ' ') q--;
            if (q > 0) k = q; else {
                k--;
                while (k > 0 && ((unsigned char)L->text.p[f->t0 + k] & 0xC0) == 0x80) k--;
            }
        }
        f->tl = k;
        f->w = k ? wfont_measure(&tf, L->text.p + f->t0, (int)k) : 0;
    }
    ef->kind = FR_TEXT;
    ef->box = L->pend_root;
    ef->x = f->x + f->w;
    ef->w = ew;
    ef->t0 = (uint32_t)L->text.len;
    ef->tl = (uint32_t)ell_len;
    wbuf_put(&L->text, ell, ell_len);
    L->pend_ef = -1;
}

// does the (in-flow) subtree show anything that the clamp hides?
static int clamp_has_content(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    if (b->flags & (BF_ABS | BF_FLOAT)) return 0;
    if (b->kind == LB_REPLACED) return 1;
    if (b->kind == LB_TEXT) {
        int rl;
        const char* r = lay_box_raw(L, bi, &rl);
        for (int i = 0; i < rl; i++) if (r[i] != ' ' && r[i] != '\t' && r[i] != '\n' && r[i] != '\r' && r[i] != '\f') return 1;
        return 0;
    }
    for (int c = b->first; c >= 0; c = L->b[c].next) if (clamp_has_content(L, c)) return 1;
    return 0;
}

// a box after the clamp point: gone (no size, no painting); content in it puts the ellipsis on
void lay_clamp_hide(struct wlayout* L, int c) {
    struct lbox* b = &L->b[c];
    if (clamp_has_content(L, c)) lay_clamp_activate(L);
    b->flags |= BF_CLAMPED;
    b->w = 0; b->h = 0;
    b->m[0] = b->m[1] = b->m[2] = b->m[3] = 0;
    b->cm[0] = b->cm[1] = b->cm[2] = b->cm[3] = 0;
    b->nfr = 0;
}

// at the end of an inline formatting context: take its lines off the count, cut at the clamp point, reserve the ellipsis
static void clamp_ifc(struct ifc* c, int bi, int fr0) {
    struct wlayout* L = c->L;
    int left = L->clamp_left;
    if (left < 0 || L->clamp_skip || c->nlr == 0) return;
    if (c->nlr <= left) {
        L->clamp_left = left - c->nlr;
        if (L->clamp_left == 0) {                       // the clamp point is right after this context: reserve the ellipsis
            int lastl = c->nlr - 1, last = -1;
            for (int i = L->nfr - 1; i >= c->lr[lastl].fr0; i--) if (L->fr[i].kind == FR_TEXT && L->fr[i].tl > 0) { last = i; break; }
            if (last >= 0) {
                struct lfrag keep = L->fr[last];
                int e = add_frag(L);
                if (e >= 0) {
                    L->fr[e].kind = FR_TEXT; L->fr[e].box = bi; L->fr[e].x = keep.x + keep.w; L->fr[e].y = keep.y; L->fr[e].h = keep.h;
                    L->pend_ef = e; L->pend_last = last; L->pend_root = bi;
                    L->pend_limit = c->lr[lastl].x0 + c->lr[lastl].avail;
                }
            }
        }
        return;
    }
    // this context holds line N + 1: cut there
    if (left == 0) {                                    // no line of it may stay: the ellipsis goes on the previous context
        L->nfr = fr0;
        L->nlines -= c->lines;
        c->y = 0; c->lines = 0; c->nlr = 0;
        lay_clamp_activate(L);
        return;
    }
    int cut = c->lr[left].fr0;
    int last = -1;
    for (int i = cut - 1; i >= c->lr[left - 1].fr0; i--) if (L->fr[i].kind == FR_TEXT && L->fr[i].tl > 0) { last = i; break; }
    L->nfr = cut;                                       // the lines after the Nth are gone
    c->y = c->lr[left].y;
    c->baseline_last = c->lr[left - 1].base;
    L->nlines -= c->lines - left;
    c->lines = left;
    c->nlr = left;
    L->clamp_left = 0;
    if (last >= 0) {
        struct lfrag keep = L->fr[last];
        int e = add_frag(L);
        if (e >= 0) {
            L->fr[e].kind = FR_TEXT; L->fr[e].box = bi; L->fr[e].x = keep.x + keep.w; L->fr[e].y = keep.y; L->fr[e].h = keep.h;
            L->pend_ef = e; L->pend_last = last; L->pend_root = bi;
            L->pend_limit = c->lr[left - 1].x0 + c->lr[left - 1].avail;
            lay_clamp_activate(L);
        }
    }
}

static void lay_ifc_run(struct wlayout* L, int bi, int32_t content_w, int32_t* height);

// text-wrap: balance: lay the lines out, then narrow the measure of each paragraph (the run between forced breaks) as far as its number
// of lines allows, so its lines come out about equally long
int lay_balance_enabled = 0;     // text-wrap: balance is parsed but off: the result differs from real engines more often than it helps (WPT: 5 net losses)

void lay_ifc(struct wlayout* L, int bi, int32_t content_w, int32_t* height) {
    const struct wstyle* st = L->b[bi].st;
    if (!lay_balance_enabled || !st->tw_balance || st->tw_nowrap || content_w <= 0 || L->ifc_balancing) { lay_ifc_run(L, bi, content_w, height); return; }
    int fr0 = L->nfr, nl0 = L->nlines, tl0 = L->text.len;
    int32_t h0 = 0;
    L->ifc_balancing = 1;
    memset(L->bal_w, 0, sizeof L->bal_w);
    memset(L->seg_lines, 0, sizeof L->seg_lines);
    L->bal_seg = 0;
    lay_ifc_run(L, bi, content_w, &h0);
    int nseg = L->bal_seg + 1;
    if (nseg > 16) nseg = 16;
    int counts[16];
    memcpy(counts, L->seg_lines, sizeof counts);
    int did = 0;
    for (int k = 0; k < nseg; k++) {
        if (counts[k] < 2 || counts[k] > 12) continue;
        int32_t lo = content_w / counts[k], hi = content_w;                  // hi always keeps the count
        while (hi - lo > 64) {
            int32_t mid = (lo + hi) / 2;
            L->nfr = fr0; L->nlines = nl0; L->text.len = tl0;
            memset(L->seg_lines, 0, sizeof L->seg_lines);
            L->bal_seg = 0;
            L->bal_w[k] = mid;
            int32_t h1;
            lay_ifc_run(L, bi, content_w, &h1);
            if (L->seg_lines[k] == counts[k]) hi = mid; else lo = mid;
        }
        L->bal_w[k] = hi;
        did = 1;
    }
    L->nfr = fr0; L->nlines = nl0; L->text.len = tl0;
    memset(L->seg_lines, 0, sizeof L->seg_lines);
    L->bal_seg = 0;
    if (did) lay_ifc_run(L, bi, content_w, height);
    else { lay_ifc_run(L, bi, content_w, height); }
    memset(L->bal_w, 0, sizeof L->bal_w);
    L->ifc_balancing = 0;
}

static void lay_ifc_run(struct wlayout* L, int bi, int32_t content_w, int32_t* height) {
    struct lbox* rb = &L->b[bi];
    nit = 0;
    npc = 0;
    struct tstate ts = { 0, 1, 1 };
    int fr0 = L->nfr;
    // flatten (needs IT scratch; nested IFCs inside atomics run later, so
    // copy the item list for this IFC)
    flatten(L, bi, rb->st, 0, -1, &ts, content_w, 0);
    int n_items = nit;
    struct iitem* items = (struct iitem*)w_malloc((n_items + 1) * sizeof(struct iitem));
    if (!items) { *height = 0; return; }
    memcpy(items, IT, n_items * sizeof(struct iitem));
    // lay out atomic inlines first (they run nested layouts that reuse the
    // scratch arrays)
    rb = &L->b[bi];
    int32_t ctop = rb->b[0] + rb->p[0], cleft = rb->b[3] + rb->p[3];
    for (int i = 0; i < n_items; i++) {
        if (items[i].kind == IT_ATOMIC) {
            int a = items[i].box;
            struct lbox* ab = &L->b[a];
            ab->pax = rb->pax + cleft;
            ab->pay = rb->pay + ctop;
            ab->cbh = -1;
            lay_box(L, a, content_w, -1, -1);
        } else if (items[i].kind == IT_FLOAT) {
            int fb = items[i].box;
            lb_resolve_box_sides(L, fb, content_w);
            struct lbox* f = &L->b[fb];
            int32_t fw = -1;
            if (f->st->width.t != WL_LEN && f->kind != LB_REPLACED && f->fc != FC_TABLE)
                fw = lay_shrink_width(L, fb, content_w - f->m[1] - f->m[3]);
            f->pax = rb->pax + cleft;
            f->pay = rb->pay + ctop;
            f->cbh = -1;
            int saved_base = L->fl_base, saved_n = L->nfl;
            L->fl_base = L->nfl;
            lay_box(L, fb, content_w, fw, -1);
            L->nfl = saved_n;
            L->fl_base = saved_base;
        }
        rb = &L->b[bi];
    }
    // restore scratch
    if (n_items > capit) {
        struct iitem* n = (struct iitem*)w_realloc(IT, n_items * sizeof(struct iitem));
        if (!n) { w_free(items); *height = 0; return; }
        IT = n;
        capit = n_items;
    }
    memcpy(IT, items, n_items * sizeof(struct iitem));
    nit = n_items;
    w_free(items);
    npc = 0;
    fr0 = L->nfr;
    struct ifc c;
    memset(&c, 0, sizeof c);
    c.L = L;
    c.root = bi;
    c.rst = rb->st;
    c.content_w = content_w;
    c.abs_left = rb->pax + cleft;
    c.abs_top = rb->pay + ctop;
    c.first_line = 1;
    c.last_brk = -1;
    c.baseline_first = -1;
    text_metrics(rb->st, &c.strut_above, &c.strut_below);
    line_bounds(&c);
    // bidi is engaged only when the paragraph or some content needs it: pure
    // ltr text without formatting characters keeps the plain layout path
    c.needs_bidi = (uint8_t)(rb->st->direction_rtl || rb->st->ubidi);
    for (int i = 0; i < nit && !c.needs_bidi; i++) {
        struct iitem* it = &IT[i];
        if (it->st && it->st->ubidi) { c.needs_bidi = 1; break; }
        if (it->kind != IT_TEXT) continue;
        for (uint32_t q = it->t0; q < it->t0 + it->tl; ) {
            uint32_t cp; int nn = w_utf8_dec(L->text.p + q, (int)(it->t0 + it->tl - q), &cp);
            if (!nn) break;
            int cl = bdi_class(cp);
            if (cl == BCL_R || cl == BCL_AL ||
                (cp >= 0x200E && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069)) {
                c.needs_bidi = 1;
                break;
            }
            q += (uint32_t)nn;
        }
    }
    for (int i = 0; i < nit; i++) {
        struct iitem* it = &IT[i];
        switch (it->kind) {
        case IT_TEXT: {
            const char* text = L->text.p;
            uint32_t p = it->t0, e = it->t0 + it->tl;
            struct tok t;
            int wrap = wraps(it->st);
            int ow = it->st->overflow_wrap || it->st->word_break == WB_BREAK_ALL;
            while (next_tok(L->text.p, p, e, it->st, &t)) {
                text = L->text.p;
                struct piece pc;
                memset(&pc, 0, sizeof pc);
                pc.item = i;
                pc.t0 = t.t0;
                pc.tl = t.tl;
                pc.kind = t.space ? P_SPACE : P_TEXT;
                int nsp = 0;
                pc.w = measure(it->st, text + t.t0, (int)t.tl, &nsp);
                pc.nsp = t.space ? nsp : 0;
                pc.hang = (uint8_t)t.hang;
                pc.brk = (uint8_t)(wrap && t.brk);
                // overflow-wrap: split a word that cannot fit on an empty line
                if (!t.space && wrap && ow && pc.w > c.avail && c.avail > 0) {
                    uint32_t q = t.t0, qe = t.t0 + t.tl;
                    while (q < qe) {
                        uint32_t s0 = q;
                        int32_t w = 0;
                        while (q < qe) {
                            uint32_t cp;
                            int nn = w_utf8_dec(L->text.p + q, (int)(qe - q), &cp);
                            int32_t cw = measure(it->st, L->text.p + q, nn, 0);
                            if (w + cw > c.avail - c.used && q > s0) break;
                            w += cw;
                            q += nn;
                            if (w > c.avail) break;
                        }
                        struct piece sp = pc;
                        sp.t0 = s0;
                        sp.tl = q - s0;
                        sp.w = w;
                        sp.brk = 1;
                        add_piece(&c, &sp, 0);
                        if (q < qe) break_at(&c, npc - 1, 0);
                    }
                    p = t.t0 + t.tl;
                    continue;
                }
                add_piece(&c, &pc, 0);
                p = t.t0 + t.tl;
            }
            break;
        }
        case IT_OPEN: {
            struct lbox* ib = &L->b[it->box];
            struct piece pc;
            memset(&pc, 0, sizeof pc);
            pc.kind = P_OPEN;
            pc.item = i;
            pc.w = ib->m[3] + ib->b[3] + ib->p[3];
            add_piece(&c, &pc, 0);
            break;
        }
        case IT_CLOSE: {
            struct lbox* ib = &L->b[it->box];
            struct piece pc;
            memset(&pc, 0, sizeof pc);
            pc.kind = P_CLOSE;
            pc.item = i;
            pc.w = ib->m[1] + ib->b[1] + ib->p[1];
            // closing sides stick to the preceding content
            int k = pc_push();
            if (k >= 0) { PC[k] = pc; c.used += pc.w; }
            break;
        }
        case IT_ATOMIC: {
            struct lbox* ab = &L->b[it->box];
            struct piece pc;
            memset(&pc, 0, sizeof pc);
            pc.kind = P_ATOMIC;
            pc.item = i;
            pc.w = ab->w + ab->m[1] + ab->m[3];
            int wrap = wraps(c.rst);
            int wb = wrap, wa = wrap;     // soft wrap opportunity before / after: not next to a no-break character
            if (atomic_nobrk(it_edge_cp(L, IT, nit, i, -1))) wb = 0;
            if (atomic_nobrk(it_edge_cp(L, IT, nit, i, 1))) wa = 0;
            if (wb) mark_break_before(&c);
            else if (wrap && npc > 0 && PC[npc - 1].brk) {       // the text before it offered a break after its last word: withdraw it
                PC[npc - 1].brk = 0;
                int k = npc - 2;
                while (k >= 0 && !PC[k].brk) k--;
                c.last_brk = k;
            }
            pc.brk = (uint8_t)wa;
            add_piece(&c, &pc, wb);
            break;
        }
        case IT_RT: {
            struct lbox* tb = &L->b[it->box];
            struct piece pc;
            memset(&pc, 0, sizeof pc);
            pc.kind = P_RT;
            pc.item = i;
            pc.rw = measure(tb->st, L->text.p + it->t0, (int)it->tl, 0);
            add_piece(&c, &pc, 0);
            break;
        }
        case IT_BR:
        finish_line(&c, npc, 2);
            L->bal_seg++;
            if (L->ifc_balancing) line_bounds(&c);       // the next paragraph has its own measure
            // open boxes persist through the break
            {
                int32_t res[128];
                int nr = 0;
                (void)res;
                (void)nr;
            }
            npc = 0;
            c.used = 0;
            c.last_brk = -1;
            break;
        case IT_WBR:
            mark_break_before(&c);
            break;
        case IT_FLOAT: {
            int fb = it->box;
            struct lbox* f;
            int32_t y = c.abs_top + c.y;
            // float goes on the current line if it fits beside its content
            int32_t need = L->b[fb].w + L->b[fb].m[1] + L->b[fb].m[3];
            if (c.used + need > c.avail && npc > 0) {
                // place below the current line
                int32_t a, bl;
                text_metrics(c.rst, &a, &bl);
                y += a + bl;
            }
            fl_place(L, fb, c.abs_left, c.abs_left + content_w, &y);
            f = &L->b[fb];
            rb = &L->b[bi];
            f->x = f->pax - rb->pax;
            f->y = f->pay - rb->pay;
            f->lparent = bi;
            // recompute the current line's extent
            int32_t old_x0 = c.line_x0;
            line_bounds(&c);
            (void)old_x0;
            break;
        }
        case IT_ABS: {
            struct lbox* ab = &L->b[it->box];
            if (!ab->pst->abs_inl && c.used > 0) {         // a block-level box would start below the line so far
                finish_line(&c, npc, 1);
                npc = 0; c.used = 0; c.last_brk = -1;
            }
            if (rb->pst->direction_rtl && rb->pst->wmode == WM_HTB) { ab->sx = c.abs_left + c.line_x0 + c.avail - c.used; ab->_pad |= 2; }
            else { ab->sx = c.abs_left + c.line_x0 + c.used; ab->_pad &= (uint16_t)~2; }
            ab->sy = c.abs_top + c.y;
            lay_abs_register(L, it->box);
            break;
        }
        }
    }
    if (npc > 0) finish_line(&c, npc, 1);
    clamp_ifc(&c, bi, fr0);
    w_free(c.lr);
    rb = &L->b[bi];
    rb->fr0 = fr0;
    rb->nfr = L->nfr - fr0;
    lay_abs_inline(L, bi);
    if (c.lines > 0) {
        rb->baseline = ctop + c.baseline_first;
        rb->last_baseline = ctop + c.baseline_last;
    }
    *height = c.y;
}

// ---- intrinsic widths ---------------------------------------------------------------

void lay_ifc_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx) {
    nit = 0;
    struct tstate ts = { 0, 1, 1 };
    uint32_t text_mark = (uint32_t)L->text.len;
    flatten(L, bi, L->b[bi].st, 0, -1, &ts, 0, 0);
    int n_items = nit;
    struct iitem* items = (struct iitem*)w_malloc((n_items + 1) * sizeof(struct iitem));
    if (!items) { *mn = *mx = 0; return; }
    memcpy(items, IT, n_items * sizeof(struct iitem));
    int32_t line = 0, best_line = 0, word = 0, best_word = 0;
    int32_t floats = 0;
    int32_t trail = 0;          // trailing collapsible spaces: they hang off the end of a line and do not count
    for (int i = 0; i < n_items; i++) {
        struct iitem* it = &items[i];
        switch (it->kind) {
        case IT_TEXT: {
            uint32_t p = it->t0, e = it->t0 + it->tl;
            struct tok t;
            int wrap = wraps(it->st);
            while (next_tok(L->text.p, p, e, it->st, &t)) {
                int32_t w = measure(it->st, L->text.p + t.t0, (int)t.tl, 0);
                line += w;
                if (t.space && it->st->white_space != WS_PRE && it->st->white_space != WS_PRE_WRAP && it->st->white_space != WS_BREAK_SPACES && !t.hang) trail += w;
                else trail = 0;
                if (t.space && wrap) {
                    if (word > best_word) best_word = word;
                    word = 0;
                } else if (!t.space && wrap && it->st->overflow_wrap == 2) {          // overflow-wrap: anywhere: every character may end a line
                    for (uint32_t q = t.t0; q < t.t0 + t.tl;) {
                        uint32_t cp;
                        int nn = w_utf8_dec(L->text.p + q, (int)(t.t0 + t.tl - q), &cp);
                        int32_t cw = measure(it->st, L->text.p + q, nn, 0);
                        if (word + cw > best_word) best_word = word + cw;
                        word = 0;
                        q += nn;
                    }
                } else {
                    word += w;
                    if (t.brk && wrap) { if (word > best_word) best_word = word; word = 0; }
                }
                p = t.t0 + t.tl;
            }
            if (it->st->overflow_wrap && it->st->word_break == WB_BREAK_ALL) {}
            break;
        }
        case IT_OPEN: case IT_CLOSE: {
            struct lbox* ib = &L->b[it->box];
            lb_resolve_box_sides(L, it->box, 0);
            int32_t w = it->kind == IT_OPEN ? ib->m[3] + ib->b[3] + ib->p[3] : ib->m[1] + ib->b[1] + ib->p[1];
            line += w;
            word += w;
            break;
        }
        case IT_ATOMIC: {
            int32_t a, z;
            lay_intrinsic(L, it->box, &a, &z);
            struct lbox* ab = &L->b[it->box];
            int32_t mm = 0;
            if (ab->st->margin[1].t == WL_LEN && !ab->st->margin[1].pct) mm += ab->st->margin[1].px;
            if (ab->st->margin[3].t == WL_LEN && !ab->st->margin[3].pct) mm += ab->st->margin[3].px;
            if (mm < 0) mm = 0;
            if (word > best_word) best_word = word;
            word = 0;
            if (a + mm > best_word) best_word = a + mm;
            line += z + mm;
            trail = 0;
            break;
        }
        case IT_BR:
            if (line - trail > best_line) best_line = line - trail;
            trail = 0;
            if (word > best_word) best_word = word;
            line = 0;
            word = 0;
            break;
        case IT_WBR:
            if (word > best_word) best_word = word;
            word = 0;
            break;
        case IT_FLOAT: {
            int32_t a, z;
            lay_intrinsic(L, it->box, &a, &z);
            if (a > best_word) best_word = a;
            floats += z;
            break;
        }
        case IT_RT:
            break;      // ruby annotations do not count toward intrinsic widths
        }
    }
    if (line - trail > best_line) best_line = line - trail;
    if (word > best_word) best_word = word;
    w_free(items);
    // processed text from this measurement pass is scratch: drop it
    L->text.len = (int)text_mark;
    best_line += floats;
    *mn = best_word;
    *mx = best_line > best_word ? best_line : best_word;
}

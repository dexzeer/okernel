// Inline formatting context: text processing, line breaking, line boxes.
#include "lay_int.h"
#include "../cjk.h"

enum { IT_TEXT, IT_OPEN, IT_CLOSE, IT_ATOMIC, IT_BR, IT_WBR, IT_FLOAT, IT_ABS };

struct iitem {
    uint8_t kind;
    int32_t box;                 // text/inline/atomic/float/abs box
    uint32_t t0, tl;             // processed text (IT_TEXT)
    const struct wstyle* st;     // style of the text / box
    int32_t shift;               // baseline shift of the enclosing inline (LU, +down)
    int32_t ibox;                // enclosing inline box (-1 = root)
};

enum { P_TEXT, P_SPACE, P_OPEN, P_CLOSE, P_ATOMIC };
struct piece {
    uint8_t kind, brk;           // brk: a line break is allowed after this piece
    uint8_t trim;                // trailing collapsible space (hangs, not drawn)
    int32_t item;
    uint32_t t0, tl;
    int32_t w;
    int32_t nsp;                 // spaces (justification)
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

static uint32_t to_upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c < 0x80) return c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & ~1u;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3C9) return c == 0x3C2 ? 0x3A3 : c - 0x20;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}

static uint32_t to_lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c | 1u;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return c;
    }
    if (c >= 0x391 && c <= 0x3A9) return c + 0x20;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

static int is_letter_cp(uint32_t c) {
    return (c < 0x80) ? w_isalnum((int)c) : (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}

// ---- text processing ---------------------------------------------------------------

struct tstate { int last_space; int line_start; int cap_next; };

static void put_cp(struct wbuf* b, uint32_t cp) {
    char u[4];
    wbuf_put(b, u, w_utf8_enc(cp, u));
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
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\f') {
            if (keep_spaces) {
                if (cp == '\t') { int nsp = 8 - (col % 8); for (int k = 0; k < nsp; k++) wbuf_putc(&L->text, ' '); col += nsp; }
                else { wbuf_putc(&L->text, ' '); col++; }
                ts->last_space = 0;
                ts->line_start = 0;
                ts->cap_next = 1;
                continue;
            }
            if (ts->last_space || ts->line_start) continue;
            wbuf_putc(&L->text, ' ');
            ts->last_space = 1;
            ts->cap_next = 1;
            col++;
            continue;
        }
        if (cp == 0xAD) continue; // soft hyphen: invisible here
        ts->last_space = 0;
        ts->line_start = 0;
        if (tt == TT_UPPER) cp = to_upper(cp);
        else if (tt == TT_LOWER) cp = to_lower(cp);
        else if (tt == TT_CAPITALIZE) {
            if (ts->cap_next && is_letter_cp(cp)) cp = to_upper(cp);
            ts->cap_next = !is_letter_cp(cp) && cp != '\'' && cp != 0x2019;
        }
        if (cp < 0x20 && cp != '\t') continue;
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
            int k = it_push();
            if (k >= 0) { IT[k].kind = IT_WBR; IT[k].box = c; IT[k].ibox = ibox; }
            break;
        }
        case LB_INLINE: {
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

static int wraps(const struct wstyle* s) {
    return s->white_space != WS_NOWRAP && s->white_space != WS_PRE;
}

// Split an IT_TEXT item into word/space pieces appended to *out callback.
// We generate pieces lazily per item inside the line builder.
struct tok { uint32_t t0, tl; int space; int brk; };

// next token from text [p, e): a run of spaces, or a word (with internal
// break opportunities at hyphens and CJK characters ending the token)
static int next_tok(const char* text, uint32_t p, uint32_t e, const struct wstyle* st, struct tok* t) {
    if (p >= e) return 0;
    int keep = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP || st->white_space == WS_BREAK_SPACES;
    t->t0 = p;
    if (text[p] == ' ') {
        uint32_t q = p;
        while (q < e && text[q] == ' ') q++;
        t->tl = q - p;
        t->space = 1;
        t->brk = 1;
        (void)keep;
        return 1;
    }
    uint32_t q = p;
    int break_all = st->word_break == WB_BREAK_ALL;
    while (q < e && text[q] != ' ') {
        uint32_t cp;
        int n = w_utf8_dec(text + q, (int)(e - q), &cp);
        if (is_cjk_cp(cp) && st->word_break != WB_KEEP_ALL) {
            if (q == p) { q += n; break; }  // a CJK char is its own token
            break;
        }
        q += n;
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
    t->brk = 0;
    if (q < e && text[q] != ' ') t->brk = 1; // internal opportunity (hyphen / CJK / break-all)
    return 1;
}

// ---- line building ----------------------------------------------------------------

struct ifc {
    struct wlayout* L;
    int root;
    const struct wstyle* rst;
    int32_t content_w;
    int32_t abs_left, abs_top;     // root content box (provisional absolute)
    int32_t y;                     // current line top (relative to content top)
    int32_t line_x0, avail;        // current line box horizontal extent
    int32_t used;                  // width of pieces on the line
    int last_brk;                  // index of last piece allowing a break after
    int first_line;
    int lines;
    int32_t strut_above, strut_below;
    int32_t baseline_first, baseline_last;
    // open inline boxes at line start
    int32_t open[128];
    int nopen;
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

// Finalize pieces [0, n) as one line; forced = ended by a forced break.
static void finish_line(struct ifc* c, int n, int forced) {
    struct wlayout* L = c->L;
    // trailing collapsible spaces hang (not counted), even when followed by
    // closing inline boxes
    for (int i = 0; i < n; i++) PC[i].trim = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (PC[i].kind == P_OPEN || PC[i].kind == P_CLOSE) continue;
        if (PC[i].kind != P_SPACE) break;
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
            if (s->white_space == WS_PRE || s->white_space == WS_PRE_WRAP) has_content = 1;
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
    if (ta == TA_START) ta = c->rst->direction_rtl ? TA_RIGHT : TA_LEFT;
    if (ta == TA_END) ta = c->rst->direction_rtl ? TA_LEFT : TA_RIGHT;
    if (ta == TA_WEBKIT_CENTER) ta = TA_CENTER;
    int32_t off = 0, extra = 0;
    int32_t freew = c->avail - used;
    if (ta == TA_RIGHT && freew > 0) off = freew;
    else if (ta == TA_CENTER && freew > 0) off = freew / 2;
    else if (ta == TA_JUSTIFY && !forced && freew > 0 && nsp > 0) extra = freew / nsp;
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
    int32_t lh = above + below;
    int32_t base = c->y + above;
    // emit fragments
    int32_t x = c->line_x0 + off;
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
        fr->x = spans[k].x0 - (fr->first ? ib->b[3] + ib->p[3] : 0);
        fr->w = spans[k].x1 - spans[k].x0 + (fr->first ? ib->b[3] + ib->p[3] : 0) + (fr->last ? ib->b[1] + ib->p[1] : 0);
        fr->y = base + sh - m.ascent - ib->p[0] - ib->b[0];
        fr->h = m.ascent + m.descent + ib->p[0] + ib->p[2] + ib->b[0] + ib->b[2];
    }
    // pass 2: text + atomic fragments
    px = x;
    no = 0;
    for (int i = 0; i < n; i++) {
        struct piece* p = &PC[i];
        struct iitem* it = &IT[p->item];
        if (p->kind == P_OPEN || p->kind == P_CLOSE) { px += p->w; cur_item = -1; continue; }
        if (p->trim) continue;
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
        px += adv;
    }
    (void)frag_first;
    (void)end;
    // open-box state after this line = state at line start + this line's
    // opens/closes
    for (int i = 0; i < n; i++) {
        if (PC[i].kind == P_OPEN && c->nopen < 128) c->open[c->nopen++] = IT[PC[i].item].box;
        else if (PC[i].kind == P_CLOSE && c->nopen) c->nopen--;
    }
    // note: boxes open before this line stay open unless closed above
    if (c->lines == 0) c->baseline_first = base;
    c->baseline_last = base;
    c->lines++;
    L->nlines++;
    c->y += lh;
    c->first_line = 0;
    line_bounds(c);
}

// Fix: pieces store the open-box state at line start. finish_line rebuilds
// the continuing-open list from the line's own pieces, so boxes opened on
// earlier lines must be re-seeded. We keep them in c->open before the call.

static void break_at(struct ifc* c, int k, int forced) {
    // pieces [0..k] form the line; the rest carries over
    int carry = npc - (k + 1);
    finish_line(c, k + 1, forced);
    // move the carried pieces to the front
    for (int i = 0; i < carry; i++) PC[i] = PC[k + 1 + i];
    npc = carry;
    // drop leading collapsible spaces on the new line
    while (npc > 0 && PC[0].kind == P_SPACE) {
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
    int is_space = np->kind == P_SPACE;
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

void lay_ifc(struct wlayout* L, int bi, int32_t content_w, int32_t* height) {
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
            if (wrap) mark_break_before(&c);
            pc.brk = (uint8_t)wrap;
            add_piece(&c, &pc, wrap);
            break;
        }
        case IT_BR:
            finish_line(&c, npc, 2);
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
            ab->sx = c.abs_left + c.line_x0 + c.used;
            ab->sy = c.abs_top + c.y;
            lay_abs_register(L, it->box);
            break;
        }
        }
    }
    if (npc > 0) finish_line(&c, npc, 1);
    rb = &L->b[bi];
    rb->fr0 = fr0;
    rb->nfr = L->nfr - fr0;
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
                if (t.space && wrap) {
                    if (word > best_word) best_word = word;
                    word = 0;
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
            break;
        }
        case IT_BR:
            if (line > best_line) best_line = line;
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
        }
    }
    if (line > best_line) best_line = line;
    if (word > best_word) best_word = word;
    w_free(items);
    // processed text from this measurement pass is scratch: drop it
    L->text.len = (int)text_mark;
    best_line += floats;
    *mn = best_word;
    *mx = best_line > best_word ? best_line : best_word;
}

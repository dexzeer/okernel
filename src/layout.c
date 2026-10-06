#include "layout.h"
#include "graphics.h"   // CHAR_W / CHAR_H
#include "cjk.h"        // LAYOUT_FLAG_CJK width helpers (inline, no link dep)
#include <string.h>

static int has_sub(const char* hay, const char* needle) {
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] == needle[k]) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Layout engine. Everything is expressed in grid cells; CSS px convert with
// CHAR_W/CHAR_H so relative sizes survive the cell quantisation.
// ---------------------------------------------------------------------------

#define MAX_ILIST 8          // inline nesting: context stack
#define MAX_LSTACK 1200      // block stack depth guard
#define MAX_INLINE_RUN 64    // consecutive inline siblings per anonymous box

struct lctx {
    const struct dom* d;
    const struct css_rule* rules;
    int n_rules;
    struct layout* o;
    int width;       // content width (cols)
    int page_left;
    uint32_t page_bg, page_fg;
    int oof_depth;   // out-of-flow placement nesting guard (no recursion)
    int oof_max;     // deepest absolute-box bottom: scrollable overflow that
                     // takes no space in flow (applied to height at the end)
};

static int px_cols(int px) { return (px + CHAR_W - 1) / CHAR_W; }
static int px_rows(int px) { return (px + CHAR_H - 1) / CHAR_H; }

// ---------------------------------------------------------------------------
// Emitters
// ---------------------------------------------------------------------------

static int new_item(struct lctx* c) {
    if (c->o->n_items >= LAYOUT_MAX_ITEMS) { c->o->truncated = 1; return -1; }
    struct layout_item* it = &c->o->items[c->o->n_items++];
    memset(it, 0, sizeof(*it));
    it->kind = LOUT_LINE;
    it->height = 1;
    return c->o->n_items - 1;
}

static void item_add_run(struct lctx* c, int item, const struct layout_run* src) {
    if (c->o->n_runs >= LAYOUT_MAX_RUNS) { c->o->truncated = 1; return; }
    int ri = c->o->n_runs++;
    c->o->runs[ri] = *src;
    if (c->o->items[item].run_count == 0)
        c->o->items[item].run_start = (uint16_t)ri;
    c->o->items[item].run_count++;
}

// Copy with ASCII case transform (text-transform). Non-ASCII bytes pass
// through untouched; capitalize uppercases the first alpha after a space.
static int text_put_tt(struct lctx* c, const char* s, int len, int mode) {
    if (len < 0) len = 0;
    if (c->o->text_len + len >= LAYOUT_MAX_TEXT) {
        len = LAYOUT_MAX_TEXT - c->o->text_len - 1;
        c->o->truncated = 1;
        if (len < 0) len = 0;
    }
    int off = c->o->text_len;
    int cap = 1; // capitalize state: next alpha starts a word
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (mode == 1 && ch >= 'a' && ch <= 'z') ch -= 32;
        else if (mode == 2 && ch >= 'A' && ch <= 'Z') ch += 32;
        else if (mode == 3) {
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') cap = 1;
            else if (cap && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
                if (ch >= 'a' && ch <= 'z') ch -= 32;
                cap = 0;
            } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) cap = 0;
        }
        c->o->text[off + i] = (char)ch;
    }
    c->o->text[off + len] = 0;
    c->o->text_len += len;
    return off;
}

static int text_put(struct lctx* c, const char* s, int len) {
    if (len < 0) len = 0;
    if (c->o->text_len + len >= LAYOUT_MAX_TEXT) {
        len = LAYOUT_MAX_TEXT - c->o->text_len - 1;
        c->o->truncated = 1;
        if (len < 0) len = 0;
    }
    int off = c->o->text_len;
    if (len > 0) memcpy(c->o->text + off, s, (size_t)len);
    c->o->text[off + len] = 0;
    c->o->text_len += len;
    return off;
}

// Build the ancestor context (nearest-first tag/class/id arrays) so CSS
// descendant/child combinators can be matched. Storage is caller-provided to
// keep this allocation-free.
#define STYLE_ANCESTOR_MAX 40
static struct css_style style_for_ctx(struct lctx* c, int node,
                                      char (*atag)[24], int *atag_len,
                                      char (*acls)[64], char (*aid)[32],
                                      struct css_style* out) {
    char tag[32], cls[128], id[64], inl[192];
    dom_tag_copy(c->d, node, tag, sizeof(tag));
    dom_attr_get(c->d, node, "class", cls, sizeof(cls));
    dom_attr_get(c->d, node, "id", id, sizeof(id));
    dom_attr_get(c->d, node, "style", inl, sizeof(inl));
    // Presentational hint: the legacy `align` attribute (align=right/center)
    // overrides the UA default for cells/blocks (HN uses align="right" on the
    // rank column). Applied after CSS so author rules still win.
    char al[16];
    if (dom_attr_get(c->d, node, "align", al, sizeof(al)) >= 0 && al[0]) {
        // append to the inline style text (highest priority) if there is room
        int il = 0; while (inl[il]) il++;
        const char* add = 0;
        if (al[0] == 'c') add = ";text-align:center";
        else if (al[0] == 'r') add = ";text-align:right";
        else if (al[0] == 'l') add = ";text-align:left";
        if (add && il < 180) {
            int k = 0;
            while (add[k] && il < 190) inl[il++] = add[k++];
            inl[il] = 0;
        }
    }

    const char* at[STYLE_ANCESTOR_MAX + 1];
    const char* ac[STYLE_ANCESTOR_MAX + 1];
    const char* ai[STYLE_ANCESTOR_MAX + 1];
    int na = 0;
    for (int p = c->d->nodes[node].parent; p != DOM_NONE && na < STYLE_ANCESTOR_MAX;
         p = c->d->nodes[p].parent) {
        int len = 0;
        // tag
        int tl = c->d->nodes[p].tag_len;
        if (tl > 23) tl = 23;
        for (int k = 0; k < tl; k++) { atag[na][k] = c->d->names[c->d->nodes[p].tag_off + k]; }
        atag[na][tl] = 0;
        (void)len;
        dom_attr_get(c->d, p, "class", acls[na], 64);
        dom_attr_get(c->d, p, "id", aid[na], 32);
        at[na] = atag[na]; ac[na] = acls[na]; ai[na] = aid[na];
        na++;
    }
    at[na] = 0; ac[na] = 0; ai[na] = 0;
    *atag_len = na;

    struct css_ctx ctx;
    ctx.tag = tag; ctx.cls = cls; ctx.id = id;
    ctx.ancestors_tag = at; ctx.ancestors_cls = ac; ctx.ancestors_id = ai;
    ctx.n_ancestors = na;
    // Viewport width in px for @media evaluation (min-width desktop layouts).
    ctx.viewport_px = (c->width + 2 * c->page_left) * CHAR_W;
    css_compute_ctx(c->rules, c->n_rules, &ctx, inl, out);
    return *out;
}

static struct css_style style_for(struct lctx* c, int node) {
    static char atag[STYLE_ANCESTOR_MAX][24];
    static char acls[STYLE_ANCESTOR_MAX][64];
    static char aid[STYLE_ANCESTOR_MAX][32];
    int na = 0;
    struct css_style st;
    style_for_ctx(c, node, atag, &na, acls, aid, &st);
    return st;
}

// ---------------------------------------------------------------------------
// Inline word collection
// ---------------------------------------------------------------------------

#define IW_TEXT 0
#define IW_LINK 1
#define IW_BR   2
#define IW_SPACE 3   // explicit separator (text " ", one cell wide)
#define IW_FIELD 4   // form control ([value]/[button]): lays out like text,
// paints like text, but flags the run clickable (is_field)

struct iword {
    int type;
    uint16_t off, len;
    uint32_t fg, bg;
    uint8_t flags;
    uint16_t node;
};

struct ictx {
    struct lctx* c;
    struct iword* words;
    int nwords, cap;
    uint32_t fg, bg;
    uint8_t flags;
    int link;              // enclosing <a> node, -1 = none
    int field;             // enclosing form control node, -1 = none
    int in_pre;
    int tt_mode;           // text-transform for pushed words (0=none, reset per element)
    int ws_nowrap;         // white-space:nowrap/pre inherited through the box
};

static void iw_push(struct ictx* ic, int type, const char* text, int len, int node) {
    if (ic->nwords >= ic->cap) { ic->c->o->truncated = 1; return; }
    struct iword* w = &ic->words[ic->nwords++];
    memset(w, 0, sizeof(*w));
    // Text words inside an <a> are link content; the run carries the <a> node
    // so the renderer can resolve the href and underline the text. Separators
    // stay IW_SPACE (the line builder re-emits them with the next word's
    // style, so link spaces still underline) — converting them would erase
    // every space inside a link.
    if (type == IW_TEXT && ic->field >= 0) {
        type = IW_FIELD;
        node = ic->field;
    } else if (type == IW_TEXT && ic->link >= 0) {
        type = IW_LINK;
        node = ic->link;
    }
    w->type = type;
    w->fg = ic->fg;
    w->bg = ic->bg;
    w->flags = ic->flags;
    if (ic->ws_nowrap) w->flags |= LAYOUT_FLAG_WS;
    w->node = (uint16_t)node;
    if (len > 0 && text) {
        if ((type == IW_TEXT || type == IW_FIELD) && ic->tt_mode)
            w->off = (uint16_t)text_put_tt(ic->c, text, len, ic->tt_mode);
        else
            w->off = (uint16_t)text_put(ic->c, text, len);
        w->len = (uint16_t)len;
        // CJK flag: the word holds raw UTF-8 (decoder passthrough — the slot
        // table is full). text_put_tt passes non-ASCII bytes through untouched
        // (only a-z/A-Z transform), so no transform exemption is needed; the
        // blit decodes per char and draws from the CJK bitmap table.
        if (type == IW_TEXT || type == IW_FIELD) {
            for (int qi = 0; qi < len;) {
                unsigned char qb = (unsigned char)text[qi];
                int qL = utf8_seq_len(qb);
                if (qL <= 1) { qi++; continue; }
                int ok = (qi + qL <= len);
                for (int q = 1; ok && q < qL; q++)
                    if (((unsigned char)text[qi + q] & 0xC0) != 0x80) ok = 0;
                if (ok) { w->flags |= LAYOUT_FLAG_CJK; break; }
                qi++;
            }
        }
    }
}

// Split text into words + separator markers. `pre` preserves runs of spaces.
static void iw_words(struct ictx* ic, const char* t, int len, int pre) {
    int i = 0;
    while (i < len) {
        unsigned char ch = (unsigned char)t[i];
        // Block slots 0x01/0x02 (KAnarchy logo) are word chars, not
        // separators — otherwise web block-art collapses to spaces.
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
            (ch < 32 && ch != 1 && ch != 2)) {
            int s = i;
            if (pre) {
                while (i < len && ((unsigned char)t[i] == ' ' || (unsigned char)t[i] == '\t')) i++;
                if (i > s) iw_push(ic, IW_SPACE, t + s, i - s, -1);
                else i++;
            } else {
                while (i < len && ((unsigned char)t[i] == ' ' || (unsigned char)t[i] == '\t' ||
                                   (unsigned char)t[i] == '\n' || (unsigned char)t[i] == '\r')) i++;
                iw_push(ic, IW_SPACE, 0, 0, -1);
            }
            continue;
        }
        int s = i;
        while (i < len) {
            unsigned char c2 = (unsigned char)t[i];
            if (c2 == ' ' || c2 == '\t' || c2 == '\n' || c2 == '\r' ||
                (c2 < 32 && c2 != 1 && c2 != 2)) break;
            i++;
        }
        iw_push(ic, IW_TEXT, t + s, i - s, -1);
    }
}

static void collect_inline(struct ictx* ic, int node);

static void collect_element(struct ictx* ic, int node) {
    struct lctx* c = ic->c;
    const struct dom* d = c->d;
    struct css_style st = style_for(c, node);
    if (st.has_display && st.display == CSS_DISPLAY_NONE) return;

    uint32_t sfg = ic->fg, sbg = ic->bg;
    uint8_t sfl = ic->flags;
    int slink = ic->link;    int stt = ic->tt_mode, sws = ic->ws_nowrap;
    int sfield = ic->field;

    if (st.has_fg_rgb) ic->fg = st.fg_rgb;
    if (st.has_bg_rgb) ic->bg = st.bg_rgb;
    if ((st.has_bold && st.bold) || dom_tag_is(d, node, "b") ||
        dom_tag_is(d, node, "strong"))
        ic->flags |= LAYOUT_FLAG_BOLD;

    if (dom_tag_is(d, node, "a")) {
        char href[192];
        int hl = dom_attr_get(d, node, "href", href, sizeof(href));
        if (hl > 0 && href[0]) {
            ic->link = node;
            ic->flags |= LAYOUT_FLAG_UL;
            if (!st.has_fg_rgb) ic->fg = 0x0000EE;
        }
    }

    // text-transform is NOT inherited: each element resets to its own rule
    // (or none). white-space/visibility DO inherit: keep parent state unless
    // the element specifies its own.
    ic->tt_mode = st.has_tt ? st.tt_mode : 0;
    if (st.has_ws) {
        ic->ws_nowrap = (st.ws_mode >= 1) ? 1 : 0;
        if (st.ws_mode == 2) ic->in_pre = 1;
    }
    if (st.has_vis && st.vis_hide) ic->flags |= LAYOUT_FLAG_HIDE;

    if (dom_tag_is(d, node, "input") || dom_tag_is(d, node, "button")) {
        int is_btn = dom_tag_is(d, node, "button");
        (void)is_btn;
        int is_hidden = 0;
        char itype[16] = {0};
        dom_attr_get(d, node, "type", itype, sizeof(itype));
        if (itype[0] == 'h' && itype[1] == 'i') is_hidden = 1;
                if (is_hidden) { ic->fg = sfg; ic->bg = sbg; ic->flags = sfl; ic->link = slink; ic->field = sfield; ic->tt_mode = stt; ic->ws_nowrap = sws; return; }
        if (!is_btn) {
            char val[160], ph[64];
            dom_attr_get(d, node, "value", val, sizeof(val));
            dom_attr_get(d, node, "placeholder", ph, sizeof(ph));
            const char* shown = val[0] ? val : ph;
            char buf[48]; int n = 0;
            buf[n++] = '[';
            for (int k = 0; shown[k] && n < 34; k++) buf[n++] = shown[k];
            for (; n < 34; n++) buf[n] = ' ';
            buf[n++] = ']'; buf[n] = 0;
            uint32_t saved_fg = ic->fg;
            ic->fg = 0x222222;
            iw_push(ic, IW_FIELD, buf, n, node);
            if (ic->nwords > 0) ic->words[ic->nwords - 1].flags |= LAYOUT_FLAG_UL;
            ic->fg = saved_fg;
            ic->fg = sfg; ic->bg = sbg; ic->flags = sfl; ic->link = slink; ic->field = sfield; ic->tt_mode = stt; ic->ws_nowrap = sws;
            return;
        }
        // <button>: render its children then wrap the label in brackets is
        // overkill — emit "[" + text + "]" by collecting children normally.
        // Everything (brackets and label) belongs to the control: mark the
        // span so clicks anywhere on it hit the button, not the text.
        iw_push(ic, IW_FIELD, "[", 1, node);
        ic->field = node;
        for (int ch = d->nodes[node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib)
            collect_inline(ic, ch);
        ic->field = sfield;
        iw_push(ic, IW_FIELD, "]", 1, node);
        ic->fg = sfg; ic->bg = sbg; ic->flags = sfl; ic->link = slink; ic->field = sfield; ic->tt_mode = stt; ic->ws_nowrap = sws;
        return;
    }

    if (dom_tag_is(d, node, "img")) {
        char alt[128] = {0};
        dom_attr_get(d, node, "alt", alt, sizeof(alt));
        if (alt[0]) {
            iw_push(ic, IW_TEXT, "[", 1, node);
            iw_push(ic, IW_TEXT, alt, (int)strlen(alt), node);
            iw_push(ic, IW_TEXT, "]", 1, node);
        }
        ic->fg = sfg; ic->bg = sbg; ic->flags = sfl; ic->link = slink; ic->field = sfield; ic->tt_mode = stt; ic->ws_nowrap = sws;
        return;
    }

    for (int ch = d->nodes[node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib)
        collect_inline(ic, ch);

    ic->fg = sfg;
    ic->bg = sbg;
    ic->flags = sfl;
    ic->link = slink; ic->field = sfield; ic->tt_mode = stt; ic->ws_nowrap = sws;
}

static int is_chrome(const struct dom* d, int node); // forward

static void collect_inline(struct ictx* ic, int node) {
    const struct dom* d = ic->c->d;
    if (node < 0 || node >= d->node_count) return;
    if (d->nodes[node].type == DOM_NODE_ELEMENT && is_chrome(d, node)) return;
    if (d->nodes[node].type == DOM_NODE_TEXT) {
        int len = 0;
        const char* s = dom_text(d, node, &len);
        if (s && len > 0) iw_words(ic, s, len, ic->in_pre);
        return;
    }
    if (d->nodes[node].type != DOM_NODE_ELEMENT) return;
    if (dom_tag_is(d, node, "br")) { iw_push(ic, IW_BR, 0, 0, -1); return; }
    if (dom_tag_is(d, node, "script") || dom_tag_is(d, node, "style") ||
        dom_tag_is(d, node, "head") || dom_tag_is(d, node, "title"))
        return;
    if (dom_is_block_node(d, node)) return;
    collect_element(ic, node);
}

// ---------------------------------------------------------------------------
// Line builder — places words into wrapped lines with per-run styles.
// ---------------------------------------------------------------------------

struct lbuild {
    struct lctx* c;
    struct ictx ic;
    int item;
    int col, start_col, max_col;
    int align;
    int line_rows;         // document rows consumed per line (1 body, scale heading)
    int link;              // currently open link node (-1 = none)
    int link_row, link_l0; // hit region accumulation (renderer uses runs)
};

static void lb_flush(struct lbuild* b) {
    if (b->item < 0) return;
    struct layout_item* it = &b->c->o->items[b->item];
    if (it->run_count == 0) { b->item = -1; return; }
    if (b->c->o->truncated) { b->item = -1; return; }
    int first = it->run_start, last = it->run_start + it->run_count - 1;
    int minc = b->c->o->runs[first].col;
    int maxc = minc;
    for (int i = first; i <= last; i++) {
        int e = b->c->o->runs[i].col + b->c->o->runs[i].text_len;
        if (e > maxc) maxc = e;
    }
    int used = maxc - minc;
    int shift = 0;
    // Alignment is relative to THIS line's content box (start_col..max_col),
    // not the page: otherwise `body{text-align:center}` re-centers every
    // table cell / list item to the page and destroys column offsets.
    int region_l = b->start_col;
    int region_w = b->max_col - b->start_col;
    if (region_w < 1) region_w = 1;
    if (b->align == CSS_ALIGN_CENTER) shift = (region_w - used) / 2 - (minc - region_l);
    else if (b->align == CSS_ALIGN_RIGHT) shift = (region_w - used) - (minc - region_l);
    if (shift < 0) shift = 0;
    if (maxc + shift > b->max_col) shift = b->max_col - maxc;
    if (shift > 0)
        for (int i = first; i <= last; i++) b->c->o->runs[i].col += (uint16_t)shift;
    b->item = -1;
}

static void lb_new_line(struct lbuild* b) {
    lb_flush(b);
    int idx = new_item(b->c);
    if (idx < 0) return;
    struct layout_item* it = &b->c->o->items[idx];
    it->kind = LOUT_LINE;
    it->row = (uint16_t)b->c->o->height;
    it->height = (uint16_t)(b->line_rows > 0 ? b->line_rows : 1);
    b->c->o->height += it->height;
    b->item = idx;
    b->col = b->start_col;
}

static void lb_add_run(struct lbuild* b, const struct iword* w, int off, int len, int col) {
    if (b->item < 0) return;
    struct layout_run r;
    memset(&r, 0, sizeof(r));
    r.text_off = (uint32_t)off;
    r.text_len = (uint16_t)len;
    r.col = (uint16_t)col;
    r.fg = w->fg;
    r.bg = w->bg;
    r.flags = w->flags & (uint8_t)~LAYOUT_FLAG_WS; // WS never reaches paint
    r.node = w->node;
    if (w->type == IW_LINK) r.is_link = 1;
    if (w->type == IW_FIELD) r.is_field = 1;
    item_add_run(b->c, b->item, &r);
}

static void lb_place(struct lbuild* b, const struct iword* w) {
    if (w->type == IW_SPACE) {
        // A separator is only meaningful between words on the same line.
        return;
    }
    // Display width in COLUMNS: bytes, except CJK runs whose UTF-8 chars
    // occupy one cell each (a 3-byte ideograph is 1 col, not 3).
    int cjk = (w->flags & LAYOUT_FLAG_CJK) != 0;
    const char* wtext = b->c->o->text + w->off;
    int width = cjk ? utf8_count_chars(wtext, w->len) : w->len;
    // Separator insertion between words: we know a space preceded if the
    // previous placed word is followed by an IW_SPACE. Track via flag.
    // (Handled by caller passing a space width.)
    int nowrap = (w->flags & LAYOUT_FLAG_WS) != 0;
    if (!nowrap && b->col > b->start_col && b->col + width > b->max_col) lb_new_line(b);
    if (!nowrap && width > b->max_col - b->start_col) {
        int off = w->off, rem = width, boff = 0;
        while (rem > 0) {
            if (b->c->o->truncated) break; // out of items/runs: stop cleanly
            if (b->item < 0) lb_new_line(b);
            if (b->item < 0) break;
            int room = b->max_col - b->col;
            if (room <= 0) { lb_new_line(b); continue; }
            int take = rem < room ? rem : room;
            int blen = take;
            if (cjk) {
                // Split at CHARACTER boundaries (never mid-UTF-8); the run
                // keeps the byte span while text_len counts display columns.
                blen = utf8_bytes_of_first_n(wtext + boff, w->len - boff, take);
                if (blen <= 0) break; // defensive: no progress possible
            }
            lb_add_run(b, w, off + boff, take, b->col);
            b->col += take; boff += blen; rem -= take;
            if (rem > 0) lb_new_line(b);
        }
    } else {
        lb_add_run(b, w, w->off, width, b->col);
        b->col += width;
    }
}

static void lb_build(struct lbuild* b) {
    int pending_space = 0;
    for (int i = 0; i < b->ic.nwords; i++) {
        struct iword* w = &b->ic.words[i];
        if (w->type == IW_BR) {
            lb_flush(b);
            b->col = b->start_col;
            b->c->o->height++; // consume the break row
            b->item = -1;
            pending_space = 0;
            continue;
        }
        if (w->type == IW_SPACE) { pending_space = 1; continue; }
        // Wrap decisions count display COLUMNS: for CJK runs that's chars,
        // not bytes (a 3-byte ideograph occupies one cell).
        int wcols = w->len;
        if ((w->flags & LAYOUT_FLAG_CJK) != 0)
            wcols = utf8_count_chars(b->c->o->text + w->off, w->len);
        if (pending_space && b->item >= 0 && b->col > b->start_col) {
            // insert one separator cell before the word, wrapping if needed
            // (nowrap words absorb the space and overflow instead).
            if (!((w->flags & LAYOUT_FLAG_WS)) &&
                b->col + 1 + wcols > b->max_col && wcols <= b->max_col - b->start_col) {
                lb_new_line(b);
            } else if (b->col + 1 <= b->max_col || (w->flags & LAYOUT_FLAG_WS)) {
                struct iword sp;
                memset(&sp, 0, sizeof(sp));
                sp.type = IW_TEXT;
                sp.fg = w->fg; sp.bg = w->bg; sp.node = w->node;
                // A separator space is never CJK content, even when it
                // precedes a CJK word: inheriting the flag made the blit
                // decode ' ' as char 0, miss the table, and paint '?'.
                sp.flags = w->flags & (uint8_t)~LAYOUT_FLAG_CJK;
                static const char ksp = ' ';
                sp.off = (uint16_t)text_put(b->c, &ksp, 1);
                sp.len = 1;
                lb_add_run(b, &sp, sp.off, 1, b->col);
                b->col++;
            }
        }
        pending_space = 0;
        lb_place(b, w);
    }
    lb_flush(b);
}

// ---------------------------------------------------------------------------
// Block walk
// ---------------------------------------------------------------------------

struct list_counter { int ordered; int num; };

static void layout_block(struct lctx* c, int node, int content_left, int content_w,
                         struct list_counter* lc);

// Paint a block's own background/border box behind its children's rows.
static void emit_box(struct lctx* c, const struct css_style* st, int top, int bottom,
                     int left, int w) {
    int bw = (st->has_bw && st->border_width > 0) ? px_cols(st->border_width) : 0;
    if (!st->has_bg_rgb && bw == 0) return;
    if (bottom <= top) return;
    int idx = new_item(c);
    if (idx < 0) return;
    struct layout_item* it = &c->o->items[idx];
    it->kind = LOUT_BAND;
    it->row = (uint16_t)top;
    it->height = (uint16_t)(bottom - top);
    it->box_left = (uint16_t)left;
    it->box_width = (uint16_t)(w < 1 ? 1 : w);
    it->box_bg = st->has_bg_rgb ? st->bg_rgb : c->page_bg;
    it->box_border = st->has_bc ? st->border_color : 0;
    it->draw_box = 1;
    if (st->has_bg_rgb && st->bg_grad >= 2) {
        it->draw_box = (uint8_t)st->bg_grad; // 2=vertical 3=horizontal gradient
        it->box_c1 = st->bg_c1;
    }
}

// Emit inline content of a block that is NOT a list item / pre / hr / table.
static void layout_inline(struct lctx* c, int node, const struct css_style* st,
                          int content_left, int content_w) {
    // Static, not stack: 2600 words x 20B = 52KB, which blows the 256KB
    // kernel stack once blocks nest 4-5 deep (each live layout_children
    // frame would add another 52KB). Safe because the array is dead across
    // nested layout calls: words are collected (inline elements only, never
    // layout_block) and fully built into items/runs before this function
    // returns or recurses into a block sibling.
    static struct iword words[LAYOUT_MAX_WORDS];
    struct ictx ic;
    memset(&ic, 0, sizeof(ic));
    ic.c = c;
    ic.words = words;
    ic.cap = LAYOUT_MAX_WORDS;
    ic.fg = c->page_fg;
    ic.bg = c->page_bg;
    ic.flags = 0;
    ic.link = -1;
    ic.field = -1;
    if (st->has_fg_rgb) ic.fg = st->fg_rgb;
    if (st->has_bg_rgb) ic.bg = st->bg_rgb;
    if (st->has_bold && st->bold) ic.flags |= LAYOUT_FLAG_BOLD;
    // Box-level text style: the block's own words (direct text children)
    // need it; nested elements re-derive it in collect_element.
    ic.tt_mode = st->has_tt ? st->tt_mode : 0;
    if (st->has_ws) {
        ic.ws_nowrap = (st->ws_mode >= 1) ? 1 : 0;
        if (st->ws_mode == 2) ic.in_pre = 1;
    }
    if (st->has_vis && st->vis_hide) ic.flags |= LAYOUT_FLAG_HIDE;

    for (int ch = c->d->nodes[node].first_child; ch != DOM_NONE; ch = c->d->nodes[ch].next_sib)
        collect_inline(&ic, ch);

    struct lbuild b;
    memset(&b, 0, sizeof(b));
    b.c = c;
    b.ic = ic;
    b.align = st->has_align ? st->align : CSS_ALIGN_LEFT;
    b.start_col = content_left;
    b.max_col = content_left + content_w;
    b.col = content_left;
    b.item = -1;
    lb_new_line(&b);
    lb_build(&b);
}

static void layout_pre(struct lctx* c, int node, const struct css_style* st,
                       int content_left, int content_w) {
    // Static, not stack: see layout_inline (52KB would blow the 256KB
    // kernel stack under nesting; pre content never nests a block while
    // the array is live).
    static struct iword words[LAYOUT_MAX_WORDS];
    struct ictx ic;
    memset(&ic, 0, sizeof(ic));
    ic.c = c;
    ic.words = words;
    ic.cap = LAYOUT_MAX_WORDS;
    ic.fg = st->has_fg_rgb ? st->fg_rgb : c->page_fg;
    ic.bg = st->has_bg_rgb ? st->bg_rgb : c->page_bg;
    ic.in_pre = 1;
    ic.link = -1;
    ic.field = -1;
    ic.tt_mode = st->has_tt ? st->tt_mode : 0;
    if (st->has_vis && st->vis_hide) ic.flags |= LAYOUT_FLAG_HIDE;
    for (int ch = c->d->nodes[node].first_child; ch != DOM_NONE; ch = c->d->nodes[ch].next_sib) {
        int t = c->d->nodes[ch].type;
        if (t == DOM_NODE_TEXT) {
            int len = 0;
            const char* s = dom_text(c->d, ch, &len);
            if (s && len > 0) {
                int i = 0;
                while (i < len) {
                    int s2 = i;
                    while (i < len && s[i] != '\n') i++;
                    if (i > s2) iw_push(&ic, IW_TEXT, s + s2, i - s2, -1);
                    if (i < len) { iw_push(&ic, IW_BR, 0, 0, -1); i++; }
                }
            }
        }
    }
    struct lbuild b;
    memset(&b, 0, sizeof(b));
    b.c = c;
    b.ic = ic;
    b.start_col = content_left;
    b.max_col = content_left + content_w;
    b.col = content_left;
    b.item = -1;
    lb_new_line(&b);
    // Place without wrapping (pre): emit raw runs, break on IW_BR.
    for (int i = 0; i < ic.nwords; i++) {
        struct iword* w = &ic.words[i];
        if (w->type == IW_BR) {
            lb_flush(&b);
            b.col = b.start_col;
            c->o->height++;
            b.item = -1;
            continue;
        }
        if (b.item < 0) lb_new_line(&b);
        lb_add_run(&b, w, w->off, w->len, b.col);
        b.col += w->len;
    }
    lb_flush(&b);
}

static void layout_hr(struct lctx* c, const struct css_style* st, int content_left,
                      int content_w) {
    struct layout_run r;
    memset(&r, 0, sizeof(r));
    static char rule[256];
    int n = content_w < 256 ? content_w : 256;
    memset(rule, (char)0xCE, (size_t)n);   // ─ line
    int off = text_put(c, rule, n);
    int idx = new_item(c);
    if (idx < 0) return;
    struct layout_item* it = &c->o->items[idx];
    it->row = (uint16_t)c->o->height;
    it->height = 1;
    c->o->height++;
    r.text_off = (uint32_t)off;
    r.text_len = (uint16_t)n;
    r.col = (uint16_t)content_left;
    r.fg = st->has_fg_rgb ? st->fg_rgb : c->page_fg;
    r.bg = st->has_bg_rgb ? st->bg_rgb : c->page_bg;
    item_add_run(c, idx, &r);
}

// Does this element contain a block-level child (directly)? A container that
// does is laid out as a block with anonymous inline boxes between its block
// children — this is what makes `<center><table>` / `<div><div>` / custom
// wrappers behave like blocks even when their own tag is not in the block list.
static int has_block_child(const struct dom* d, int node) {
    for (int ch = d->nodes[node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib) {
        if (d->nodes[ch].type != DOM_NODE_ELEMENT) continue;
        if (dom_is_block_node(d, ch)) return 1;
    }
    return 0;
}

// Lay out the children of a block as anonymous inline boxes between block
// children (the CSS 2.1 model): consecutive inline-level children flow into
// one wrapped paragraph; each block-level child recurses on its own line.
static int node_is_inline_level(const struct dom* d, int node) {
    if (d->nodes[node].type == DOM_NODE_TEXT) return 1;
    if (d->nodes[node].type != DOM_NODE_ELEMENT) return 0;
    if (dom_is_block_node(d, node)) return 0;
    if (has_block_child(d, node)) return 0; // wrapper around blocks
    return 1;
}

// Style-aware variant used by the block walker: an element is block-level when
// its computed display says so (flex/grid/table/block), even on a tag that is
// inline by default.
static int node_is_inline_level_styled(struct lctx* c, int node) {
    if (c->d->nodes[node].type != DOM_NODE_ELEMENT) return 1;
    struct css_style st = style_for(c, node);
    if (st.has_display) {
        int dsp = st.display;
        if (dsp == CSS_DISPLAY_BLOCK || dsp == CSS_DISPLAY_FLEX ||
            dsp == CSS_DISPLAY_GRID || dsp == CSS_DISPLAY_TABLE ||
            dsp == CSS_DISPLAY_TABLE_ROW || dsp == CSS_DISPLAY_TABLE_CELL)
            return 0;
        if (dsp == CSS_DISPLAY_INLINE || dsp == CSS_DISPLAY_INLINE_BLOCK) return 1;
    }
    return node_is_inline_level(c->d, node);
}

static void layout_children(struct lctx* c, int node, int content_left, int content_w,
                            struct list_counter* lc, int heading_level) {
    const struct dom* d = c->d;
    int ch = d->nodes[node].first_child;
    while (ch != DOM_NONE) {
        if (d->nodes[ch].type == DOM_NODE_ELEMENT && is_chrome(d, ch)) {
            ch = d->nodes[ch].next_sib;
            continue;
        }
        if (node_is_inline_level_styled(c, ch)) {
            // Gather the run of consecutive inline-level siblings into an
            // anonymous inline container and lay it out as one paragraph.
            int inline_nodes[MAX_INLINE_RUN];
            // Static, not stack: see layout_inline (52KB would blow the
            // 256KB kernel stack under nesting; each inline run is fully
            // collected and built before the walk advances to siblings,
            // so the array is dead across nested layout_block calls).
            static struct iword words[LAYOUT_MAX_WORDS];
            struct ictx ic;
            memset(&ic, 0, sizeof(ic));
            ic.c = c;
            ic.words = words;
            ic.cap = LAYOUT_MAX_WORDS;
            ic.fg = c->page_fg;
            ic.bg = c->page_bg;
            ic.link = -1;
    ic.field = -1;
            // Seed from the OWNING block's style: its direct text children
            // (which never pass through collect_element) need the block's
            // colors/transform/visibility. Nested elements re-derive their
            // own state in collect_element (save/restore), so seeding here
            // only affects otherwise-unstyled text.
            {
                struct css_style bst = style_for(c, node);
                if (bst.has_fg_rgb) ic.fg = bst.fg_rgb;
                if (bst.has_bg_rgb) ic.bg = bst.bg_rgb;
                if (bst.has_bold && bst.bold) ic.flags |= LAYOUT_FLAG_BOLD;
                ic.tt_mode = bst.has_tt ? bst.tt_mode : 0;
                if (bst.has_ws) {
                    ic.ws_nowrap = (bst.ws_mode >= 1) ? 1 : 0;
                    if (bst.ws_mode == 2) ic.in_pre = 1;
                }
                if (bst.has_vis && bst.vis_hide) ic.flags |= LAYOUT_FLAG_HIDE;
            }
            int count = 0;
            while (ch != DOM_NONE && node_is_inline_level_styled(c, ch)) {
                if (count < MAX_INLINE_RUN) {
                    // Skip pure-whitespace text nodes and leading blanks
                    if (d->nodes[ch].type == DOM_NODE_TEXT) {
                        int len = 0;
                        const char* s = dom_text(c->d, ch, &len);
                        int ws = 1;
                        for (int i = 0; s && i < len; i++)
                            if ((unsigned char)s[i] > ' ') { ws = 0; break; }
                        if (ws && count == 0) { ch = d->nodes[ch].next_sib; continue; }
                    }
                    collect_inline(&ic, ch);
                    inline_nodes[count] = ch;
                    count++;
                }
                ch = d->nodes[ch].next_sib;
            }
            if (ic.nwords > 0) {
                int hlevel = heading_level;
                int scale = hlevel == 1 ? 3 : hlevel == 2 ? 2 : 1;
                int first_item = c->o->n_items;
                struct lbuild b;
                memset(&b, 0, sizeof(b));
                b.c = c;
                b.ic = ic;
                b.start_col = content_left;
                // Heading chars occupy `scale` columns each, so wrap at the
                // narrower character capacity (content_w / scale); each heading
                // line also consumes `scale` document rows. Body text uses the
                // full width and one row.
                b.max_col = content_left + (scale > 1 ? content_w / scale : content_w);
                b.line_rows = scale;
                b.col = content_left;
                b.item = -1;
                lb_new_line(&b);
                lb_build(&b);
                if (hlevel) {
                    for (int j = first_item; j < c->o->n_items; j++) {
                        struct layout_item* it = &c->o->items[j];
                        it->heading = (uint8_t)hlevel;
                        if (scale > 1) {
                            int ri0 = it->run_start;
                            for (int ri = ri0; ri < ri0 + it->run_count; ri++) {
                                struct layout_run* run = &c->o->runs[ri];
                                run->col = (uint16_t)(content_left +
                                    (run->col - content_left) * scale);
                            }
                        }
                    }
                }
            }
            continue;
        }
        // Block-level child.
        layout_block(c, ch, content_left, content_w, lc);
        ch = d->nodes[ch].next_sib;
    }
}

// --- Tables ---------------------------------------------------------------
// Faithful-enough table formatting: rows become horizontal bands, cells flow
// side-by-side at computed column offsets. A cell's content is laid out at its
// own column offset; rows advance by the tallest cell. col/rowspan are not
// modeled (a spanning cell just occupies its own column).

static int hexval_(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Forward decls (table code calls the block walker before its definition).
static void layout_block(struct lctx* c, int node, int content_left, int content_w,
                         struct list_counter* lc);
static void layout_children(struct lctx* c, int node, int content_left, int content_w,
                            struct list_counter* lc, int heading_level);

// Lay one cell's contents starting at `top` rows and `left` cols. Returns the
// cell's height in rows. `o->height` is restored by the caller.
static int layout_cell_contents(struct lctx* c, int node, int left, int w,
                                struct css_style* st, struct list_counter* lc,
                                int is_header) {
    int saved_h = c->o->height;
    int pt = st->has_pt ? px_rows(st->padding_top) : 0;
    int pb = st->has_pb ? px_rows(st->padding_bottom) : 0;
    for (int i = 0; i < pt; i++) c->o->height++;
    int inner_left = left, inner_w = w - 1; // 1 col for the cell separator
    if (inner_w < 2) inner_w = 2;
    inner_left = left + 1;
    layout_children(c, node, inner_left, inner_w, lc, 0);
    for (int i = 0; i < pb; i++) c->o->height++;
    int h = c->o->height - saved_h;
    c->o->height = saved_h;
    (void)is_header;
    return h;
}

// Compute per-cell widths for a row (cols). Fixed `width` attributes win;
// the rest share the remaining width, weighted by a rough content estimate so
// a long title cell gets more room than a "1." rank cell.
static void table_widths(struct lctx* c, int row_node, int avail_w,
                         int* widths, int* ncells) {
    const struct dom* d = c->d;
    int n = 0;
    int total_fixed = 0, total_weight = 0;
    int weight[64];
    for (int ch = d->nodes[row_node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib) {
        if (d->nodes[ch].type != DOM_NODE_ELEMENT) continue;
        if (!dom_tag_is(d, ch, "td") && !dom_tag_is(d, ch, "th")) continue;
        if (n >= 64) break;
        widths[n] = 0; weight[n] = 0;
        char wat[16];
        if (dom_attr_get(d, ch, "width", wat, sizeof(wat)) >= 0) {
            int v = 0, k = 0;
            while (wat[k] >= '0' && wat[k] <= '9') v = v * 10 + (wat[k++] - '0');
            if (v > 0 && (strchr(wat, '%') == 0)) widths[n] = v;      // px-ish
            else if (v > 0 && strchr(wat, '%')) widths[n] = avail_w * v / 100;
        }
        if (widths[n] > 0) {
            total_fixed += widths[n];
        } else {
            // weight = rough text length in the subtree (capped)
            int w2 = 0;
            int stack[32], sp = 0; stack[sp++] = ch;
            while (sp > 0 && w2 < 400) {
                int cur = stack[--sp];
                for (int cn = d->nodes[cur].first_child; cn != DOM_NONE; cn = d->nodes[cn].next_sib) {
                    if (d->nodes[cn].type == DOM_NODE_TEXT) {
                        w2 += d->nodes[cn].text_len;
                    } else if (d->nodes[cn].type == DOM_NODE_ELEMENT && sp < 32) {
                        stack[sp++] = cn;
                    }
                    if (w2 >= 400) break;
                }
            }
            weight[n] = w2 + 4; // +4 so empty cells still get a minimum
            total_weight += weight[n];
        }
        n++;
    }
    if (n == 0) { *ncells = 0; return; }
    int rem = avail_w - total_fixed;
    if (rem < n) rem = n;
    if (total_weight > 0) {
        int used = 0;
        for (int i = 0; i < n; i++) {
            if (widths[i] > 0) continue;
            int w = rem * weight[i] / total_weight;
            if (w < 2) w = 2;
            widths[i] = w;
            used += w;
        }
        // Correct rounding drift by shaving the widest auto cell.
        int slack = rem - used;
        if (slack != 0) {
            int widest = -1;
            for (int i = 0; i < n; i++) if (widths[i] > 2 && (widest < 0 || widths[i] > widths[widest])) widest = i;
            if (widest >= 0) { widths[widest] += slack; if (widths[widest] < 2) widths[widest] = 2; }
        }
    } else {
        int each = rem / n;
        for (int i = 0; i < n; i++) if (widths[i] == 0) widths[i] = each < 2 ? 2 : each;
    }
    *ncells = n;
}

// Emit background bands for cells (behind text). Called after the row's cells
// have been laid out so the band spans the real row height.
static void table_cell_bg(struct lctx* c, int node, int left, int w, int top, int h) {
    struct css_style st = style_for(c, node);
    char bgattr[16];
    uint32_t bg = 0;
    int has_bg = 0;
    if (st.has_bg_rgb) { bg = st.bg_rgb; has_bg = 1; }
    else if (dom_attr_get(c->d, node, "bgcolor", bgattr, sizeof(bgattr)) >= 0) {
        // named + #hex
        uint32_t col = 0;
        int ok = 0;
        if (bgattr[0] == '#' && strlen(bgattr) == 7) {
            for (int k = 1; k < 7; k++) {
                int hx = hexval_(bgattr[k]);
                if (hx < 0) { ok = 0; break; }
                col = (col << 4) | (uint32_t)hx; ok = 1;
            }
        }
        if (ok) { bg = col; has_bg = 1; }
    }
    if (!has_bg || h <= 0) return;
    int idx = new_item(c);
    if (idx < 0) return;
    struct layout_item* it = &c->o->items[idx];
    it->kind = LOUT_BAND;
    it->row = (uint16_t)top;
    it->height = (uint16_t)h;
    it->box_left = (uint16_t)left;
    it->box_width = (uint16_t)(w < 1 ? 1 : w);
    it->box_bg = bg;
    it->draw_box = 1;
}

// Lay out a table's rows. `content_left`/`content_w` are the table's box.
static void layout_table(struct lctx* c, int node, const struct css_style* st,
                         int content_left, int content_w) {
    const struct dom* d = c->d;
    int pt = st->has_pt ? px_rows(st->padding_top) : 0;
    int pb = st->has_pb ? px_rows(st->padding_bottom) : 0;
    int pl = st->has_pl ? px_cols(st->padding_left) : 0;
    for (int i = 0; i < pt; i++) c->o->height++;
    int table_left = content_left + pl;
    int table_w = content_w - pl;
    if (table_w < 4) table_w = 4;

    // Walk rows, descending through thead/tbody/tfoot.
    int stack[16], sp = 0;
    stack[sp++] = node;
    while (sp > 0) {
        int cur = stack[--sp];
        for (int ch = d->nodes[cur].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib) {
            if (d->nodes[ch].type != DOM_NODE_ELEMENT) continue;
            if (dom_tag_is(d, ch, "thead") || dom_tag_is(d, ch, "tbody") ||
                dom_tag_is(d, ch, "tfoot")) {
                if (sp < 16) stack[sp++] = ch;
                continue;
            }
            if (!dom_tag_is(d, ch, "tr")) {
                // A non-row child directly in a table: lay it as a block.
                layout_block(c, ch, table_left, table_w, 0);
                continue;
            }
            // One row: compute widths, lay cells side-by-side at row_top,
            // then advance by the tallest cell.
            int row_top = c->o->height;
            int widths[64], ncells = 0;
            table_widths(c, ch, table_w, widths, &ncells);
            int left = table_left;
            int maxh = 1;
            int i = 0;
            for (int cell = d->nodes[ch].first_child; cell != DOM_NONE;
                 cell = d->nodes[cell].next_sib) {
                if (d->nodes[cell].type != DOM_NODE_ELEMENT) continue;
                if (!dom_tag_is(d, cell, "td") && !dom_tag_is(d, cell, "th")) continue;
                if (i >= 64) break;
                int w = widths[i] < 1 ? 1 : widths[i];
                struct css_style cst = style_for(c, cell);
                c->o->height = row_top;
                int h = layout_cell_contents(c, cell, left, w, &cst, 0,
                                             dom_tag_is(d, cell, "th"));
                if (h > maxh) maxh = h;
                table_cell_bg(c, cell, left, w, row_top, h);
                left += w;
                i++;
            }
            c->o->height = row_top + maxh;
        }
    }
    for (int i = 0; i < pb; i++) c->o->height++;
}

// --- Flex / Grid containers ----------------------------------------------
// Minimal modern layout: flex rows lay children side-by-side; grid places
// children into parsed tracks (fixed px/rem or Nfr). This is what makes
// Wikipedia's Vector 2022 page container (grid-template-columns:15.5rem
// minmax(0,1fr)) put its sidebar in a narrow column instead of stacking it.

// Lay `node` as a block at (left,w) starting at document row `top`, returning
// its height without disturbing the running document height.
static int layout_block_at(struct lctx* c, int node, int left, int w, int top) {
    int save = c->o->height;
    c->o->height = top;
    layout_block(c, node, left, w, 0);
    int h = c->o->height - top;
    c->o->height = save;
    if (h < 1) h = 1;
    return h;
}

static int child_width_cols(struct lctx* c, int node, int avail) {
    struct css_style cs = style_for(c, node);
    int minw = 0;
    if (cs.has_minw && cs.min_width > 0) {
        minw = px_cols(cs.min_width);
        if (minw < 1) minw = 1;
        if (minw > avail) minw = avail;
    }
    if (cs.has_w && cs.width > 0) {
        int w = px_cols(cs.width);
        if (w >= 1 && w <= avail) {
            if (minw && w < minw) w = minw;
            return w;
        }
    }
    if (cs.has_wpct && cs.wpct > 0) {
        int w = avail * cs.wpct / 100;
        if (w >= 1) {
            if (minw && w < minw) w = minw;
            return w;
        }
    }
    if (minw) return minw; // min-width without explicit width floors the size
    return -1;
}

static int count_block_children(const struct dom* d, int node) {
    int n = 0;
    for (int ch = d->nodes[node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib)
        if (d->nodes[ch].type == DOM_NODE_ELEMENT) n++;
    return n;
}

// Flex row: children side-by-side, widths from CSS or equal share.
static void layout_flex(struct lctx* c, int node, const struct css_style* st,
                        int left, int w) {
    if (st->has_fdir && st->flex_dir == 1) {
        layout_children(c, node, left, w, 0, 0); // column: stack like block
        return;
    }
    const struct dom* d = c->d;
    int n = count_block_children(d, node);
    if (n == 0) { layout_children(c, node, left, w, 0, 0); return; }
    int top = c->o->height;
    int x = left, maxh = 0, i = 0, fixed = 0;
    for (int ch = d->nodes[node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib) {
        if (d->nodes[ch].type != DOM_NODE_ELEMENT) continue;
        int cw = child_width_cols(c, ch, w);
        if (cw > 0) fixed += cw;
    }
    int auto_n = n;
    for (int ch = d->nodes[node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib) {
        if (d->nodes[ch].type != DOM_NODE_ELEMENT) continue;
        int cw = child_width_cols(c, ch, w);
        if (cw <= 0) {
            int rem = w - fixed;
            if (rem < 1) rem = 1;
            cw = rem / (auto_n > 0 ? auto_n : 1);
            auto_n--;
            fixed += cw;
        }
        if (i == n - 1) cw = left + w - x; // last child takes the remainder
        if (cw < 1) cw = 1;
        int h = layout_block_at(c, ch, x, cw, top);
        if (h > maxh) maxh = h;
        x += cw;
        i++;
        if (x >= left + w) break;
    }
    c->o->height = top + maxh;
}

// Parse one grid track into a width in cols, or a fr weight (>0).
struct gtrack { int cols; int fr; };
static int parse_tracks(const char* v, struct gtrack* t, int max) {
    int n = 0;
    const char* s = v;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        if (strncmp(s, "repeat(", 7) == 0) {
            // repeat(N, <track>) — expand N copies of the inner track.
            const char* p = s + 7;
            int cnt = 0;
            while (*p >= '0' && *p <= '9') cnt = cnt * 10 + (*p++ - '0');
            while (*p == ' ') p++;
            if (*p == ',') p++;
            while (*p == ' ') p++;
            const char* inner = p;
            int depth = 1;
            while (*p && depth) { if (*p=='(') depth++; else if (*p==')') depth--; if (depth) p++; }
            char ibuf[48]; int il = 0;
            while (inner < p && il < 47) ibuf[il++] = *inner++;
            ibuf[il] = 0;
            if (cnt < 1) cnt = 1;
            if (cnt > 8) cnt = 8;
            for (int k = 0; k < cnt && n < max; k++) {
                struct gtrack tmp[1];
                parse_tracks(ibuf, tmp, 1);
                t[n++] = tmp[0];
            }
            s = (*p == ')') ? p + 1 : p;
            continue;
        }
        // read one token (respecting parens)
        const char* start = s;
        int depth = 0;
        while (*s && (depth || (*s != ' ' && *s != ','))) {
            if (*s == '(') depth++;
            else if (*s == ')') depth--;
            s++;
        }
        int tl = (int)(s - start);
        char buf[48]; if (tl > 47) tl = 47;
        for (int k = 0; k < tl; k++) buf[k] = start[k];
        buf[tl] = 0;
        // strip minmax( ... ) to its second arg (usually 1fr)
        char* inner = buf;
        if (strncmp(buf, "minmax(", 7) == 0) {
            char* comma = buf + 7;
            int d = 1; char* q = comma;
            while (*q && d) { if (*q=='(') d++; else if (*q==')') d--; if (d) q++; }
            *q = 0;
            while (*comma && *comma != ',') comma++;
            if (*comma == ',') comma++;
            inner = comma;
        }
        int fr = 0, cols = 0;
        if (has_sub(inner, "fr")) { fr = 1; }
        else if (has_sub(inner, "auto") || has_sub(inner, "content")) { fr = 1; }
        else {
            // numeric with optional unit; decimals in hundredths
            int whole = 0, seen = 0, frac = 0, fdiv = 1;
            const char* p = inner;
            while (*p >= '0' && *p <= '9') { whole = whole*10 + (*p++ - '0'); seen = 1; }
            if (*p == '.') {
                p++;
                while (*p >= '0' && *p <= '9') { frac = frac*10 + (*p++ - '0'); fdiv *= 10; }
            }
            int unit = 1;
            if (strncmp(p, "rem", 3) == 0) unit = 16;
            else if (strncmp(p, "em", 2) == 0) unit = 16;
            else if (strncmp(p, "ch", 2) == 0) unit = 8;
            else if (strncmp(p, "px", 2) == 0) unit = 1;
            else if (!seen) { fr = 1; }
            int px = whole * unit + (frac * unit) / fdiv;
            cols = px > 0 ? px_cols(px) : 0;
            if (cols > 200) cols = 200; // sanity: a track cannot exceed the page
        }
        t[n].fr = fr; t[n].cols = cols;
        n++;
    }
    return n;
}

static void layout_grid(struct lctx* c, int node, const struct css_style* st,
                        int left, int w) {
    const struct dom* d = c->d;
    struct gtrack t[16];
    int nt = parse_tracks(st->has_gcols ? st->grid_cols : "", t, 16);
    if (nt <= 0) { layout_children(c, node, left, w, 0, 0); return; }
    // Resolve track widths.
    int fixed = 0, nfr = 0;
    for (int i = 0; i < nt; i++) {
        if (t[i].fr > 0) nfr += t[i].fr;
        else fixed += t[i].cols;
    }
    int rem = w - fixed;
    if (rem < nfr) rem = nfr;
    int widths[16], x[16], cx = left;
    for (int i = 0; i < nt; i++) {
        widths[i] = t[i].fr > 0 ? (rem * t[i].fr / (nfr > 0 ? nfr : 1)) : t[i].cols;
        if (widths[i] < 1) widths[i] = 1;
        x[i] = cx;
        cx += widths[i];
    }
    // Place children row-major into the tracks.
    int idx = 0, top = c->o->height;
    int rowtop = top, rowmax = 0;
    for (int ch = d->nodes[node].first_child; ch != DOM_NONE; ch = d->nodes[ch].next_sib) {
        if (d->nodes[ch].type != DOM_NODE_ELEMENT) continue;
        int col = idx % nt;
        if (col == 0 && idx > 0) {
            top = rowtop + rowmax;
            rowtop = top; rowmax = 0;
        }
        int h = layout_block_at(c, ch, x[col], widths[col], rowtop);
        if (h > rowmax) rowmax = h;
        idx++;
    }
    c->o->height = rowtop + rowmax;
}

// A block-level element: margins, its own box, and its contents.
// Shift emitted items [n0, n_items): rows by dy, columns by dx, clamped at 0.
// Used for relative offsets (paint moves, layout untouched) and for snapping
// fixed boxes from their static spot to viewport rows.
static void oof_shift(struct lctx* c, int n0, int dx, int dy) {
    if (!dx && !dy) return;
    for (int i = n0; i < c->o->n_items; i++) {
        struct layout_item* it = &c->o->items[i];
        if (dy) {
            int r = (int)it->row + dy;
            it->row = (uint16_t)(r < 0 ? 0 : r);
        }
        if (dx) {
            if (it->kind == LOUT_BAND) {
                int b = (int)it->box_left + dx;
                it->box_left = (uint16_t)(b < 0 ? 0 : b);
            }
            for (int ri = it->run_start; ri < it->run_start + it->run_count; ri++) {
                int cc = (int)c->o->runs[ri].col + dx;
                c->o->runs[ri].col = (uint16_t)(cc < 0 ? 0 : cc);
            }
        }
    }
}

static void layout_block(struct lctx* c, int node, int content_left, int content_w,
                         struct list_counter* lc) {
    if (c->o->truncated) return;
    struct css_style st = style_for(c, node);
    if (st.has_display && st.display == CSS_DISPLAY_NONE) return;

    // Out-of-flow boxes (absolute/fixed): laid out via layout_block_at so the
    // flow cursor neither feeds nor advances them. Nested out-of-flow boxes
    // lay out statically (oof_depth guards the self-recursion: the inner
    // layout_block call must not re-enter this branch for the same node).
    // v1: containing block = parent content box (absolute) / viewport
    // (fixed); offsets are px only; right/bottom shift back from the static
    // spot (no containing-block edge without a tracked ancestor box).
    if (st.has_pos && (st.pos_mode == 2 || st.pos_mode == 3) && c->oof_depth == 0) {
        int save_h = c->o->height;
        int dy = 0, dx = 0;
        if (st.has_top) dy += px_rows(st.top);
        if (st.has_bottom) dy -= px_rows(st.bottom);
        if (st.has_left) dx += px_cols(st.left);
        if (st.has_right) dx -= px_cols(st.right);
        int base_left = (st.pos_mode == 3) ? c->page_left : content_left;
        int place_top = save_h + dy;
        int place_left = base_left + dx;
        if (place_top < 0) place_top = 0;
        if (place_left < 0) place_left = 0;
        int n0 = c->o->n_items;
        c->oof_depth++;
        int h = layout_block_at(c, node, place_left, content_w, place_top);
        c->oof_depth--;
        // Out-of-flow rows paint above normal flow (blit phase 1); fixed
        // rows additionally pin to viewport rows (no scroll offset).
        for (int i = n0; i < c->o->n_items; i++)
            c->o->items[i].is_oof = 1;
        if (st.pos_mode == 3) {
            // Viewport-pinned: rows become viewport rows (the blit maps them
            // without the scroll offset). An explicit top pins to that row;
            // otherwise the box keeps its static spot as a viewport row.
            if (st.has_top) {
                int vtop = px_rows(st.top);
                if (vtop < 0) vtop = 0;
                oof_shift(c, n0, 0, vtop - place_top);
            }
            for (int i = n0; i < c->o->n_items; i++)
                c->o->items[i].is_fixed = 1;
            c->o->height = save_h;
        } else {
            // Absolute: takes no space in flow — the cursor returns to
            // save_h so following siblings lay out as if the box were
            // absent. Its bottom feeds oof_max (scrollable overflow,
            // applied to the document height at the end of layout_run).
            int bottom = place_top + h;
            c->o->height = save_h;
            if (bottom > c->oof_max) c->oof_max = bottom;
        }
        return;
    }
    int n0 = c->o->n_items;

    int mtop = st.has_mt ? px_rows(st.margin_top) : 0;
    int mbot = st.has_mb ? px_rows(st.margin_bottom) : 0;
    int ml = st.has_ml ? px_cols(st.margin_left) : 0;
    int mr = st.has_mr ? px_cols(st.margin_right) : 0;

    c->o->height += mtop;

    int inner_left = content_left + ml;
    int inner_w = content_w - ml - mr;
    if (inner_w < 4) { inner_w = 4; inner_left = content_left; }

    int top = c->o->height;

    char tag[32];
    dom_tag_copy(c->d, node, tag, sizeof(tag));

    // Heading level for this block's anonymous inline box: h1-h6 -> 1..6,
    // everything else 0. Propagated to children so a heading's text lines get
    // the scaled pixel overlay.
    int heading_level = 0;
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0)
        heading_level = tag[1] - '0';

    if (st.has_display && st.display == CSS_DISPLAY_FLEX) {
        layout_flex(c, node, &st, inner_left, inner_w);
    } else if (st.has_display && st.display == CSS_DISPLAY_GRID) {
        layout_grid(c, node, &st, inner_left, inner_w);
    } else if (dom_tag_is(c->d, node, "table") ||
               (st.has_display && st.display == CSS_DISPLAY_TABLE)) {
        layout_table(c, node, &st, inner_left, inner_w);
    } else if (dom_tag_is(c->d, node, "hr")) {
        layout_hr(c, &st, inner_left, inner_w);
    } else if (dom_tag_is(c->d, node, "pre")) {
        layout_pre(c, node, &st, inner_left, inner_w);
    } else {
        // List items get a marker prefix; the marker run carries the same
        // node so a click inside the text still resolves.
        int depth = 0;
        for (int i = 0; tag[i]; i++) depth++;
        if (dom_tag_is(c->d, node, "li")) {
            // Marker on its own line prefix: emit as part of the first line.
            // list-style-type comes from the <li> itself or is inherited
            // from the enclosing <ol>/<ul> (the usual authoring spot).
            // Default: decimal for ordered, disc for unordered.
            struct layout_run r;
            memset(&r, 0, sizeof(r));
            char mk[8]; int mn = 0;
            int lst = st.has_lst ? st.lst_type : 0;
            if (!st.has_lst) {
                int p = c->d->nodes[node].parent;
                if (p >= 0 && (dom_tag_is(c->d, p, "ol") || dom_tag_is(c->d, p, "ul"))) {
                    struct css_style pst = style_for(c, p);
                    if (pst.has_lst) lst = pst.lst_type;
                }
            }
            if (lst == 1) {
                mn = 0; // none: no marker at all
            } else if ((lc && lc->ordered) || lst == 5 || lst == 6 ||
                       lst == 7 || lst == 8 || lst == 9) {
                int v = (lc && lc->ordered) ? lc->num++ : 1;
                int kind = (lst >= 5) ? lst : 5; // ordered or explicit numeric
                if (kind == 6 || kind == 7) {
                    // lower/upper-alpha: 1=a, 27=aa, ...
                    char tmp[8]; int tn = 0, vv = v;
                    while (vv > 0 && tn < 6) {
                        vv--;
                        tmp[tn++] = (char)((kind == 6 ? 'a' : 'A') + (vv % 26));
                        vv /= 26;
                    }
                    if (tn == 0) tmp[tn++] = (kind == 6 ? 'a' : 'A');
                    while (tn > 0 && mn < 6) mk[mn++] = tmp[--tn];
                    mk[mn++] = '.'; mk[mn++] = ' ';
                } else if (kind == 8 || kind == 9) {
                    // lower/upper-roman via subtractive table; falls back to
                    // decimal when it would overflow the marker buffer.
                    static const int rv[] = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
                    static const char* rs[] = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
                    char tmp[16]; int tn = 0, vv = v;
                    for (int ri = 0; ri < 13 && vv > 0 && tn < 14; ri++)
                        while (vv >= rv[ri] && tn < 14) {
                            int sl = 0; while (rs[ri][sl] && sl < 2) sl++;
                            for (int k = 0; k < sl && tn < 14; k++)
                                tmp[tn++] = rs[ri][k];
                            vv -= rv[ri];
                        }
                    if (vv == 0 && tn + 2 <= 7) {
                        for (int k = 0; k < tn; k++) {
                            char ch = tmp[k];
                            if (kind == 9 && ch >= 'a' && ch <= 'z') ch -= 32;
                            mk[mn++] = ch;
                        }
                        mk[mn++] = '.'; mk[mn++] = ' ';
                    } else {
                        if (v >= 10) mk[mn++] = (char)('0' + (v / 10) % 10);
                        mk[mn++] = (char)('0' + v % 10);
                        mk[mn++] = '.'; mk[mn++] = ' ';
                    }
                } else {
                    if (v >= 10) mk[mn++] = (char)('0' + (v / 10) % 10);
                    mk[mn++] = (char)('0' + v % 10);
                    mk[mn++] = '.'; mk[mn++] = ' ';
                }
            } else if (lst == 3) {
                mk[mn++] = 'o'; mk[mn++] = ' ';
            } else if (lst == 4) {
                mk[mn++] = '#'; mk[mn++] = ' ';
            } else {
                int v = (lc && lc->ordered) ? lc->num++ : 0;
                if (lc && lc->ordered) {
                    if (v >= 10) mk[mn++] = (char)('0' + (v / 10) % 10);
                    mk[mn++] = (char)('0' + v % 10);
                    mk[mn++] = '.'; mk[mn++] = ' ';
                } else {
                    mk[mn++] = (char)0xC6; mk[mn++] = ' ';
                }
            }
            int off = text_put(c, mk, mn);
            // Render inline children, then prefix the marker to the first run.
            int first_item = c->o->n_items;
            layout_inline(c, node, &st, inner_left + 3, inner_w - 3);
            if (mn > 0 && c->o->n_items > first_item &&
                c->o->items[first_item].run_count > 0) {
                // Shift the first line's runs right by the marker width and
                // prepend the marker run to that line.
                int li = first_item;
                int ri = c->o->items[li].run_start;
                int cnt = c->o->items[li].run_count;
                for (int k = 0; k < cnt; k++)
                    c->o->runs[ri + k].col += (uint16_t)mn;
                r.text_off = (uint32_t)off;
                r.text_len = (uint16_t)mn;
                r.col = (uint16_t)(c->o->runs[ri].col - mn);
                r.fg = st.has_fg_rgb ? st.fg_rgb : c->page_fg;
                r.bg = st.has_bg_rgb ? st.bg_rgb : c->page_bg;
                // insert before the first run: shift runs array right
                if (c->o->n_runs < LAYOUT_MAX_RUNS) {
                    for (int k = c->o->n_runs; k > ri; k--) c->o->runs[k] = c->o->runs[k - 1];
                    c->o->runs[ri] = r;
                    c->o->n_runs++;
                    c->o->items[li].run_count++;
                    // fix run_start of any later item that pointed past ri
                    for (int j = first_item + 1; j < c->o->n_items; j++)
                        if (c->o->items[j].run_count > 0 &&
                            c->o->items[j].run_start > ri)
                            c->o->items[j].run_start++;
                }
            }
        } else {
            if (dom_tag_is(c->d, node, "ol")) {
                struct list_counter sub; sub.ordered = 1; sub.num = 1;
                layout_children(c, node, inner_left, inner_w, &sub, heading_level);
            } else if (dom_tag_is(c->d, node, "ul") || dom_tag_is(c->d, node, "dl")) {
                struct list_counter sub; sub.ordered = 0; sub.num = 0;
                layout_children(c, node, inner_left, inner_w, &sub, heading_level);
            } else {
                layout_children(c, node, inner_left, inner_w, lc, heading_level);
            }
        }
    }

    int bottom = c->o->height;
    emit_box(c, &st, top, bottom, inner_left, inner_w);
    if (dom_tag_is(c->d, node, "br")) { c->o->height++; }
    // Relative: the paint shifts but the layout stands (flow cursor and
    // document height already final). Applies at any oof depth — relatively
    // positioned descendants of an absolute box shift within it.
    if (st.has_pos && st.pos_mode == 1) {
        int dy = 0, dx = 0;
        if (st.has_top) dy += px_rows(st.top);
        if (st.has_bottom) dy -= px_rows(st.bottom);
        if (st.has_left) dx += px_cols(st.left);
        if (st.has_right) dx -= px_cols(st.right);
        oof_shift(c, n0, dx, dy);
    }
    c->o->height += mbot;
}

int dom_is_block_node(const struct dom* d, int node) {
    char tag[32];
    dom_tag_copy(d, node, tag, sizeof(tag));
    int n = 0; while (tag[n]) n++;
    return dom_is_block(tag, n);
}

// ---------------------------------------------------------------------------

// Chrome containers that are never article content: semantic landmarks and
// the ubiquitous nav/menu/sidebar class patterns. A narrow-viewport browser
// (and reader mode) drops these; without it, Wikipedia's sidebar becomes a
// full-width wall of links.
static int str_has(const char* hay, const char* needle) {
    if (!hay) return 0;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] == needle[k]) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

static int chrome_class_hit(const char* buf) {
    return str_has(buf, "menu") || str_has(buf, "sidebar") ||
        str_has(buf, "navigation") || str_has(buf, "-nav") ||
        str_has(buf, "nav-") || str_has(buf, "portlet") ||
        str_has(buf, "siteNotice") || str_has(buf, "site-notice") ||
        str_has(buf, "catlinks") || str_has(buf, "printfooter");
}

static int chrome_id_hit(const char* buf) {
    return str_has(buf, "mw-panel") || str_has(buf, "p-navigation") ||
        str_has(buf, "p-tb") || str_has(buf, "p-search") ||
        str_has(buf, "catlinks") || str_has(buf, "siteNotice");
}

static int is_chrome(const struct dom* d, int node) {
    if (d->nodes[node].type != DOM_NODE_ELEMENT) return 0;
    if (dom_tag_is(d, node, "noscript") ||
        dom_tag_is(d, node, "template"))
        return 1;
    // NOTE: <nav>/<aside>/<footer> are NOT blanket-dropped. Landmarks are
    // sometimes the primary content (Wikipedia's portal keeps its whole
    // language grid in <nav aria-label="Top languages">) — dropping the tag
    // outright hid every language link (0 link runs on a page of links).
    // Real sidebars/footers still match the class/id patterns below.
    if (dom_tag_is(d, node, "nav") || dom_tag_is(d, node, "aside") ||
        dom_tag_is(d, node, "footer")) {
        char abuf[160];
        int sig = 0;
        if (dom_attr_get(d, node, "class", abuf, sizeof(abuf)) >= 0)
            sig = chrome_class_hit(abuf);
        if (!sig && dom_attr_get(d, node, "id", abuf, sizeof(abuf)) >= 0)
            sig = chrome_id_hit(abuf);
        // Bare landmark with no class/id at all: still navigation chrome
        // by definition (a labeled landmark would carry attributes).
        char cb[8], ib[8];
        int has_class = dom_attr_get(d, node, "class", cb, sizeof(cb)) >= 0;
        int has_id = dom_attr_get(d, node, "id", ib, sizeof(ib)) >= 0;
        if (!has_class && !has_id) return 1;
        return sig;
    }
    char buf[160];
    if (dom_attr_get(d, node, "class", buf, sizeof(buf)) >= 0) {
        if (chrome_class_hit(buf))
            return 1;
    }
    if (dom_attr_get(d, node, "id", buf, sizeof(buf)) >= 0) {
        if (chrome_id_hit(buf))
            return 1;
    }
    return 0;
}

void layout_run(const struct dom* d, const struct css_rule* rules, int n_rules,
                const struct layout_opts* opt, struct layout* out) {
    struct lctx c;
    memset(&c, 0, sizeof(c));
    c.d = d;
    c.rules = rules;
    c.n_rules = n_rules;
    c.o = out;
    c.width = (opt && opt->width_cols > 0) ? opt->width_cols : 60;
    c.page_left = opt ? opt->page_left : 0;
    c.page_bg = opt ? opt->page_bg : 0xFFFFFF;
    c.page_fg = opt ? opt->page_fg : 0x000000;

    memset(out, 0, sizeof(*out));
    out->width = c.width;
    out->page_left = c.page_left;
    out->page_bg = c.page_bg;
    out->page_fg = c.page_fg;

    if (!d || d->root < 0) { out->height = 1; return; }

    // Render the <body> contents (falling back to <html> or the root).
    int body = dom_first_tag(d, "body");
    int start = body >= 0 ? body : d->root;
    struct list_counter lc;
    memset(&lc, 0, sizeof(lc));

    // <body> background becomes the page background unless already set.
    struct css_style bs = style_for(&c, start);
    if (bs.has_bg_rgb && !(opt && opt->page_bg)) out->page_bg = bs.bg_rgb;
    if (bs.has_fg_rgb && !(opt && opt->page_fg)) out->page_fg = bs.fg_rgb;
    c.page_bg = out->page_bg;
    c.page_fg = out->page_fg;

    layout_children(&c, start, c.page_left, c.width, &lc, 0);
    // Out-of-flow overflow: absolute boxes below the flow end stay reachable
    // by scroll without having taken space in flow.
    if (c.oof_max > out->height) out->height = c.oof_max;
    if (out->height < 1) out->height = 1;
}
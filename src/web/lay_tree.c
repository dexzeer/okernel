// Box tree construction + anonymous box fixups.
#include "lay_int.h"

const struct wstyle* lay_anon_style(struct wlayout* L, const struct wstyle* parent, int display) {
    if (L->anon_used % 128 == 0) {
        if (L->nanon_chunk >= 512) return parent;
        struct wstyle* c = (struct wstyle*)w_malloc(128 * sizeof(struct wstyle));
        if (!c) return parent;
        L->anon_chunk[L->nanon_chunk++] = c;
    }
    struct wstyle* s = &L->anon_chunk[L->nanon_chunk - 1][L->anon_used % 128];
    L->anon_used++;
    css_anon_style(parent, display, s);
    return s;
}

const char* lay_box_raw(struct wlayout* L, int bi, int* len) {
    struct lbox* b = &L->b[bi];
    *len = (int)b->tl;
    if (b->flags & BF_RAWTEXT) return L->text.p + b->t0;
    if (b->node >= 0 && L->d->n[b->node].type == WN_TEXT) {
        *len = (int)L->d->n[b->node].tlen;
        return L->d->text + L->d->n[b->node].text;
    }
    *len = 0;
    return "";
}

static int is_block_level(const struct lbox* b) {
    if (b->flags & (BF_ABS | BF_FLOAT)) return 0;
    return b->kind == LB_BLOCK && !(b->flags & BF_INLINE_LEVEL) ? 1 :
           (b->kind == LB_REPLACED && !(b->flags & BF_INLINE_LEVEL));
}

static int is_inline_level(const struct lbox* b) {
    if (b->flags & (BF_ABS | BF_FLOAT)) return 0;
    return !is_block_level(b);
}

static int collapsible_ws_text(struct wlayout* L, int bi) {
    const struct lbox* b = &L->b[bi];
    if (b->kind != LB_TEXT) return 0;
    int ws = b->st ? b->st->white_space : WS_NORMAL;
    if (ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_BREAK_SPACES) return 0;
    int len;
    const char* s = lay_box_raw(L, bi, &len);
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return 0;
        if (c == '\n' && ws == WS_PRE_LINE) return 0;
    }
    return 1;
}

// detach child list of box into an array (returns count)
static int children_of(struct wlayout* L, int bi, int** out) {
    int n = 0;
    for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) n++;
    if (!n) { *out = 0; return 0; }
    int* a = (int*)w_malloc(n * sizeof(int));
    if (!a) { *out = 0; return 0; }
    n = 0;
    for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) a[n++] = c;
    *out = a;
    return n;
}

static void clear_children(struct wlayout* L, int bi) { L->b[bi].first = L->b[bi].last = -1; }

static int anon_block(struct wlayout* L, int parent, int fc) {
    const struct wstyle* ps = L->b[parent].st;
    int disp = fc == FC_TABLE ? D_TABLE : fc == FC_ROWGROUP ? D_TABLE_ROW_GROUP :
               fc == FC_ROW ? D_TABLE_ROW : fc == FC_CELL ? D_TABLE_CELL : D_BLOCK;
    const struct wstyle* st = lay_anon_style(L, ps, disp);
    int a = lb_new(L, -1, st, LB_BLOCK);
    if (a < 0) return -1;
    L->b[a].fc = (uint8_t)fc;
    L->b[a].flags |= BF_ANON | (L->b[parent].flags & BF_FIXED);
    return a;
}

static int is_table_part(const struct lbox* b) {
    return b->kind == LB_BLOCK && !(b->flags & (BF_ABS | BF_FLOAT)) &&
           (b->fc == FC_ROWGROUP || b->fc == FC_ROW || b->fc == FC_CELL);
}

// Fix the children of block container bi.
static void fixup_flow(struct wlayout* L, int bi) {
    int* ch;
    int n = children_of(L, bi, &ch);
    if (!n) return;
    int any_block = 0, any_inline = 0, any_tpart = 0;
    for (int i = 0; i < n; i++) {
        struct lbox* c = &L->b[ch[i]];
        if (is_table_part(c)) any_tpart = 1;
        else if (is_block_level(c)) any_block = 1;
        else if (is_inline_level(c) && !collapsible_ws_text(L, ch[i])) any_inline = 1;
    }
    if (any_tpart) {
        // wrap runs of table parts into anonymous tables
        clear_children(L, bi);
        for (int i = 0; i < n;) {
            if (is_table_part(&L->b[ch[i]])) {
                int t = anon_block(L, bi, FC_TABLE);
                if (t < 0) break;
                while (i < n && (is_table_part(&L->b[ch[i]]) || collapsible_ws_text(L, ch[i]))) {
                    lb_append(L, t, ch[i]);
                    i++;
                }
                lb_append(L, bi, t);
                continue;
            }
            lb_append(L, bi, ch[i]);
            i++;
        }
        w_free(ch);
        n = children_of(L, bi, &ch);
        any_block = 1;
        any_inline = 0;
        for (int i = 0; i < n; i++)
            if (is_inline_level(&L->b[ch[i]]) && !collapsible_ws_text(L, ch[i])) any_inline = 1;
    }
    if (!any_block) {
        if (any_inline) L->b[bi].flags |= BF_IFC;
        w_free(ch);
        return;
    }
    // mixed (or block only): wrap inline runs, drop whitespace-only runs
    clear_children(L, bi);
    for (int i = 0; i < n;) {
        struct lbox* c = &L->b[ch[i]];
        if (is_block_level(c) || is_table_part(c)) { lb_append(L, bi, ch[i]); i++; continue; }
        // collect a run of non-block children
        int j = i, real = 0;
        while (j < n && !is_block_level(&L->b[ch[j]]) && !is_table_part(&L->b[ch[j]])) {
            if (is_inline_level(&L->b[ch[j]]) && !collapsible_ws_text(L, ch[j])) real = 1;
            j++;
        }
        if (real) {
            int a = anon_block(L, bi, FC_FLOW);
            if (a < 0) { w_free(ch); return; }
            L->b[a].flags |= BF_IFC;
            for (int k = i; k < j; k++) lb_append(L, a, ch[k]);
            lb_append(L, bi, a);
        } else {
            for (int k = i; k < j; k++)
                if (L->b[ch[k]].flags & (BF_ABS | BF_FLOAT)) lb_append(L, bi, ch[k]);
        }
        i = j;
    }
    w_free(ch);
}

// flex/grid containers: every in-flow child is an item; text runs become
// anonymous block items
static void fixup_items(struct wlayout* L, int bi) {
    int* ch;
    int n = children_of(L, bi, &ch);
    if (!n) return;
    clear_children(L, bi);
    for (int i = 0; i < n;) {
        struct lbox* c = &L->b[ch[i]];
        if ((c->flags & (BF_ABS)) || c->kind == LB_BLOCK || c->kind == LB_REPLACED) {
            if (c->kind == LB_BLOCK || c->kind == LB_REPLACED) {
                c->flags &= ~(BF_INLINE_LEVEL | BF_FLOAT);
            }
            lb_append(L, bi, ch[i]);
            i++;
            continue;
        }
        int j = i, real = 0;
        while (j < n && !(L->b[ch[j]].flags & BF_ABS) && L->b[ch[j]].kind != LB_BLOCK &&
               L->b[ch[j]].kind != LB_REPLACED) {
            if (!collapsible_ws_text(L, ch[j])) real = 1;
            j++;
        }
        if (real) {
            int a = anon_block(L, bi, FC_FLOW);
            if (a < 0) break;
            L->b[a].flags |= BF_IFC;
            for (int k = i; k < j; k++) lb_append(L, a, ch[k]);
            lb_append(L, bi, a);
        }
        i = j;
    }
    w_free(ch);
}

static void fixup_table(struct wlayout* L, int bi, int fc) {
    int* ch;
    int n = children_of(L, bi, &ch);
    if (!n) return;
    clear_children(L, bi);
    int want = fc == FC_TABLE ? FC_ROWGROUP : fc == FC_ROWGROUP ? FC_ROW : FC_CELL;
    for (int i = 0; i < n;) {
        struct lbox* c = &L->b[ch[i]];
        if (c->kind == LB_BLOCK && (c->fc == want || (fc == FC_TABLE && (c->fc == FC_CAPTION ||
            c->fc == FC_COLUMN)) || (c->flags & BF_ABS))) {
            lb_append(L, bi, ch[i]);
            i++;
            continue;
        }
        if (collapsible_ws_text(L, ch[i])) { i++; continue; }
        // wrap a run of misparented children
        int a = anon_block(L, bi, want);
        if (a < 0) break;
        int j = i;
        while (j < n) {
            struct lbox* d = &L->b[ch[j]];
            if (d->kind == LB_BLOCK && (d->fc == want || (fc == FC_TABLE && (d->fc == FC_CAPTION || d->fc == FC_COLUMN))))
                break;
            lb_append(L, a, ch[j]);
            j++;
        }
        lb_append(L, bi, a);
        if (want == FC_ROWGROUP) fixup_table(L, a, FC_ROWGROUP);
        else if (want == FC_ROW) fixup_table(L, a, FC_ROW);
        else fixup_flow(L, a);
        i = j;
    }
    w_free(ch);
}

static void fixup(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    if (b->kind != LB_BLOCK) return;
    switch (b->fc) {
    case FC_FLEX: case FC_GRID: fixup_items(L, bi); break;
    case FC_TABLE: case FC_ROWGROUP: case FC_ROW: fixup_table(L, bi, b->fc); break;
    case FC_COLUMN: clear_children(L, bi); break;
    default: fixup_flow(L, bi); break;
    }
}

// ---- list markers -------------------------------------------------------------

static int roman(int n, char* out, int upper) {
    static const int V[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char* const S[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    int o = 0;
    if (n <= 0 || n > 3999) return 0;
    for (int i = 0; i < 13; i++)
        while (n >= V[i]) {
            for (const char* p = S[i]; *p; p++) out[o++] = upper ? (char)(*p - 32) : *p;
            n -= V[i];
        }
    return o;
}

static int marker_text(const struct wstyle* st, int ord, char* out, const struct wstyleset* ss) {
    int o = 0;
    switch (st->list_style_type) {
    case LS_NONE: return 0;
    case LS_DISC: o = w_utf8_enc(0x2022, out); break;
    case LS_CIRCLE: o = w_utf8_enc(0x25E6, out); break;
    case LS_SQUARE: o = w_utf8_enc(0x25AA, out); break;
    case LS_DISCLOSURE_OPEN: o = w_utf8_enc(0x25BE, out); break;
    case LS_DISCLOSURE_CLOSED: o = w_utf8_enc(0x25B8, out); break;
    case LS_STRING: {
        const char* s = css_str(ss, st->list_marker, 0);
        int n = 0;
        while (s[n] && n < 60) { out[n] = s[n]; n++; }
        return n;
    }
    case LS_LOWER_ALPHA: case LS_UPPER_ALPHA: {
        char tmp[16]; int t = 0, v = ord;
        if (v <= 0) v = 1;
        while (v > 0 && t < 15) { v--; tmp[t++] = (char)((st->list_style_type == LS_UPPER_ALPHA ? 'A' : 'a') + v % 26); v /= 26; }
        while (t) out[o++] = tmp[--t];
        out[o++] = '.';
        break;
    }
    case LS_LOWER_ROMAN: case LS_UPPER_ROMAN:
        o = roman(ord, out, st->list_style_type == LS_UPPER_ROMAN);
        if (!o) goto dec;
        out[o++] = '.';
        break;
    case LS_LOWER_GREEK:
        if (ord >= 1 && ord <= 24) { o = w_utf8_enc(0x3B1 + ord - 1 + (ord > 17 ? 1 : 0), out); out[o++] = '.'; break; }
        goto dec;
    case LS_DECIMAL_LZ:
        if (ord >= 0 && ord < 10) out[o++] = '0';
        goto dec;
    default:
    dec: {
        char tmp[16]; int t = 0, v = ord < 0 ? -ord : ord;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 15);
        if (ord < 0) out[o++] = '-';
        while (t) out[o++] = tmp[--t];
        out[o++] = '.';
    }
    }
    out[o++] = ' ';
    return o;
}

// ---- building ---------------------------------------------------------------------

static int rk_of(struct wlayout* L, int el, int* is_replaced) {
    struct wdom* d = L->d;
    const struct wnode* n = &d->n[el];
    *is_replaced = 0;
    if (n->ns == NS_SVG) {
        if (n->tag == T_svg) { *is_replaced = 1; return RK_SVG; }
        return -1;
    }
    if (n->ns != NS_HTML) return -1;
    switch (n->tag) {
    case T_img: *is_replaced = 1; return RK_IMG;
    case T_video: *is_replaced = 1; return RK_VIDEO;
    case T_canvas: *is_replaced = 1; return RK_CANVAS;
    case T_iframe: *is_replaced = 1; return RK_IFRAME;
    case T_embed: case T_object: *is_replaced = 1; return RK_OBJECT;
    case T_select: *is_replaced = 1; return RK_SELECT;
    case T_textarea: *is_replaced = 1; return RK_TEXTAREA;
    case T_meter: case T_progress: *is_replaced = 1; return RK_METER;
    case T_input: {
        int tl;
        const char* t = wdom_attr(d, el, A_type, &tl);
        *is_replaced = 1;
        if (!t) return RK_INPUT_TEXT;
        if (w_ieq(t, tl, "checkbox")) return RK_CHECKBOX;
        if (w_ieq(t, tl, "radio")) return RK_RADIO;
        if (w_ieq(t, tl, "submit") || w_ieq(t, tl, "button") || w_ieq(t, tl, "reset") ||
            w_ieq(t, tl, "file")) return RK_INPUT_BUTTON;
        if (w_ieq(t, tl, "image")) return RK_IMG;
        if (w_ieq(t, tl, "range") || w_ieq(t, tl, "color")) return RK_METER;
        return RK_INPUT_TEXT;
    }
    }
    return -1;
}

static int fc_of(int display) {
    switch (display) {
    case D_FLEX: case D_INLINE_FLEX: return FC_FLEX;
    case D_GRID: case D_INLINE_GRID: return FC_GRID;
    case D_TABLE: case D_INLINE_TABLE: return FC_TABLE;
    case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP: case D_TABLE_FOOTER_GROUP: return FC_ROWGROUP;
    case D_TABLE_ROW: return FC_ROW;
    case D_TABLE_CELL: return FC_CELL;
    case D_TABLE_CAPTION: return FC_CAPTION;
    case D_TABLE_COLUMN: case D_TABLE_COLUMN_GROUP: return FC_COLUMN;
    }
    return FC_FLOW;
}

static int is_inline_display(int d) {
    return d == D_INLINE_BLOCK || d == D_INLINE_FLEX || d == D_INLINE_GRID || d == D_INLINE_TABLE;
}

static int text_box(struct wlayout* L, int parent, int node, const struct wstyle* st) {
    int t = lb_new(L, node, st, LB_TEXT);
    if (t < 0) return -1;
    L->b[t].flags |= L->b[parent].flags & BF_FIXED;
    lb_append(L, parent, t);
    return t;
}

static void build_children(struct wlayout* L, int el, int box);

// Pseudo-element box (::before = 1, ::after = 2) for element el into box.
static void build_pseudo(struct wlayout* L, int el, int box, int which) {
    const struct wstyle* ps = css_pseudo_of(L->ss, el, which);
    if (!ps || !ps->content || ps->display == D_NONE) return;
    int kind = (ps->display == D_INLINE) ? LB_INLINE : LB_BLOCK;
    int pb = lb_new(L, -1, ps, kind);
    if (pb < 0) return;
    struct lbox* b = &L->b[pb];
    b->pseudo = (uint8_t)which;
    b->node = -1;
    b->fc = (uint8_t)fc_of(ps->display);
    if (kind == LB_BLOCK && is_inline_display(ps->display)) b->flags |= BF_INLINE_LEVEL;
    if (ps->position == POS_ABSOLUTE || ps->position == POS_FIXED) b->flags |= BF_ABS;
    else if (ps->float_ != FL_NONE) b->flags |= BF_FLOAT;
    if (ps->position != POS_STATIC) b->flags |= BF_POSITIONED;
    b->flags |= (L->b[box].flags & BF_FIXED) | (ps->position == POS_FIXED ? BF_FIXED : 0);
    b->list_ord = el; // owning element (hit testing / debugging)
    if (ps->content_len > 0) {
        const char* s = css_str(L->ss, ps->content, ps->content_len);
        uint32_t off = (uint32_t)L->text.len;
        wbuf_put(&L->text, s, ps->content_len);
        int t = lb_new(L, -1, ps, LB_TEXT);
        if (t >= 0) {
            L->b[t].t0 = off;
            L->b[t].tl = (uint32_t)ps->content_len;
            L->b[t].flags |= BF_RAWTEXT | (b->flags & BF_FIXED);
            lb_append(L, pb, t);
        }
    }
    lb_append(L, box, pb);
    if (kind == LB_BLOCK) fixup(L, pb);
}

static void build_el(struct wlayout* L, int el, int parent_box, int* list_ctr, int list_rev) {
    struct wdom* d = L->d;
    const struct wstyle* st = css_style_of(L->ss, el);
    if (!st || st->display == D_NONE) return;
    if (L->depth > 400) return; // pathological nesting
    if (st->display == D_CONTENTS) {
        L->depth++;
        build_children(L, el, parent_box);
        L->depth--;
        return;
    }
    const struct wnode* n = &d->n[el];
    int is_rep = 0;
    int rk = rk_of(L, el, &is_rep);
    int kind;
    if (n->ns == NS_HTML && n->tag == T_br) kind = LB_BR;
    else if (n->ns == NS_HTML && n->tag == T_wbr) kind = LB_WBR;
    else if (is_rep) kind = LB_REPLACED;
    else if (st->display == D_INLINE) kind = LB_INLINE;
    else kind = LB_BLOCK;
    if (n->ns == NS_SVG && !is_rep) return; // svg internals are painted by the svg box
    if (n->ns == NS_MATH && n->tag != T_math && st->display == D_BLOCK) kind = LB_INLINE;
    int bi = lb_new(L, el, st, kind);
    if (bi < 0) return;
    struct lbox* b = &L->b[bi];
    b->rk = (uint8_t)(rk < 0 ? 0 : rk);
    b->fc = (uint8_t)fc_of(st->display);
    if (kind == LB_REPLACED || kind == LB_BLOCK) {
        if (is_inline_display(st->display) || (kind == LB_REPLACED && st->display == D_INLINE))
            b->flags |= BF_INLINE_LEVEL;
    }
    if (kind == LB_BR || kind == LB_WBR || kind == LB_INLINE) b->flags |= BF_INLINE_LEVEL;
    if (st->position == POS_ABSOLUTE || st->position == POS_FIXED) b->flags |= BF_ABS;
    else if (st->float_ != FL_NONE && kind != LB_INLINE) b->flags |= BF_FLOAT;
    if (st->position != POS_STATIC) b->flags |= BF_POSITIONED;
    b->flags |= (L->b[parent_box].flags & BF_FIXED) | (st->position == POS_FIXED ? BF_FIXED : 0);
    if (L->node_box[el] < 0) L->node_box[el] = bi;
    // list items: ordinal + marker
    if (st->display == D_LIST_ITEM) {
        int ord = *list_ctr;
        if (st->list_value_set & 1) ord = st->list_value;
        *list_ctr = ord + (list_rev ? -1 : 1);
        b->list_ord = ord;
        const struct wstyle* ms = css_pseudo_of(L->ss, el, 3);
        char mt[80];
        int ml;
        if (ms && ms->content) {
            const char* s = css_str(L->ss, ms->content, ms->content_len);
            ml = ms->content_len < 79 ? ms->content_len : 79;
            memcpy(mt, s, ml);
        } else ml = marker_text(st, ord, mt, L->ss);
        if (ml > 0) {
            uint32_t off = (uint32_t)L->text.len;
            wbuf_put(&L->text, mt, ml);
            if (st->list_style_inside) {
                int m = lb_new(L, -1, ms ? ms : st, LB_MARKER);
                if (m >= 0) {
                    L->b[m].t0 = off;
                    L->b[m].tl = (uint32_t)ml;
                    L->b[m].flags |= BF_INLINE_LEVEL | BF_RAWTEXT | (b->flags & BF_FIXED);
                    lb_append(L, bi, m);
                }
            } else {
                b = &L->b[bi];
                b->flags |= BF_MARKER;
                b->t0 = off;
                b->tl = (uint32_t)ml;
            }
        }
    }
    lb_append(L, parent_box, bi);
    if (kind == LB_REPLACED || kind == LB_BR || kind == LB_WBR) {
        if (kind == LB_REPLACED && n->tag == T_button) build_children(L, el, bi);
        return;
    }
    L->depth++;
    build_pseudo(L, el, bi, 1);
    build_children(L, el, bi);
    build_pseudo(L, el, bi, 2);
    L->depth--;
    b = &L->b[bi];
    if (kind == LB_INLINE) {
        // block-in-inline: an inline containing block-level boxes becomes a block
        for (int c = b->first; c >= 0; c = L->b[c].next)
            if (is_block_level(&L->b[c]) || is_table_part(&L->b[c])) {
                b->kind = LB_BLOCK;
                b->fc = FC_FLOW;
                b->flags &= ~BF_INLINE_LEVEL;
                break;
            }
    }
    if (b->kind == LB_BLOCK) fixup(L, bi);
}

static void build_children(struct wlayout* L, int el, int box) {
    struct wdom* d = L->d;
    const struct wstyle* st = css_style_of(L->ss, el);
    int ctr = 1, rev = 0;
    if (st && (st->list_value_set & 1)) ctr = st->list_value;
    if (st && (st->list_value_set & 2)) {
        rev = 1;
        if (!(st->list_value_set & 1)) {
            ctr = 0;
            for (int c = d->n[el].first; c >= 0; c = d->n[c].next) {
                const struct wstyle* cs = d->n[c].type == WN_ELEM ? css_style_of(L->ss, c) : 0;
                if (cs && cs->display == D_LIST_ITEM) ctr++;
            }
        }
    }
    const struct wstyle* bst = L->b[box].st;
    for (int c = d->n[el].first; c >= 0; c = d->n[c].next) {
        const struct wnode* n = &d->n[c];
        if (n->type == WN_TEXT) {
            if (n->tlen) text_box(L, box, c, bst);
        } else if (n->type == WN_ELEM) {
            build_el(L, c, box, &ctr, rev);
        }
    }
}

int lay_build_tree(struct wlayout* L) {
    struct wdom* d = L->d;
    int html = d->html >= 0 ? d->html : wdom_first_tag(d, T_html);
    // initial containing block holder (anonymous, not painted)
    const struct wstyle* rs = html >= 0 ? css_style_of(L->ss, html) : 0;
    struct wstyle* icb_style = (struct wstyle*)lay_anon_style(L, rs, D_BLOCK);
    int icb = lb_new(L, -1, icb_style, LB_BLOCK);
    if (icb < 0) return -1;
    L->b[icb].flags |= BF_ANON | BF_BFC | BF_POSITIONED;
    L->root = icb;
    if (html < 0) return icb;
    int ctr = 1;
    build_el(L, html, icb, &ctr, 0);
    fixup(L, icb);
    return icb;
}

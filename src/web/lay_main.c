// Layout core: boxes, block flow, margins, floats, positioning, finalize.
#include "lay_int.h"

// ---- box storage ----------------------------------------------------------------

int lb_new(struct wlayout* L, int node, const struct wstyle* st, int kind) {
    if (L->nb >= L->capb) {
        int nc = L->capb ? L->capb * 2 : 1024;
        struct lbox* nb = (struct lbox*)w_realloc(L->b, nc * sizeof(struct lbox));
        if (!nb) return -1;
        L->b = nb;
        L->capb = nc;
    }
    int i = L->nb++;
    struct lbox* b = &L->b[i];
    memset(b, 0, sizeof *b);
    b->node = node;
    b->st = st;
    b->kind = (uint8_t)kind;
    b->parent = b->first = b->last = b->next = b->prev = -1;
    b->lparent = -1;
    b->baseline = b->last_baseline = -1;
    b->img = -1;
    b->imin = b->imax = -1;
    b->abs_head = b->abs_next = -1;
    b->cbh = -1;
    return i;
}

void lb_append(struct wlayout* L, int parent, int child) {
    struct lbox* c = &L->b[child];
    c->parent = parent;
    c->next = -1;
    c->prev = L->b[parent].last;
    if (c->prev >= 0) L->b[c->prev].next = child;
    else L->b[parent].first = child;
    L->b[parent].last = child;
}

void lb_font(const struct wstyle* st, struct wfont* f) {
    f->family = st->font_family == FAM_MONO ? WF_FAMILY_MONO :
                st->font_family == FAM_SERIF ? WF_FAMILY_SERIF : WF_FAMILY_SANS;
    f->bold = st->font_weight >= 600;
    f->italic = st->font_style != 0;
    int px = LU_ROUND(st->font_size);
    if (px < 1) px = 1;
    if (px > 400) px = 400;
    f->px = (uint16_t)px;
}

void lb_font_metrics(const struct wstyle* st, struct wfmetrics* m) {
    struct wfont f;
    lb_font(st, &f);
    wfont_metrics(&f, m);
}

int32_t lb_line_height(const struct wstyle* st) {
    if (st->line_height > 0) return st->line_height;
    struct wfmetrics m;
    lb_font_metrics(st, &m);
    return m.ascent + m.descent + m.line_gap;
}

void lb_resolve_box_sides(struct wlayout* L, int bi, int32_t cbw) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    for (int i = 0; i < 4; i++) {
        b->m[i] = wl_resolve(s->margin[i], cbw, 0);
        b->p[i] = wl_resolve(s->padding[i], cbw, 0);
        if (b->p[i] < 0) b->p[i] = 0;
        b->b[i] = s->bw[i];
    }
    if (b->kind == LB_INLINE || b->kind == LB_TEXT) return;
    // table rows/groups have no padding; cells inside collapsing tables keep theirs
    if (b->fc == FC_ROW || b->fc == FC_ROWGROUP) for (int i = 0; i < 4; i++) { b->p[i] = 0; b->m[i] = 0; }
    if (b->fc == FC_CELL) for (int i = 0; i < 4; i++) b->m[i] = 0;
}

int32_t lay_content_w(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    int32_t w = b->w - b->b[1] - b->b[3] - b->p[1] - b->p[3];
    return w < 0 ? 0 : w;
}

int lay_is_bfc(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    if (b->flags & (BF_ABS | BF_FLOAT | BF_INLINE_LEVEL)) return 1;
    if (b->fc != FC_FLOW) return 1;
    if (s->display == D_FLOW_ROOT) return 1;
    if (s->overflow_x != OV_VISIBLE || s->overflow_y != OV_VISIBLE) return 1;
    if (b->kind == LB_REPLACED) return 1;
    if (b->parent >= 0) {
        int pfc = L->b[b->parent].fc;
        if (pfc == FC_FLEX || pfc == FC_GRID) return 1;
    }
    return b->parent < 0 || b->parent == L->root;
}

// ---- margins ------------------------------------------------------------------

struct mpair { int32_t p, n; };
static struct mpair mp_of(int32_t v) { struct mpair m = { v > 0 ? v : 0, v < 0 ? v : 0 }; return m; }
static struct mpair mp_merge(struct mpair a, struct mpair b) {
    struct mpair m = { a.p > b.p ? a.p : b.p, a.n < b.n ? a.n : b.n };
    return m;
}
static int32_t mp_val(struct mpair m) { return m.p + m.n; }

static int first_inflow(struct wlayout* L, int bi) {
    for (int c = L->b[bi].first; c >= 0; c = L->b[c].next)
        if (lb_is_inflow(&L->b[c])) return c;
    return -1;
}
static int last_inflow(struct wlayout* L, int bi) {
    int r = -1;
    for (int c = L->b[bi].first; c >= 0; c = L->b[c].next)
        if (lb_is_inflow(&L->b[c])) r = c;
    return r;
}

// Can the top margin of block bi collapse with its first child's?
static int top_collapsible(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    if (b->kind != LB_BLOCK || b->fc != FC_FLOW || (b->flags & BF_IFC)) return 0;
    if (lay_is_bfc(L, bi)) return 0;
    return b->st->bw[0] == 0 && wl_resolve(b->st->padding[0], 0, 1) == 0 &&
           b->st->padding[0].pct == 0 && b->st->padding[0].px == 0;
}
static int bottom_collapsible(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    if (b->kind != LB_BLOCK || b->fc != FC_FLOW || (b->flags & BF_IFC)) return 0;
    if (lay_is_bfc(L, bi)) return 0;
    if (b->st->height.t == WL_LEN) return 0;
    return b->st->bw[2] == 0 && b->st->padding[2].px == 0 && b->st->padding[2].pct == 0;
}

// Leading (top) margin of a block-level box, including margins collapsed
// through its first in-flow children.
static struct mpair leading_margin(struct wlayout* L, int bi, int32_t cbw, int depth) {
    struct lbox* b = &L->b[bi];
    struct mpair m = mp_of(wl_resolve(b->st->margin[0], cbw, 0));
    if (b->fc == FC_ROW || b->fc == FC_ROWGROUP || b->fc == FC_CELL) m = mp_of(0);
    if (depth > 40 || !top_collapsible(L, bi)) return m;
    int c = first_inflow(L, bi);
    if (c < 0) return m;
    return mp_merge(m, leading_margin(L, c, cbw, depth + 1));
}

// ---- floats --------------------------------------------------------------------

static void fl_push(struct wlayout* L, int32_t x, int32_t y, int32_t w, int32_t h, int right) {
    if (L->nfl >= L->capfl) {
        int nc = L->capfl ? L->capfl * 2 : 64;
        struct bfloat* nf = (struct bfloat*)w_realloc(L->fl, nc * sizeof(struct bfloat));
        if (!nf) return;
        L->fl = nf;
        L->capfl = nc;
    }
    struct bfloat* f = &L->fl[L->nfl++];
    f->x = x; f->y = y; f->w = w; f->h = h; f->right = (uint8_t)right;
}

void fl_avail(struct wlayout* L, int32_t y, int32_t h, int32_t left, int32_t right,
              int32_t* l_out, int32_t* r_out) {
    int32_t l = left, r = right;
    if (h < 1) h = 1;
    for (int i = L->fl_base; i < L->nfl; i++) {
        struct bfloat* f = &L->fl[i];
        if (f->y >= y + h || f->y + f->h <= y) continue;
        if (!f->right) { if (f->x + f->w > l) l = f->x + f->w; }
        else if (f->x < r) r = f->x;
    }
    *l_out = l;
    *r_out = r;
}

int32_t fl_clear_y(struct wlayout* L, int32_t y, int clear) {
    for (int i = L->fl_base; i < L->nfl; i++) {
        struct bfloat* f = &L->fl[i];
        int hit = clear == CLR_BOTH || (clear == CLR_LEFT && !f->right) || (clear == CLR_RIGHT && f->right);
        if (hit && f->y + f->h > y) y = f->y + f->h;
    }
    return y;
}

int32_t fl_bottom(struct wlayout* L) {
    int32_t b = INT32_MIN;
    for (int i = L->fl_base; i < L->nfl; i++)
        if (L->fl[i].y + L->fl[i].h > b) b = L->fl[i].y + L->fl[i].h;
    return b;
}

// Place float bi (already laid out: w/h/margins known) within [cb_left,
// cb_right] at or below *y_io (absolute). Sets pax/pay.
void fl_place(struct wlayout* L, int bi, int32_t cb_left, int32_t cb_right, int32_t* y_io) {
    struct lbox* b = &L->b[bi];
    int right = b->st->float_ == FL_RIGHT;
    int32_t W = b->w + b->m[1] + b->m[3];
    int32_t H = b->h + b->m[0] + b->m[2];
    int32_t y = *y_io;
    // a float may not be placed above earlier floats
    for (int i = L->fl_base; i < L->nfl; i++) if (L->fl[i].y > y) y = L->fl[i].y;
    if (b->st->clear != CLR_NONE) y = fl_clear_y(L, y, b->st->clear);
    for (int guard = 0; guard < 200; guard++) {
        int32_t l, r;
        fl_avail(L, y, H, cb_left, cb_right, &l, &r);
        if (r - l >= W || (l == cb_left && r == cb_right)) {
            int32_t x = right ? r - W : l;
            fl_push(L, x, y, W, H, right);
            b->pax = x + b->m[3];
            b->pay = y + b->m[0];
            *y_io = y;
            return;
        }
        // move below the nearest float bottom that overlaps
        int32_t nb = INT32_MAX;
        for (int i = L->fl_base; i < L->nfl; i++) {
            struct bfloat* f = &L->fl[i];
            if (f->y + f->h > y && f->y + f->h < nb) nb = f->y + f->h;
        }
        if (nb == INT32_MAX) break;
        y = nb;
    }
    int32_t x = right ? cb_right - W : cb_left;
    fl_push(L, x, y, W, H, right);
    b->pax = x + b->m[3];
    b->pay = y + b->m[0];
    *y_io = y;
}

// ---- replaced sizing --------------------------------------------------------------

static void replaced_size(struct wlayout* L, int bi, int32_t cbw, int32_t* cw, int32_t* ch) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    struct wfmetrics fm;
    lb_font_metrics(s, &fm);
    int32_t fs = s->font_size;
    int32_t iw = 0, ih = 0;    // intrinsic LU
    int32_t ratio = 0;          // w/h * 1000
    switch (b->rk) {
    case RK_IMG: case RK_VIDEO: case RK_CANVAS: case RK_OBJECT: case RK_SVG: {
        if (b->iw > 0 && b->ih > 0) { iw = PX(b->iw); ih = PX(b->ih); ratio = w_muldiv(b->iw, 1000, b->ih); }
        if (b->rk == RK_VIDEO || b->rk == RK_CANVAS || b->rk == RK_OBJECT) {
            if (!iw) { iw = PX(300); ih = PX(150); ratio = 2000; }
        }
        if (b->rk == RK_SVG && !iw) {
            // viewBox -> ratio; default 300x150
            int vl;
            const char* vb = b->node >= 0 ? wdom_attr(L->d, b->node, A_viewbox, &vl) : 0;
            if (vb) {
                int32_t n4[4]; int k = 0, pos = 0;
                while (k < 4 && pos < vl) {
                    while (pos < vl && (vb[pos] == ' ' || vb[pos] == ',')) pos++;
                    int32_t m; int u;
                    if (!cv_number(vb + pos, vl - pos, &m, &u)) break;
                    n4[k++] = m;
                    pos += u;
                }
                if (k == 4 && n4[2] > 0 && n4[3] > 0) {
                    ratio = w_muldiv(n4[2], 1000, n4[3]);
                    iw = PX(150) * ratio / 1000;
                    ih = PX(150);
                }
            }
            if (!iw) { iw = PX(300); ih = PX(150); }
        }
        if (b->rk == RK_IMG && !iw) {
            // not loaded: alt text keeps a small box; no alt -> 0x0
            iw = 0; ih = 0;
        }
        break;
    }
    case RK_IFRAME: iw = PX(300); ih = PX(150); break;
    case RK_INPUT_TEXT: {
        int sl, size = 20;
        const char* sz = b->node >= 0 ? wdom_attr(L->d, b->node, A_size, &sl) : 0;
        if (sz) { int32_t m; int u; if (cv_number(sz, sl, &m, &u) && m >= 1000) size = m / 1000; }
        struct wfont f; lb_font(s, &f);
        int32_t avg = wfont_measure(&f, "0", 1);
        iw = avg * size + avg / 2;
        ih = lb_line_height(s);
        break;
    }
    case RK_TEXTAREA: {
        int cl, rl, cols = 20, rows = 2;
        const char* c = b->node >= 0 ? wdom_attr(L->d, b->node, A_cols, &cl) : 0;
        const char* r = b->node >= 0 ? wdom_attr(L->d, b->node, A_rows, &rl) : 0;
        int32_t m; int u;
        if (c && cv_number(c, cl, &m, &u) && m >= 1000) cols = m / 1000;
        if (r && cv_number(r, rl, &m, &u) && m >= 1000) rows = m / 1000;
        struct wfont f; lb_font(s, &f);
        iw = wfont_measure(&f, "0", 1) * cols;
        ih = lb_line_height(s) * rows;
        break;
    }
    case RK_INPUT_BUTTON: {
        char lab[128];
        int n = b->node >= 0 ? wdom_attr_copy(L->d, b->node, A_value, lab, sizeof lab) : -1;
        if (n < 0) {
            int tl;
            const char* t = wdom_attr(L->d, b->node, A_type, &tl);
            const char* d = (t && w_ieq(t, tl, "reset")) ? "Reset" : (t && w_ieq(t, tl, "file")) ? "Choose File" :
                            (t && w_ieq(t, tl, "button")) ? "" : "Submit";
            n = (int)strlen(d);
            memcpy(lab, d, n);
        }
        struct wfont f; lb_font(s, &f);
        iw = wfont_measure(&f, lab, n);
        ih = lb_line_height(s);
        break;
    }
    case RK_CHECKBOX: case RK_RADIO: iw = PX(13); ih = PX(13); break;
    case RK_SELECT: {
        // widest option + arrow
        struct wfont f; lb_font(s, &f);
        int32_t mx = 0;
        if (b->node >= 0)
            for (int c = L->d->n[b->node].first; c >= 0; c = wdom_next(L->d, c, b->node)) {
                if (!wdom_is(L->d, c, T_option)) continue;
                char t[256];
                int n = wdom_text_content(L->d, c, t, sizeof t);
                int32_t w = wfont_measure(&f, t, n);
                if (w > mx) mx = w;
            }
        iw = mx + PX(20);
        ih = lb_line_height(s);
        break;
    }
    case RK_METER: iw = PX(80); ih = fs; break;
    }
    (void)fm;
    if (s->aspect_ratio > 0) ratio = s->aspect_ratio;
    // CSS sizes
    int32_t box_extra_w = b->p[1] + b->p[3] + b->b[1] + b->b[3];
    int32_t box_extra_h = b->p[0] + b->p[2] + b->b[0] + b->b[2];
    int bb = s->box_sizing == BX_BORDER;
    int32_t W = -1, H = -1;
    if (s->width.t == WL_LEN) {
        W = wl_resolve(s->width, cbw, 0);
        if (bb) W -= box_extra_w;
        if (W < 0) W = 0;
    }
    if (s->height.t == WL_LEN && (s->height.pct == 0 || b->cbh >= 0)) {
        H = wl_resolve(s->height, b->cbh >= 0 ? b->cbh : 0, 0);
        if (bb) H -= box_extra_h;
        if (H < 0) H = 0;
    }
    if (W < 0 && H < 0) {
        W = iw; H = ih;
        if (b->rk == RK_IMG && !iw && ratio == 0) { W = 0; H = 0; }
    } else if (W < 0) {
        W = ratio ? w_muldiv(H, ratio, 1000) : iw;
    } else if (H < 0) {
        H = ratio ? w_muldiv(W, 1000, ratio) : ih;
    }
    // min/max
    if (s->max_w.t == WL_LEN) {
        int32_t mx = wl_resolve(s->max_w, cbw, 0) - (bb ? box_extra_w : 0);
        if (W > mx) { if (ratio && s->height.t != WL_LEN) H = w_muldiv(mx, 1000, ratio); W = mx; }
    }
    if (s->min_w.t == WL_LEN) {
        int32_t mn = wl_resolve(s->min_w, cbw, 0) - (bb ? box_extra_w : 0);
        if (W < mn) W = mn;
    }
    if (s->max_h.t == WL_LEN && (s->max_h.pct == 0 || b->cbh >= 0)) {
        int32_t mx = wl_resolve(s->max_h, b->cbh >= 0 ? b->cbh : 0, 0) - (bb ? box_extra_h : 0);
        if (H > mx) { if (ratio && s->width.t != WL_LEN) W = w_muldiv(mx, ratio, 1000); H = mx; }
    }
    if (s->min_h.t == WL_LEN && s->min_h.pct == 0) {
        int32_t mn = wl_resolve(s->min_h, 0, 0) - (bb ? box_extra_h : 0);
        if (H < mn) H = mn;
    }
    *cw = W < 0 ? 0 : W;
    *ch = H < 0 ? 0 : H;
}

// ---- intrinsic widths --------------------------------------------------------------

static int32_t fixed_width(struct wlayout* L, int bi, int* ok) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    *ok = 0;
    if (s->width.t != WL_LEN || s->width.pct) return 0;
    *ok = 1;
    int32_t w = s->width.px;
    if (s->box_sizing != BX_BORDER)
        w += b->p[1] + b->p[3] + b->b[1] + b->b[3];
    return w;
}

void lay_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx) {
    struct lbox* b = &L->b[bi];
    if (b->imin >= 0) { *mn = b->imin; *mx = b->imax; return; }
    lb_resolve_box_sides(L, bi, 0);
    b = &L->b[bi];
    int32_t extra = b->p[1] + b->p[3] + b->b[1] + b->b[3];
    int32_t a = 0, z = 0;
    int ok;
    int32_t fw = fixed_width(L, bi, &ok);
    if (b->kind == LB_REPLACED && !(b->kind == LB_BLOCK)) {
        int32_t cw, ch;
        replaced_size(L, bi, 0, &cw, &ch);
        b = &L->b[bi];
        a = z = cw + extra;
        if (b->st->width.t == WL_LEN && b->st->width.pct) { a = 0; }
    } else if (ok) {
        a = z = fw;
    } else if (b->kind == LB_BLOCK) {
        int32_t cmn = 0, cmx = 0;
        if (b->fc == FC_FLEX) lay_flex_intrinsic(L, bi, &cmn, &cmx);
        else if (b->fc == FC_GRID) lay_grid_intrinsic(L, bi, &cmn, &cmx);
        else if (b->fc == FC_TABLE) { lay_table_intrinsic(L, bi, &cmn, &cmx); cmn -= extra; cmx -= extra; }
        else if (b->flags & BF_IFC) lay_ifc_intrinsic(L, bi, &cmn, &cmx);
        else {
            int32_t float_sum = 0;
            for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) {
                struct lbox* cb = &L->b[c];
                if (cb->flags & BF_ABS) continue;
                int32_t m1, m2;
                lay_intrinsic(L, c, &m1, &m2);
                cb = &L->b[c];
                int32_t mm = 0;
                if (cb->st->margin[1].t == WL_LEN && !cb->st->margin[1].pct) mm += cb->st->margin[1].px;
                if (cb->st->margin[3].t == WL_LEN && !cb->st->margin[3].pct) mm += cb->st->margin[3].px;
                if (mm < 0) mm = 0;
                if (m1 + mm > cmn) cmn = m1 + mm;
                if (cb->flags & BF_FLOAT) float_sum += m2 + mm;
                else if (m2 + mm > cmx) cmx = m2 + mm;
            }
            if (float_sum > cmx) cmx = float_sum;
        }
        a = cmn + extra;
        z = cmx + extra;
    }
    b = &L->b[bi];
    const struct wstyle* s = b->st;
    if (s->min_w.t == WL_LEN && !s->min_w.pct) {
        int32_t m = s->min_w.px + (s->box_sizing == BX_BORDER ? 0 : extra);
        if (a < m) a = m;
        if (z < m) z = m;
    }
    if (s->max_w.t == WL_LEN && !s->max_w.pct) {
        int32_t m = s->max_w.px + (s->box_sizing == BX_BORDER ? 0 : extra);
        if (z > m) z = m;
        if (a > m) a = m;
    }
    if (z < a) z = a;
    b->imin = a;
    b->imax = z;
    *mn = a;
    *mx = z;
}

int32_t lay_shrink_width(struct wlayout* L, int bi, int32_t avail) {
    int32_t mn, mx;
    lay_intrinsic(L, bi, &mn, &mx);
    int32_t w = mx < avail ? mx : avail;
    if (w < mn) w = mn;
    return w;
}

// ---- absolute positioning -------------------------------------------------------------

void lay_abs_register(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    int cb = L->root;
    if (b->st->position != POS_FIXED) {
        for (int p = b->parent; p >= 0; p = L->b[p].parent)
            if ((L->b[p].flags & BF_POSITIONED) && (L->b[p].kind == LB_BLOCK || L->b[p].kind == LB_REPLACED)) { cb = p; break; }
    }
    b->abs_next = L->b[cb].abs_head;
    L->b[cb].abs_head = bi;
}

static void lay_abs_children(struct wlayout* L, int cbi);

static void lay_one_abs(struct wlayout* L, int ai, int cbi) {
    struct lbox* cb = &L->b[cbi];
    int fixed = L->b[ai].st->position == POS_FIXED;
    // containing block = padding box of cb (viewport for fixed / the ICB)
    int32_t cx, cy, cw, ch;
    if (fixed || cbi == L->root) { cx = 0; cy = 0; cw = PX(L->vw); ch = PX(L->vh); }
    else {
        cx = cb->b[3]; cy = cb->b[0];
        cw = cb->w - cb->b[1] - cb->b[3];
        ch = cb->h - cb->b[0] - cb->b[2];
    }
    struct lbox* b = &L->b[ai];
    const struct wstyle* s = b->st;
    lb_resolve_box_sides(L, ai, cw);
    b = &L->b[ai];
    int32_t ins[4];
    int has[4];
    for (int i = 0; i < 4; i++) {
        has[i] = s->inset[i].t == WL_LEN;
        ins[i] = wl_resolve(s->inset[i], (i == 0 || i == 2) ? ch : cw, 0);
    }
    int auto_ml = s->margin[3].t == WL_AUTO, auto_mr = s->margin[1].t == WL_AUTO;
    int auto_mt = s->margin[0].t == WL_AUTO, auto_mb = s->margin[2].t == WL_AUTO;
    if (auto_ml) b->m[3] = 0;
    if (auto_mr) b->m[1] = 0;
    if (auto_mt) b->m[0] = 0;
    if (auto_mb) b->m[2] = 0;
    int32_t forced_w = -1, forced_h = -1;
    if (s->width.t != WL_LEN && b->kind != LB_REPLACED && b->fc != FC_TABLE) {
        if (has[1] && has[3]) forced_w = cw - ins[1] - ins[3] - b->m[1] - b->m[3];
        else forced_w = lay_shrink_width(L, ai, cw - (has[3] ? ins[3] : 0) - (has[1] ? ins[1] : 0) - b->m[1] - b->m[3]);
        if (forced_w < 0) forced_w = 0;
        // min/max-width clamp
        int32_t extra = b->p[1] + b->p[3] + b->b[1] + b->b[3];
        if (s->max_w.t == WL_LEN) {
            int32_t m = wl_resolve(s->max_w, cw, 0) + (s->box_sizing == BX_BORDER ? 0 : extra);
            if (forced_w > m) forced_w = m;
        }
        if (s->min_w.t == WL_LEN) {
            int32_t m = wl_resolve(s->min_w, cw, 0) + (s->box_sizing == BX_BORDER ? 0 : extra);
            if (forced_w < m) forced_w = m;
        }
    }
    if (s->height.t != WL_LEN && has[0] && has[2] && b->kind != LB_REPLACED) {
        forced_h = ch - ins[0] - ins[2] - b->m[0] - b->m[2];
        if (forced_h < 0) forced_h = 0;
    }
    b->cbh = ch;
    b->pax = cb->pax + cx;
    b->pay = cb->pay + cy;
    lay_box(L, ai, cw, forced_w, forced_h);
    b = &L->b[ai];
    // horizontal position
    int32_t x;
    if (has[3] && has[1] && s->width.t == WL_LEN && auto_ml && auto_mr) {
        int32_t free = cw - ins[3] - ins[1] - b->w;
        x = ins[3] + (free > 0 ? free / 2 : 0);
    } else if (has[3]) x = ins[3] + b->m[3];
    else if (has[1]) x = cw - ins[1] - b->m[1] - b->w;
    else x = b->sx - (cb->pax + cx) + b->m[3];
    int32_t y;
    if (has[0] && has[2] && s->height.t == WL_LEN && auto_mt && auto_mb) {
        int32_t free = ch - ins[0] - ins[2] - b->h;
        y = ins[0] + (free > 0 ? free / 2 : 0);
    } else if (has[0]) y = ins[0] + b->m[0];
    else if (has[2]) y = ch - ins[2] - b->m[2] - b->h;
    else y = b->sy - (cb->pay + cy) + b->m[0];
    b->x = cx + x;
    b->y = cy + y;
    b->lparent = fixed ? -1 : cbi;
    if (fixed) { b->x = x; b->y = y; }
    b->pax = (fixed ? 0 : cb->pax) + b->x;
    b->pay = (fixed ? 0 : cb->pay) + b->y;
}

static void lay_abs_children(struct wlayout* L, int cbi) {
    int list = L->b[cbi].abs_head;
    L->b[cbi].abs_head = -1;
    // registration order is reversed: lay out in tree order
    int order[512], n = 0;
    for (int a = list; a >= 0 && n < 512; a = L->b[a].abs_next) order[n++] = a;
    for (int k = n - 1; k >= 0; k--) {
        int saved_base = L->fl_base, saved_n = L->nfl;
        L->fl_base = L->nfl;
        lay_one_abs(L, order[k], cbi);
        L->nfl = saved_n;
        L->fl_base = saved_base;
    }
}

// ---- block flow --------------------------------------------------------------------

static void lay_flow(struct wlayout* L, int bi, int32_t content_w, int32_t* content_h) {
    struct lbox* b = &L->b[bi];
    int32_t ctop = b->b[0] + b->p[0];
    int32_t cleft = b->b[3] + b->p[3];
    int32_t abs_ctop = b->pay + ctop;
    int32_t abs_cleft = b->pax + cleft;
    int collapse_top = top_collapsible(L, bi);
    int collapse_bot = bottom_collapsible(L, bi);
    int32_t cursor = 0;
    struct mpair pending = { 0, 0 };
    int placed = 0;
    int32_t cbh_for_children = -1;
    if (b->st->height.t == WL_LEN && (b->st->height.pct == 0 || b->cbh >= 0)) {
        cbh_for_children = wl_resolve(b->st->height, b->cbh >= 0 ? b->cbh : 0, 0);
        if (b->st->box_sizing == BX_BORDER) cbh_for_children -= b->p[0] + b->p[2] + b->b[0] + b->b[2];
    } else if (b->flags & BF_HEIGHT_DEF) {
        cbh_for_children = b->h - b->p[0] - b->p[2] - b->b[0] - b->b[2];
    }
    for (int c = b->first; c >= 0; c = L->b[c].next) {
        struct lbox* cb = &L->b[c];
        if (cb->flags & BF_ABS) {
            cb->sx = abs_cleft;
            cb->sy = abs_ctop + cursor + (placed || !collapse_top ? mp_val(pending) : 0);
            lay_abs_register(L, c);
            continue;
        }
        if (cb->flags & BF_FLOAT) {
            lb_resolve_box_sides(L, c, content_w);
            cb = &L->b[c];
            cb->cbh = cbh_for_children;
            int32_t fw = -1;
            if (cb->st->width.t != WL_LEN && cb->kind != LB_REPLACED && cb->fc != FC_TABLE)
                fw = lay_shrink_width(L, c, content_w - cb->m[1] - cb->m[3]);
            cb->pax = abs_cleft;
            cb->pay = abs_ctop + cursor;
            int saved_base = L->fl_base, saved_n = L->nfl;
            L->fl_base = L->nfl;
            lay_box(L, c, content_w, fw, -1);
            L->nfl = saved_n;
            L->fl_base = saved_base;
            int32_t y = abs_ctop + cursor + mp_val(pending);
            fl_place(L, c, abs_cleft, abs_cleft + content_w, &y);
            cb = &L->b[c];
            cb->x = cb->pax - b->pax;
            cb->y = cb->pay - b->pay;
            cb->lparent = bi;
            continue;
        }
        lb_resolve_box_sides(L, c, content_w);
        cb = &L->b[c];
        cb->cbh = cbh_for_children;
        struct mpair lead = leading_margin(L, c, content_w, 0);
        struct mpair m = mp_merge(pending, lead);
        int32_t cy;
        if (!placed && collapse_top) cy = 0;  // margin already applied outside us
        else cy = cursor + mp_val(m);
        if (cb->st->clear != CLR_NONE) {
            int32_t cl = fl_clear_y(L, abs_ctop + cy, cb->st->clear) - abs_ctop;
            if (cl > cy) cy = cl;
        }
        // BFC roots avoid floats: shrink available width
        int32_t avail = content_w, xoff = 0;
        if (lay_is_bfc(L, c) && L->nfl > L->fl_base) {
            int32_t l, r;
            fl_avail(L, abs_ctop + cy, PX(1), abs_cleft, abs_cleft + content_w, &l, &r);
            xoff = l - abs_cleft;
            avail = r - l;
        }
        // auto margins + width are resolved inside lay_box
        cb->pax = abs_cleft + xoff + cb->m[3];
        cb->pay = abs_ctop + cy;
        if (cb->flags & BF_COLLAPSE_TOP) {}
        lay_box(L, c, avail, -1, -1);
        cb = &L->b[c];
        cb->x = cleft + xoff + cb->m[3];
        cb->y = ctop + cy;
        cb->lparent = bi;
        if ((cb->flags & BF_EMPTY) && cb->h == 0) {
            // margins collapse through an empty block
            struct mpair mb = { cb->cm[2], cb->cm[3] };
            pending = mp_merge(m, mb);
            if (placed || !collapse_top) cb->y = ctop + cursor + mp_val(m) - mp_val(lead) + (lead.p + lead.n);
            continue;
        }
        if (b->baseline < 0 && cb->baseline >= 0) b->baseline = cb->y + cb->baseline;
        if (cb->last_baseline >= 0) b->last_baseline = cb->y + cb->last_baseline;
        cursor = cy + cb->h;
        struct mpair mb = { cb->cm[2], cb->cm[3] };
        pending = mb;
        placed = 1;
    }
    b = &L->b[bi];
    if (!placed) {
        // no in-flow content: margins may collapse through this box
        *content_h = 0;
        struct mpair own_t = mp_of(b->m[0]), own_b = mp_of(b->m[2]);
        struct mpair all = mp_merge(mp_merge(own_t, own_b), pending);
        if (collapse_top && collapse_bot && b->st->min_h.t != WL_LEN) {
            b->flags |= BF_EMPTY;
            b->cm[2] = all.p;
            b->cm[3] = all.n;
        } else {
            b->cm[2] = own_b.p;
            b->cm[3] = own_b.n;
        }
        if (lay_is_bfc(L, bi) && L->nfl > L->fl_base) {
            int32_t fb = fl_bottom(L) - abs_ctop;
            if (fb > *content_h) *content_h = fb;
        }
        return;
    }
    if (collapse_bot) {
        *content_h = cursor;
        struct mpair own = mp_of(b->m[2]);
        struct mpair out = mp_merge(own, pending);
        b->cm[2] = out.p;
        b->cm[3] = out.n;
    } else {
        *content_h = cursor + mp_val(pending);
        struct mpair own = mp_of(b->m[2]);
        b->cm[2] = own.p;
        b->cm[3] = own.n;
    }
    if (lay_is_bfc(L, bi) && L->nfl > L->fl_base) {
        int32_t fb = fl_bottom(L) - abs_ctop;
        if (fb > *content_h) *content_h = fb;
    }
}

// ---- generic box layout ------------------------------------------------------------

void lay_box(struct wlayout* L, int bi, int32_t avail, int32_t forced_w, int32_t forced_h) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    if (++L->depth > 300) { L->depth--; b->w = forced_w > 0 ? forced_w : 0; b->h = 0; return; }
    b->flags |= BF_LAID;
    b->baseline = b->last_baseline = -1;
    b->nfr = 0;
    int is_abs = (b->flags & BF_ABS) != 0;
    if (b->kind == LB_INLINE || b->kind == LB_TEXT) { L->depth--; return; }
    if (!(b->flags & (BF_FLOAT | BF_ABS))) lb_resolve_box_sides(L, bi, avail);
    b = &L->b[bi];
    int32_t extra_w = b->p[1] + b->p[3] + b->b[1] + b->b[3];
    int32_t extra_h = b->p[0] + b->p[2] + b->b[0] + b->b[2];
    int bb = s->box_sizing == BX_BORDER;
    // ---- width ----
    int32_t W; // border box
    if (b->kind == LB_REPLACED) {
        int32_t cw, ch;
        replaced_size(L, bi, avail, &cw, &ch);
        b = &L->b[bi];
        W = cw + extra_w;
        if (forced_w >= 0) W = forced_w;
        b->w = W;
        b->h = forced_h >= 0 ? forced_h : ch + extra_h;
        b->baseline = b->h; // replaced elements sit on their bottom margin edge
        if (b->rk == RK_INPUT_TEXT || b->rk == RK_INPUT_BUTTON || b->rk == RK_SELECT) {
            struct wfmetrics fm;
            lb_font_metrics(s, &fm);
            int32_t lh = lb_line_height(s);
            int32_t inner = b->h - extra_h;
            b->baseline = b->b[0] + b->p[0] + (inner - lh) / 2 + (lh - fm.ascent - fm.descent) / 2 + fm.ascent;
        }
        if (b->rk == RK_CHECKBOX || b->rk == RK_RADIO) b->baseline = b->h - PX(2);
        // auto margins centering (block-level)
        if (!(b->flags & (BF_INLINE_LEVEL | BF_FLOAT | BF_ABS))) {
            int aml = s->margin[3].t == WL_AUTO, amr = s->margin[1].t == WL_AUTO;
            int32_t free = avail - W - b->m[1] - b->m[3];
            if (aml && amr && free > 0) { b->m[3] = free / 2; b->m[1] = free - free / 2; }
            else if (aml && free > 0) b->m[3] = free;
        }
        b->cm[2] = b->m[2] > 0 ? b->m[2] : 0;
        b->cm[3] = b->m[2] < 0 ? b->m[2] : 0;
        if (b->rk == RK_INPUT_BUTTON || b->kind == LB_BLOCK) {}
        L->depth--;
        if (b->flags & BF_POSITIONED) lay_abs_children(L, bi);
        return;
    }
    if (b->fc == FC_TABLE) {
        int32_t tw, th;
        int32_t av = avail - b->m[1] - b->m[3];
        if (forced_w >= 0) av = forced_w;
        lay_table(L, bi, av, &tw, &th);
        b = &L->b[bi];
        if (forced_w >= 0 && tw < forced_w) tw = forced_w;
        b->w = tw;
        b->h = th;
        if (forced_h >= 0 && forced_h > b->h) b->h = forced_h;
        // centering via auto margins
        if (!(b->flags & (BF_INLINE_LEVEL | BF_FLOAT | BF_ABS))) {
            int aml = s->margin[3].t == WL_AUTO, amr = s->margin[1].t == WL_AUTO;
            int32_t free = avail - b->w - b->m[1] - b->m[3];
            if (aml && amr && free > 0) { b->m[3] = free / 2; b->m[1] = free - free / 2; }
            else if (aml && free > 0) b->m[3] = free;
        }
        b->cm[2] = b->m[2] > 0 ? b->m[2] : 0;
        b->cm[3] = b->m[2] < 0 ? b->m[2] : 0;
        L->depth--;
        if (b->flags & BF_POSITIONED) lay_abs_children(L, bi);
        return;
    }
    if (forced_w >= 0) {
        W = forced_w;
    } else if (s->width.t == WL_LEN) {
        W = wl_resolve(s->width, avail, 0) + (bb ? 0 : extra_w);
    } else if (s->width.t == WL_MIN || s->width.t == WL_MAX || s->width.t == WL_FIT) {
        int32_t mn, mx;
        lay_intrinsic(L, bi, &mn, &mx);
        W = s->width.t == WL_MIN ? mn : s->width.t == WL_MAX ? mx : lay_shrink_width(L, bi, avail - b->m[1] - b->m[3]);
    } else if (b->flags & (BF_INLINE_LEVEL | BF_FLOAT | BF_ABS)) {
        W = lay_shrink_width(L, bi, avail - b->m[1] - b->m[3]);
    } else {
        W = avail - b->m[1] - b->m[3];
    }
    if (forced_w < 0) {
        if (s->max_w.t == WL_LEN) {
            int32_t mx = wl_resolve(s->max_w, avail, 0) + (bb ? 0 : extra_w);
            if (W > mx) W = mx;
        }
        if (s->min_w.t == WL_LEN) {
            int32_t mn = wl_resolve(s->min_w, avail, 0) + (bb ? 0 : extra_w);
            if (W < mn) W = mn;
        }
    }
    if (W < extra_w) W = extra_w;
    // auto margins (block-level in normal flow)
    if (!(b->flags & (BF_INLINE_LEVEL | BF_FLOAT | BF_ABS)) && forced_w < 0) {
        int aml = s->margin[3].t == WL_AUTO, amr = s->margin[1].t == WL_AUTO;
        int32_t free = avail - W - b->m[1] - b->m[3];
        if (aml && amr) { if (free > 0) { b->m[3] = free / 2; b->m[1] = free - free / 2; } }
        else if (aml) { if (free > 0) b->m[3] = free; }
        else if (amr) { if (free > 0) b->m[1] = free; }
    }
    b->w = W;
    int32_t content_w = W - extra_w;
    // ---- definite height (for % children and stretch) ----
    int32_t H = -1;
    if (forced_h >= 0) H = forced_h;
    else if (s->height.t == WL_LEN && (s->height.pct == 0 || b->cbh >= 0)) {
        H = wl_resolve(s->height, b->cbh >= 0 ? b->cbh : 0, 0) + (bb ? 0 : extra_h);
    } else if (is_abs && (b->flags & BF_HEIGHT_DEF)) H = b->h;
    if (H >= 0) {
        b->h = H;
        b->flags |= BF_HEIGHT_DEF;
    } else b->flags &= ~BF_HEIGHT_DEF;
    // ---- content ----
    int bfc = lay_is_bfc(L, bi);
    int saved_base = L->fl_base, saved_n = L->nfl;
    if (bfc) L->fl_base = L->nfl;
    int32_t content_h = 0;
    switch (b->fc) {
    case FC_FLEX: lay_flex(L, bi, content_w, &content_h); break;
    case FC_GRID: lay_grid(L, bi, content_w, &content_h); break;
    default:
        if (b->flags & BF_IFC) lay_ifc(L, bi, content_w, &content_h);
        else lay_flow(L, bi, content_w, &content_h);
        break;
    }
    b = &L->b[bi];
    if (bfc) {
        if (L->nfl > L->fl_base) {
            int32_t fb = fl_bottom(L) - (b->pay + b->b[0] + b->p[0]);
            if (fb > content_h) content_h = fb;
        }
        L->nfl = saved_n;
        L->fl_base = saved_base;
    }
    if (b->flags & BF_IFC) {
        b->cm[2] = b->m[2] > 0 ? b->m[2] : 0;
        b->cm[3] = b->m[2] < 0 ? b->m[2] : 0;
        if (content_h == 0 && b->nfr == 0 && top_collapsible(L, bi) == 0 && s->min_h.t != WL_LEN &&
            extra_h == 0 && s->height.t != WL_LEN) {
            // an anonymous/inline-only block that produced no lines
            b->flags |= BF_EMPTY;
            struct mpair a = mp_merge(mp_of(b->m[0]), mp_of(b->m[2]));
            b->cm[2] = a.p;
            b->cm[3] = a.n;
        }
    } else if (b->fc != FC_FLOW && b->fc != FC_CELL && b->fc != FC_CAPTION) {
        b->cm[2] = b->m[2] > 0 ? b->m[2] : 0;
        b->cm[3] = b->m[2] < 0 ? b->m[2] : 0;
    }
    if (H < 0) {
        H = content_h + extra_h;
        if (s->min_h.t == WL_LEN && (s->min_h.pct == 0 || b->cbh >= 0)) {
            int32_t mn = wl_resolve(s->min_h, b->cbh >= 0 ? b->cbh : 0, 0) + (bb ? 0 : extra_h);
            if (H < mn) H = mn;
        }
        if (s->max_h.t == WL_LEN && (s->max_h.pct == 0 || b->cbh >= 0)) {
            int32_t mx = wl_resolve(s->max_h, b->cbh >= 0 ? b->cbh : 0, 0) + (bb ? 0 : extra_h);
            if (H > mx) H = mx;
        }
        b->h = H;
        if (H > 0) b->flags &= ~BF_EMPTY;
    } else {
        if (s->min_h.t == WL_LEN && s->min_h.pct == 0) {
            int32_t mn = s->min_h.px + (bb ? 0 : extra_h);
            if (b->h < mn) b->h = mn;
        }
    }
    if (b->baseline >= 0 && (b->flags & BF_IFC)) {} // set by lay_ifc
    if (b->h < 0) b->h = 0;
    L->depth--;
    if (b->flags & BF_POSITIONED) lay_abs_children(L, bi);
}

// ---- finalize ------------------------------------------------------------------------

static void finalize(struct wlayout* L) {
    int n = L->nb;
    // DFS order (lparent is always an ancestor or -1)
    int* stack = (int*)w_malloc((n + 1) * sizeof(int));
    if (!stack) return;
    int sp = 0;
    stack[sp++] = L->root;
    int32_t maxb = PX(L->vh), maxr = PX(L->vw);
    while (sp) {
        int bi = stack[--sp];
        struct lbox* b = &L->b[bi];
        int32_t px = 0, py = 0;
        if (b->lparent >= 0) { px = L->b[b->lparent].ax; py = L->b[b->lparent].ay; }
        int32_t x = b->x, y = b->y;
        const struct wstyle* s = b->st;
        if (s && (b->kind == LB_BLOCK || b->kind == LB_REPLACED)) {
            if (s->position == POS_RELATIVE || s->position == POS_STICKY) {
                int32_t cw = b->lparent >= 0 ? lay_content_w(L, b->lparent) : PX(L->vw);
                if (s->inset[3].t == WL_LEN) x += wl_resolve(s->inset[3], cw, 0);
                else if (s->inset[1].t == WL_LEN) x -= wl_resolve(s->inset[1], cw, 0);
                if (s->inset[0].t == WL_LEN && s->position == POS_RELATIVE) y += wl_resolve(s->inset[0], 0, 0);
                else if (s->inset[2].t == WL_LEN && s->position == POS_RELATIVE) y -= wl_resolve(s->inset[2], 0, 0);
            }
            if (s->translate_x.px || s->translate_x.pct) x += wl_resolve(s->translate_x, b->w, 0);
            if (s->translate_y.px || s->translate_y.pct) y += wl_resolve(s->translate_y, b->h, 0);
        }
        b->ax = px + x;
        b->ay = py + y;
        if (!(b->flags & BF_FIXED) && b->kind != LB_INLINE && b->kind != LB_TEXT &&
            (b->flags & BF_LAID)) {
            if (b->ay + b->h > maxb && b->h < PX(200000)) maxb = b->ay + b->h;
            if (b->ax + b->w > maxr && b->w < PX(100000)) maxr = b->ax + b->w;
        }
        // push children in reverse so they pop in order
        int cnt = 0;
        for (int c = b->first; c >= 0; c = L->b[c].next) cnt++;
        if (sp + cnt > n) break;
        int k = sp + cnt;
        for (int c = b->first; c >= 0; c = L->b[c].next) stack[--k] = c;
        sp += cnt;
    }
    w_free(stack);
    L->doc_h = maxb;
    L->doc_w = maxr;
}

// ---- driver ---------------------------------------------------------------------------

struct wlayout* wlay_run(struct wdom* d, struct wstyleset* ss, int vw, int vh,
                         const struct wlay_images* imgs) {
    struct wlayout* L = (struct wlayout*)w_calloc(1, sizeof(struct wlayout));
    if (!L) return 0;
    L->d = d;
    L->ss = ss;
    L->imgs = imgs;
    L->vw = vw;
    L->vh = vh;
    L->alpha = 255;
    L->node_box = (int32_t*)w_malloc((d->nn + 1) * sizeof(int32_t));
    if (!L->node_box) { w_free(L); return 0; }
    for (int i = 0; i <= d->nn; i++) L->node_box[i] = -1;
    if (lay_build_tree(L) < 0) return L;
    // replaced: intrinsic image sizes
    for (int i = 0; i < L->nb; i++) {
        struct lbox* b = &L->b[i];
        if (b->kind == LB_REPLACED && (b->rk == RK_IMG || b->rk == RK_VIDEO) && imgs && imgs->for_node) {
            int w = 0, h = 0;
            b->img = imgs->for_node(imgs->ctx, b->node, &w, &h);
            b->iw = w;
            b->ih = h;
        }
    }
    struct lbox* r = &L->b[L->root];
    r->w = PX(vw);
    r->h = PX(vh);
    r->pax = r->pay = 0;
    r->cbh = PX(vh);
    // the root element
    int html = r->first;
    int32_t ch = 0;
    if (html >= 0) {
        struct lbox* hb = &L->b[html];
        hb->cbh = PX(vh);
        lb_resolve_box_sides(L, html, PX(vw));
        hb = &L->b[html];
        hb->pax = hb->m[3];
        hb->pay = hb->m[0];
        lay_box(L, html, PX(vw), -1, -1);
        hb = &L->b[html];
        hb->x = hb->m[3];
        hb->y = hb->m[0];
        hb->lparent = L->root;
        ch = hb->y + hb->h + hb->m[2];
    }
    r = &L->b[L->root];
    r->h = ch > PX(vh) ? ch : PX(vh);
    r->flags |= BF_LAID;
    lay_abs_children(L, L->root);
    finalize(L);
    // canvas background: root element, else body (propagation)
    L->canvas_bg = 0xFFFFFFFF;
    if (html >= 0) {
        const struct wstyle* hs = L->b[html].st;
        if ((hs->bg_color >> 24) || hs->bg_grad >= 0 || hs->bg_image) L->canvas_bg = hs->bg_color;
        else if (d->body >= 0) {
            const struct wstyle* bs = css_style_of(ss, d->body);
            if (bs && (bs->bg_color >> 24)) L->canvas_bg = bs->bg_color;
        }
        if ((L->canvas_bg >> 24) == 0) L->canvas_bg = 0xFFFFFFFF;
        else if ((L->canvas_bg >> 24) < 255) {
            // blend translucent canvas over white
            L->canvas_bg = 0xFF000000 | ws_blend(0xFFFFFF, L->canvas_bg, L->canvas_bg >> 24);
        }
    }
    lay_emit(L);
    return L;
}

void wlay_free(struct wlayout* L) {
    if (!L) return;
    w_free(L->b); w_free(L->fr); wbuf_free(&L->text); w_free(L->di); w_free(L->bord);
    w_free(L->dtx); w_free(L->dsh); w_free(L->dbg); w_free(L->rad); w_free(L->hits);
    w_free(L->node_box); w_free(L->fl);
    for (int i = 0; i < L->nanon_chunk; i++) w_free(L->anon_chunk[i]);
    w_free(L);
}

int wlay_doc_height(const struct wlayout* L) { return LU_ROUND(L->doc_h); }
int wlay_doc_width(const struct wlayout* L) { return LU_ROUND(L->doc_w); }
uint32_t wlay_canvas_bg(const struct wlayout* L) { return L->canvas_bg; }
int wlay_items(const struct wlayout* L, const struct ditem** items) { *items = L->di; return L->ndi; }
const struct dborder* wlay_border(const struct wlayout* L, int ref) { return &L->bord[ref]; }
const struct dtext* wlay_text_ref(const struct wlayout* L, int ref) { return &L->dtx[ref]; }
const struct dshadow* wlay_shadow(const struct wlayout* L, int ref) { return &L->dsh[ref]; }
const struct dbgimg* wlay_bgimg(const struct wlayout* L, int ref) { return &L->dbg[ref]; }
const int32_t* wlay_radii(const struct wlayout* L, int ref) { return L->rad[ref]; }
const char* wlay_text(const struct wlayout* L) { return L->text.p; }
int wlay_hits(const struct wlayout* L, const struct dhit** hits) { *hits = L->hits; return L->nhits; }

int wlay_node_rect(const struct wlayout* L, int node, int32_t* x, int32_t* y, int32_t* w, int32_t* h) {
    if (node < 0 || node >= L->d->nn) return 0;
    int b = L->node_box[node];
    if (b < 0) return 0;
    *x = L->b[b].ax; *y = L->b[b].ay; *w = L->b[b].w; *h = L->b[b].h;
    return 1;
}

void wlay_stats(const struct wlayout* L, int* boxes, int* items, int* lines) {
    *boxes = L->nb;
    *items = L->ndi;
    *lines = L->nlines;
}

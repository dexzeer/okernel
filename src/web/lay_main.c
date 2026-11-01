// Layout core: boxes, block flow, margins, floats, positioning, finalize.
#include "lay_int.h"

static int lay_wm_of(const struct lbox* b);
static int lay_pwm_of(struct wlayout* L, const struct lbox* b);
static int lay_orth_swap(struct wlayout* L, const struct lbox* b);

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
    b->pst = st;
    b->st_in = st;
    b->kind = (uint8_t)kind;
    b->parent = b->first = b->last = b->next = b->prev = -1;
    b->lparent = -1;
    b->baseline = b->last_baseline = -1;
    b->img = -1;
    b->imin = b->imax = -1;
    b->abs_head = b->abs_next = -1;
    b->cbh = -1;
    b->mo_ok = 0;
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
    f->webfont = (int16_t)st->webfont;
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
    if (b->pst && lay_wm_of(b) != lay_pwm_of(L, b)) return 1;            // an orthogonal flow is independent
    if (s->display == D_FLOW_ROOT) return 1;
    if (s->contain & (CT_LAYOUT | CT_PAINT)) return 1;                  // layout/paint containment: independent formatting context
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
static int __attribute__((unused)) last_inflow(struct wlayout* L, int bi) {
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
    // a definite height blocks the collapse only when it is nonzero: a
    // zero-height box's top and bottom edges adjoin, so it collapses through
    if (b->st->height.t == WL_LEN && (b->st->height.px != 0 || b->st->height.pct != 0)) return 0;
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
    struct mpair cm = leading_margin(L, c, cbw, depth + 1);
    // margin-trim: the collapsed child margin adjoining this container's top
    // content edge is trimmed (at whichever box has the property)
    if (b->st->margin_trim & MT_BLOCK_START) cm.p = 0;
    return mp_merge(m, cm);
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

// cbw < 0: indefinite containing block (intrinsic sizing) — percentage
// width / min-width / max-width then behave as auto / none (CSS Sizing 3
// section 5.2.1c); the caller zeroes the min-content contribution instead.
// The height a percentage height of box bi resolves against while its container has no height yet (intrinsic sizing): the specified height
// of the nearest ancestor that has one, looking through auto-height blocks and through stretched row flex items. -1 = none.
static int32_t pct_height_basis(struct wlayout* L, int bi) {
    int c = bi;
    for (int hop = 0; hop < 8; hop++) {
        int p = L->b[c].parent;
        if (p < 0) return -1;
        const struct lbox* pb = &L->b[p];
        if (!pb->st) return -1;
        int definite = pb->st->height.t == WL_LEN && !pb->st->height.pct;
        int32_t pct_h = -1;
        if (!definite && pb->st->height.t == WL_LEN && pb->st->height.pct && !(pb->flags & BF_ABS)) {
            int32_t base = pb->cbh >= 0 ? pb->cbh : pct_height_basis(L, p);          // a percentage of a definite containing block height
            if (base >= 0) { pct_h = wl_resolve(pb->st->height, base, 0); definite = 1; }
        }
        if (pb->fc == FC_FLEX && c != bi) {
            const struct wstyle* cs = L->b[c].st;
            int al = cs->align_self == AL_AUTO ? pb->st->align_items : cs->align_self;
            int row = pb->st->flex_direction == FD_ROW || pb->st->flex_direction == FD_ROW_REV;
            if (!definite || !row || cs->height.t == WL_LEN || (al != AL_STRETCH && al != AL_NORMAL)) return -1;
        }
        if (!definite && pb->st->height.t == WL_AUTO && pb->st->aspect_ratio > 0 && pb->st->width.t == WL_LEN && !pb->st->width.pct && !(pb->flags & BF_ABS)) {
            // a definite width and a ratio make the height definite
            int32_t cw = pb->st->width.px;
            if (pb->st->box_sizing == BX_BORDER) cw -= pb->p[1] + pb->p[3] + pb->b[1] + pb->b[3];
            pct_h = w_muldiv(cw < 0 ? 0 : cw, 1000, pb->st->aspect_ratio);
            if (pb->st->box_sizing == BX_BORDER) pct_h += pb->p[0] + pb->p[2] + pb->b[0] + pb->b[2];
            definite = 1;
        }
        if (definite) {
            int32_t h = pct_h >= 0 ? pct_h : pb->st->height.px;
            if (pb->st->box_sizing == BX_BORDER) { h -= pb->p[0] + pb->p[2] + pb->b[0] + pb->b[2]; if (h < 0) h = 0; }
            return h;
        }
        if (pb->st->height.t != WL_AUTO || pb->kind != LB_BLOCK) return -1;
        if (pb->fc != FC_FLOW) return -1;
        c = p;
    }
    return -1;
}

static void replaced_size(struct wlayout* L, int bi, int32_t cbw, int32_t* cw, int32_t* ch) {
    int indef = cbw < 0;
    if (indef) cbw = 0;
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    // the height a percentage resolves against: the container's, or (while sizing intrinsically) its specified one
    int32_t cbh = b->cbh;
    if (cbh < 0) cbh = pct_height_basis(L, bi);
    struct wfmetrics fm;
    lb_font_metrics(s, &fm);
    int32_t fs = s->font_size;
    int32_t iw = 0, ih = 0;    // intrinsic LU
    int32_t ratio = 0;          // w/h * 1000
    switch (b->rk) {
    case RK_IMG: case RK_VIDEO: case RK_CANVAS: case RK_IFRAME: case RK_OBJECT: case RK_SVG: {
        if (b->iw > 0 && b->ih > 0) { iw = PX(b->iw); ih = PX(b->ih); ratio = w_muldiv(b->iw, 1000, b->ih); }
        if (b->rk == RK_IMG && b->img >= 0 && L->imgs && L->imgs->info) {
            // an SVG image may lack a width and/or height (or both) of its own: CSS Images 3 default object size (300x150) and its ratio
            int dw, dh; int32_t rt;
            if (L->imgs->info(L->imgs->ctx, b->img, &dw, &dh, &rt) && (dw <= 0 || dh <= 0)) {
                int32_t r1000 = rt > 0 ? (int32_t)w_div64((int64_t)rt * 1000, 65536) : 0;
                ratio = r1000;
                if (dw > 0) { iw = PX(dw); ih = r1000 ? w_muldiv(iw, 1000, r1000) : PX(150); }
                else if (dh > 0) { ih = PX(dh); iw = r1000 ? w_muldiv(ih, r1000, 1000) : PX(300); }
                else if (r1000) {
                    if (r1000 >= 2000) { iw = PX(300); ih = w_muldiv(iw, 1000, r1000); }
                    else { ih = PX(150); iw = w_muldiv(ih, r1000, 1000); }
                } else { iw = PX(300); ih = PX(150); }
            }
        }
        if (b->rk == RK_VIDEO || b->rk == RK_CANVAS || b->rk == RK_IFRAME || b->rk == RK_OBJECT) {
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
    if (s->width.t == WL_LEN && !(indef && s->width.pct)) {
        W = wl_resolve(s->width, cbw, 0);
        if (bb) W -= box_extra_w;
        if (W < 0) W = 0;
    }
    if (s->height.t == WL_LEN && (s->height.pct == 0 || cbh >= 0)) {
        H = wl_resolve(s->height, cbh >= 0 ? cbh : 0, 0);
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
    if (s->max_w.t == WL_LEN && !(indef && s->max_w.pct)) {
        int32_t mx = wl_resolve(s->max_w, cbw, 0) - (bb ? box_extra_w : 0);
        if (W > mx) { if (ratio && s->height.t != WL_LEN) H = w_muldiv(mx, 1000, ratio); W = mx; }
    }
    if (s->min_w.t == WL_LEN && !(indef && s->min_w.pct)) {
        int32_t mn = wl_resolve(s->min_w, cbw, 0) - (bb ? box_extra_w : 0);
        if (W < mn) W = mn;
    }
    if (s->max_h.t == WL_LEN && (s->max_h.pct == 0 || cbh >= 0)) {
        int32_t mx = wl_resolve(s->max_h, cbh >= 0 ? cbh : 0, 0) - (bb ? box_extra_h : 0);
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

// min-width / max-width given as min-content, max-content or fit-content(...): the border-box width, or -1 when l is a plain length
static int32_t size_kw(struct wlayout* L, int bi, struct wlen l, int32_t avail) {
    if (l.t != WL_MIN && l.t != WL_MAX && l.t != WL_FIT) return -1;
    int32_t mn, mx;
    lay_intrinsic_content(L, bi, &mn, &mx);
    if (l.t == WL_MIN) return mn;
    if (l.t == WL_MAX) return mx;
    struct lbox* b = &L->b[bi];
    int32_t extra = b->p[1] + b->p[3] + b->b[1] + b->b[3];
    int32_t a = avail < 0 ? mx : avail;       // avail < 0: no definite containing block (an intrinsic size): a percentage or plain fit-content acts as max-content
    if (l.pct & WL_FIT_ARG) {
        struct wlen al = { l.px, l.pct & ~WL_FIT_ARG, WL_LEN };
        if (avail >= 0 || !al.pct) a = wl_resolve(al, avail < 0 ? 0 : avail, 0) + (b->st->box_sizing == BX_BORDER ? 0 : extra);
    }
    if (a > mx) a = mx;
    if (a < mn) a = mn;
    return a;
}

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

// ignore_w: size by the content only, as if `width` were auto (the content-based automatic minimum of a flex item): not cached
static void intrinsic_impl(struct wlayout* L, int bi, int32_t* mn, int32_t* mx, int ignore_w) {
    struct lbox* b = &L->b[bi];
    if (!ignore_w && b->imin >= 0) { *mn = b->imin; *mx = b->imax; return; }
    if (b->kind == LB_BLOCK && !(b->_pad & 1) && lay_orth_swap(L, b)) {
        // an orthogonal flow: its width in the parent's frame is its block size, known only by laying it out
        lay_box(L, bi, -1, -1, -1);
        b = &L->b[bi];
        *mn = *mx = b->w;
        if (!ignore_w) { b->imin = b->imax = b->w; }
        return;
    }
    lb_resolve_box_sides(L, bi, 0);
    b = &L->b[bi];
    int32_t extra = b->p[1] + b->p[3] + b->b[1] + b->b[3];
    int32_t a = 0, z = 0;
    int ok;
    int32_t fw = fixed_width(L, bi, &ok);
    if (ignore_w) ok = 0;
    if (b->kind == LB_REPLACED && !(b->kind == LB_BLOCK)) {
        int32_t cw, ch;
        replaced_size(L, bi, -1, &cw, &ch);
        b = &L->b[bi];
        a = z = cw + extra;
        // compressible: a percentage width / max-width gives a zero
        // min-content contribution (max-content keeps the natural size)
        if ((b->st->width.t == WL_LEN && b->st->width.pct) ||
            (b->st->max_w.t == WL_LEN && b->st->max_w.pct)) a = 0;
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
        if (b->st->contain & (CT_SIZE | CT_INLINE_SIZE)) cmn = cmx = b->st->cis_w;       // size containment: the content has no say
        else if (!ignore_w && b->st->aspect_ratio > 0 && b->st->height.t == WL_LEN && !b->st->height.pct && b->st->display != D_TABLE_CELL && b->st->display != D_TABLE_ROW) {
            // a definite height and a ratio give the width (never below the content's min-content)
            int32_t hh = b->st->height.px;
            int32_t bw = b->st->box_sizing == BX_BORDER ? w_muldiv(hh, b->st->aspect_ratio, 1000) - extra : w_muldiv(hh, b->st->aspect_ratio, 1000);
            if (bw < cmn) bw = cmn;
            if (bw < 0) bw = 0;
            cmx = bw;
            if (b->st->overflow_x == OV_VISIBLE) cmn = bw;      // a scroll container is compressible below the transferred size
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
    if (!ignore_w && b->kind == LB_BLOCK) {
        if (s->max_w.t >= WL_MIN && s->max_w.t <= WL_FIT) { int32_t m = size_kw(L, bi, s->max_w, -1); b = &L->b[bi]; if (m >= 0) { if (z > m) z = m; if (a > m) a = m; } }
        if (s->min_w.t >= WL_MIN && s->min_w.t <= WL_FIT) { int32_t m = size_kw(L, bi, s->min_w, -1); b = &L->b[bi]; if (m >= 0) { if (a < m) a = m; if (z < m) z = m; } }
    }
    if (z < a) z = a;
    if (!ignore_w) { b->imin = a; b->imax = z; }
    *mn = a;
    *mx = z;
}

void lay_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx) { intrinsic_impl(L, bi, mn, mx, 0); }
void lay_intrinsic_content(struct wlayout* L, int bi, int32_t* mn, int32_t* mx) { intrinsic_impl(L, bi, mn, mx, 1); }

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
            if ((L->b[p].flags & BF_POSITIONED) && (L->b[p].kind == LB_BLOCK || L->b[p].kind == LB_REPLACED || L->b[p].kind == LB_INLINE)) { cb = p; break; }
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
    int ar_w = s->aspect_ratio > 0 && s->height.t == WL_LEN && !(has[1] && has[3]) && !(s->contain & CT_SIZE);   // width follows the definite height (lay_box)
    if (s->width.t == WL_AUTO && b->kind != LB_REPLACED && b->fc != FC_TABLE && !ar_w) {
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
    // an abspos child of a flex container sits where a lone flex item would: justify-content on the main axis, align-self across
    int fx_al = 0, fy_al = 0;       // 0 start, 1 center, 2 end
    if (b->parent >= 0 && L->b[b->parent].fc == FC_FLEX && L->b[b->parent].pst && L->b[b->parent].pst->wmode == WM_HTB) {
        const struct wstyle* ps = L->b[b->parent].pst;
        int row = ps->flex_direction == FD_ROW || ps->flex_direction == FD_ROW_REV;
        int rev = (ps->flex_direction == FD_ROW_REV || ps->flex_direction == FD_COL_REV) != (row && ps->direction_rtl);
        int jc = ps->justify_content == AL_CENTER ? 1 : (ps->justify_content == AL_END || ps->justify_content == AL_RIGHT) ? 2 : 0;
        if (rev && jc != 1) jc = jc == 0 ? 2 : 0;
        int as = s->align_self == AL_AUTO ? ps->align_items : s->align_self;
        int ca = as == AL_CENTER ? 1 : as == AL_END ? 2 : 0;
        if (ps->flex_wrap == FW_WRAP_REV && ca != 1) ca = ca == 0 ? 2 : 0;
        fx_al = row ? jc : ca;
        fy_al = row ? ca : jc;
    }
    // horizontal position
    int32_t x;
    if (has[3] && has[1] && s->width.t != WL_AUTO && auto_ml && auto_mr) {
        int32_t free = cw - ins[3] - ins[1] - b->w;
        x = ins[3] + (free > 0 ? free / 2 : 0);
    } else if (has[3]) x = ins[3] + b->m[3];
    else if (has[1]) x = cw - ins[1] - b->m[1] - b->w;
    else if (b->_pad & 2) x = b->sx - (cb->pax + cx) - b->w - b->m[1];       // the static position is a right edge (rtl)
    else {
        x = b->sx - (cb->pax + cx) + b->m[3];
        if (fx_al) {
            const struct lbox* pb = &L->b[b->parent];
            int32_t free = pb->w - pb->b[1] - pb->b[3] - pb->p[1] - pb->p[3] - b->w - b->m[1] - b->m[3];
            if (free > 0) x += fx_al == 1 ? free / 2 : free;
        }
    }
    int32_t y;
    if (has[0] && has[2] && s->height.t != WL_AUTO && auto_mt && auto_mb) {
        int32_t free = ch - ins[0] - ins[2] - b->h;
        y = ins[0] + (free > 0 ? free / 2 : 0);
    } else if (has[0]) y = ins[0] + b->m[0];
    else if (has[2]) y = ch - ins[2] - b->m[2] - b->h;
    else {
        y = b->sy - (cb->pay + cy) + b->m[0];
        if (fy_al) {
            const struct lbox* pb = &L->b[b->parent];
            int32_t free = pb->h - pb->b[0] - pb->b[2] - pb->p[0] - pb->p[2] - b->h - b->m[0] - b->m[2];
            if (free > 0) y += fy_al == 1 ? free / 2 : free;
        }
    }
    b->x = cx + x;
    b->y = cy + y;
    b->lparent = fixed ? -1 : cbi;
    if (fixed) { b->x = x; b->y = y; }
    b->pax = (fixed ? 0 : cb->pax) + b->x;
    b->pay = (fixed ? 0 : cb->pay) + b->y;
}

// Absolute boxes whose containing block is a positioned INLINE box (the padding box of its line fragments' union): laid out once the
// inline formatting context has its fragments. root = the context's block container (absolute boxes end up positioned against it).
void lay_abs_inline(struct wlayout* L, int root) {
    struct lbox* rb = &L->b[root];
    int32_t ox = rb->b[3] + rb->p[3], oy = rb->b[0] + rb->p[0];
    // iterative walk over the root's inline descendants
    int stack[256], sp = 0;
    for (int c = rb->first; c >= 0 && sp < 255; c = L->b[c].next) stack[sp++] = c;
    while (sp > 0) {
        int ib = stack[--sp];
        struct lbox* b = &L->b[ib];
        if (b->kind != LB_INLINE) continue;
        for (int c = b->first; c >= 0 && sp < 255; c = L->b[c].next) stack[sp++] = c;
        if (b->abs_head < 0 || !(b->flags & BF_POSITIONED)) continue;
        int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (int i = rb->fr0; i < rb->fr0 + rb->nfr && i < L->nfr; i++) {
            const struct lfrag* f = &L->fr[i];
            if (f->kind != FR_IBOX || f->box != ib) continue;
            if (f->x < x0) x0 = f->x;
            if (f->y < y0) y0 = f->y;
            if (f->x + f->w > x1) x1 = f->x + f->w;
            if (f->y + f->h > y1) y1 = f->y + f->h;
        }
        if (x1 < x0 || y1 < y0) { x0 = 0; y0 = 0; x1 = 0; y1 = 0; }
        int list[512], n = 0;
        for (int a = b->abs_head; a >= 0 && n < 512; a = L->b[a].abs_next) list[n++] = a;
        b->pax = rb->pax + ox + x0;
        b->pay = rb->pay + oy + y0;
        b->w = x1 - x0;
        b->h = y1 - y0;
        lay_abs_children(L, ib);
        for (int k = 0; k < n; k++) {
            struct lbox* ab = &L->b[list[k]];
            if (ab->st->position == POS_FIXED) continue;
            ab->lparent = root;
            ab->x = ab->pax - rb->pax;
            ab->y = ab->pay - rb->pay;
        }
    }
}

static void lay_abs_children(struct wlayout* L, int cbi) {
    // Laying out an absolute box can register more boxes with the same containing block: a fixed (or root-anchored) box
    // nested inside an absolutely positioned one reaches the ICB only now, long after the first list was taken. A
    // few rounds pick those up (YouTube's <ytd-app> is position:absolute and its masthead is fixed inside it).
    for (int round = 0; round < 8 && L->b[cbi].abs_head >= 0; round++) {
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
    L->b[cbi].abs_head = -1;
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
        if (L->clamp_left == 0 && !L->clamp_skip) { lay_clamp_hide(L, c); continue; }   // line-clamp: everything after the clamp point is hidden
        if (cb->flags & BF_ABS) {
            {
                // the static position's inline edge: the start edge of the block (the right one in rtl); an inline-level box sits on
                // its own line there, so text-align decides
                const struct wstyle* ps = b->pst;
                int ta = ps->text_align, rtl = ps->direction_rtl && ps->wmode == WM_HTB;
                int right = rtl;
                if (cb->pst->abs_inl) right = ta == TA_RIGHT || (ta == TA_START && rtl) || (ta == TA_END && !rtl);
                cb->sx = abs_cleft + (right ? b->w - b->b[1] - b->b[3] - b->p[1] - b->p[3] : 0);
                if (right) cb->_pad |= 2; else cb->_pad &= (uint16_t)~2;
            }
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
        // margin-trim: the leading margin of the first in-flow children
        // adjoins the container's top content edge and is trimmed (the
        // collapse-through case is also handled in leading_margin itself)
        if (!placed && (L->b[bi].st->margin_trim & MT_BLOCK_START)) lead.p = 0;
        struct mpair m = mp_merge(pending, lead);
        int32_t cy;
        if (!placed && collapse_top) cy = 0;  // margin already applied outside us
        else cy = cursor + mp_val(m);
        if (cb->st->clear != CLR_NONE) {
            int32_t cl = fl_clear_y(L, abs_ctop + cy, cb->st->clear) - abs_ctop;
            if (cl > cy) cy = cl;
        }
        // BFC roots avoid floats: shrink available width — or drop below the
        // floats when the border box would overlap one. Beside a float the
        // box's margin box fills the band, so a negative inline margin props
        // the border box up into the float, which CSS2.1 9.5 forbids.
        int32_t avail = content_w, xoff = 0;
        if (lay_is_bfc(L, c) && L->nfl > L->fl_base) {
            int32_t y = abs_ctop + cy;
            int32_t l = abs_cleft, r = abs_cleft + content_w;
            for (int guard = 0; guard < 40; guard++) {
                fl_avail(L, y, PX(1), abs_cleft, abs_cleft + content_w, &l, &r);
                int auto_w = wl_is_auto(cb->st->width);
                int fits = r - l >= PX(1) &&
                           !(l > abs_cleft && cb->m[3] < 0) &&
                           !(auto_w && r < abs_cleft + content_w && cb->m[1] < 0);
                if (fits) break;
                int32_t nb = INT32_MAX;
                for (int i = L->fl_base; i < L->nfl; i++) {
                    struct bfloat* f = &L->fl[i];
                    if (f->y + f->h > y && f->y + f->h < nb) nb = f->y + f->h;
                }
                if (nb == INT32_MAX) break;
                y = nb;
            }
            if (y != abs_ctop + cy) cy = y - abs_ctop;
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
    // margin-trim: the trailing margin of the last in-flow child is trimmed
    if (placed && (b->st->margin_trim & MT_BLOCK_END)) { pending.p = 0; pending.n = 0; }
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

static void lay_box_impl(struct wlayout* L, int bi, int32_t avail, int32_t forced_w, int32_t forced_h);

// ---- writing modes: logical frames ----------------------------------------------------------------
// A box in a vertical writing mode is laid out in its own logical frame (x = inline axis, y = block axis) by the ordinary
// horizontal code, reading a copy of its style whose physical properties are swapped to logical ones. Where a box's frame differs
// from its parent's, the finished subtree is transformed into the parent's frame (wm_xform), so a parent always sees its children
// in its own frame. After layout everything is physical, and painting / finalize read pst.
struct wmf { uint8_t swap, fx, fy; };       // physical -> logical: swap the axes, then flip x, then flip y

// A frame code is the writing mode, plus 4 for a vertical mode with direction: rtl (the inline axis runs the other way).
static struct wmf wm_frame(int code) {
    struct wmf f = { 0, 0, 0 };
    int wm = code & 3;
    if (wm == WM_VRL) { f.swap = 1; f.fy = 1; }
    else if (wm == WM_VLR) f.swap = 1;
    else if (wm == WM_SLR) { f.swap = 1; f.fx = 1; }
    if (code & 4) f.fx ^= 1;
    return f;
}

struct wmr { int32_t x, y, w, h, cw, ch; };      // a rectangle inside a container of cw x ch

static void wm_swap(struct wmr* r) {
    int32_t t = r->x; r->x = r->y; r->y = t;
    t = r->w; r->w = r->h; r->h = t;
    t = r->cw; r->cw = r->ch; r->ch = t;
}
static void wm_flip(struct wmr* r, struct wmf f) {
    if (f.fy) r->y = r->ch - r->y - r->h;
    if (f.fx) r->x = r->cw - r->x - r->w;
}
static void wm_conv(struct wmr* r, struct wmf from, struct wmf to) {      // from-frame -> to-frame
    wm_flip(r, from);                       // undo the flips (they commute), then the swap: physical
    if (from.swap) wm_swap(r);
    if (to.swap) wm_swap(r);                // and into the target frame
    wm_flip(r, to);
}

// side k (0 top 1 right 2 bottom 3 left) of the from-frame becomes which side of the to-frame
static int wm_side(struct wmf from, struct wmf to, int k) {
    struct wmr r;
    r.cw = 3; r.ch = 5;
    switch (k) {
    case 0: r.x = 0; r.y = 0; r.w = 3; r.h = 1; break;
    case 1: r.x = 2; r.y = 0; r.w = 1; r.h = 5; break;
    case 2: r.x = 0; r.y = 4; r.w = 3; r.h = 1; break;
    default: r.x = 0; r.y = 0; r.w = 1; r.h = 5; break;
    }
    wm_conv(&r, from, to);
    if (r.w == r.cw) return r.y == 0 ? 0 : 2;
    return r.x == 0 ? 3 : 1;
}

// physical side of logical side k in frame wm (ltr): 0 top 1 right 2 bottom 3 left
static int wm_phys_side(int wm, int k) {
    struct wmf f = wm_frame(wm), id = { 0, 0, 0 };
    return wm_side(f, id, k);
}

static void swap_len(struct wlen* a, struct wlen* b) { struct wlen t = *a; *a = *b; *b = t; }

// The style of a box as laid out in frame wm: width <-> height and the four-sided properties permuted.
static const struct wstyle* wm_style(struct wlayout* L, const struct wstyle* ps, int wm) {
    if (wm == 0) return ps;
    unsigned h = ((unsigned)(uintptr_t)ps >> 4) * 2654435761u;
    int slot = (int)((h >> 20) & 63);
    if (L->wm_key[slot] == ps && L->wm_keyf[slot] == wm) return L->wm_val[slot];
    if (L->anon_used % 128 == 0) {
        if (L->nanon_chunk >= 512) return ps;
        struct wstyle* c = (struct wstyle*)w_malloc(128 * sizeof(struct wstyle));
        if (!c) return ps;
        L->anon_chunk[L->nanon_chunk++] = c;
    }
    struct wstyle* s = &L->anon_chunk[L->nanon_chunk - 1][L->anon_used % 128];
    L->anon_used++;
    *s = *ps;
    if (wm & 4) s->direction_rtl = 0;               // the mirrored frame already runs the inline axis backwards
    swap_len(&s->width, &s->height);
    swap_len(&s->min_w, &s->min_h);
    swap_len(&s->max_w, &s->max_h);
    uint8_t t8 = s->overflow_x; s->overflow_x = s->overflow_y; s->overflow_y = t8;
    int32_t t32 = s->cis_w; s->cis_w = s->cis_h; s->cis_h = t32;
    if (s->aspect_ratio > 0) s->aspect_ratio = w_muldiv(1000, 1000, s->aspect_ratio);
    // logical side k (0 block-start, 1 inline-end, 2 block-end, 3 inline-start) = physical side ph[k]
    int ph[4];
    for (int k = 0; k < 4; k++) ph[k] = wm_phys_side(wm, k);
    for (int k = 0; k < 4; k++) {
        s->margin[k] = ps->margin[ph[k]];
        s->padding[k] = ps->padding[ph[k]];
        s->inset[k] = ps->inset[ph[k]];
        s->bw[k] = ps->bw[ph[k]];
        s->bs[k] = ps->bs[ph[k]];
    }
    L->wm_key[slot] = ps; L->wm_keyf[slot] = (uint8_t)wm; L->wm_val[slot] = s;
    return s;
}

static int lay_wm_of(const struct lbox* b) { return b->pst ? (b->pst->wmode | (b->pst->wmode != WM_HTB && b->pst->direction_rtl ? 4 : 0)) : 0; }
static int lay_orth_swap(struct wlayout* L, const struct lbox* b) { return wm_frame(lay_wm_of(b)).swap != wm_frame(lay_pwm_of(L, b)).swap; }
static int lay_pwm_of(struct wlayout* L, const struct lbox* b) { return b->parent >= 0 ? lay_wm_of(&L->b[b->parent]) : 0; }

// frame of a box as its parent's layout sees it, and its own
static void lay_views(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    int fo = lay_wm_of(b), fp = lay_pwm_of(L, b);
    b->st_in = wm_style(L, b->pst, fo);
    b->st = fo == fp ? b->st_in : wm_style(L, b->pst, fp);
}

void lay_assign_views(struct wlayout* L) {
    for (int i = 0; i < L->nb; i++) lay_views(L, i);
}

// Transform the laid-out subtree of box root from frame `from` (its own) into frame `to` (its parent's).
static void wm_xform(struct wlayout* L, int root, struct wmf from, struct wmf to) {
    int n = 0;
    size_t cnt = (size_t)L->nb + 1;
    int* list = (int*)w_malloc(cnt * sizeof(int));
    int32_t* ow = (int32_t*)w_malloc(cnt * sizeof(int32_t));
    int32_t* oh = (int32_t*)w_malloc(cnt * sizeof(int32_t));
    int* pos = (int*)w_malloc(cnt * sizeof(int));
    int* stk = (int*)w_malloc(cnt * sizeof(int));
    if (!list || !ow || !oh || !pos || !stk) { w_free(list); w_free(ow); w_free(oh); w_free(pos); w_free(stk); return; }
    for (int i = 0; i < L->nb; i++) pos[i] = -1;
    int sp = 0;
    stk[sp++] = root;
    while (sp) {                                    // depth first, a box before its children
        int bi = stk[--sp];
        pos[bi] = n; list[n++] = bi;
        for (int c = L->b[bi].last; c >= 0; c = L->b[c].prev) if (sp < L->nb) stk[sp++] = c;
    }
    for (int i = 0; i < n; i++) { ow[i] = L->b[list[i]].w; oh[i] = L->b[list[i]].h; }
    // boxes: position relative to the layout parent, size
    for (int i = 0; i < n; i++) {
        struct lbox* b = &L->b[list[i]];
        struct wmr r;
        int lp = (i > 0 && b->lparent >= 0 && b->lparent < L->nb) ? pos[b->lparent] : -1;
        if (lp < 0) {
            r.x = 0; r.y = 0; r.w = ow[i]; r.h = oh[i]; r.cw = ow[i]; r.ch = oh[i];
            wm_conv(&r, from, to);
            b->w = r.w; b->h = r.h;
        } else {
            r.x = b->x; r.y = b->y; r.w = ow[i]; r.h = oh[i]; r.cw = ow[lp]; r.ch = oh[lp];
            wm_conv(&r, from, to);
            b->x = r.x; b->y = r.y; b->w = r.w; b->h = r.h;
        }
    }
    // inline fragments: relative to the content box of the block that owns them (its old size less its old sides)
    for (int i = 0; i < n; i++) {
        struct lbox* b = &L->b[list[i]];
        if (!(b->flags & BF_IFC) || b->nfr <= 0) continue;
        int32_t cw = ow[i] - b->b[1] - b->b[3] - b->p[1] - b->p[3];
        int32_t ch = oh[i] - b->b[0] - b->b[2] - b->p[0] - b->p[2];
        for (int k = b->fr0; k < b->fr0 + b->nfr && k < L->nfr; k++) {
            struct lfrag* f = &L->fr[k];
            struct wmr r;
            r.cw = cw; r.ch = ch;
            if ((f->kind == FR_TEXT || f->kind == FR_MARKER || f->kind == FR_MARKS) && !(f->_p & 1)) {      // baseline form -> rectangle form
                struct wfmetrics m;
                const struct lbox* fb = &L->b[f->box >= 0 ? f->box : list[i]];
                if (f->kind == FR_MARKS) {          // the marks are drawn at half the font size
                    struct wfont f50;
                    lb_font(fb->st_in ? fb->st_in : fb->st, &f50);
                    f50.px = (uint16_t)(f50.px / 2);
                    wfont_metrics(&f50, &m);
                } else {
                    lb_font_metrics(fb->st_in ? fb->st_in : fb->st, &m);
                }
                r.x = f->x; r.y = f->y - m.ascent; r.w = f->w; r.h = m.ascent + m.descent;
                f->_p |= 1;
            } else { r.x = f->x; r.y = f->y; r.w = f->w; r.h = f->h; }
            wm_conv(&r, from, to);
            f->x = r.x; f->y = r.y; f->w = r.w; f->h = r.h;
        }
    }
    // sides (after the fragments, which used the old ones)
    for (int i = 0; i < n; i++) {
        struct lbox* b = &L->b[list[i]];
        int32_t nm[4], nb[4], np[4];
        for (int k = 0; k < 4; k++) { int t = wm_side(from, to, k); nm[t] = b->m[k]; nb[t] = b->b[k]; np[t] = b->p[k]; }
        for (int k = 0; k < 4; k++) { b->m[k] = nm[k]; b->b[k] = nb[k]; b->p[k] = np[k]; }
        b->baseline = b->last_baseline = -1;
    }
    struct lbox* rb = &L->b[root];
    rb->cm[0] = rb->m[0] > 0 ? rb->m[0] : 0; rb->cm[1] = rb->m[0] < 0 ? rb->m[0] : 0;
    rb->cm[2] = rb->m[2] > 0 ? rb->m[2] : 0; rb->cm[3] = rb->m[2] < 0 ? rb->m[2] : 0;
    w_free(list); w_free(ow); w_free(oh); w_free(pos); w_free(stk);
}

// Laying a flex/grid item out takes several passes (measure, place, stretch), each of which lays out its own items the same
// way: nested containers multiplied the work per level (a YouTube page: 3,600 layouts of every box, 130-600 ms per relayout).
// A block that starts its own formatting context depends only on the inputs below, so a second layout with identical
// inputs is the first one over again: restore what the box itself ended up with (its subtree is already in that state).
#define MEMO_KEEP (BF_EMPTY | BF_HEIGHT_DEF | BF_LAID)
int lay_memo_enabled = 1;      // host A/B testing (tests/web/wbrowse.c: WB_NOMEMO=1)
static void lay_box_core(struct wlayout* L, int bi, int32_t avail, int32_t forced_w, int32_t forced_h) {
    W_PROGRESS();
    struct lbox* b = &L->b[bi];
    int memo = lay_memo_enabled && L->clamp_left < 0 && !L->wm_orth && b->kind == LB_BLOCK && !(b->flags & (BF_ABS | BF_FLOAT | BF_INLINE_LEVEL)) && lay_is_bfc(L, bi);
    if (memo && b->mo_ok && b->mo_av == avail && b->mo_fw == forced_w && b->mo_fh == forced_h && b->mo_cbh == b->cbh &&
        b->mo_pax == b->pax && b->mo_pay == b->pay) {
        b->w = b->mo_w; b->h = b->mo_h;
        for (int i = 0; i < 4; i++) { b->m[i] = b->mo_m[i]; b->cm[i] = b->mo_cm[i]; }
        b->baseline = b->mo_base; b->last_baseline = b->mo_lbase;
        b->flags = (uint16_t)((b->flags & ~MEMO_KEEP) | b->mo_flags);
        return;
    }
    {
        // a line-clamp container counts the lines of everything below it; its state is private to it
        int n = b->kind == LB_BLOCK ? lay_clamp_lines(L, b) : 0;
        int sl = L->clamp_left, se = L->pend_ef, sa = L->pend_last, sr = L->pend_root;
        int sk = L->clamp_skip;
        int32_t sm = L->pend_limit;
        const struct wstyle* scs = L->clamp_st;
        if (n > 0) { L->clamp_left = n; L->pend_ef = -1; L->clamp_st = b->st; }
        else if (n == 0 && L->clamp_left >= 0 && !L->clamp_skip && L->clamp_st && L->clamp_st->clamp_webkit &&
                 lay_is_bfc(L, bi)) {
            // legacy -webkit-line-clamp: the lines inside a BFC descendant are
            // skipped — they render in full and do not count toward N
            L->clamp_skip = 1;
        }
        lay_box_impl(L, bi, avail, forced_w, forced_h);
        L->clamp_skip = sk;
        if (n > 0) { L->clamp_left = sl; L->pend_ef = se; L->pend_last = sa; L->pend_root = sr; L->pend_limit = sm; L->clamp_st = scs; }
    }
    if (memo) {
        b = &L->b[bi];
        b->mo_av = avail; b->mo_fw = forced_w; b->mo_fh = forced_h; b->mo_cbh = b->cbh; b->mo_pax = b->pax; b->mo_pay = b->pay;
        b->mo_w = b->w; b->mo_h = b->h;
        for (int i = 0; i < 4; i++) { b->mo_m[i] = b->m[i]; b->mo_cm[i] = b->cm[i]; }
        b->mo_base = b->baseline; b->mo_lbase = b->last_baseline;
        b->mo_flags = (uint16_t)(b->flags & MEMO_KEEP);
        b->mo_ok = 1;
    }
}

// Entry point of block layout. A box whose writing mode has another frame than its parent's is laid out in its own frame and then
// transformed into the parent's (the parent's width is the box's block axis when the axes are swapped).
void lay_box(struct wlayout* L, int bi, int32_t avail, int32_t forced_w, int32_t forced_h) {
    struct lbox* b = &L->b[bi];
    int fo = lay_wm_of(b), fp = lay_pwm_of(L, b);
    if (fo == fp) { lay_box_core(L, bi, avail, forced_w, forced_h); return; }
    struct wmf Fo = wm_frame(fo), Fp = wm_frame(fp);
    const struct wstyle* out = b->st;
    int32_t scbh = b->cbh;
    int32_t av = avail, fw = forced_w, fh = forced_h;
    if (Fo.swap != Fp.swap) {
        // its inline axis is the parent's block axis: the available inline size is the containing block's height, else the initial one's
        av = scbh >= 0 ? scbh : PX(L->vh);
        fw = forced_h; fh = forced_w;
        b->cbh = avail;
    }
    b->st = b->st_in;
    b->_pad |= 1;                       // laying out: its own intrinsic sizes are in its own frame now
    L->wm_orth++;
    if (Fo.swap != Fp.swap && fw < 0 && b->st->width.t == WL_AUTO && b->kind == LB_BLOCK && !(b->flags & BF_ABS)) {
        // an orthogonal flow's automatic inline size is fit-content (not the whole available extent)
        lb_resolve_box_sides(L, bi, av);
        b = &L->b[bi];
        fw = lay_shrink_width(L, bi, av - b->m[1] - b->m[3]);
    }
    lay_box_core(L, bi, av, fw, fh);
    L->wm_orth--;
    b = &L->b[bi];
    b->_pad &= (uint16_t)~1;
    b->st = out;
    b->cbh = scbh;
    wm_xform(L, bi, Fo, Fp);
}

static void lay_box_impl(struct wlayout* L, int bi, int32_t avail, int32_t forced_w, int32_t forced_h) {
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
            if (!aml && !amr && b->parent >= 0 && L->b[b->parent].st &&
                L->b[b->parent].st->text_align == TA_WEBKIT_CENTER && b->m[1] == 0 && b->m[3] == 0)
                aml = amr = 1;
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
        if (s->width.t == WL_FIT && (s->width.pct & WL_FIT_ARG)) W = size_kw(L, bi, s->width, avail);
        else W = s->width.t == WL_MIN ? mn : s->width.t == WL_MAX ? mx : lay_shrink_width(L, bi, avail - b->m[1] - b->m[3]);
    } else if (s->aspect_ratio > 0 && s->height.t == WL_LEN && (s->height.pct == 0 || b->cbh >= 0) && !(s->contain & CT_SIZE)) {
        // aspect-ratio with a definite height and an auto width: the width follows the height (it is not stretched to the container)
        int32_t hh = wl_resolve(s->height, b->cbh >= 0 ? b->cbh : 0, 0);
        W = w_muldiv(hh, s->aspect_ratio, 1000) + (bb ? 0 : extra_w);
        if (s->overflow_x == OV_VISIBLE) {          // automatic minimum size: the content's min-content width
            int32_t mn, mx;
            lay_intrinsic(L, bi, &mn, &mx);
            if (W < mn) W = mn;
        }
    } else if (b->flags & (BF_INLINE_LEVEL | BF_FLOAT | BF_ABS)) {
        W = lay_shrink_width(L, bi, avail - b->m[1] - b->m[3]);
    } else {
        W = avail - b->m[1] - b->m[3];
    }
    if (forced_w < 0 && s->width.t != WL_LEN && s->aspect_ratio > 0 && !(s->contain & CT_SIZE)) {
        // min/max block sizes transfer through the ratio to the inline size
        // (css-sizing-4 5.2); only for a non-definite width
        if (s->max_h.t == WL_LEN && (s->max_h.pct == 0 || b->cbh >= 0)) {
            int32_t hh = wl_resolve(s->max_h, b->cbh >= 0 ? b->cbh : 0, 0);
            int32_t mw = w_muldiv(hh, s->aspect_ratio, 1000) + (bb ? 0 : extra_w);
            if (W > mw) W = mw;
        }
        if (s->min_h.t == WL_LEN && (s->min_h.pct == 0 || b->cbh >= 0)) {
            int32_t hh = wl_resolve(s->min_h, b->cbh >= 0 ? b->cbh : 0, 0);
            int32_t mw = w_muldiv(hh, s->aspect_ratio, 1000) + (bb ? 0 : extra_w);
            if (W < mw) W = mw;
        }
    }
    if (forced_w < 0) {
        if (s->max_w.t == WL_LEN) {
            int32_t mx = wl_resolve(s->max_w, avail, 0) + (bb ? 0 : extra_w);
            if (W > mx) W = mx;
        } else if (s->max_w.t >= WL_MIN && s->max_w.t <= WL_FIT) {
            int32_t mx = size_kw(L, bi, s->max_w, avail);
            if (mx >= 0 && W > mx) W = mx;
        }
        if (s->min_w.t == WL_LEN) {
            int32_t mn = wl_resolve(s->min_w, avail, 0) + (bb ? 0 : extra_w);
            if (W < mn) W = mn;
        } else if (s->min_w.t >= WL_MIN && s->min_w.t <= WL_FIT) {
            int32_t mn = size_kw(L, bi, s->min_w, avail);
            if (mn >= 0 && W < mn) W = mn;
        }
    }
    if (W < extra_w) W = extra_w;
    // auto margins (block-level in normal flow)
    if (!(b->flags & (BF_INLINE_LEVEL | BF_FLOAT | BF_ABS)) && forced_w < 0) {
        int aml = s->margin[3].t == WL_AUTO, amr = s->margin[1].t == WL_AUTO;
        // legacy <center> / align=center: -webkit-center on the parent
        if (!aml && !amr && b->parent >= 0 && L->b[b->parent].st &&
            L->b[b->parent].st->text_align == TA_WEBKIT_CENTER && b->m[1] == 0 && b->m[3] == 0)
            aml = amr = 1;
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
        if (H < extra_h) H = extra_h;      // border-box: the content height clamps at 0, padding/border stay (video.js height:0 + padding-top)
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
        H = ((s->contain & CT_SIZE) ? s->cis_h : content_h) + extra_h;       // size containment: laid out as if empty (plus contain-intrinsic-height)
        if (s->aspect_ratio > 0 && b->kind != LB_REPLACED && W > 0) {          // aspect-ratio: the height follows the width (taller when the content needs it)
            int32_t rh = bb ? w_muldiv(W, 1000, s->aspect_ratio) : w_muldiv(W - extra_w, 1000, s->aspect_ratio) + extra_h;
            if (s->overflow_y != OV_VISIBLE || rh > H) H = rh;
        }
        if (s->min_h.t == WL_LEN && (s->min_h.pct == 0 || b->cbh >= 0)) {
            int32_t mn = wl_resolve(s->min_h, b->cbh >= 0 ? b->cbh : 0, 0) + (bb ? 0 : extra_h);
            if (H < mn) H = mn;
        }
        if (s->max_h.t == WL_LEN && (s->max_h.pct == 0 || b->cbh >= 0)) {
            int32_t mx = wl_resolve(s->max_h, b->cbh >= 0 ? b->cbh : 0, 0) + (bb ? 0 : extra_h);
            if (H > mx) H = mx;
        }
        if (H < extra_h) H = extra_h;
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

// A box that scrolls its content: overflow other than visible / clip on a block with a DOM node.
static int lay_is_scroller(const struct lbox* b) {
    const struct wstyle* s = b->pst ? b->pst : b->st;
    return s && b->node >= 0 && b->kind == LB_BLOCK && (b->flags & BF_LAID) &&
           ((s->overflow_x != OV_VISIBLE && s->overflow_x != OV_CLIP) || (s->overflow_y != OV_VISIBLE && s->overflow_y != OV_CLIP));
}

// position: sticky. (ux, uy) is the box's origin in the unscrolled layout, S the nearest scroll container (its scroll offset and padding
// box are the scrollport), and the box stays inside its containing block: returns the shift to apply.
static void lay_sticky_shift(struct wlayout* L, const struct lbox* b, const struct lbox* S, const struct lbox* P,
                             int32_t ux, int32_t uy, int32_t* dx, int32_t* dy) {
    const struct wstyle* s = b->pst;
    *dx = *dy = 0;
    int32_t cw = S->w - S->b[1] - S->b[3], ch = S->h - S->b[0] - S->b[2];     // scrollport (padding box)
    int32_t tx = ux - (S->ux + S->b[3]), ty = uy - (S->uy + S->b[0]);          // position inside the scrolled content
    int32_t sx = S->sox, sy = S->soy;
    int32_t ddx = 0, ddy = 0;
    // vertical
    if (s->inset[0].t == WL_LEN) { int32_t t = wl_resolve(s->inset[0], ch, 0); if (ty + ddy < sy + t) ddy = sy + t - ty; }
    if (s->inset[2].t == WL_LEN) { int32_t t = wl_resolve(s->inset[2], ch, 0); if (ty + ddy + b->h > sy + ch - t) ddy = sy + ch - t - b->h - ty; }
    if (s->inset[3].t == WL_LEN) { int32_t t = wl_resolve(s->inset[3], cw, 0); if (tx + ddx < sx + t) ddx = sx + t - tx; }
    if (s->inset[1].t == WL_LEN) { int32_t t = wl_resolve(s->inset[1], cw, 0); if (tx + ddx + b->w > sx + cw - t) ddx = sx + cw - t - b->w - tx; }
    // stay inside the containing block (the parent's content box), margins included
    if (P) {
        int32_t pt = P->uy + P->b[0] + P->p[0], pb = P->uy + P->h - P->b[2] - P->p[2];
        int32_t pl = P->ux + P->b[3] + P->p[3], pr = P->ux + P->w - P->b[1] - P->p[1];
        int32_t maxdy = pb - (uy + b->h + b->m[2]), mindy = pt - (uy - b->m[0]);
        int32_t maxdx = pr - (ux + b->w + b->m[1]), mindx = pl - (ux - b->m[3]);
        if (maxdy < 0) maxdy = 0;                         // already outside its containing block: sticking never moves it further out
        if (mindy > 0) mindy = 0;
        if (maxdx < 0) maxdx = 0;
        if (mindx > 0) mindx = 0;
        if (ddy > maxdy) ddy = maxdy;
        if (ddy < mindy) ddy = mindy;
        if (ddx > maxdx) ddx = maxdx;
        if (ddx < mindx) ddx = mindx;
    }
    *dx = ddx; *dy = ddy;
    (void)L;
}

static void finalize(struct wlayout* L) {
    int n = L->nb;
    // DFS order (lparent is always an ancestor or -1)
    int* stack = (int*)w_malloc((n + 1) * sizeof(int));
    if (!stack) return;
    int sp = 0;
    stack[sp++] = L->root;
    int32_t maxb = PX(L->vh), maxr = PX(L->vw);
    int any_scroller = 0;
    while (sp) {
        int bi = stack[--sp];
        struct lbox* b = &L->b[bi];
        int32_t px = 0, py = 0, pux = 0, puy = 0;
        if (b->lparent >= 0) {
            const struct lbox* P = &L->b[b->lparent];
            px = P->ax - P->sox; py = P->ay - P->soy;       // a scroll container shifts what it contains
            pux = P->ux; puy = P->uy;
        }
        int32_t x = b->x, y = b->y;
        const struct wstyle* s = b->pst ? b->pst : b->st;
        if (s && (b->kind == LB_BLOCK || b->kind == LB_REPLACED)) {
            if (s->position == POS_RELATIVE) {
                int32_t cw = b->lparent >= 0 ? lay_content_w(L, b->lparent) : PX(L->vw);
                if (s->inset[3].t == WL_LEN) x += wl_resolve(s->inset[3], cw, 0);
                else if (s->inset[1].t == WL_LEN) x -= wl_resolve(s->inset[1], cw, 0);
                int32_t chh = 0;
                if (b->lparent >= 0 && ((L->b[b->lparent].flags & BF_HEIGHT_DEF) || (L->b[b->lparent].pst && L->b[b->lparent].pst->height.t == WL_LEN && !L->b[b->lparent].pst->height.pct) || L->b[b->lparent].fc == FC_TABLE)) {
                    const struct lbox* lp = &L->b[b->lparent];
                    chh = lp->h - lp->b[0] - lp->b[2] - lp->p[0] - lp->p[2];
                    if (chh < 0) chh = 0;
                }
                if (s->inset[0].t == WL_LEN) y += wl_resolve(s->inset[0], chh, 0);
                else if (s->inset[2].t == WL_LEN) y -= wl_resolve(s->inset[2], chh, 0);
            }
            if (s->translate_x.px || s->translate_x.pct) x += wl_resolve(s->translate_x, b->w, 0);
            if (s->translate_y.px || s->translate_y.pct) y += wl_resolve(s->translate_y, b->h, 0);
        }
        b->ax = px + x;
        b->ay = py + y;
        b->ux = pux + x;
        b->uy = puy + y;
        if (s && s->position == POS_STICKY && b->kind == LB_BLOCK && b->lparent >= 0) {
            int sc = b->lparent;                               // the nearest scroll container above
            while (sc >= 0 && !lay_is_scroller(&L->b[sc])) sc = L->b[sc].lparent;
            if (sc >= 0) {
                int32_t dx, dy;
                lay_sticky_shift(L, b, &L->b[sc], &L->b[b->lparent], b->ux, b->uy, &dx, &dy);
                b->ax += dx; b->ay += dy;                      // ux / uy stay the layout position: a shift does not change the scrollable overflow
            }
        }
        b->sox = b->soy = 0;
        if (b->node >= 0 && lay_is_scroller(b)) {
            any_scroller = 1;
            int sx, sy;
            wdom_scroll_get(L->d, b->node, &sx, &sy);
            b->sox = PX(sx); b->soy = PX(sy);
            b->sw = b->w - b->b[1] - b->b[3];
            b->sh = b->h - b->b[0] - b->b[2];
        }
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
    // scrollable overflow of every scroll container: what lies inside it, relative to its padding box
    if (any_scroller) {
        for (int i = 0; i < n; i++) {
            const struct lbox* d = &L->b[i];
            if (!(d->flags & BF_LAID) || (d->flags & BF_FIXED) || d->lparent < 0) continue;
            int sc = d->lparent;
            while (sc >= 0 && !lay_is_scroller(&L->b[sc])) sc = L->b[sc].lparent;
            if (sc < 0) continue;
            struct lbox* S = &L->b[sc];
            int32_t ox = S->ux + S->b[3], oy = S->uy + S->b[0];
            if (d->kind == LB_BLOCK || d->kind == LB_REPLACED) {
                if (d->ux + d->w - ox > S->sw) S->sw = d->ux + d->w - ox;
                if (d->uy + d->h - oy > S->sh) S->sh = d->uy + d->h - oy;
            }
            if ((d->flags & BF_IFC) && d->nfr > 0) {
                int32_t fx0 = d->ux + d->b[3] + d->p[3], fy0 = d->uy + d->b[0] + d->p[0];
                for (int k = d->fr0; k < d->fr0 + d->nfr && k < L->nfr; k++) {
                    const struct lfrag* f = &L->fr[k];
                    int32_t r = fx0 + f->x + f->w - ox, bt;
                    if ((f->kind == FR_TEXT || f->kind == FR_MARKER) && !(f->_p & 1)) {
                        struct wfmetrics m;
                        const struct lbox* fb = &L->b[f->box >= 0 ? f->box : i];
                        lb_font_metrics(fb->pst ? fb->pst : fb->st, &m);
                        bt = fy0 + f->y + m.descent - oy;
                    } else bt = fy0 + f->y + f->h - oy;
                    if (r > S->sw) S->sw = r;
                    if (bt > S->sh) S->sh = bt;
                }
            }
        }
    }
    L->doc_h = maxb;
    L->doc_w = maxr;
}

// Scrollable overflow size and padding box size of a scroll container (LU); 0 when the node's box does not scroll.
int wlay_scroll_info(const struct wlayout* L, int node, int32_t* sw, int32_t* sh, int32_t* cw, int32_t* ch) {
    if (node < 0 || node >= L->d->nn || node >= L->node_box_n) return 0;
    int bi = L->node_box[node];
    if (bi < 0) return 0;
    const struct lbox* b = &L->b[bi];
    if (!lay_is_scroller(b)) return 0;
    *sw = b->sw; *sh = b->sh;
    *cw = b->w - b->b[1] - b->b[3];
    *ch = b->h - b->b[0] - b->b[2];
    return 1;
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
    L->clamp_left = -1;
    L->clamp_skip = 0;
    L->poff_x = L->poff_y = 0;
    L->psx = L->psy = 65536;
    L->pend_ef = -1;
    L->node_box = (int32_t*)w_malloc((d->nn + 1) * sizeof(int32_t));
    if (!L->node_box) { w_free(L); return 0; }
    for (int i = 0; i <= d->nn; i++) L->node_box[i] = -1;
    L->node_box_n = d->nn + 1;
    if (lay_build_tree(L) < 0) return L;
    lay_assign_views(L);
    // replaced: intrinsic image sizes
    for (int i = 0; i < L->nb; i++) {
        struct lbox* b = &L->b[i];
        if (b->kind == LB_REPLACED && (b->rk == RK_IMG || b->rk == RK_VIDEO || b->rk == RK_CANVAS || b->rk == RK_IFRAME || b->rk == RK_OBJECT) && imgs && imgs->for_node) {
            int w = 0, h = 0;
            b->img = imgs->for_node(imgs->ctx, b->node, &w, &h);
            b->iw = w;
            b->ih = h;
        }
        if (b->kind == LB_REPLACED && (b->rk == RK_CANVAS || b->rk == RK_IFRAME) && b->img < 0 && b->node >= 0) {
            // a canvas nobody has drawn on: the width / height attributes (default 300 x 150)
            int vl, w = 300, h = 150;
            const char* v = wdom_attr(L->d, b->node, A_width, &vl);
            if (v) { int n = 0, k = 0; while (k < vl && v[k] >= '0' && v[k] <= '9' && n < 100000) n = n * 10 + (v[k++] - '0'); if (k) w = n; }
            v = wdom_attr(L->d, b->node, A_height, &vl);
            if (v) { int n = 0, k = 0; while (k < vl && v[k] >= '0' && v[k] <= '9' && n < 100000) n = n * 10 + (v[k++] - '0'); if (k) h = n; }
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
    w_free(L->node_box); w_free(L->fl); w_free(L->rank); w_free(L->ctr);
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
    // node_box is sized for the DOM at layout time; scripts may have grown it since
    if (node < 0 || node >= L->d->nn || node >= L->node_box_n) return 0;
    int b = L->node_box[node];
    if (b < 0) return 0;
    if (L->b[b].kind == LB_INLINE) {
        // an inline box has no rectangle of its own: the bounding box of its line fragments
        int root = L->b[b].parent;
        while (root >= 0 && !(L->b[root].flags & BF_IFC)) root = L->b[root].parent;
        if (root >= 0) {
            const struct lbox* rb = &L->b[root];
            int32_t ox = rb->ax + rb->b[3] + rb->p[3], oy = rb->ay + rb->b[0] + rb->p[0];
            int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
            for (int i = rb->fr0; i < rb->fr0 + rb->nfr && i < L->nfr; i++) {
                const struct lfrag* f = &L->fr[i];
                if (f->kind != FR_IBOX || f->box != b) continue;
                if (f->x < x0) x0 = f->x;
                if (f->y < y0) y0 = f->y;
                if (f->x + f->w > x1) x1 = f->x + f->w;
                if (f->y + f->h > y1) y1 = f->y + f->h;
            }
            if (x1 >= x0 && y1 >= y0) { *x = ox + x0; *y = oy + y0; *w = x1 - x0; *h = y1 - y0; return 1; }
        }
    }
    *x = L->b[b].ax; *y = L->b[b].ay; *w = L->b[b].w; *h = L->b[b].h;
    return 1;
}

// Where a node stands in paint order: the rank of its own box, or of the nearest ancestor box that was painted (inline
// elements are painted by their block's line boxes). *fixed = it sits inside a position:fixed box (viewport coordinates).
int wlay_paint_rank(const struct wlayout* L, int node, int* fixed) {
    *fixed = 0;
    if (!wlay_knows_node(L, node) || !L->rank) return 0;
    int b = L->node_box[node], rank = 0;
    for (int hops = 0; b >= 0 && hops < 4096; b = L->b[b].parent, hops++) {
        if (L->b[b].flags & BF_FIXED) *fixed = 1;
        if (!rank && b < L->nrank) rank = L->rank[b];
    }
    return rank;
}

int wlay_knows_node(const struct wlayout* L, int node) { return node >= 0 && node < L->node_box_n - 1; }   // node_box has one spare slot (nn + 1)

void wlay_stats(const struct wlayout* L, int* boxes, int* items, int* lines) {
    *boxes = L->nb;
    *items = L->ndi;
    *lines = L->nlines;
}


#if !(defined(KERNEL) && KERNEL)
// Host diagnostics (wbrowse WB_DUMP=1): every text fragment of the layout, in px relative to the box that owns it
void wlay_dump(const struct wlayout* L) {
    if (getenv("WB_DUMPBOX"))
        for (int i = 0; i < L->nb; i++) {
            const struct lbox* b = &L->b[i];
            printf("BOX %d kind=%d par=%d x=%d y=%d w=%d h=%d\n", i,b->kind, b->parent, b->x / 64, b->y / 64, b->w / 64, b->h / 64);
        }
    for (int i = 0; i < L->nfr; i++) {
        const struct lfrag* f = &L->fr[i];
        if (f->kind != FR_TEXT && f->kind != FR_MARKER) continue;
        char buf[96];
        int n = 0;
        for (uint32_t k = 0; k < f->tl && n < 90; k++) { char ch = L->text.p[f->t0 + k]; buf[n++] = ch == '\n' ? '|' : ch; }
        buf[n] = 0;
        printf("FR x=%d y=%d w=%d  \"%s\"\n", f->x / 64, f->y / 64, f->w / 64, buf);
    }
}
#endif

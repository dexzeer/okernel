// Table layout: grid of cells (colspan/rowspan), auto + fixed column sizing.
#include "lay_int.h"

#define MAXCOL 128
#define MAXROW 4096

struct tcell { int box, row, col, rs, cs; };

struct tstate {
    int rows[MAXROW];       // row boxes in visual order
    int rowgroup[MAXROW];
    int nrows;
    struct tcell* cells;
    int ncells, capcells;
    int ncols;
    int32_t cmin[MAXCOL], cmax[MAXCOL];
    int32_t cpct[MAXCOL];   // 1/100 % (0 none)
    int32_t cfix[MAXCOL];   // fixed px width (LU, -1 none)
    int32_t colw[MAXCOL];
    int captions[16];
    int ncap;
};

static int span_attr(struct wlayout* L, int node, int atom, int dflt, int max) {
    if (node < 0) return dflt;
    int vl;
    const char* v = wdom_attr(L->d, node, atom, &vl);
    if (!v) return dflt;
    int n = 0, any = 0;
    for (int i = 0; i < vl && w_isdigit((unsigned char)v[i]); i++) { n = n * 10 + (v[i] - '0'); any = 1; if (n > 10000) break; }
    if (!any) return dflt;
    if (n < 1) n = (atom == A_rowspan && n == 0) ? max : 1;
    if (n > max) n = max;
    return n;
}

static void collect_rows(struct wlayout* L, int bi, struct tstate* t) {
    // thead groups first, then bodies/rows in order, tfoot last
    for (int pass = 0; pass < 3; pass++) {
        for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) {
            struct lbox* cb = &L->b[c];
            if (cb->flags & BF_ABS) continue;
            if (cb->fc == FC_CAPTION) { if (pass == 0 && t->ncap < 16) t->captions[t->ncap++] = c; continue; }
            if (cb->fc != FC_ROWGROUP) continue;
            int d = cb->st->display;
            int want = d == D_TABLE_HEADER_GROUP ? 0 : d == D_TABLE_FOOTER_GROUP ? 2 : 1;
            if (want != pass) continue;
            for (int r = cb->first; r >= 0; r = L->b[r].next) {
                if (L->b[r].fc != FC_ROW || t->nrows >= MAXROW) continue;
                t->rowgroup[t->nrows] = c;
                t->rows[t->nrows++] = r;
            }
        }
    }
}

static int build_grid(struct wlayout* L, int bi, struct tstate* t) {
    memset(t, 0, offsetof(struct tstate, cells));
    t->cells = 0;
    t->ncells = t->capcells = 0;
    t->ncap = 0;
    collect_rows(L, bi, t);
    // occupancy via a per-row "next free column" scan with rowspan carry
    int16_t* carry = (int16_t*)w_calloc(MAXCOL, sizeof(int16_t)); // remaining rowspan per column
    if (!carry) return 0;
    int ncols = 0;
    for (int r = 0; r < t->nrows; r++) {
        int col = 0;
        for (int c = L->b[t->rows[r]].first; c >= 0; c = L->b[c].next) {
            struct lbox* cb = &L->b[c];
            if (cb->fc != FC_CELL || (cb->flags & BF_ABS)) continue;
            while (col < MAXCOL && carry[col] > 0) col++;
            if (col >= MAXCOL) break;
            int cs = span_attr(L, cb->node, A_colspan, 1, 1000);
            int rs = span_attr(L, cb->node, A_rowspan, 1, t->nrows - r);
            if (col + cs > MAXCOL) cs = MAXCOL - col;
            if (t->ncells >= t->capcells) {
                int nc = t->capcells ? t->capcells * 2 : 64;
                struct tcell* n = (struct tcell*)w_realloc(t->cells, nc * sizeof(struct tcell));
                if (!n) break;
                t->cells = n;
                t->capcells = nc;
            }
            struct tcell* tc = &t->cells[t->ncells++];
            tc->box = c; tc->row = r; tc->col = col; tc->rs = rs; tc->cs = cs;
            cb->gc0 = col; cb->gc1 = col + cs; cb->gr0 = r; cb->gr1 = r + rs;
            for (int k = col; k < col + cs; k++) carry[k] = (int16_t)rs;
            col += cs;
            if (col > ncols) ncols = col;
        }
        for (int k = 0; k < MAXCOL; k++) if (carry[k] > 0) carry[k]--;
    }
    w_free(carry);
    t->ncols = ncols;
    return ncols;
}

static void column_widths(struct wlayout* L, int bi, struct tstate* t) {
    int nc = t->ncols;
    for (int i = 0; i < nc; i++) { t->cmin[i] = 0; t->cmax[i] = 0; t->cpct[i] = 0; t->cfix[i] = -1; }
    // <col width> hints
    int ci = 0;
    for (int c = L->b[bi].first; c >= 0 && ci < nc; c = L->b[c].next) {
        struct lbox* cb = &L->b[c];
        if (cb->fc != FC_COLUMN) continue;
        int span = span_attr(L, cb->node, A_span, 1, MAXCOL);
        for (int k = 0; k < span && ci < nc; k++, ci++)
            if (cb->st->width.t == WL_LEN) {
                if (cb->st->width.pct) t->cpct[ci] = cb->st->width.pct;
                else t->cfix[ci] = cb->st->width.px;
            }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < t->ncells; k++) {
            struct tcell* tc = &t->cells[k];
            if ((pass == 0) != (tc->cs == 1)) continue;
            int32_t mn, mx;
            lay_intrinsic(L, tc->box, &mn, &mx);
            struct lbox* cb = &L->b[tc->box];
            const struct wlen* w = &cb->st->width;
            if (pass == 0) {
                int i = tc->col;
                if (mn > t->cmin[i]) t->cmin[i] = mn;
                if (mx > t->cmax[i]) t->cmax[i] = mx;
                if (w->t == WL_LEN) {
                    if (w->pct) { if (w->pct > t->cpct[i]) t->cpct[i] = w->pct; }
                    else {
                        int32_t fw = w->px + (cb->st->box_sizing == BX_BORDER ? 0 : cb->p[1] + cb->p[3] + cb->b[1] + cb->b[3]);
                        if (fw > t->cfix[i]) t->cfix[i] = fw;
                    }
                }
            } else {
                int32_t have = 0, havex = 0;
                for (int i = tc->col; i < tc->col + tc->cs && i < nc; i++) { have += t->cmin[i]; havex += t->cmax[i]; }
                if (mn > have) {
                    int32_t add = (mn - have) / tc->cs;
                    for (int i = tc->col; i < tc->col + tc->cs && i < nc; i++) t->cmin[i] += add;
                }
                if (mx > havex) {
                    int32_t add = (mx - havex) / tc->cs;
                    for (int i = tc->col; i < tc->col + tc->cs && i < nc; i++) t->cmax[i] += add;
                }
            }
        }
    }
    for (int i = 0; i < nc; i++) {
        if (t->cfix[i] >= 0) {
            if (t->cfix[i] > t->cmin[i]) t->cmax[i] = t->cfix[i];
            else t->cmax[i] = t->cmin[i];
        }
        if (t->cmax[i] < t->cmin[i]) t->cmax[i] = t->cmin[i];
    }
}

static int32_t spacing_h(struct wlayout* L, int bi) {
    const struct wstyle* s = L->b[bi].st;
    return s->border_collapse ? 0 : s->border_spacing_h;
}
static int32_t spacing_v(struct wlayout* L, int bi) {
    const struct wstyle* s = L->b[bi].st;
    return s->border_collapse ? 0 : s->border_spacing_v;
}

void lay_table_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx) {
    struct tstate* t = (struct tstate*)w_malloc(sizeof(struct tstate));
    if (!t) { *mn = *mx = 0; return; }
    lb_resolve_box_sides(L, bi, 0);
    build_grid(L, bi, t);
    column_widths(L, bi, t);
    struct lbox* b = &L->b[bi];
    int32_t extra = b->p[1] + b->p[3] + b->b[1] + b->b[3] + spacing_h(L, bi) * (t->ncols + 1);
    if (b->st->border_collapse) extra -= b->p[1] + b->p[3];
    int32_t a = extra, z = extra;
    for (int i = 0; i < t->ncols; i++) { a += t->cmin[i]; z += t->cmax[i]; }
    for (int k = 0; k < t->ncap; k++) {
        int32_t c1, c2;
        lay_intrinsic(L, t->captions[k], &c1, &c2);
        if (c1 > a) a = c1;
        if (c1 > z) z = c1;
    }
    if (b->st->width.t == WL_LEN && !b->st->width.pct) {
        int32_t w = b->st->width.px;
        if (w < a) w = a;
        a = z = w;
    }
    w_free(t->cells);
    w_free(t);
    *mn = a;
    *mx = z;
}

void lay_table(struct wlayout* L, int bi, int32_t avail, int32_t* w_out, int32_t* h_out) {
    struct tstate* t = (struct tstate*)w_malloc(sizeof(struct tstate));
    if (!t) { *w_out = *h_out = 0; return; }
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    int collapse = s->border_collapse;
    if (collapse) for (int i = 0; i < 4; i++) b->p[i] = 0;
    build_grid(L, bi, t);
    column_widths(L, bi, t);
    int nc = t->ncols;
    int32_t hs = spacing_h(L, bi), vs = spacing_v(L, bi);
    int32_t extra = b->p[1] + b->p[3] + b->b[1] + b->b[3];
    int32_t space_total = hs * (nc + 1);
    int32_t smin = 0, smax = 0;
    for (int i = 0; i < nc; i++) { smin += t->cmin[i]; smax += t->cmax[i]; }
    // table border-box width
    int32_t tw;
    int fixed_layout = s->table_layout_fixed && s->width.t == WL_LEN;
    if (s->width.t == WL_LEN) {
        tw = wl_resolve(s->width, avail, 0);
        if (s->box_sizing != BX_BORDER) tw += extra;
        if (!fixed_layout && tw < smin + space_total + extra) tw = smin + space_total + extra;
    } else {
        tw = smax + space_total + extra;
        if (tw > avail) tw = avail;
        if (tw < smin + space_total + extra) tw = smin + space_total + extra;
    }
    if (s->min_w.t == WL_LEN) { int32_t m = wl_resolve(s->min_w, avail, 0); if (tw < m) tw = m; }
    for (int k = 0; k < t->ncap; k++) {
        int32_t c1, c2;
        lay_intrinsic(L, t->captions[k], &c1, &c2);
        if (tw < c1) tw = c1;
    }
    int32_t cols_w = tw - extra - space_total;
    if (cols_w < 0) cols_w = 0;
    // distribute
    if (nc > 0) {
        if (fixed_layout) {
            int32_t used = 0;
            int nfree = 0;
            for (int i = 0; i < nc; i++) {
                if (t->cfix[i] >= 0) { t->colw[i] = t->cfix[i]; used += t->colw[i]; }
                else if (t->cpct[i]) { t->colw[i] = w_muldiv(cols_w, t->cpct[i], 10000); used += t->colw[i]; }
                else { t->colw[i] = -1; nfree++; }
            }
            int32_t rest = cols_w - used;
            for (int i = 0; i < nc; i++) if (t->colw[i] < 0) t->colw[i] = nfree ? (rest > 0 ? rest / nfree : 0) : 0;
        } else {
            // percentage columns first
            int32_t used = 0;
            int32_t pmin = 0;
            for (int i = 0; i < nc; i++) {
                if (t->cpct[i]) {
                    int32_t v = w_muldiv(cols_w, t->cpct[i], 10000);
                    if (v < t->cmin[i]) v = t->cmin[i];
                    t->colw[i] = v;
                    used += v;
                } else t->colw[i] = -1;
            }
            (void)pmin;
            int32_t rem = cols_w - used;
            int32_t amin = 0, amax = 0, fmax = 0;
            int nauto = 0, nfixedc = 0;
            for (int i = 0; i < nc; i++) {
                if (t->colw[i] >= 0) continue;
                amin += t->cmin[i];
                amax += t->cmax[i];
                if (t->cfix[i] >= 0) { fmax += t->cmax[i]; nfixedc++; } else nauto++;
            }
            if (rem >= amax) {
                // everything fits at max: give extra to auto columns (or all)
                int32_t extra_w = rem - amax;
                int32_t base = 0;
                for (int i = 0; i < nc; i++) if (t->colw[i] < 0 && (nauto == 0 || t->cfix[i] < 0)) base += t->cmax[i];
                for (int i = 0; i < nc; i++) {
                    if (t->colw[i] >= 0) continue;
                    int32_t v = t->cmax[i];
                    if (nauto == 0 || t->cfix[i] < 0) {
                        if (base > 0) v += w_div64((int64_t)extra_w * t->cmax[i], base);
                        else v += extra_w / (nauto ? nauto : nc);
                    }
                    t->colw[i] = v;
                }
            } else if (rem >= amin) {
                int32_t extra_w = rem - amin;
                int32_t span = amax - amin;
                for (int i = 0; i < nc; i++) {
                    if (t->colw[i] >= 0) continue;
                    int32_t v = t->cmin[i];
                    if (span > 0) v += w_div64((int64_t)extra_w * (t->cmax[i] - t->cmin[i]), span);
                    t->colw[i] = v;
                }
            } else {
                for (int i = 0; i < nc; i++) if (t->colw[i] < 0) t->colw[i] = t->cmin[i];
            }
            (void)fmax;
            (void)nfixedc;
        }
    }
    // column x positions (relative to the table's content box)
    int32_t colx[MAXCOL + 1];
    int32_t x = hs;
    for (int i = 0; i < nc; i++) { colx[i] = x; x += t->colw[i] + hs; }
    colx[nc] = x;
    int32_t cleft = b->b[3] + b->p[3];
    int32_t ctop = b->b[0] + b->p[0];
    int32_t y = 0;
    // captions (top)
    for (int k = 0; k < t->ncap; k++) {
        int cap = t->captions[k];
        if (L->b[cap].st->caption_bottom) continue;
        lb_resolve_box_sides(L, cap, tw - extra);
        L->b[cap].pax = b->pax + cleft;
        L->b[cap].pay = b->pay + ctop + y;
        lay_box(L, cap, tw - extra, tw - extra - L->b[cap].m[1] - L->b[cap].m[3], -1);
        L->b[cap].x = cleft + L->b[cap].m[3];
        L->b[cap].y = ctop + y + L->b[cap].m[0];
        L->b[cap].lparent = bi;
        y += L->b[cap].h + L->b[cap].m[0] + L->b[cap].m[2];
    }
    int32_t grid_top = y;
    // lay out cells at their widths
    int32_t* rowh = (int32_t*)w_calloc(t->nrows + 1, sizeof(int32_t));
    int32_t* rowy = (int32_t*)w_calloc(t->nrows + 1, sizeof(int32_t));
    if (!rowh || !rowy) { w_free(rowh); w_free(rowy); w_free(t->cells); w_free(t); *w_out = tw; *h_out = 0; return; }
    for (int r = 0; r < t->nrows; r++) {
        struct lbox* rb = &L->b[t->rows[r]];
        if (rb->st->height.t == WL_LEN && !rb->st->height.pct) rowh[r] = rb->st->height.px;
    }
    for (int k = 0; k < t->ncells; k++) {
        struct tcell* tc = &t->cells[k];
        int c1 = tc->col + tc->cs;
        if (c1 > nc) c1 = nc;
        int32_t cw = colx[c1] - hs - colx[tc->col];
        struct lbox* cb = &L->b[tc->box];
        lb_resolve_box_sides(L, tc->box, cw);
        cb = &L->b[tc->box];
        cb->pax = b->pax + cleft + colx[tc->col];
        cb->pay = b->pay + ctop + grid_top;
        cb->cbh = -1;
        lay_box(L, tc->box, cw, cw, -1);
        cb = &L->b[tc->box];
        if (cb->st->height.t == WL_LEN && !cb->st->height.pct) {
            int32_t hh = cb->st->height.px + (cb->st->box_sizing == BX_BORDER ? 0 : cb->p[0] + cb->p[2] + cb->b[0] + cb->b[2]);
            if (hh > cb->h) cb->h = hh;
        }
        if (tc->rs == 1 && cb->h > rowh[tc->row]) rowh[tc->row] = cb->h;
    }
    for (int k = 0; k < t->ncells; k++) {
        struct tcell* tc = &t->cells[k];
        if (tc->rs <= 1) continue;
        int r1 = tc->row + tc->rs;
        if (r1 > t->nrows) r1 = t->nrows;
        int32_t have = vs * (r1 - tc->row - 1);
        for (int r = tc->row; r < r1; r++) have += rowh[r];
        int32_t h = L->b[tc->box].h;
        if (h > have) rowh[r1 - 1] += h - have;
    }
    // explicit table height: grow rows proportionally
    {
        int32_t sum = vs * (t->nrows + 1);
        for (int r = 0; r < t->nrows; r++) sum += rowh[r];
        if (s->height.t == WL_LEN && !s->height.pct && t->nrows > 0) {
            int32_t want = s->height.px - (s->box_sizing == BX_BORDER ? extra : 0);
            if (want > sum) {
                int32_t add = (want - sum) / t->nrows;
                for (int r = 0; r < t->nrows; r++) rowh[r] += add;
            }
        }
    }
    int32_t ry = grid_top + vs;
    for (int r = 0; r < t->nrows; r++) { rowy[r] = ry; ry += rowh[r] + vs; }
    rowy[t->nrows] = ry;
    int32_t grid_bottom = t->nrows ? ry : grid_top;
    // position row groups / rows (geometry for backgrounds)
    for (int r = 0; r < t->nrows; r++) {
        int g = t->rowgroup[r], rw = t->rows[r];
        struct lbox* gb = &L->b[g];
        struct lbox* rb = &L->b[rw];
        if (r == 0 || t->rowgroup[r - 1] != g) {
            int r_end = r;
            while (r_end + 1 < t->nrows && t->rowgroup[r_end + 1] == g) r_end++;
            gb->x = cleft + hs;
            gb->y = ctop + rowy[r];
            gb->w = colx[nc] - 2 * hs;
            if (gb->w < 0) gb->w = 0;
            gb->h = rowy[r_end] + rowh[r_end] - rowy[r];
            gb->lparent = bi;
            gb->pax = b->pax + gb->x;
            gb->pay = b->pay + gb->y;
            gb->flags |= BF_LAID;
            for (int i = 0; i < 4; i++) { gb->m[i] = 0; gb->p[i] = 0; gb->b[i] = 0; }
        }
        rb->x = 0;
        rb->y = rowy[r] - (gb->y - ctop);
        rb->w = gb->w;
        rb->h = rowh[r];
        rb->lparent = g;
        rb->pax = gb->pax;
        rb->pay = gb->pay + rb->y;
        rb->flags |= BF_LAID;
        for (int i = 0; i < 4; i++) { rb->m[i] = 0; rb->p[i] = 0; rb->b[i] = 0; }
    }
    // cells: final height + vertical alignment
    for (int k = 0; k < t->ncells; k++) {
        struct tcell* tc = &t->cells[k];
        int r1 = tc->row + tc->rs;
        if (r1 > t->nrows) r1 = t->nrows;
        int32_t h = rowy[r1 - 1] + rowh[r1 - 1] - rowy[tc->row];
        struct lbox* cb = &L->b[tc->box];
        int32_t natural = cb->h;
        int va = cb->st->vertical_align;
        int32_t off = 0;
        if (h > natural) {
            if (va == VA_MIDDLE) off = (h - natural) / 2;
            else if (va == VA_BOTTOM) off = h - natural;
            else if (va == VA_BASELINE && cb->baseline >= 0) {
                // align first baselines within the row
                int32_t maxb = 0;
                for (int q = 0; q < t->ncells; q++)
                    if (t->cells[q].row == tc->row && L->b[t->cells[q].box].baseline > maxb &&
                        L->b[t->cells[q].box].st->vertical_align == VA_BASELINE)
                        maxb = L->b[t->cells[q].box].baseline;
                off = maxb - cb->baseline;
                if (off < 0 || off > h - natural) off = 0;
            }
        }
        if (off) {
            // every box positioned relative to the cell (incl. atomic inlines
            // nested in inline boxes) moves with its content
            int stack[256], sp = 0;
            for (int c = cb->first; c >= 0 && sp < 256; c = L->b[c].next) stack[sp++] = c;
            while (sp) {
                int c = stack[--sp];
                if (L->b[c].lparent == tc->box) L->b[c].y += off;
                else if (L->b[c].kind == LB_INLINE)
                    for (int d = L->b[c].first; d >= 0 && sp < 256; d = L->b[d].next) stack[sp++] = d;
            }
            for (int f = cb->fr0; f < cb->fr0 + cb->nfr; f++) L->fr[f].y += off;
            if (cb->baseline >= 0) cb->baseline += off;
        }
        cb->h = h;
        // position relative to its row
        int rw = t->rows[tc->row];
        struct lbox* rb = &L->b[rw];
        int32_t cx = cleft + colx[tc->col];
        cb->x = cx - (L->b[t->rowgroup[tc->row]].x) - rb->x;
        cb->y = 0;
        if (collapse) {
            // collapsed borders: neighbouring cells share the border line
            if (tc->col > 0) { cb->x -= cb->b[3]; cb->w += cb->b[3]; }
            if (tc->row > 0) { cb->y -= cb->b[0]; cb->h += cb->b[0]; }
        }
        cb->lparent = rw;
        cb->pax = rb->pax + cb->x;
        cb->pay = rb->pay + cb->y;
        if (b->baseline < 0 && tc->row == 0 && cb->baseline >= 0) b->baseline = ctop + rowy[0] + cb->baseline;
    }
    y = grid_bottom;
    // captions (bottom)
    for (int k = 0; k < t->ncap; k++) {
        int cap = t->captions[k];
        if (!L->b[cap].st->caption_bottom) continue;
        lb_resolve_box_sides(L, cap, tw - extra);
        L->b[cap].pax = b->pax + cleft;
        L->b[cap].pay = b->pay + ctop + y;
        lay_box(L, cap, tw - extra, tw - extra - L->b[cap].m[1] - L->b[cap].m[3], -1);
        L->b[cap].x = cleft + L->b[cap].m[3];
        L->b[cap].y = ctop + y + L->b[cap].m[0];
        L->b[cap].lparent = bi;
        y += L->b[cap].h + L->b[cap].m[0] + L->b[cap].m[2];
    }
    // abs children of the table / groups
    for (int c = b->first; c >= 0; c = L->b[c].next)
        if (L->b[c].flags & BF_ABS) {
            L->b[c].sx = b->pax + cleft;
            L->b[c].sy = b->pay + ctop;
            lay_abs_register(L, c);
        }
    int32_t th = y + b->p[0] + b->p[2] + b->b[0] + b->b[2];
    if (s->height.t == WL_LEN && !s->height.pct) {
        int32_t want = s->height.px + (s->box_sizing == BX_BORDER ? 0 : extra);
        if (th < want) th = want;
    }
    w_free(rowh);
    w_free(rowy);
    w_free(t->cells);
    w_free(t);
    *w_out = tw;
    *h_out = th;
}

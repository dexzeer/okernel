// Flexbox layout (CSS Flexible Box Layout Module, section 9 algorithm).
#include "lay_int.h"

#define MAXFI 1024

struct fitem {
    int box;
    int32_t base, hyp, minm, maxm;   // main sizes (border box)
    int32_t mainm0, mainm1;          // main-axis margins (start, end)
    int32_t crossm0, crossm1;
    uint8_t auto_m0, auto_m1, auto_c0, auto_c1;
    int32_t target;                  // resolved main size
    uint8_t frozen;
    int32_t cross;                   // border-box cross size
    int32_t line;
    int32_t mainpos, crosspos;
};

static int32_t resolve_main_len(struct wlen l, int32_t basis, int def) {
    if (l.t != WL_LEN) return -1;
    if (l.pct && basis < 0) return -1;
    (void)def;
    return wl_resolve(l, basis < 0 ? 0 : basis, 0);
}

static void sort_by_order(struct wlayout* L, int* a, int n) {
    for (int i = 1; i < n; i++) {
        int k = a[i];
        int ko = L->b[k].st->order;
        int j = i - 1;
        while (j >= 0 && L->b[a[j]].st->order > ko) { a[j + 1] = a[j]; j--; }
        a[j + 1] = k;
    }
}

void lay_flex(struct wlayout* L, int bi, int32_t content_w, int32_t* content_h) {
    struct lbox* fb = &L->b[bi];
    const struct wstyle* s = fb->st;
    int row = s->flex_direction == FD_ROW || s->flex_direction == FD_ROW_REV;
    int rev = s->flex_direction == FD_ROW_REV || s->flex_direction == FD_COL_REV;
    int rev0 = rev;                                   // as written (start / end keywords follow the writing mode, not this)
    if (row && s->direction_rtl) rev = !rev;       // a row follows the inline direction
    int wrap = s->flex_wrap != FW_NOWRAP;
    int32_t cleft = fb->b[3] + fb->p[3], ctop = fb->b[0] + fb->p[0];
    // definite inner height?
    int32_t inner_h = -1;
    if (fb->flags & BF_HEIGHT_DEF) inner_h = fb->h - fb->b[0] - fb->b[2] - fb->p[0] - fb->p[2];
    int32_t main_size = row ? content_w : inner_h;
    int32_t cross_size = row ? inner_h : content_w;
    int32_t gap_main = wl_resolve(row ? s->column_gap : s->row_gap, row ? content_w : (inner_h > 0 ? inner_h : 0), 0);
    int32_t gap_cross = wl_resolve(row ? s->row_gap : s->column_gap, row ? (inner_h > 0 ? inner_h : 0) : content_w, 0);
    int total = 0;
    for (int c = fb->first; c >= 0; c = L->b[c].next) total++;
    if (total > MAXFI) total = MAXFI;
    // per-call scratch (flex containers nest): items + order + per-line arrays
    size_t need = (size_t)(total + 2) * (sizeof(struct fitem) + 9 * sizeof(int32_t));
    char* scratch = (char*)w_malloc(need);
    if (!scratch) { *content_h = 0; return; }
    struct fitem* items = (struct fitem*)scratch;
    int* order = (int*)(items + total + 2);
    int* line_start = order + total + 2;
    int32_t* line_cross = line_start + total + 2;
    int32_t* line_off = line_cross + total + 2;
    int32_t* om_main0 = line_off + total + 2;
    int32_t* om_main1 = om_main0 + total + 2;
    int32_t* om_cross0 = om_main1 + total + 2;
    int32_t* om_cross1 = om_cross0 + total + 2;
    int n = 0;
    for (int c = fb->first; c >= 0; c = L->b[c].next) {
        struct lbox* cb = &L->b[c];
        if (cb->flags & BF_ABS) {
            cb->sx = fb->pax + cleft;
            cb->sy = fb->pay + ctop;
            lay_abs_register(L, c);
            continue;
        }
        if (n < total) order[n++] = c;
    }
    sort_by_order(L, order, n);
    // ---- base sizes ----
    for (int i = 0; i < n; i++) {
        struct fitem* it = &items[i];
        memset(it, 0, sizeof *it);
        int c = order[i];
        it->box = c;
        lb_resolve_box_sides(L, c, content_w);
        struct lbox* cb = &L->b[c];
        const struct wstyle* cs = cb->st;
        cb->cbh = row ? inner_h : -1;
        int32_t extra_main = row ? cb->p[1] + cb->p[3] + cb->b[1] + cb->b[3] : cb->p[0] + cb->p[2] + cb->b[0] + cb->b[2];
        int bb = cs->box_sizing == BX_BORDER;
        if (row) {
            int ms = rev ? 1 : 3, me = rev ? 3 : 1;       // the main-start margin is the right one in a mirrored (reversed) row
            it->mainm0 = cb->m[ms]; it->mainm1 = cb->m[me];
            it->crossm0 = cb->m[0]; it->crossm1 = cb->m[2];
            it->auto_m0 = cs->margin[ms].t == WL_AUTO; it->auto_m1 = cs->margin[me].t == WL_AUTO;
            it->auto_c0 = cs->margin[0].t == WL_AUTO; it->auto_c1 = cs->margin[2].t == WL_AUTO;
        } else {
            it->mainm0 = cb->m[0]; it->mainm1 = cb->m[2];
            it->crossm0 = cb->m[3]; it->crossm1 = cb->m[1];
            it->auto_m0 = cs->margin[0].t == WL_AUTO; it->auto_m1 = cs->margin[2].t == WL_AUTO;
            it->auto_c0 = cs->margin[3].t == WL_AUTO; it->auto_c1 = cs->margin[1].t == WL_AUTO;
        }
        if (it->auto_m0) it->mainm0 = 0;
        if (it->auto_m1) it->mainm1 = 0;
        if (it->auto_c0) it->crossm0 = 0;
        if (it->auto_c1) it->crossm1 = 0;
        struct wlen msz = row ? cs->width : cs->height;
        int32_t base = -1;
        if (cs->flex_basis.t == WL_LEN) {
            base = resolve_main_len(cs->flex_basis, main_size, 1);
            if (base >= 0 && !bb) base += extra_main;
        }
        if (base < 0 && (cs->flex_basis.t == WL_AUTO)) {
            int32_t v = resolve_main_len(msz, main_size, 1);
            if (v >= 0) base = v + (bb ? 0 : extra_main);
        }
        int32_t content_main = -1;
        if (base < 0 && row && !wrap && cs->aspect_ratio > 0 && cs->height.t != WL_LEN && cs->width.t != WL_LEN && cross_size >= 0 &&
            !it->auto_c0 && !it->auto_c1 && !(cs->contain & CT_SIZE)) {
            // a stretched item has a definite cross size, so its ratio gives the main size
            int st = cs->align_self == AL_AUTO ? s->align_items : cs->align_self;
            if (st == AL_STRETCH || st == AL_NORMAL) {
                int32_t extra_cross = cb->p[0] + cb->p[2] + cb->b[0] + cb->b[2];
                int32_t hb = cross_size - it->crossm0 - it->crossm1;       // border-box cross size
                if (hb < 0) hb = 0;
                base = bb ? w_muldiv(hb, cs->aspect_ratio, 1000) : w_muldiv(hb - extra_cross, cs->aspect_ratio, 1000) + extra_main;
                if (base < 0) base = 0;
            }
        }
        if (base < 0) {
            if (row) {
                int32_t mn, mx;
                lay_intrinsic(L, c, &mn, &mx);
                base = mx;
            } else {
                // column: lay out at the cross size to measure the height
                int32_t cw = content_w - it->crossm0 - it->crossm1;
                int stretch = (cs->align_self == AL_AUTO ? s->align_items : cs->align_self);
                int32_t fw = (cs->width.t != WL_LEN && (stretch == AL_STRETCH || stretch == AL_NORMAL) &&
                              !it->auto_c0 && !it->auto_c1) ? cw : -1;
                cb->pax = fb->pax + cleft;
                cb->pay = fb->pay + ctop;
                lay_box(L, c, content_w, fw >= 0 ? fw : (cs->width.t == WL_LEN ? -1 : lay_shrink_width(L, c, cw)), -1);
                base = L->b[c].h;
                content_main = base;
            }
        }
        cb = &L->b[c];
        // min / max main
        struct wlen mn_l = row ? cs->min_w : cs->min_h;
        struct wlen mx_l = row ? cs->max_w : cs->max_h;
        int32_t minm = 0;
        if (mn_l.t == WL_LEN) {
            minm = resolve_main_len(mn_l, main_size, 1);
            if (minm < 0) minm = 0;
            else if (!bb) minm += extra_main;
        } else if (mn_l.t == WL_AUTO && (row ? cs->overflow_x : cs->overflow_y) == OV_VISIBLE) {
            // automatic minimum size: min-content, capped by a definite size
            if (row) {
                int32_t a, z;
                lay_intrinsic_content(L, c, &a, &z);     // the content's min-content size, not the item's specified width
                minm = a;
            } else {
                if (content_main < 0) {
                    // flex-basis (or height) was definite, so the item was not
                    // measured for its base: lay it out at its cross size to
                    // get the content size suggestion
                    int32_t cw = content_w - it->crossm0 - it->crossm1;
                    int stretch = (cs->align_self == AL_AUTO ? s->align_items : cs->align_self);
                    int32_t fw = (cs->width.t != WL_LEN && (stretch == AL_STRETCH || stretch == AL_NORMAL) &&
                                  !it->auto_c0 && !it->auto_c1) ? cw : -1;
                    cb->pax = fb->pax + cleft;
                    cb->pay = fb->pay + ctop;
                    lay_box(L, c, content_w, fw >= 0 ? fw : (cs->width.t == WL_LEN ? -1 : lay_shrink_width(L, c, cw)), -1);
                    content_main = L->b[c].h;
                }
                minm = content_main;
                if (minm > PX(20000)) minm = 0;
            }
            int32_t v = resolve_main_len(msz, main_size, 1);
            if (v >= 0 && v + (bb ? 0 : extra_main) < minm) minm = v + (bb ? 0 : extra_main);
            if (cb->kind == LB_REPLACED && row) {
                int32_t rv = resolve_main_len(cs->width, main_size, 1);
                if (rv < 0) minm = minm < base ? minm : base;
            }
        }
        int32_t maxm = INT32_MAX;
        if (mx_l.t == WL_LEN) {
            int32_t v = resolve_main_len(mx_l, main_size, 1);
            if (v >= 0) maxm = v + (bb ? 0 : extra_main);
        }
        if (minm < extra_main) minm = extra_main;
        if (maxm < minm) maxm = minm;
        it->base = base;
        it->minm = minm;
        it->maxm = maxm;
        it->hyp = W_CLAMP(base, minm, maxm);
    }
    // ---- lines ----
    int nlines = 0;
    line_start[0] = 0;
    {
        int32_t used = 0;
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            int32_t outer = items[i].hyp + items[i].mainm0 + items[i].mainm1;
            if (wrap && main_size >= 0 && cnt > 0 && used + gap_main + outer > main_size) {
                line_start[++nlines] = i;
                used = 0;
                cnt = 0;
            }
            used += (cnt ? gap_main : 0) + outer;
            cnt++;
        }
        nlines++;
        line_start[nlines] = n;
    }
    // margin-trim (css-box-4): adjoining child margins at the container's
    // edges are treated as zero — first/last item of each line for the main
    // axis, all items of the first/last line for the cross axis. Trimming
    // changes the main sizes, so the lines are re-broken once with trimmed
    // margins and the trims are re-applied to the new lines.
    if (s->margin_trim) {
        for (int i = 0; i < n; i++) {
            om_main0[i] = items[i].mainm0; om_main1[i] = items[i].mainm1;
            om_cross0[i] = items[i].crossm0; om_cross1[i] = items[i].crossm1;
        }
        for (int pass = 0; pass < 2; pass++) {
            int mt = s->margin_trim;
            for (int ln = 0; ln < nlines; ln++) {
                int a = line_start[ln], z = line_start[ln + 1];
                int first_line = ln == 0, last_line = ln == nlines - 1;
                for (int i = a; i < z; i++) {
                    struct fitem* it = &items[i];
                    struct lbox* cb = &L->b[it->box];
                    if (row) {
                        if (first_line && (mt & MT_BLOCK_START)) { it->crossm0 = 0; cb->m[0] = 0; }
                        if (last_line && (mt & MT_BLOCK_END)) { it->crossm1 = 0; cb->m[2] = 0; }
                        if ((mt & MT_INLINE_START) && i == a) { it->mainm0 = 0; cb->m[3] = 0; }
                        if ((mt & MT_INLINE_END) && i == z - 1) { it->mainm1 = 0; cb->m[1] = 0; }
                    } else {
                        if (first_line && (mt & MT_INLINE_START)) { it->crossm0 = 0; cb->m[3] = 0; }
                        if (last_line && (mt & MT_INLINE_END)) { it->crossm1 = 0; cb->m[1] = 0; }
                        if ((mt & MT_BLOCK_START) && i == a) { it->mainm0 = 0; cb->m[0] = 0; }
                        if ((mt & MT_BLOCK_END) && i == z - 1) { it->mainm1 = 0; cb->m[2] = 0; }
                    }
                }
            }
            if (pass == 1 || !wrap || main_size < 0) break;
            // re-break with trimmed main margins
            nlines = 0;
            line_start[0] = 0;
            int32_t used = 0;
            int cnt = 0;
            for (int i = 0; i < n; i++) {
                int32_t outer = items[i].hyp + items[i].mainm0 + items[i].mainm1;
                if (wrap && main_size >= 0 && cnt > 0 && used + gap_main + outer > main_size) {
                    line_start[++nlines] = i;
                    used = 0;
                    cnt = 0;
                }
                used += (cnt ? gap_main : 0) + outer;
                cnt++;
            }
            nlines++;
            line_start[nlines] = n;
            // restore the margins and trim again for the new line assignment
            for (int i = 0; i < n; i++) {
                struct lbox* cb = &L->b[items[i].box];
                items[i].mainm0 = om_main0[i]; items[i].mainm1 = om_main1[i];
                items[i].crossm0 = om_cross0[i]; items[i].crossm1 = om_cross1[i];
                if (row) { cb->m[0] = om_cross0[i]; cb->m[2] = om_cross1[i]; cb->m[3] = om_main0[i]; cb->m[1] = om_main1[i]; }
                else { cb->m[3] = om_cross0[i]; cb->m[1] = om_cross1[i]; cb->m[0] = om_main0[i]; cb->m[2] = om_main1[i]; }
            }
        }
    }
    // ---- resolve flexible lengths per line ----
    for (int ln = 0; ln < nlines; ln++) {
        int a = line_start[ln], z = line_start[ln + 1];
        if (main_size < 0) {
            for (int i = a; i < z; i++) items[i].target = items[i].hyp;
            continue;
        }
        int32_t sum_hyp = 0;
        for (int i = a; i < z; i++) sum_hyp += items[i].hyp + items[i].mainm0 + items[i].mainm1;
        sum_hyp += gap_main * (z - a - 1);
        int grow = sum_hyp < main_size;
        for (int i = a; i < z; i++) {
            const struct wstyle* cs = L->b[items[i].box].st;
            items[i].frozen = 0;
            items[i].target = items[i].base;
            if ((grow && cs->flex_grow == 0) || (!grow && cs->flex_shrink == 0) ||
                (grow && items[i].base > items[i].hyp) || (!grow && items[i].base < items[i].hyp)) {
                items[i].frozen = 1;
                items[i].target = items[i].hyp;
            }
        }
        for (int iter = 0; iter < 12; iter++) {
            int32_t used = gap_main * (z - a - 1);
            int64_t sumf = 0;
            int unfrozen = 0;
            for (int i = a; i < z; i++) {
                const struct wstyle* cs = L->b[items[i].box].st;
                used += items[i].mainm0 + items[i].mainm1 + (items[i].frozen ? items[i].target : items[i].base);
                if (!items[i].frozen) {
                    unfrozen++;
                    sumf += grow ? cs->flex_grow : (int64_t)cs->flex_shrink * items[i].base / 64;
                }
            }
            if (!unfrozen) break;
            int32_t free = main_size - used;
            if (grow && sumf < 1000) {
                // sum of flex-grow < 1: only that fraction of the free space is used
                free = w_muldiv(free, (int32_t)sumf, 1000);
            }
            int64_t total_viol = 0;
            for (int i = a; i < z; i++) {
                if (items[i].frozen) continue;
                const struct wstyle* cs = L->b[items[i].box].st;
                int32_t t;
                if (sumf <= 0) t = items[i].base;
                else if (grow) t = items[i].base + w_div64((int64_t)free * cs->flex_grow, (int32_t)(sumf > 0x7FFFFFFF ? 0x7FFFFFFF : sumf));
                else {
                    int64_t sf = (int64_t)cs->flex_shrink * items[i].base / 64;
                    t = items[i].base + w_div64((int64_t)free * sf, (int32_t)(sumf > 0x7FFFFFFF ? 0x7FFFFFFF : sumf));
                }
                int32_t cl = W_CLAMP(t, items[i].minm, items[i].maxm);
                total_viol += cl - t;
                items[i].target = cl;
            }
            // freeze per spec: on positive total violation freeze min-violators,
            // on negative freeze max-violators, on zero freeze everything
            int any = 0;
            for (int i = a; i < z; i++) {
                if (items[i].frozen) continue;
                int32_t t = items[i].target;
                int at_min = t == items[i].minm, at_max = t == items[i].maxm;
                if (total_viol == 0 || (total_viol > 0 && at_min) || (total_viol < 0 && at_max)) {
                    items[i].frozen = 1;
                    any = 1;
                }
            }
            if (total_viol == 0) break;
            if (!any) break;
        }
        for (int i = a; i < z; i++) if (!items[i].frozen) items[i].frozen = 1;
    }
    // ---- cross sizes: lay out each item at its main size ----
    for (int ln = 0; ln < nlines; ln++) {
        int32_t lc = 0;
        for (int i = line_start[ln]; i < line_start[ln + 1]; i++) {
            struct fitem* it = &items[i];
            int c = it->box;
            struct lbox* cb = &L->b[c];
            const struct wstyle* cs = cb->st;
            cb->pax = fb->pax + cleft;
            cb->pay = fb->pay + ctop;
            if (row) {
                lay_box(L, c, content_w, it->target, -1);
                it->cross = L->b[c].h;
            } else {
                int al = cs->align_self == AL_AUTO ? s->align_items : cs->align_self;
                int32_t fw;
                if (cs->width.t == WL_LEN) fw = -1;
                else if ((al == AL_STRETCH || al == AL_NORMAL) && !it->auto_c0 && !it->auto_c1)
                    fw = content_w - it->crossm0 - it->crossm1;
                else fw = lay_shrink_width(L, c, content_w - it->crossm0 - it->crossm1);
                lay_box(L, c, content_w, fw, it->target);
                it->cross = L->b[c].w;
            }
            int32_t outer = it->cross + it->crossm0 + it->crossm1;
            if (outer > lc) lc = outer;
        }
        line_cross[ln] = lc;
    }
    if (nlines == 1 && cross_size >= 0) line_cross[0] = cross_size;
    // ---- stretch ----
    for (int ln = 0; ln < nlines; ln++) {
        for (int i = line_start[ln]; i < line_start[ln + 1]; i++) {
            struct fitem* it = &items[i];
            int c = it->box;
            const struct wstyle* cs = L->b[c].st;
            int al = cs->align_self == AL_AUTO ? s->align_items : cs->align_self;
            int cross_auto = row ? cs->height.t != WL_LEN : cs->width.t != WL_LEN;
            if ((al == AL_STRETCH || al == AL_NORMAL) && cross_auto && !it->auto_c0 && !it->auto_c1 &&
                (L->b[c].kind != LB_REPLACED || L->b[c].rk == RK_INPUT_TEXT || L->b[c].rk == RK_INPUT_BUTTON || L->b[c].rk == RK_SELECT)) {
                int32_t want = line_cross[ln] - it->crossm0 - it->crossm1;
                struct wlen mxc = row ? cs->max_h : cs->max_w;
                if (mxc.t == WL_LEN && !mxc.pct) { if (want > mxc.px) want = mxc.px; }
                if (want != it->cross && want >= 0) {
                    if (row) lay_box(L, c, content_w, it->target, want);
                    else lay_box(L, c, content_w, want, it->target);
                    it->cross = want;
                }
            }
        }
    }
    // ---- container cross size / align-content ----
    int32_t total_cross = 0;
    for (int ln = 0; ln < nlines; ln++) total_cross += line_cross[ln];
    total_cross += gap_cross * (nlines - 1);
    int32_t container_cross = cross_size >= 0 ? cross_size : total_cross;
    {
        int32_t free = container_cross - total_cross;
        int32_t start = 0, between = 0;
        int ac = s->align_content;
        if (ac == AL_WM_START) ac = s->flex_wrap == FW_WRAP_REV ? AL_END : AL_START;
        else if (ac == AL_WM_END) ac = s->flex_wrap == FW_WRAP_REV ? AL_START : AL_END;
        if (nlines > 1 || wrap) {
            if (free > 0) {
                if (ac == AL_END) start = free;
                else if (ac == AL_CENTER) start = free / 2;
                else if (ac == AL_SPACE_BETWEEN && nlines > 1) between = free / (nlines - 1);
                else if (ac == AL_SPACE_AROUND) { between = free / nlines; start = between / 2; }
                else if (ac == AL_SPACE_EVENLY) { between = free / (nlines + 1); start = between; }
                else if (ac == AL_STRETCH || ac == AL_NORMAL) {
                    int32_t add = free / nlines;
                    for (int ln = 0; ln < nlines; ln++) line_cross[ln] += add;
                }
            }
        }
        int32_t y = start;
        for (int ln = 0; ln < nlines; ln++) {
            line_off[ln] = y;
            y += line_cross[ln] + gap_cross + between;
        }
    }
    // ---- main-axis placement ----
    int32_t main_extent = 0;
    for (int ln = 0; ln < nlines; ln++) {
        int a = line_start[ln], z = line_start[ln + 1];
        int32_t used = gap_main * (z - a - 1);
        int nauto = 0;
        for (int i = a; i < z; i++) {
            used += items[i].target + items[i].mainm0 + items[i].mainm1;
            nauto += items[i].auto_m0 + items[i].auto_m1;
        }
        int32_t avail = main_size >= 0 ? main_size : used;
        int32_t free = avail - used;
        int32_t start = 0, between = 0;
        if (nauto && free > 0) {
            int32_t each = free / nauto;
            for (int i = a; i < z; i++) {
                if (items[i].auto_m0) items[i].mainm0 = each;
                if (items[i].auto_m1) items[i].mainm1 = each;
            }
            free = 0;
        }
        int jc = s->justify_content;
        // start / end / left / right are physical sides of the writing mode: row-reverse mirrors the positions afterwards
        if (jc == AL_WM_START) jc = rev0 ? AL_END : AL_START;
        else if (jc == AL_WM_END) jc = rev0 ? AL_START : AL_END;
        else if (jc == AL_LEFT) jc = (row && rev) ? AL_END : AL_START;
        else if (jc == AL_RIGHT) jc = (row && rev) ? AL_START : (row ? AL_END : AL_START);
        int cnt = z - a;
        if (free > 0 && cnt > 0) {                  // an empty line has nothing to distribute the space over
            if (jc == AL_END || jc == AL_RIGHT) start = free;
            else if (jc == AL_CENTER) start = free / 2;
            else if (jc == AL_SPACE_BETWEEN && cnt > 1) between = free / (cnt - 1);
            else if (jc == AL_SPACE_AROUND) { between = free / cnt; start = between / 2; }
            else if (jc == AL_SPACE_EVENLY) { between = free / (cnt + 1); start = between; }
        } else if (free < 0 && (jc == AL_CENTER)) start = free / 2;
        int32_t pos = start;
        for (int i = a; i < z; i++) {
            items[i].mainpos = pos + items[i].mainm0;
            pos += items[i].mainm0 + items[i].target + items[i].mainm1 + gap_main + between;
        }
        if (pos - gap_main - between > main_extent) main_extent = pos - gap_main - between;
        // cross placement within the line
        for (int i = a; i < z; i++) {
            struct fitem* it = &items[i];
            const struct wstyle* cs = L->b[it->box].st;
            int al = cs->align_self == AL_AUTO ? s->align_items : cs->align_self;
            if (al == AL_WM_START) al = s->flex_wrap == FW_WRAP_REV ? AL_END : AL_START;
            else if (al == AL_WM_END) al = s->flex_wrap == FW_WRAP_REV ? AL_START : AL_END;
            int32_t lc = line_cross[ln];
            int32_t outer = it->cross + it->crossm0 + it->crossm1;
            int32_t off = 0;
            if (it->auto_c0 && it->auto_c1) off = (lc - outer) / 2;
            else if (it->auto_c0) off = lc - outer;
            else if (al == AL_END) off = lc - outer;
            else if (al == AL_CENTER) off = (lc - outer) / 2;
            if (s->flex_wrap == FW_WRAP_REV) off = lc - outer - off;
            it->crosspos = line_off[ln] + off + it->crossm0;
            it->line = ln;
        }
    }
    if (s->flex_wrap == FW_WRAP_REV) {
        for (int i = 0; i < n; i++) {
            struct fitem* it = &items[i];
            it->crosspos = container_cross - (it->crosspos - it->crossm0) - it->cross - it->crossm1 + it->crossm0;
        }
    }
    int32_t final_main = main_size >= 0 ? main_size : main_extent;
    // ---- write positions ----
    fb = &L->b[bi];
    for (int i = 0; i < n; i++) {
        struct fitem* it = &items[i];
        struct lbox* cb = &L->b[it->box];
        int32_t mp = it->mainpos;
        if (rev) mp = final_main - mp - it->target;
        if (row) { cb->x = cleft + mp; cb->y = ctop + it->crosspos; }
        else { cb->x = cleft + it->crosspos; cb->y = ctop + mp; }
        cb->lparent = bi;
        cb->pax = fb->pax + cb->x;
        cb->pay = fb->pay + cb->y;
        if (fb->baseline < 0 && cb->baseline >= 0 && (!row || it->line == 0))
            fb->baseline = cb->y + cb->baseline;
    }
    if (fb->baseline < 0 && n > 0) {
        struct lbox* cb = &L->b[items[0].box];
        fb->baseline = cb->y + cb->h;
    }
    *content_h = row ? container_cross : final_main;
    w_free(scratch);
}

void lay_flex_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx) {
    struct lbox* fb = &L->b[bi];
    const struct wstyle* s = fb->st;
    int row = s->flex_direction == FD_ROW || s->flex_direction == FD_ROW_REV;
    int wrap = s->flex_wrap != FW_NOWRAP;
    int32_t gap = (s->column_gap.t == WL_LEN && !s->column_gap.pct) ? s->column_gap.px : 0;
    int32_t a = 0, z = 0;
    int cnt = 0;
    // margin-trim: the first item's start / last item's end margin does not
    // count toward the intrinsic main size (row case)
    int mt = s->margin_trim;
    int first_c = -1, last_c = -1;
    for (int c = fb->first; c >= 0; c = L->b[c].next) {
        if (L->b[c].flags & BF_ABS) continue;
        if (first_c < 0) first_c = c;
        last_c = c;
    }
    for (int c = fb->first; c >= 0; c = L->b[c].next) {
        if (L->b[c].flags & BF_ABS) continue;
        int32_t m1, m2;
        lay_intrinsic(L, c, &m1, &m2);
        struct lbox* cb = &L->b[c];
        if (row && cb->st->aspect_ratio > 0 && cb->st->width.t == WL_AUTO && cb->st->height.t == WL_AUTO &&
            s->height.t == WL_LEN && !s->height.pct && (s->align_items == AL_STRETCH || s->align_items == AL_NORMAL) &&
            (cb->st->align_self == AL_AUTO || cb->st->align_self == AL_STRETCH || cb->st->align_self == AL_NORMAL)) {
            // a stretched item's cross size is the container's definite height: its ratio turns that into a width
            lb_resolve_box_sides(L, c, 0);
            cb = &L->b[c];
            int32_t inner = s->height.px;
            if (s->box_sizing == BX_BORDER) { lb_resolve_box_sides(L, bi, 0); fb = &L->b[bi]; inner -= fb->p[0] + fb->p[2] + fb->b[0] + fb->b[2]; }
            int32_t hb = inner - cb->m[0] - cb->m[2];
            int32_t ex_h = cb->p[0] + cb->p[2] + cb->b[0] + cb->b[2], ex_w = cb->p[1] + cb->p[3] + cb->b[1] + cb->b[3];
            int32_t w = cb->st->box_sizing == BX_BORDER ? w_muldiv(hb, cb->st->aspect_ratio, 1000) : w_muldiv(hb - ex_h, cb->st->aspect_ratio, 1000) + ex_w;
            if (w > m1) m1 = w;
            if (w > m2) m2 = w;
        }
        int32_t m1px = (cb->st->margin[1].t == WL_LEN && !cb->st->margin[1].pct) ? cb->st->margin[1].px : 0;
        int32_t m3px = (cb->st->margin[3].t == WL_LEN && !cb->st->margin[3].pct) ? cb->st->margin[3].px : 0;
        int32_t mm = m1px + m3px;
        if (row && (mt & MT_INLINE_START) && c == first_c) mm -= m3px;
        if (row && (mt & MT_INLINE_END) && c == last_c) mm -= m1px;
        if (mm < 0) mm = 0;
        // a flex-basis length overrides the content size for max-content
        if (row && cb->st->flex_basis.t == WL_LEN && !cb->st->flex_basis.pct) {
            int32_t fbv = cb->st->flex_basis.px;
            if (fbv > m2) m2 = fbv;
        }
        if (row) {
            z += m2 + mm + (cnt ? gap : 0);
            if (wrap) { if (m1 + mm > a) a = m1 + mm; }
            else a += m1 + mm + (cnt ? gap : 0);
        } else {
            if (m2 + mm > z) z = m2 + mm;
            if (m1 + mm > a) a = m1 + mm;
        }
        cnt++;
    }
    *mn = a;
    *mx = z > a ? z : a;
}

// CSS Grid layout (subset: explicit/implicit tracks, repeat/minmax/fr,
// template areas, line/span placement, row-flow auto placement).
#include "lay_int.h"

#define MAXTRACK 256
#define MAXGI 2048

enum { TK_FIXED, TK_PCT, TK_FR, TK_AUTO, TK_MINC, TK_MAXC, TK_FITC };
struct track { uint8_t mink, maxk; int32_t minv, maxv; int32_t size; };

struct gitem { int box; int c0, c1, r0, r1; };

static int parse_size(const char* s, int len, uint8_t* kind, int32_t* val, int32_t fs) {
    const char* t = s; int tl = len;
    cv_trim(&t, &tl);
    if (w_ieq(t, tl, "auto")) { *kind = TK_AUTO; *val = 0; return 1; }
    if (w_ieq(t, tl, "min-content")) { *kind = TK_MINC; *val = 0; return 1; }
    if (w_ieq(t, tl, "max-content")) { *kind = TK_MAXC; *val = 0; return 1; }
    int32_t m; int u;
    if (!cv_number(t, tl, &m, &u)) return 0;
    if (u == tl) { *kind = TK_FIXED; *val = w_muldiv(m, 64, 1000); return 1; }
    if (t[u] == '%') { *kind = TK_PCT; *val = m / 10; return 1; }
    int un = cv_unit(t + u, tl - u);
    if (un == U_FR) { *kind = TK_FR; *val = m; return 1; }
    if (un == U_PX) { *kind = TK_FIXED; *val = w_muldiv(m, 64, 1000); return 1; }
    if (un == U_EM || un == U_REM) { *kind = TK_FIXED; *val = w_muldiv(m, un == U_REM ? PX(16) : fs, 1000); return 1; }
    if (un == U_CH) { *kind = TK_FIXED; *val = w_muldiv(m, fs / 2, 1000); return 1; }
    if (un == U_PT) { *kind = TK_FIXED; *val = w_muldiv(m, 256, 3000); return 1; }
    if (un == U_VW || un == U_VH) { *kind = TK_FIXED; *val = w_muldiv(m, PX(1280), 100000); return 1; }
    return 0;
}

static int parse_track(const char* s, int len, struct track* t, int32_t fs) {
    const char* x = s; int xl = len;
    cv_trim(&x, &xl);
    memset(t, 0, sizeof *t);
    if (w_ieq_prefix(x, xl, "minmax(") && x[xl - 1] == ')') {
        int pos = 0; const char* a; int al; const char* b; int bl;
        const char* in = x + 7; int il = xl - 8;
        if (!cv_next_comma(in, il, &pos, &a, &al) || !cv_next_comma(in, il, &pos, &b, &bl)) return 0;
        if (!parse_size(a, al, &t->mink, &t->minv, fs)) return 0;
        if (!parse_size(b, bl, &t->maxk, &t->maxv, fs)) return 0;
        if (t->mink == TK_FR) { t->mink = TK_AUTO; t->minv = 0; }
        return 1;
    }
    if (w_ieq_prefix(x, xl, "fit-content(") && x[xl - 1] == ')') {
        uint8_t k; int32_t v;
        if (!parse_size(x + 12, xl - 13, &k, &v, fs)) return 0;
        t->mink = TK_AUTO;
        t->maxk = TK_FITC;
        t->maxv = v;
        return 1;
    }
    if (w_ieq_prefix(x, xl, "calc(")) {
        t->mink = t->maxk = TK_AUTO;
        return 1;
    }
    uint8_t k; int32_t v;
    if (!parse_size(x, xl, &k, &v, fs)) return 0;
    if (k == TK_FR) { t->mink = TK_AUTO; t->maxk = TK_FR; t->maxv = v; return 1; }
    t->mink = t->maxk = k;
    t->minv = t->maxv = v;
    return 1;
}

// Parse a track list into tracks (expanding repeat()).
static int parse_track_list(const char* s, int len, struct track* out, int max, int32_t avail,
                            int32_t gap, int32_t fs, int* auto_fit) {
    int n = 0;
    int pos = 0;
    const char* c; int cl;
    *auto_fit = 0;
    if (len <= 0) return 0;
    if (w_ieq(s, len, "none") || w_ieq_prefix(s, len, "subgrid") || w_ieq_prefix(s, len, "masonry")) return 0;
    while (cv_next(s, len, &pos, &c, &cl)) {
        if (cl > 0 && c[0] == '[') {
            // named lines: skip to ']'
            while (cl > 0 && c[cl - 1] != ']' && pos < len) { if (!cv_next(s, len, &pos, &c, &cl)) break; }
            continue;
        }
        if (w_ieq_prefix(c, cl, "repeat(") && c[cl - 1] == ')') {
            const char* in = c + 7; int il = cl - 8;
            int p2 = 0; const char* cnt; int cntl;
            if (!cv_next_comma(in, il, &p2, &cnt, &cntl)) continue;
            const char* rest = in + p2; int rl = il - p2;
            if (rl < 0) rl = 0;
            struct track rt[32];
            int nr = 0, q = 0;
            const char* tk; int tkl;
            while (nr < 32 && cv_next(rest, rl, &q, &tk, &tkl)) {
                if (tkl > 0 && tk[0] == '[') continue;
                if (parse_track(tk, tkl, &rt[nr], fs)) nr++;
            }
            if (!nr) continue;
            int times = 1;
            if (w_ieq(cnt, cntl, "auto-fill") || w_ieq(cnt, cntl, "auto-fit")) {
                if (w_ieq(cnt, cntl, "auto-fit")) *auto_fit = 1;
                int32_t one = 0;
                for (int k = 0; k < nr; k++) {
                    int32_t v = rt[k].maxk == TK_FIXED ? rt[k].maxv : rt[k].mink == TK_FIXED ? rt[k].minv : 0;
                    if (rt[k].maxk == TK_PCT) v = w_muldiv(avail, rt[k].maxv, 10000);
                    if (rt[k].mink == TK_FIXED && rt[k].minv > v) v = rt[k].minv;
                    if (rt[k].mink == TK_PCT) { int32_t pv = w_muldiv(avail, rt[k].minv, 10000); if (pv > v) v = pv; }
                    one += v + gap;
                }
                times = one > 0 && avail > 0 ? (avail + gap) / one : 1;
                if (times < 1) times = 1;
            } else {
                int32_t m; int u;
                if (cv_number(cnt, cntl, &m, &u)) times = m / 1000;
                if (times < 1) times = 1;
            }
            for (int t = 0; t < times; t++)
                for (int k = 0; k < nr && n < max; k++) out[n++] = rt[k];
            continue;
        }
        if (n < max && parse_track(c, cl, &out[n], fs)) n++;
    }
    return n;
}

// grid-template-areas -> rectangles per name
struct garea { char name[32]; int r0, r1, c0, c1; };

static int parse_areas(const char* s, int len, struct garea* out, int max, int* rows, int* cols) {
    int na = 0, r = 0, maxc = 0;
    int i = 0;
    while (i < len) {
        while (i < len && s[i] != '"' && s[i] != '\'') i++;
        if (i >= len) break;
        char q = s[i++];
        int st = i;
        while (i < len && s[i] != q) i++;
        const char* row = s + st;
        int rl = i - st;
        i++;
        int p = 0, c = 0;
        const char* tk; int tl;
        while (cv_next(row, rl, &p, &tk, &tl)) {
            int dot = 1;
            for (int k = 0; k < tl; k++) if (tk[k] != '.') dot = 0;
            if (!dot) {
                int f = -1;
                for (int k = 0; k < na; k++)
                    if ((int)strlen(out[k].name) == tl && !memcmp(out[k].name, tk, tl)) { f = k; break; }
                if (f < 0 && na < max) {
                    f = na++;
                    int cl2 = tl < 31 ? tl : 31;
                    memcpy(out[f].name, tk, cl2);
                    out[f].name[cl2] = 0;
                    out[f].r0 = r; out[f].c0 = c; out[f].r1 = r + 1; out[f].c1 = c + 1;
                } else if (f >= 0) {
                    if (r + 1 > out[f].r1) out[f].r1 = r + 1;
                    if (c + 1 > out[f].c1) out[f].c1 = c + 1;
                    if (c < out[f].c0) out[f].c0 = c;
                }
            }
            c++;
        }
        if (c > maxc) maxc = c;
        r++;
    }
    *rows = r;
    *cols = maxc;
    return na;
}

static int32_t track_fixed_min(const struct track* t, int32_t avail) {
    if (t->mink == TK_FIXED) return t->minv;
    if (t->mink == TK_PCT) return w_muldiv(avail, t->minv, 10000);
    return 0;
}

// line value -> (start, end) for an axis with `explicit` tracks
static void resolve_lines(int16_t s, int16_t e, int explicit, int* a, int* b, int* is_auto) {
    *is_auto = 0;
    int span = 1;
    int sa = 0, ea = 0;
    int sv = s, ev = e;
    if (sv > 1000) { span = sv - 1000; sv = 0; }
    if (ev > 1000) { span = ev - 1000; ev = 0; }
    if (sv < 0) sv = explicit + 1 + sv + 1;
    if (ev < 0) ev = explicit + 1 + ev + 1;
    if (sv > 0) sa = sv - 1; else sa = -1;
    if (ev > 0) ea = ev - 1; else ea = -1;
    if (sa >= 0 && ea >= 0) { if (ea <= sa) ea = sa + 1; *a = sa; *b = ea; return; }
    if (sa >= 0) { *a = sa; *b = sa + span; return; }
    if (ea >= 0) { *b = ea; *a = ea - span; if (*a < 0) { *a = 0; *b = span; } return; }
    *is_auto = 1;
    *a = 0;
    *b = span;
}

struct gstate {
    struct track cols[MAXTRACK], rows[MAXTRACK];
    int ncols, nrows;
    struct gitem items[MAXGI];
    int nitems;
    uint8_t occ[MAXTRACK * 64];   // occupancy (rows * ncols), rows capped
};

static int grid_place(struct wlayout* L, int bi, struct gstate* g, int32_t content_w, int32_t gapc) {
    struct lbox* gb = &L->b[bi];
    const struct wstyle* s = gb->st;
    int32_t fs = s->font_size;
    int af;
    const char* cs = s->grid_cols ? css_str(L->ss, s->grid_cols, s->grid_cols_len) : "";
    g->ncols = parse_track_list(cs, s->grid_cols ? s->grid_cols_len : 0, g->cols, MAXTRACK, content_w, gapc, fs, &af);
    const char* rs = s->grid_rows ? css_str(L->ss, s->grid_rows, s->grid_rows_len) : "";
    g->nrows = parse_track_list(rs, s->grid_rows ? s->grid_rows_len : 0, g->rows, MAXTRACK, -1, 0, fs, &af);
    struct garea areas[64];
    int arows = 0, acols = 0, na = 0;
    if (s->grid_areas)
        na = parse_areas(css_str(L->ss, s->grid_areas, s->grid_areas_len), s->grid_areas_len, areas, 64, &arows, &acols);
    int explicit_cols = g->ncols > acols ? g->ncols : acols;
    int explicit_rows = g->nrows > arows ? g->nrows : arows;
    struct track autot;
    memset(&autot, 0, sizeof autot);
    autot.mink = autot.maxk = TK_AUTO;
    struct track autoc = autot, autor = autot;
    if (s->grid_auto_cols) {
        struct track tt[4]; int a2;
        if (parse_track_list(css_str(L->ss, s->grid_auto_cols, s->grid_auto_cols_len), s->grid_auto_cols_len, tt, 4, content_w, 0, fs, &a2) > 0) autoc = tt[0];
    }
    if (s->grid_auto_rows) {
        struct track tt[4]; int a2;
        if (parse_track_list(css_str(L->ss, s->grid_auto_rows, s->grid_auto_rows_len), s->grid_auto_rows_len, tt, 4, -1, 0, fs, &a2) > 0) autor = tt[0];
    }
    while (g->ncols < explicit_cols && g->ncols < MAXTRACK) g->cols[g->ncols++] = autoc;
    if (g->ncols == 0) g->cols[g->ncols++] = autoc;
    int ncols = g->ncols;
    int maxrows = (int)(sizeof g->occ / ncols);
    if (maxrows > MAXTRACK) maxrows = MAXTRACK;
    memset(g->occ, 0, sizeof g->occ);
    g->nitems = 0;
    int cur_r = 0, cur_c = 0;
    int used_rows = explicit_rows;
    for (int c = gb->first; c >= 0 && g->nitems < MAXGI; c = L->b[c].next) {
        struct lbox* cb = &L->b[c];
        if (cb->flags & BF_ABS) {
            cb->sx = gb->pax + gb->b[3] + gb->p[3];
            cb->sy = gb->pay + gb->b[0] + gb->p[0];
            lay_abs_register(L, c);
            continue;
        }
        const struct wstyle* is = cb->st;
        int c0, c1, r0, r1, cauto, rauto;
        int named = 0;
        if (is->grid_area_name && na) {
            const char* nm = css_str(L->ss, is->grid_area_name, is->grid_area_name_len);
            for (int k = 0; k < na; k++)
                if ((int)strlen(areas[k].name) == is->grid_area_name_len &&
                    !memcmp(areas[k].name, nm, is->grid_area_name_len)) {
                    c0 = areas[k].c0; c1 = areas[k].c1; r0 = areas[k].r0; r1 = areas[k].r1;
                    named = 1;
                    break;
                }
        }
        if (!named) {
            resolve_lines(is->gc_start, is->gc_end, explicit_cols, &c0, &c1, &cauto);
            resolve_lines(is->gr_start, is->gr_end, explicit_rows, &r0, &r1, &rauto);
            if (c1 > ncols) {
                // implicit columns
                while (g->ncols < c1 && g->ncols < MAXTRACK) g->cols[g->ncols++] = autoc;
                ncols = g->ncols;
                maxrows = (int)(sizeof g->occ / ncols);
                if (maxrows > MAXTRACK) maxrows = MAXTRACK;
            }
            if (c1 > ncols) { c1 = ncols; if (c0 >= c1) c0 = c1 - 1; }
            if (cauto || rauto) {
                int cspan = c1 - c0, rspan = r1 - r0;
                if (cspan > ncols) cspan = ncols;
                int found = 0;
                if (!rauto) {
                    // fixed row, find a column
                    for (int cc = 0; cc + cspan <= ncols && !found; cc++) {
                        int ok = 1;
                        for (int rr = r0; rr < r1 && ok; rr++)
                            for (int k = cc; k < cc + cspan; k++) if (rr < maxrows && g->occ[rr * ncols + k]) ok = 0;
                        if (ok) { c0 = cc; c1 = cc + cspan; found = 1; }
                    }
                } else {
                    int rr = cauto ? cur_r : 0;
                    int cc0 = cauto ? cur_c : c0;
                    for (; rr < maxrows && !found; rr++) {
                        for (int cc = (cauto ? cc0 : c0); cc + cspan <= ncols; cc++) {
                            int ok = 1;
                            for (int r2 = rr; r2 < rr + rspan && ok; r2++)
                                for (int k = cc; k < cc + cspan; k++)
                                    if (r2 >= maxrows || g->occ[r2 * ncols + k]) { ok = 0; break; }
                            if (ok) { c0 = cc; c1 = cc + cspan; r0 = rr; r1 = rr + rspan; found = 1; break; }
                            if (!cauto) break;
                        }
                        cc0 = 0;
                    }
                    if (found && cauto) { cur_r = r0; cur_c = c1; if (cur_c >= ncols) { cur_c = 0; cur_r++; } }
                }
                if (!found) { r0 = used_rows; r1 = r0 + rspan; }
            }
        }
        if (r1 > maxrows) r1 = maxrows;
        if (r0 >= r1) r0 = r1 - 1;
        if (r0 < 0) { r0 = 0; r1 = 1; }
        for (int rr = r0; rr < r1; rr++)
            for (int k = c0; k < c1 && k < ncols; k++) g->occ[rr * ncols + k] = 1;
        if (r1 > used_rows) used_rows = r1;
        struct gitem* gi = &g->items[g->nitems++];
        gi->box = c; gi->c0 = c0; gi->c1 = c1; gi->r0 = r0; gi->r1 = r1;
    }
    while (g->nrows < used_rows && g->nrows < MAXTRACK) g->rows[g->nrows++] = autor;
    return g->ncols;
}

static void size_columns(struct wlayout* L, struct gstate* g, int32_t content_w, int32_t gap, int intrinsic_max,
                         int32_t* mn_out, int32_t* mx_out) {
    int nc = g->ncols;
    int32_t minc[MAXTRACK], maxc[MAXTRACK];
    for (int i = 0; i < nc; i++) { minc[i] = 0; maxc[i] = 0; }
    // contributions of single-column items
    for (int k = 0; k < g->nitems; k++) {
        struct gitem* gi = &g->items[k];
        if (gi->c1 - gi->c0 != 1) continue;
        int32_t a, z;
        lay_intrinsic(L, gi->box, &a, &z);
        struct lbox* cb = &L->b[gi->box];
        int32_t mm = 0;
        if (cb->st->margin[1].t == WL_LEN && !cb->st->margin[1].pct) mm += cb->st->margin[1].px;
        if (cb->st->margin[3].t == WL_LEN && !cb->st->margin[3].pct) mm += cb->st->margin[3].px;
        if (a + mm > minc[gi->c0]) minc[gi->c0] = a + mm;
        if (z + mm > maxc[gi->c0]) maxc[gi->c0] = z + mm;
    }
    // multi-column items: spread the excess evenly
    for (int k = 0; k < g->nitems; k++) {
        struct gitem* gi = &g->items[k];
        int span = gi->c1 - gi->c0;
        if (span <= 1) continue;
        int32_t a, z;
        lay_intrinsic(L, gi->box, &a, &z);
        int32_t have = gap * (span - 1), havez = gap * (span - 1);
        for (int c = gi->c0; c < gi->c1; c++) { have += minc[c]; havez += maxc[c]; }
        if (a > have) for (int c = gi->c0; c < gi->c1; c++) minc[c] += (a - have) / span;
        if (z > havez) for (int c = gi->c0; c < gi->c1; c++) maxc[c] += (z - havez) / span;
    }
    int32_t total_gap = gap * (nc - 1);
    if (mn_out) {
        int32_t a = total_gap, z = total_gap;
        for (int i = 0; i < nc; i++) {
            struct track* t = &g->cols[i];
            int32_t fmin = t->mink == TK_FIXED ? t->minv : 0;
            int32_t lo = t->mink == TK_FIXED ? fmin : minc[i];
            int32_t hi = t->maxk == TK_FIXED ? t->maxv : maxc[i];
            if (hi < lo) hi = lo;
            a += lo;
            z += hi;
        }
        *mn_out = a;
        *mx_out = z;
        (void)intrinsic_max;
        return;
    }
    // base sizes
    int32_t used = total_gap;
    int64_t frsum = 0;
    for (int i = 0; i < nc; i++) {
        struct track* t = &g->cols[i];
        int32_t sz;
        switch (t->maxk) {
        case TK_FIXED: sz = t->maxv; if (t->mink == TK_FIXED && t->minv > sz) sz = t->minv; break;
        case TK_PCT: sz = w_muldiv(content_w, t->maxv, 10000); break;
        case TK_MINC: sz = minc[i]; break;
        case TK_MAXC: sz = maxc[i]; break;
        case TK_FITC: { int32_t lim = t->maxv; sz = maxc[i] < lim ? maxc[i] : lim; if (sz < minc[i]) sz = minc[i]; break; }
        case TK_FR: sz = track_fixed_min(t, content_w); if (t->mink == TK_AUTO && minc[i] > sz) sz = minc[i]; frsum += t->maxv; break;
        default: sz = t->mink == TK_FIXED ? t->minv : minc[i]; break; // auto
        }
        if (t->mink == TK_FIXED && sz < t->minv) sz = t->minv;
        if (t->mink == TK_PCT) { int32_t pv = w_muldiv(content_w, t->minv, 10000); if (sz < pv) sz = pv; }
        t->size = sz;
        if (t->maxk != TK_FR) used += sz;
    }
    // fr distribution (with min-content floors)
    if (frsum > 0) {
        uint8_t fixed[MAXTRACK];
        for (int i = 0; i < nc; i++) fixed[i] = g->cols[i].maxk != TK_FR;
        for (int iter = 0; iter < 8; iter++) {
            int32_t free = content_w - used;
            int64_t fs = 0;
            for (int i = 0; i < nc; i++) if (!fixed[i]) fs += g->cols[i].maxv;
            if (fs <= 0) break;
            if (fs < 1000) fs = 1000; // sum of fr < 1 uses only that fraction
            int changed = 0;
            for (int i = 0; i < nc; i++) {
                if (fixed[i]) continue;
                int32_t want = free > 0 ? w_div64((int64_t)free * g->cols[i].maxv, (int32_t)fs) : 0;
                int32_t floor_ = g->cols[i].size;
                if (want < floor_) {
                    fixed[i] = 1;
                    used += floor_;
                    changed = 1;
                }
            }
            if (!changed) {
                for (int i = 0; i < nc; i++)
                    if (!fixed[i]) g->cols[i].size = free > 0 ? w_div64((int64_t)free * g->cols[i].maxv, (int32_t)fs) : g->cols[i].size;
                break;
            }
        }
    } else {
        // no fr: stretch auto tracks into leftover space
        int32_t free = content_w - used;
        int nauto = 0;
        for (int i = 0; i < nc; i++) if (g->cols[i].maxk == TK_AUTO) nauto++;
        if (free > 0 && nauto) {
            // first grow auto tracks up to their max-content, then distribute
            for (int i = 0; i < nc && free > 0; i++) {
                if (g->cols[i].maxk != TK_AUTO) continue;
                int32_t grow = maxc[i] - g->cols[i].size;
                if (grow > 0) { if (grow > free) grow = free; g->cols[i].size += grow; free -= grow; }
            }
            if (free > 0) {
                int32_t each = free / nauto;
                for (int i = 0; i < nc; i++) if (g->cols[i].maxk == TK_AUTO) g->cols[i].size += each;
            }
        }
    }
}

void lay_grid(struct wlayout* L, int bi, int32_t content_w, int32_t* content_h) {
    struct gstate* g = (struct gstate*)w_malloc(sizeof(struct gstate));
    if (!g) { *content_h = 0; return; }
    struct lbox* gb = &L->b[bi];
    const struct wstyle* s = gb->st;
    int32_t inner_h = (gb->flags & BF_HEIGHT_DEF) ? gb->h - gb->b[0] - gb->b[2] - gb->p[0] - gb->p[2] : -1;
    int32_t gapc = wl_resolve(s->column_gap, content_w, 0);
    int32_t gapr = wl_resolve(s->row_gap, inner_h > 0 ? inner_h : 0, 0);
    grid_place(L, bi, g, content_w, gapc);
    size_columns(L, g, content_w, gapc, 0, 0, 0);
    int nc = g->ncols, nr = g->nrows;
    int32_t colx[MAXTRACK + 1];
    {
        int32_t x = 0;
        for (int i = 0; i < nc; i++) { colx[i] = x; x += g->cols[i].size + gapc; }
        colx[nc] = x - gapc;
    }
    // justify-content for the grid columns
    int32_t total_w = colx[nc];
    if (total_w < content_w && nc > 0) {
        int32_t free = content_w - total_w;
        int jc = s->justify_content;
        int32_t shift = 0, between = 0;
        if (jc == AL_CENTER) shift = free / 2;
        else if (jc == AL_END || jc == AL_RIGHT) shift = free;
        else if (jc == AL_SPACE_BETWEEN && nc > 1) between = free / (nc - 1);
        else if (jc == AL_SPACE_AROUND) { between = free / nc; shift = between / 2; }
        else if (jc == AL_SPACE_EVENLY) { between = free / (nc + 1); shift = between; }
        for (int i = 0; i <= nc; i++) colx[i] += shift + between * (i < nc ? i : nc - 1);
    }
    // lay out items at their column widths -> row heights
    int32_t rowh[MAXTRACK];
    for (int r = 0; r < nr; r++) {
        struct track* t = &g->rows[r];
        rowh[r] = t->maxk == TK_FIXED ? t->maxv : (t->maxk == TK_PCT && inner_h >= 0) ? w_muldiv(inner_h, t->maxv, 10000) : 0;
        if (t->mink == TK_FIXED && rowh[r] < t->minv) rowh[r] = t->minv;
    }
    for (int k = 0; k < g->nitems; k++) {
        struct gitem* gi = &g->items[k];
        struct lbox* cb = &L->b[gi->box];
        int32_t w = colx[gi->c1 > nc ? nc : gi->c1] - colx[gi->c0] - (gi->c1 < nc ? gapc : 0);
        if (gi->c1 >= nc) w = colx[nc] - colx[gi->c0];
        lb_resolve_box_sides(L, gi->box, w);
        cb = &L->b[gi->box];
        int js = cb->st->justify_self == AL_AUTO ? s->justify_items : cb->st->justify_self;
        int32_t fw = -1;
        int32_t avail = w - cb->m[1] - cb->m[3];
        if (cb->st->width.t != WL_LEN && cb->kind != LB_REPLACED) {
            if (js == AL_STRETCH || js == AL_NORMAL || js == AL_AUTO) fw = avail;
            else fw = lay_shrink_width(L, gi->box, avail);
        }
        cb->pax = gb->pax + gb->b[3] + gb->p[3] + colx[gi->c0];
        cb->pay = gb->pay + gb->b[0] + gb->p[0];
        cb->cbh = -1;
        lay_box(L, gi->box, w, fw, -1);
        cb = &L->b[gi->box];
        if (gi->r1 - gi->r0 == 1) {
            struct track* t = &g->rows[gi->r0];
            if (t->maxk != TK_FIXED && !(t->maxk == TK_PCT && inner_h >= 0)) {
                int32_t h = cb->h + cb->m[0] + cb->m[2];
                if (h > rowh[gi->r0]) rowh[gi->r0] = h;
            }
        }
    }
    // spanning items: grow the last row they span
    for (int k = 0; k < g->nitems; k++) {
        struct gitem* gi = &g->items[k];
        if (gi->r1 - gi->r0 <= 1) continue;
        struct lbox* cb = &L->b[gi->box];
        int32_t h = cb->h + cb->m[0] + cb->m[2];
        int32_t have = gapr * (gi->r1 - gi->r0 - 1);
        for (int r = gi->r0; r < gi->r1; r++) have += rowh[r];
        if (h > have) rowh[gi->r1 - 1] += h - have;
    }
    // fr rows with a definite height
    if (inner_h >= 0) {
        int64_t frs = 0;
        int32_t used = gapr * (nr - 1);
        for (int r = 0; r < nr; r++) { if (g->rows[r].maxk == TK_FR) frs += g->rows[r].maxv; else used += rowh[r]; }
        if (frs > 0 && inner_h > used) {
            for (int r = 0; r < nr; r++)
                if (g->rows[r].maxk == TK_FR) {
                    int32_t v = w_div64((int64_t)(inner_h - used) * g->rows[r].maxv, (int32_t)(frs < 1000 ? 1000 : frs));
                    if (v > rowh[r]) rowh[r] = v;
                }
        }
    }
    int32_t rowy[MAXTRACK + 1];
    {
        int32_t y = 0;
        for (int r = 0; r < nr; r++) { rowy[r] = y; y += rowh[r] + gapr; }
        rowy[nr] = nr ? y - gapr : 0;
    }
    // place + align
    gb = &L->b[bi];
    int32_t cleft = gb->b[3] + gb->p[3], ctop = gb->b[0] + gb->p[0];
    for (int k = 0; k < g->nitems; k++) {
        struct gitem* gi = &g->items[k];
        struct lbox* cb = &L->b[gi->box];
        int32_t cw = (gi->c1 >= nc ? colx[nc] : colx[gi->c1] - gapc) - colx[gi->c0];
        int32_t ch = (gi->r1 >= nr ? rowy[nr] : rowy[gi->r1] - gapr) - rowy[gi->r0];
        int as = cb->st->align_self == AL_AUTO ? s->align_items : cb->st->align_self;
        int js = cb->st->justify_self == AL_AUTO ? s->justify_items : cb->st->justify_self;
        if ((as == AL_STRETCH || as == AL_NORMAL) && cb->st->height.t != WL_LEN && cb->kind != LB_REPLACED) {
            int32_t want = ch - cb->m[0] - cb->m[2];
            if (want > cb->h) lay_box(L, gi->box, cw, cb->w, want);
            cb = &L->b[gi->box];
        }
        int32_t ox = cb->m[3], oy = cb->m[0];
        int32_t fx = cw - cb->w - cb->m[1] - cb->m[3];
        int32_t fy = ch - cb->h - cb->m[0] - cb->m[2];
        if (js == AL_CENTER) ox += fx / 2;
        else if (js == AL_END || js == AL_RIGHT) ox += fx;
        if (as == AL_CENTER) oy += fy / 2;
        else if (as == AL_END) oy += fy;
        cb->x = cleft + colx[gi->c0] + ox;
        cb->y = ctop + rowy[gi->r0] + oy;
        cb->lparent = bi;
        cb->pax = gb->pax + cb->x;
        cb->pay = gb->pay + cb->y;
        if (gb->baseline < 0 && cb->baseline >= 0 && gi->r0 == 0) gb->baseline = cb->y + cb->baseline;
    }
    *content_h = rowy[nr];
    w_free(g);
}

void lay_grid_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx) {
    struct gstate* g = (struct gstate*)w_malloc(sizeof(struct gstate));
    if (!g) { *mn = *mx = 0; return; }
    const struct wstyle* s = L->b[bi].st;
    int32_t gapc = (s->column_gap.t == WL_LEN && !s->column_gap.pct) ? s->column_gap.px : 0;
    // placement with an "infinite" width so auto-fill yields one repetition
    grid_place(L, bi, g, 0, gapc);
    int32_t a = 0, z = 0;
    size_columns(L, g, 0, gapc, 1, &a, &z);
    w_free(g);
    *mn = a;
    *mx = z;
}

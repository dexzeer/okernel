// H.264 inter prediction (8.4): motion vector prediction, P_Skip, spatial and temporal direct,
// fractional-sample interpolation and weighted prediction. Progressive frames, 4:2:0.
#include "h264_int.h"

static inline int iabs(int x) { return x < 0 ? -x : x; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int clip3(int lo, int hi, int x) { return x < lo ? lo : x > hi ? hi : x; }
static inline uint8_t clip1(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }
static inline int median3(int a, int b, int c) { return imax(imin(a, b), imin(imax(a, b), c)); }

struct nb { int avail, ref, mx, my; };

// motion data of the 4x4 block at (bx, by) relative to the current macroblock (may lie outside it);
// `mask` has a bit per 4x4 block of the current macroblock whose motion is already known
static void nb_fetch(struct h264_dec* d, int list, int bx, int by, uint16_t mask, struct nb* o) {
    int mbw = d->asps->mb_width;
    int avail;
    if (bx >= 0 && bx < 4 && by >= 0 && by < 4) avail = (mask >> (by * 4 + bx)) & 1;
    else {
        int nmx = d->mbx + (bx < 0 ? -1 : bx >= 4 ? 1 : 0), nmy = d->mby + (by < 0 ? -1 : 0);
        if (nmx < 0 || nmx >= mbw || nmy < 0 || (nmy == d->mby && nmx > d->mbx)) avail = 0;
        else avail = d->mb[nmy * mbw + nmx].slice_num == d->slice_num;
    }
    if (!avail) { o->avail = 0; o->ref = -1; o->mx = o->my = 0; return; }
    o->avail = 1;
    int ax = d->mbx * 4 + bx, ay = d->mby * 4 + by;
    const struct h264_frame* f = d->cur;
    int r = f->ref_idx[list][(ay >> 1) * (mbw * 2) + (ax >> 1)];
    if (r < 0) { o->ref = -1; o->mx = o->my = 0; return; }
    o->ref = r;
    const int16_t* v = f->mv[list] + (ay * mbw * 4 + ax) * 2;
    o->mx = v[0]; o->my = v[1];
}

// 8.4.1.3: shape 0 median, 1/2 the two 16x8 partitions, 3/4 the two 8x16 partitions
static void mv_predict(struct h264_dec* d, int list, int ref, int bx, int by, int w4, int shape, uint16_t mask, int* px, int* py) {
    struct nb A, B, C;
    nb_fetch(d, list, bx - 1, by, mask, &A);
    nb_fetch(d, list, bx, by - 1, mask, &B);
    nb_fetch(d, list, bx + w4, by - 1, mask, &C);
    if (!C.avail) nb_fetch(d, list, bx - 1, by - 1, mask, &C);
    if (shape == 1 && B.ref == ref) { *px = B.mx; *py = B.my; return; }
    if (shape == 2 && A.ref == ref) { *px = A.mx; *py = A.my; return; }
    if (shape == 3 && A.ref == ref) { *px = A.mx; *py = A.my; return; }
    if (shape == 4 && C.ref == ref) { *px = C.mx; *py = C.my; return; }
    if (!B.avail && !C.avail && A.avail) { B = A; C = A; }
    int n = (A.ref == ref) + (B.ref == ref) + (C.ref == ref);
    if (n == 1) {
        const struct nb* s = A.ref == ref ? &A : B.ref == ref ? &B : &C;
        *px = s->mx; *py = s->my;
        return;
    }
    *px = median3(A.mx, B.mx, C.mx);
    *py = median3(A.my, B.my, C.my);
}

static void store_motion(struct h264_dec* d, int list, int bx, int by, int w4, int h4, int ref, int mx, int my) {
    int mbw = d->asps->mb_width;
    struct h264_frame* f = d->cur;
    for (int y = 0; y < h4; y++)
        for (int x = 0; x < w4; x++) {
            int16_t* v = f->mv[list] + (((d->mby * 4 + by + y) * mbw * 4) + d->mbx * 4 + bx + x) * 2;
            v[0] = (int16_t)mx; v[1] = (int16_t)my;
        }
    int32_t uid = ref >= 0 && d->ref_list[list][ref] ? d->ref_list[list][ref]->uid : -1;
    for (int y = by >> 1; y <= (by + h4 - 1) >> 1; y++)
        for (int x = bx >> 1; x <= (bx + w4 - 1) >> 1; x++) {
            int qi = (d->mby * 2 + y) * (mbw * 2) + d->mbx * 2 + x;
            f->ref_idx[list][qi] = (int8_t)ref;
            f->ref_uid[list][qi] = uid;
        }
}

// ---------------------------------------------------------------------------------------------
// direct prediction (8.4.1.2)

static int minpositive(int a, int b) { return (a >= 0 && b >= 0) ? imin(a, b) : imax(a, b); }

struct spatial_direct { int ref[2]; int mvx[2], mvy[2]; int zero; };

static void spatial_direct_setup(struct h264_dec* d, struct spatial_direct* s) {
    for (int l = 0; l < 2; l++) {
        struct nb A, B, C;
        nb_fetch(d, l, -1, 0, 0, &A);
        nb_fetch(d, l, 0, -1, 0, &B);
        nb_fetch(d, l, 4, -1, 0, &C);
        if (!C.avail) nb_fetch(d, l, -1, -1, 0, &C);
        s->ref[l] = minpositive(A.ref, minpositive(B.ref, C.ref));
    }
    s->zero = 0;
    if (s->ref[0] < 0 && s->ref[1] < 0) { s->ref[0] = s->ref[1] = 0; s->zero = 1; }
    for (int l = 0; l < 2; l++) {
        s->mvx[l] = s->mvy[l] = 0;
        if (s->ref[l] >= 0 && !s->zero) mv_predict(d, l, s->ref[l], 0, 0, 4, 0, 0, &s->mvx[l], &s->mvy[l]);
    }
}

// co-located block (progressive frames): picture RefPicList1[0], same 4x4 position or the corner of the 8x8
static void colocated(struct h264_dec* d, int bx, int by, int* ref_col, int* mvx, int* mvy, int32_t* uid) {
    const struct h264_frame* col = d->ref_list[1][0];
    int mbw = d->asps->mb_width;
    if (d->asps->direct_8x8_inference) { bx = (bx >> 1) * 3; by = (by >> 1) * 3; }
    int ax = d->mbx * 4 + bx, ay = d->mby * 4 + by;
    int qi = (ay >> 1) * (mbw * 2) + (ax >> 1);
    int l = col->ref_idx[0][qi] >= 0 ? 0 : (col->ref_idx[1][qi] >= 0 ? 1 : -1);
    if (l < 0) { *ref_col = -1; *mvx = *mvy = 0; *uid = -1; return; }
    *ref_col = col->ref_idx[l][qi];
    *uid = col->ref_uid[l][qi];
    const int16_t* v = col->mv[l] + (ay * mbw * 4 + ax) * 2;
    *mvx = v[0]; *mvy = v[1];
}

static int map_col_to_list0(struct h264_dec* d, int32_t uid) {
    for (int i = 0; i < d->ref_count[0]; i++) if (d->ref_list[0][i] && d->ref_list[0][i]->uid == uid) return i;
    return 0;
}

// derive the direct motion of quadrant q (0..3) of the current macroblock
static void derive_direct_quadrant(struct h264_dec* d, int q, const struct spatial_direct* sp) {
    int qx = (q & 1) * 2, qy = (q >> 1) * 2;
    int per4 = !d->asps->direct_8x8_inference;
    for (int by = qy; by < qy + 2; by += per4 ? 1 : 2)
        for (int bx = qx; bx < qx + 2; bx += per4 ? 1 : 2) {
            int w = per4 ? 1 : 2;
            int ref_col, cmx, cmy; int32_t uid;
            colocated(d, bx, by, &ref_col, &cmx, &cmy, &uid);
            if (d->sl.direct_spatial_mv_pred) {
                const struct h264_frame* c1 = d->ref_list[1][0];
                int col_zero = c1 && c1->ref_state == 1 && ref_col == 0 && cmx >= -1 && cmx <= 1 && cmy >= -1 && cmy <= 1;
                for (int l = 0; l < 2; l++) {
                    int ref = sp->ref[l], mx = 0, my = 0;
                    if (ref >= 0 && !sp->zero && !(ref == 0 && col_zero)) { mx = sp->mvx[l]; my = sp->mvy[l]; }
                    store_motion(d, l, bx, by, w, w, ref, mx, my);
                }
            } else {
                int ref0 = ref_col < 0 ? 0 : map_col_to_list0(d, uid);
                int mx0, my0, mx1, my1;
                struct h264_frame* p0 = d->ref_list[0][ref0];
                struct h264_frame* p1 = d->ref_list[1][0];
                int mvcx = ref_col < 0 ? 0 : cmx, mvcy = ref_col < 0 ? 0 : cmy;
                int tb = p0 ? clip3(-128, 127, d->cur->poc - p0->poc) : 0;
                int td = (p0 && p1) ? clip3(-128, 127, p1->poc - p0->poc) : 0;
                if ((p0 && p0->ref_state == 2) || td == 0) { mx0 = mvcx; my0 = mvcy; mx1 = my1 = 0; }
                else {
                    int tx = (16384 + iabs(td / 2)) / td;
                    int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
                    mx0 = (dsf * mvcx + 128) >> 8; my0 = (dsf * mvcy + 128) >> 8;
                    mx1 = mx0 - mvcx; my1 = my0 - mvcy;
                }
                store_motion(d, 0, bx, by, w, w, ref0, mx0, my0);
                store_motion(d, 1, bx, by, w, w, 0, mx1, my1);
            }
        }
}

// ---------------------------------------------------------------------------------------------
// motion derivation for the current macroblock

static int derive_motion(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    uint16_t mask = 0;
    int is_b = d->sl.slice_type == 1;
    if (m->is_p_skip) {
        struct nb A, B;
        nb_fetch(d, 0, -1, 0, 0, &A);
        nb_fetch(d, 0, 0, -1, 0, &B);
        int mx = 0, my = 0;
        if (A.avail && B.avail && !(A.ref == 0 && A.mx == 0 && A.my == 0) && !(B.ref == 0 && B.mx == 0 && B.my == 0))
            mv_predict(d, 0, 0, 0, 0, 4, 0, 0, &mx, &my);
        store_motion(d, 0, 0, 0, 4, 4, 0, mx, my);
        store_motion(d, 1, 0, 0, 4, 4, -1, 0, 0);
        return d->ref_list[0][0] ? 0 : H264_ERR_NOREF;
    }
    struct spatial_direct sp;
    int have_sp = 0;
    if (is_b && (m->is_b_skip || m->is_direct16 || m->part_mode == 3)) {
        if (!d->ref_list[1][0]) return H264_ERR_NOREF;
        if (d->sl.direct_spatial_mv_pred) { spatial_direct_setup(d, &sp); have_sp = 1; }
    }
    if (m->is_b_skip || m->is_direct16) {
        for (int q = 0; q < 4; q++) derive_direct_quadrant(d, q, have_sp ? &sp : 0);
        return 0;
    }
    // the partitions in decoding order (direct sub-macroblocks are handled when their turn comes)
    for (int l = 0; l < 2; l++) for (int q = 0; q < 4; q++) store_motion(d, l, (q & 1) * 2, (q >> 1) * 2, 2, 2, -1, 0, 0);
    int pi = 0;
    int nquads = m->part_mode == 3 ? 4 : 1;
    for (int qi = 0; qi < nquads; qi++) {
        if (m->part_mode == 3 && m->sub_direct[qi]) {
            derive_direct_quadrant(d, qi, have_sp ? &sp : 0);
            int qx = (qi & 1) * 2, qy = (qi >> 1) * 2;
            for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) mask |= (uint16_t)(1u << ((qy + y) * 4 + qx + x));
            continue;
        }
        // all partitions belonging to this quadrant (or, for 16x16/16x8/8x16, all partitions)
        while (pi < m->nparts && (m->part_mode != 3 || m->parts[pi].quad == qi)) {
            struct h264_part* p = &m->parts[pi++];
            int shape = 0;
            if (m->part_mode == 1) shape = 1 + (p->y == 2);
            else if (m->part_mode == 2) shape = 3 + (p->x == 2);
            for (int l = 0; l < 2; l++) {
                if (!(p->flags & (1 << l))) { store_motion(d, l, p->x, p->y, p->w, p->h, -1, 0, 0); continue; }
                int quad = (p->y >> 1) * 2 + (p->x >> 1);
                int ref = m->ref[l][quad];
                if (ref < 0 || ref >= d->ref_count[l] || !d->ref_list[l][ref]) return H264_ERR_NOREF;
                int px, py;
                mv_predict(d, l, ref, p->x, p->y, p->w, shape, mask, &px, &py);
                store_motion(d, l, p->x, p->y, p->w, p->h, ref, px + p->mvd[l][0], py + p->mvd[l][1]);
            }
            for (int y = 0; y < p->h; y++) for (int x = 0; x < p->w; x++) mask |= (uint16_t)(1u << ((p->y + y) * 4 + p->x + x));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// sample interpolation (8.4.2.2)

#define TW 24
// fetch the (w+5) x (h+5) integer samples around the block with edge replication
static void fetch_block(const uint8_t* plane, int stride, int pw, int ph, int x0, int y0, int w, int h, uint8_t* t) {
    for (int y = 0; y < h; y++) {
        int sy = clip3(0, ph - 1, y0 + y);
        const uint8_t* row = plane + (size_t)sy * stride;
        if (x0 >= 0 && x0 + w <= pw) memcpy(t + y * TW, row + x0, (size_t)w);
        else for (int x = 0; x < w; x++) t[y * TW + x] = row[clip3(0, pw - 1, x0 + x)];
    }
}

static inline int tapv(int a, int b, int c, int dd, int e, int f) { return a - 5 * b + 20 * c + 20 * dd - 5 * e + f; }

// luma prediction of a w x h block at integer position (xi, yi) with quarter-sample fraction (fx, fy)
static void mc_luma(uint8_t* dst, int dstride, const uint8_t* plane, int stride, int pw, int ph, int xi, int yi, int fx, int fy, int w, int h) {
    uint8_t t[(16 + 5) * TW];
    fetch_block(plane, stride, pw, ph, xi - 2, yi - 2, w + 5, h + 5, t);
#define T(x, y) t[((y) + 2) * TW + (x) + 2]
#define BH(x, y) clip1((tapv(T((x) - 2, y), T((x) - 1, y), T(x, y), T((x) + 1, y), T((x) + 2, y), T((x) + 3, y)) + 16) >> 5)
#define HV(x, y) clip1((tapv(T(x, (y) - 2), T(x, (y) - 1), T(x, y), T(x, (y) + 1), T(x, (y) + 2), T(x, (y) + 3)) + 16) >> 5)
    if (fx == 0 && fy == 0) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) dst[y * dstride + x] = T(x, y);
        return;
    }
    int need_j = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    int j1[16];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v;
            int G = T(x, y);
            int j = 0;
            if (need_j) {
                int c[6];
                for (int k = 0; k < 6; k++) {
                    int yy = y - 2 + k;
                    c[k] = tapv(T(x - 2, yy), T(x - 1, yy), T(x, yy), T(x + 1, yy), T(x + 2, yy), T(x + 3, yy));
                }
                j = clip1((tapv(c[0], c[1], c[2], c[3], c[4], c[5]) + 512) >> 10);
            }
            switch (fy * 4 + fx) {
            case 1: v = (G + BH(x, y) + 1) >> 1; break;                          // a
            case 2: v = BH(x, y); break;                                         // b
            case 3: v = (T(x + 1, y) + BH(x, y) + 1) >> 1; break;                // c
            case 4: v = (G + HV(x, y) + 1) >> 1; break;                          // d
            case 5: v = (BH(x, y) + HV(x, y) + 1) >> 1; break;                   // e
            case 6: v = (BH(x, y) + j + 1) >> 1; break;                          // f
            case 7: v = (BH(x, y) + HV(x + 1, y) + 1) >> 1; break;               // g
            case 8: v = HV(x, y); break;                                         // h
            case 9: v = (HV(x, y) + j + 1) >> 1; break;                          // i
            case 10: v = j; break;                                               // j
            case 11: v = (j + HV(x + 1, y) + 1) >> 1; break;                     // k
            case 12: v = (T(x, y + 1) + HV(x, y) + 1) >> 1; break;              // n
            case 13: v = (HV(x, y) + BH(x, y + 1) + 1) >> 1; break;             // p
            case 14: v = (j + BH(x, y + 1) + 1) >> 1; break;                    // q
            default: v = (HV(x + 1, y) + BH(x, y + 1) + 1) >> 1; break;         // r
            }
            dst[y * dstride + x] = (uint8_t)v;
        }
    (void)j1;
#undef T
#undef BH
#undef HV
}

static void mc_chroma(uint8_t* dst, int dstride, const uint8_t* plane, int stride, int pw, int ph, int xi, int yi, int fx, int fy, int w, int h) {
    uint8_t t[(8 + 1) * TW];
    fetch_block(plane, stride, pw, ph, xi, yi, w + 1, h + 1, t);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int A = t[y * TW + x], B = t[y * TW + x + 1], C = t[(y + 1) * TW + x], D = t[(y + 1) * TW + x + 1];
            dst[y * dstride + x] = (uint8_t)(((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B + (8 - fx) * fy * C + fx * fy * D + 32) >> 6);
        }
}

// ---------------------------------------------------------------------------------------------
// weighted prediction (8.4.2.3)

static void combine(struct h264_dec* d, uint8_t* dst, int dstride, const uint8_t* p0, const uint8_t* p1, int w, int h, int plane,
                    int used0, int used1, int ref0, int ref1) {
    const struct h264_slice* s = &d->sl;
    int is_b = s->slice_type == 1;
    int explicit_wp = is_b ? d->app->weighted_bipred_idc == 1 : d->app->weighted_pred;
    int implicit_wp = is_b && d->app->weighted_bipred_idc == 2;
    int logwd = plane ? s->chroma_log2_denom : s->luma_log2_denom;
    if (used0 && used1) {
        if (implicit_wp) {
            int w1 = d->impl_w[ref0][ref1], w0 = 64 - w1;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
                dst[y * dstride + x] = clip1((p0[y * 16 + x] * w0 + p1[y * 16 + x] * w1 + 32) >> 6);
        } else if (explicit_wp) {
            const struct h264_wp_entry* e0 = &s->wp[0][ref0]; const struct h264_wp_entry* e1 = &s->wp[1][ref1];
            int w0 = e0->w[plane], w1 = e1->w[plane], o = (e0->o[plane] + e1->o[plane] + 1) >> 1;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
                dst[y * dstride + x] = clip1(((p0[y * 16 + x] * w0 + p1[y * 16 + x] * w1 + (1 << logwd)) >> (logwd + 1)) + o);
        } else {
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
                dst[y * dstride + x] = (uint8_t)((p0[y * 16 + x] + p1[y * 16 + x] + 1) >> 1);
        }
        return;
    }
    const uint8_t* p = used0 ? p0 : p1;
    if (explicit_wp) {
        const struct h264_wp_entry* e = used0 ? &s->wp[0][ref0] : &s->wp[1][ref1];
        int wt = e->w[plane], o = e->o[plane];
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int v = p[y * 16 + x];
            v = logwd >= 1 ? ((v * wt + (1 << (logwd - 1))) >> logwd) + o : v * wt + o;
            dst[y * dstride + x] = clip1(v);
        }
    } else {
        for (int y = 0; y < h; y++) memcpy(dst + y * dstride, p + y * 16, (size_t)w);
    }
}

// predict the block (bx, by, w4, h4) of the current macroblock (4x4 units) from the motion stored in the frame
static void predict_block(struct h264_dec* d, int bx, int by, int w4, int h4) {
    struct h264_frame* f = d->cur;
    int mbw = d->asps->mb_width;
    int ax = d->mbx * 4 + bx, ay = d->mby * 4 + by;
    int qi = (ay >> 1) * (mbw * 2) + (ax >> 1);
    int ref[2], used[2], mv[2][2];
    for (int l = 0; l < 2; l++) {
        ref[l] = f->ref_idx[l][qi];
        used[l] = ref[l] >= 0;
        const int16_t* v = f->mv[l] + (ay * mbw * 4 + ax) * 2;
        mv[l][0] = v[0]; mv[l][1] = v[1];
    }
    int w = w4 * 4, h = h4 * 4;
    uint8_t tmp[2][3][16 * 16];
    for (int l = 0; l < 2; l++) {
        if (!used[l]) continue;
        const struct h264_frame* r = d->ref_list[l][ref[l]];
        if (!r) continue;
        int x = d->mbx * 16 + bx * 4, y = d->mby * 16 + by * 4;
        mc_luma(tmp[l][0], 16, r->plane[0], r->stride[0], r->width, r->height, x + (mv[l][0] >> 2), y + (mv[l][1] >> 2), mv[l][0] & 3, mv[l][1] & 3, w, h);
        for (int c = 1; c < 3; c++)
            mc_chroma(tmp[l][c], 16, r->plane[c], r->stride[c], r->width / 2, r->height / 2, x / 2 + (mv[l][0] >> 3), y / 2 + (mv[l][1] >> 3),
                      mv[l][0] & 7, mv[l][1] & 7, w / 2, h / 2);
    }
    uint8_t* dstl = f->plane[0] + (size_t)(d->mby * 16 + by * 4) * f->stride[0] + d->mbx * 16 + bx * 4;
    combine(d, dstl, f->stride[0], tmp[0][0], tmp[1][0], w, h, 0, used[0], used[1], ref[0], ref[1]);
    for (int c = 1; c < 3; c++) {
        uint8_t* dstc = f->plane[c] + (size_t)(d->mby * 8 + by * 2) * f->stride[c] + d->mbx * 8 + bx * 2;
        combine(d, dstc, f->stride[c], tmp[0][c], tmp[1][c], w / 2, h / 2, c, used[0], used[1], ref[0], ref[1]);
    }
}

static int same_motion(const struct h264_frame* f, int mbw, int ax, int ay, int bx2, int by2) {
    for (int l = 0; l < 2; l++) {
        int ra = f->ref_idx[l][(ay >> 1) * (mbw * 2) + (ax >> 1)], rb = f->ref_idx[l][(by2 >> 1) * (mbw * 2) + (bx2 >> 1)];
        if (ra != rb) return 0;
        if (ra < 0) continue;
        const int16_t* va = f->mv[l] + (ay * mbw * 4 + ax) * 2; const int16_t* vb = f->mv[l] + (by2 * mbw * 4 + bx2) * 2;
        if (va[0] != vb[0] || va[1] != vb[1]) return 0;
    }
    return 1;
}

static void predict_mb(struct h264_dec* d) {
    struct h264_frame* f = d->cur;
    int mbw = d->asps->mb_width;
    int ox = d->mbx * 4, oy = d->mby * 4;
    // whole macroblock in one piece when the motion is uniform
    int uniform = 1;
    for (int y = 0; y < 4 && uniform; y++) for (int x = 0; x < 4; x++) if (!same_motion(f, mbw, ox, oy, ox + x, oy + y)) { uniform = 0; break; }
    if (uniform) { predict_block(d, 0, 0, 4, 4); return; }
    for (int q = 0; q < 4; q++) {
        int bx = (q & 1) * 2, by = (q >> 1) * 2;
        int u = 1;
        for (int y = 0; y < 2 && u; y++) for (int x = 0; x < 2; x++) if (!same_motion(f, mbw, ox + bx, oy + by, ox + bx + x, oy + by + y)) { u = 0; break; }
        if (u) predict_block(d, bx, by, 2, 2);
        else for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) predict_block(d, bx + x, by + y, 1, 1);
    }
}

int h264_inter_decode_mb(struct h264_dec* d) {
    int rc = derive_motion(d);
    if (rc < 0) return rc;
    predict_mb(d);
    return 0;
}

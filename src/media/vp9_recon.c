// VP9 reconstruction: intra prediction (8.5.1), inter prediction (8.5.2), dequantised
// coefficient -> residual via the inverse transforms (8.6.2, 8.7) and reference border
// extension. Written from the specification text.

#include "vp9_int.h"
#include "vp9_tables.h"

// ---------------------------------------------------------------------------
// intra prediction

void vp9_predict_intra(struct vp9_dec* d, int plane, int x, int y, int have_left, int have_above, int not_on_right,
                       int tx_sz, int block_idx) {
    struct vp9_buf* b = d->cur;
    pel* base = b->plane[plane];
    int stride = b->stride[plane];
    int mode = plane > 0 ? d->uv_mode : (d->mi_size >= BLOCK_8X8 ? d->y_mode : d->sub_modes[block_idx]);
    int log2 = tx_sz + 2;
    int size = 1 << log2;
    int sub_x = plane > 0 ? d->subsampling_x : 0, sub_y = plane > 0 ? d->subsampling_y : 0;
    int max_x = ((d->mi_cols * 8) >> sub_x) - 1;
    int max_y = ((d->mi_rows * 8) >> sub_y) - 1;
    int bd = d->bit_depth;
    int mid = 1 << (bd - 1);
    int maxv = (1 << bd) - 1;

    int above_buf[1 + 64];                 // aboveRow[-1 .. 2*size-1]
    int* A = above_buf + 1;
    int L[32];
    if (!have_above) {
        for (int i = 0; i < 2 * size; i++) A[i] = mid - 1;
    } else {
        const pel* row = base + (size_t)(y - 1) * stride;
        for (int i = 0; i < size; i++) A[i] = row[vp9_imin(max_x, x + i)];
        if (not_on_right && tx_sz == 0) {
            for (int i = size; i < 2 * size; i++) A[i] = row[vp9_imin(max_x, x + i)];
        } else {
            for (int i = size; i < 2 * size; i++) A[i] = A[size - 1];
        }
    }
    if (have_above && have_left) A[-1] = base[(size_t)(y - 1) * stride + vp9_imin(max_x, x - 1)];
    else if (have_above) A[-1] = mid + 1;
    else A[-1] = mid - 1;
    if (have_left) {
        for (int i = 0; i < size; i++) L[i] = base[(size_t)vp9_imin(max_y, y + i) * stride + x - 1];
    } else {
        for (int i = 0; i < size; i++) L[i] = mid + 1;
    }

    pel* dst = base + (size_t)y * stride + x;
#define P(i, j) dst[(size_t)(i) * stride + (j)]
#define R2(v, n) (((v) + (1 << ((n) - 1))) >> (n))
    switch (mode) {
    case V_PRED:
        for (int i = 0; i < size; i++)
            for (int j = 0; j < size; j++) P(i, j) = (pel)A[j];
        break;
    case H_PRED:
        for (int i = 0; i < size; i++)
            for (int j = 0; j < size; j++) P(i, j) = (pel)L[i];
        break;
    case D207_PRED:
        for (int j = 0; j < size; j++) P(size - 1, j) = (pel)L[size - 1];
        for (int i = 0; i < size - 1; i++) P(i, 0) = (pel)R2(L[i] + L[i + 1], 1);
        for (int i = 0; i < size - 2; i++) P(i, 1) = (pel)R2(L[i] + 2 * L[i + 1] + L[i + 2], 2);
        P(size - 2, 1) = (pel)R2(L[size - 2] + 3 * L[size - 1], 2);
        for (int j = 2; j < size; j++)
            for (int i = size - 2; i >= 0; i--) P(i, j) = P(i + 1, j - 2);
        break;
    case D45_PRED:
        for (int i = 0; i < size; i++)
            for (int j = 0; j < size; j++)
                P(i, j) = (i + j + 2 < size * 2) ? (pel)R2(A[i + j] + A[i + j + 1] * 2 + A[i + j + 2], 2)
                                                 : (pel)A[2 * size - 1];
        break;
    case D63_PRED:
        for (int i = 0; i < size; i++)
            for (int j = 0; j < size; j++) {
                int i2 = i / 2;
                P(i, j) = (i & 1) ? (pel)R2(A[i2 + j] + A[i2 + j + 1] * 2 + A[i2 + j + 2], 2)
                                  : (pel)R2(A[i2 + j] + A[i2 + j + 1], 1);
            }
        break;
    case D117_PRED:
        for (int j = 0; j < size; j++) P(0, j) = (pel)R2(A[j - 1] + A[j], 1);
        P(1, 0) = (pel)R2(L[0] + 2 * A[-1] + A[0], 2);
        for (int j = 1; j < size; j++) P(1, j) = (pel)R2(A[j - 2] + 2 * A[j - 1] + A[j], 2);
        P(2, 0) = (pel)R2(A[-1] + 2 * L[0] + L[1], 2);
        for (int i = 3; i < size; i++) P(i, 0) = (pel)R2(L[i - 3] + 2 * L[i - 2] + L[i - 1], 2);
        for (int i = 2; i < size; i++)
            for (int j = 1; j < size; j++) P(i, j) = P(i - 2, j - 1);
        break;
    case D135_PRED:
        P(0, 0) = (pel)R2(L[0] + 2 * A[-1] + A[0], 2);
        for (int j = 1; j < size; j++) P(0, j) = (pel)R2(A[j - 2] + 2 * A[j - 1] + A[j], 2);
        P(1, 0) = (pel)R2(A[-1] + 2 * L[0] + L[1], 2);
        for (int i = 2; i < size; i++) P(i, 0) = (pel)R2(L[i - 2] + 2 * L[i - 1] + L[i], 2);
        for (int i = 1; i < size; i++)
            for (int j = 1; j < size; j++) P(i, j) = P(i - 1, j - 1);
        break;
    case D153_PRED:
        P(0, 0) = (pel)R2(L[0] + A[-1], 1);
        for (int i = 1; i < size; i++) P(i, 0) = (pel)R2(L[i - 1] + L[i], 1);
        P(0, 1) = (pel)R2(L[0] + 2 * A[-1] + A[0], 2);
        P(1, 1) = (pel)R2(A[-1] + 2 * L[0] + L[1], 2);
        for (int i = 2; i < size; i++) P(i, 1) = (pel)R2(L[i - 2] + 2 * L[i - 1] + L[i], 2);
        for (int j = 2; j < size; j++) P(0, j) = (pel)R2(A[j - 3] + 2 * A[j - 2] + A[j - 1], 2);
        for (int i = 1; i < size; i++)
            for (int j = 2; j < size; j++) P(i, j) = P(i - 1, j - 2);
        break;
    case TM_PRED:
        for (int i = 0; i < size; i++)
            for (int j = 0; j < size; j++) P(i, j) = (pel)vp9_clip3(0, maxv, A[j] + L[i] - A[-1]);
        break;
    default: {                                                     // DC_PRED
        int v;
        if (have_left && have_above) {
            int sum = 0;
            for (int k = 0; k < size; k++) sum += L[k] + A[k];
            v = (sum + size) >> (log2 + 1);
        } else if (have_left) {
            int sum = 0;
            for (int k = 0; k < size; k++) sum += L[k];
            v = (sum + (1 << (log2 - 1))) >> log2;
        } else if (have_above) {
            int sum = 0;
            for (int k = 0; k < size; k++) sum += A[k];
            v = (sum + (1 << (log2 - 1))) >> log2;
        } else {
            v = 1 << (bd - 1);
        }
        for (int i = 0; i < size; i++)
            for (int j = 0; j < size; j++) P(i, j) = (pel)v;
        break;
    }
    }
#undef P
#undef R2
}

// ---------------------------------------------------------------------------
// inter prediction

static inline int round_mv_q2(int v) { return (v < 0 ? v - 1 : v + 1) / 2; }
static inline int round_mv_q4(int v) { return (v < 0 ? v - 2 : v + 2) / 4; }

// reference block fetch + 8-tap separable interpolation; writes w x h samples to dst
static void block_inter(const struct vp9_dec* d, const struct vp9_buf* ref, int plane, int start_x, int start_y,
                        int step_x, int step_y, int w, int h, int filter, pel* dst, int dst_stride) {
    int sub_x = plane > 0 ? ref->subx : 0, sub_y = plane > 0 ? ref->suby : 0;
    int last_x = ((ref->width + sub_x) >> sub_x) - 1;
    int last_y = ((ref->height + sub_y) >> sub_y) - 1;
    const pel* rp = ref->plane[plane];
    int rs = ref->stride[plane];
    int maxv = (1 << d->bit_depth) - 1;
    const int16_t(*kern)[8] = subpel_filters[filter];

    if (step_x == 16 && step_y == 16) {
        int x0 = start_x >> 4, fx = start_x & 15;
        int y0 = start_y >> 4, fy = start_y & 15;
        // source window: columns x0-3 .. x0+w+4, rows y0-3 .. y0+h+4
        const pel* src;
        int ss;
        pel tmp[(64 + 8) * (64 + 8)];
        int ext_w = ref->ext_w[plane], ext_h = ref->ext_h[plane];
        if (x0 - 3 >= -VP9_BORDER && x0 + w + 4 < ext_w + VP9_BORDER && y0 - 3 >= -VP9_BORDER &&
            y0 + h + 4 < ext_h + VP9_BORDER) {
            src = rp + (ptrdiff_t)y0 * rs + x0;
            ss = rs;
        } else {
            int tw = w + 7, th = h + 7;
            for (int r = 0; r < th; r++) {
                int yy = vp9_clip3(0, last_y, y0 - 3 + r);
                const pel* row = rp + (ptrdiff_t)yy * rs;
                for (int c = 0; c < tw; c++) tmp[r * (64 + 8) + c] = row[vp9_clip3(0, last_x, x0 - 3 + c)];
            }
            src = tmp + 3 * (64 + 8) + 3;
            ss = 64 + 8;
        }
        if (!fx && !fy) {
            for (int r = 0; r < h; r++) memcpy(dst + (size_t)r * dst_stride, src + (ptrdiff_t)r * ss, (size_t)w * sizeof(pel));
            return;
        }
        if (!fy) {                                             // horizontal only
            const int16_t* k = kern[fx];
            for (int r = 0; r < h; r++) {
                const pel* s = src + (ptrdiff_t)r * ss - 3;
                pel* o = dst + (size_t)r * dst_stride;
                for (int c = 0; c < w; c++) {
                    int sum = 0;
                    for (int t = 0; t < 8; t++) sum += k[t] * s[c + t];
                    o[c] = (pel)vp9_clip3(0, maxv, (sum + 64) >> 7);
                }
            }
            return;
        }
        if (!fx) {                                             // vertical only
            const int16_t* k = kern[fy];
            for (int r = 0; r < h; r++) {
                pel* o = dst + (size_t)r * dst_stride;
                for (int c = 0; c < w; c++) {
                    int sum = 0;
                    for (int t = 0; t < 8; t++) sum += k[t] * src[(ptrdiff_t)(r + t - 3) * ss + c];
                    o[c] = (pel)vp9_clip3(0, maxv, (sum + 64) >> 7);
                }
            }
            return;
        }
        int16_t mid[(64 + 8) * 64];
        const int16_t* kx = kern[fx];
        for (int r = 0; r < h + 7; r++) {
            const pel* s = src + (ptrdiff_t)(r - 3) * ss - 3;
            for (int c = 0; c < w; c++) {
                int sum = 0;
                for (int t = 0; t < 8; t++) sum += kx[t] * s[c + t];
                mid[r * 64 + c] = (int16_t)vp9_clip3(0, maxv, (sum + 64) >> 7);
            }
        }
        const int16_t* ky = kern[fy];
        for (int r = 0; r < h; r++) {
            pel* o = dst + (size_t)r * dst_stride;
            for (int c = 0; c < w; c++) {
                int sum = 0;
                for (int t = 0; t < 8; t++) sum += ky[t] * mid[(r + t) * 64 + c];
                o[c] = (pel)vp9_clip3(0, maxv, (sum + 64) >> 7);
            }
        }
        return;
    }

    // scaled reference: the general form of the specification
    int inter_h = (((h - 1) * step_y + 15) >> 4) + 8;
    int32_t* mid = (int32_t*)d->inter_tmp;
    for (int r = 0; r < inter_h; r++) {
        int yy = vp9_clip3(0, last_y, (start_y >> 4) + r - 3);
        const pel* row = rp + (ptrdiff_t)yy * rs;
        for (int c = 0; c < w; c++) {
            int p = start_x + step_x * c;
            const int16_t* k = kern[p & 15];
            int sum = 0;
            for (int t = 0; t < 8; t++) sum += k[t] * row[vp9_clip3(0, last_x, (p >> 4) + t - 3)];
            mid[r * 64 + c] = vp9_clip3(0, maxv, (sum + 64) >> 7);
        }
    }
    for (int r = 0; r < h; r++) {
        int p = (start_y & 15) + step_y * r;
        const int16_t* k = kern[p & 15];
        pel* o = dst + (size_t)r * dst_stride;
        for (int c = 0; c < w; c++) {
            int sum = 0;
            for (int t = 0; t < 8; t++) sum += k[t] * mid[((p >> 4) + t) * 64 + c];
            o[c] = (pel)vp9_clip3(0, maxv, (sum + 64) >> 7);
        }
    }
}

void vp9_predict_inter(struct vp9_dec* d, int plane, int x, int y, int w, int h, int block_idx) {
    struct vp9_buf* cur = d->cur;
    int sub_x = plane > 0 ? d->subsampling_x : 0, sub_y = plane > 0 ? d->subsampling_y : 0;
    // only the part inside the decoded area is ever read back
    w = vp9_imin(w, ((d->mi_cols * 8) >> sub_x) - x);
    h = vp9_imin(h, ((d->mi_rows * 8) >> sub_y) - y);
    if (w <= 0 || h <= 0) return;
    int is_compound = d->ref_frame[1] > NONE_FRAME;
    pel* dst = cur->plane[plane] + (size_t)y * cur->stride[plane] + x;
    int dst_stride = cur->stride[plane];

    for (int ref_list = 0; ref_list < 1 + is_compound; ref_list++) {
        // 8.5.2.1 motion vector selection
        int mvr, mvc;
        const mv_t* bm = d->block_mvs[ref_list];
        if (plane == 0 || d->mi_size >= BLOCK_8X8 || (!d->subsampling_x && !d->subsampling_y)) {
            mvr = bm[block_idx].row;
            mvc = bm[block_idx].col;
        } else if (!d->subsampling_x && d->subsampling_y) {
            mvr = round_mv_q2(bm[block_idx].row + bm[block_idx + 2].row);
            mvc = round_mv_q2(bm[block_idx].col + bm[block_idx + 2].col);
        } else if (d->subsampling_x && !d->subsampling_y) {
            mvr = round_mv_q2(bm[block_idx].row + bm[block_idx + 1].row);
            mvc = round_mv_q2(bm[block_idx].col + bm[block_idx + 1].col);
        } else {
            mvr = round_mv_q4(bm[0].row + bm[1].row + bm[2].row + bm[3].row);
            mvc = round_mv_q4(bm[0].col + bm[1].col + bm[2].col + bm[3].col);
        }
        // 8.5.2.2 clamping
        int bh = vp9_num_8x8_high[d->mi_size], bw = vp9_num_8x8_wide[d->mi_size];
        int to_top = -((d->mi_row * MI_SIZE) * 16) >> sub_y;
        int to_bottom = (((d->mi_rows - bh - d->mi_row) * MI_SIZE) * 16) >> sub_y;
        int to_left = -((d->mi_col * MI_SIZE) * 16) >> sub_x;
        int to_right = (((d->mi_cols - bw - d->mi_col) * MI_SIZE) * 16) >> sub_x;
        int spel_left = (INTERP_EXTEND + ((bw * MI_SIZE) >> sub_x)) << SUBPEL_BITS;
        int spel_right = spel_left - SUBPEL_SHIFTS;
        int spel_top = (INTERP_EXTEND + ((bh * MI_SIZE) >> sub_y)) << SUBPEL_BITS;
        int spel_bottom = spel_top - SUBPEL_SHIFTS;
        int cmv_r = vp9_clip3(to_top - spel_top, to_bottom + spel_bottom, (2 * mvr) >> sub_y);
        int cmv_c = vp9_clip3(to_left - spel_left, to_right + spel_right, (2 * mvc) >> sub_x);
        // 8.5.2.3 scaling
        int ref_i = d->ref_frame[ref_list] - LAST_FRAME;
        if (ref_i < 0 || ref_i >= REFS_PER_FRAME || !d->ref_ok[ref_i]) { d->bad_ref = 1; return; }
        struct vp9_buf* ref = d->ref[d->ref_frame_idx[ref_i]];
        int x_scale = (int)(((int64_t)ref->width << REF_SCALE_SHIFT) / d->frame_width);
        int y_scale = (int)(((int64_t)ref->height << REF_SCALE_SHIFT) / d->frame_height);
        int base_x = (int)(((int64_t)x * x_scale) >> REF_SCALE_SHIFT);
        int base_y = (int)(((int64_t)y * y_scale) >> REF_SCALE_SHIFT);
        int luma_x = plane > 0 ? x << d->subsampling_x : x;
        int luma_y = plane > 0 ? y << d->subsampling_y : y;
        int frac_x = (int)(((int64_t)16 * luma_x * x_scale) >> REF_SCALE_SHIFT) & SUBPEL_MASK;
        int frac_y = (int)(((int64_t)16 * luma_y * y_scale) >> REF_SCALE_SHIFT) & SUBPEL_MASK;
        int dx = (int)(((int64_t)cmv_c * x_scale) >> REF_SCALE_SHIFT) + frac_x;
        int dy = (int)(((int64_t)cmv_r * y_scale) >> REF_SCALE_SHIFT) + frac_y;
        int step_x = (16 * x_scale) >> REF_SCALE_SHIFT;
        int step_y = (16 * y_scale) >> REF_SCALE_SHIFT;
        int start_x = (base_x << SUBPEL_BITS) + dx;
        int start_y = (base_y << SUBPEL_BITS) + dy;
        if (ref_list == 0) {
            block_inter(d, ref, plane, start_x, start_y, step_x, step_y, w, h, d->interp_filter, dst, dst_stride);
        } else {
            pel* tmp = d->pred_tmp[1];
            block_inter(d, ref, plane, start_x, start_y, step_x, step_y, w, h, d->interp_filter, tmp, 64);
            for (int r = 0; r < h; r++) {
                pel* o = dst + (size_t)r * dst_stride;
                const pel* t = tmp + r * 64;
                for (int c = 0; c < w; c++) o[c] = (pel)((o[c] + t[c] + 1) >> 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// inverse transforms (8.7)

static inline int32_t cos64(int angle) {
    int a = angle & 127;
    if (a <= 32) return cos64_lookup[a];
    if (a <= 64) return -cos64_lookup[64 - a];
    if (a <= 96) return -cos64_lookup[a - 64];
    return cos64_lookup[128 - a];
}
static inline int32_t sin64(int angle) { return cos64(angle - 32); }
static inline int32_t r14(int64_t x) { return (int32_t)((x + 8192) >> 14); }

static int brev(int bits, int x) {
    int t = 0;
    for (int i = 0; i < bits; i++) t += ((x >> i) & 1) << (bits - 1 - i);
    return t;
}

static void bfly(int32_t* T, int a, int b, int angle, int flip) {
    int64_t c = cos64(angle), s = sin64(angle);
    int64_t x = T[a] * c - T[b] * s;
    int64_t y = T[a] * s + T[b] * c;
    int32_t ta = r14(x), tb = r14(y);
    if (flip) { T[a] = tb; T[b] = ta; }
    else { T[a] = ta; T[b] = tb; }
}

static void had(int32_t* T, int a, int b, int flip) {
    if (flip) { int t = a; a = b; b = t; }
    int32_t x = T[a], y = T[b];
    T[a] = x + y;
    T[b] = x - y;
}

static void sbfly(int64_t* S, const int32_t* T, int a, int b, int angle, int flip) {
    int64_t c = cos64(angle), s = sin64(angle);
    int64_t x = T[a] * c - T[b] * s;
    int64_t y = T[a] * s + T[b] * c;
    if (flip) { S[a] = y; S[b] = x; }
    else { S[a] = x; S[b] = y; }
}

static void shad(int32_t* T, const int64_t* S, int a, int b) {
    int32_t x = r14(S[a] + S[b]);
    int32_t y = r14(S[a] - S[b]);
    T[a] = x;
    T[b] = y;
}

static void idct_core(int32_t* T, int n) {
    int n0 = 1 << n, n1 = n0 >> 1, n2 = n0 >> 2, n3 = n0 >> 3;
    if (n == 2) bfly(T, 0, 1, 16, 1);
    else idct_core(T, n - 1);
    for (int i = 0; i < n2; i++) bfly(T, n1 + i, n0 - 1 - i, 32 - brev(5, n1 + i), 0);
    if (n >= 3)
        for (int i = 0; i < n3; i++)
            for (int j = 0; j < 2; j++) had(T, n1 + 4 * i + 2 * j, n1 + 1 + 4 * i + 2 * j, j);
    if (n == 5) {
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                bfly(T, n0 - n + 3 - n2 * j - 4 * i, n1 + n - 4 + n2 * j + 4 * i, 28 - 16 * i + 56 * j, 1);
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 4; j++) had(T, n1 + n3 * j + i, n1 + n2 - 5 + n3 * j - i, j & 1);
    }
    if (n >= 4) {
        for (int i = 0; i <= (n == 5); i++)
            for (int j = 0; j < 2; j++) bfly(T, n0 - n + 2 - i - n2 * j, n1 + n - 3 + i + n2 * j, 24 + 48 * j, 1);
        for (int i = 0; i <= 2 * n - 7; i++)
            for (int j = 0; j < 2; j++) had(T, n1 + n2 * j + i, n1 + n2 - 1 + n2 * j - i, j & 1);
    }
    if (n >= 3)
        for (int i = 0; i < n3; i++) bfly(T, n0 - n3 - 1 - i, n1 + n3 + i, 16, 1);
    for (int i = 0; i < n1; i++) had(T, i, n0 - 1 - i, 0);
}

static void idct(int32_t* T, int n) {
    int32_t c[32];
    int n0 = 1 << n;
    memcpy(c, T, (size_t)n0 * sizeof(int32_t));
    for (int i = 0; i < n0; i++) T[i] = c[brev(n, i)];
    idct_core(T, n);
}

static void adst_in_perm(int32_t* T, int n) {
    int32_t c[16];
    int n0 = 1 << n, n1 = n0 >> 1;
    memcpy(c, T, (size_t)n0 * sizeof(int32_t));
    for (int i = 0; i < n1; i++) {
        T[2 * i] = c[n0 - 1 - 2 * i];
        T[2 * i + 1] = c[2 * i];
    }
}

static void adst_out_perm(int32_t* T, int n) {
    int32_t c[16];
    memcpy(c, T, (size_t)(1 << n) * sizeof(int32_t));
    if (n == 4) {
        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++)
                for (int cc = 0; cc < 2; cc++)
                    for (int dd = 0; dd < 2; dd++)
                        T[8 * a + 4 * b + 2 * cc + dd] = c[8 * (dd ^ cc) + 4 * (cc ^ b) + 2 * (b ^ a) + a];
    } else {
        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++)
                for (int cc = 0; cc < 2; cc++) T[4 * a + 2 * b + cc] = c[4 * (cc ^ b) + 2 * (b ^ a) + a];
    }
}

#define SINPI_1_9 5283
#define SINPI_2_9 9929
#define SINPI_3_9 13377
#define SINPI_4_9 15212

static void iadst4(int32_t* T) {
    int64_t s0 = (int64_t)SINPI_1_9 * T[0];
    int64_t s1 = (int64_t)SINPI_2_9 * T[0];
    int64_t s2 = (int64_t)SINPI_3_9 * T[1];
    int64_t s3 = (int64_t)SINPI_4_9 * T[2];
    int64_t s4 = (int64_t)SINPI_1_9 * T[2];
    int64_t s5 = (int64_t)SINPI_2_9 * T[3];
    int64_t s6 = (int64_t)SINPI_4_9 * T[3];
    int32_t v = T[0] - T[2] + T[3];
    int64_t s7 = (int64_t)SINPI_3_9 * v;
    int64_t x0 = s0 + s3 + s5;
    int64_t x1 = s1 - s4 - s6;
    int64_t x2 = s7;
    int64_t x3 = s2;
    s0 = x0 + x3;
    s1 = x1 + x3;
    s2 = x2;
    s3 = x0 + x1 - x3;
    T[0] = r14(s0);
    T[1] = r14(s1);
    T[2] = r14(s2);
    T[3] = r14(s3);
}

static void iadst8(int32_t* T) {
    int64_t S[8];
    adst_in_perm(T, 3);
    for (int i = 0; i < 4; i++) sbfly(S, T, 2 * i, 1 + 2 * i, 30 - 8 * i, 1);
    for (int i = 0; i < 4; i++) shad(T, S, i, 4 + i);
    for (int i = 0; i < 2; i++) sbfly(S, T, 4 + 3 * i, 5 + i, 24 - 16 * i, 1);
    for (int i = 0; i < 2; i++) shad(T, S, 4 + i, 6 + i);
    for (int i = 0; i < 2; i++) had(T, i, 2 + i, 0);
    for (int i = 0; i < 2; i++) bfly(T, 2 + 4 * i, 3 + 4 * i, 16, 1);
    adst_out_perm(T, 3);
    for (int i = 0; i < 4; i++) T[1 + 2 * i] = -T[1 + 2 * i];
}

static void iadst16(int32_t* T) {
    int64_t S[16];
    adst_in_perm(T, 4);
    for (int i = 0; i < 8; i++) sbfly(S, T, 2 * i, 1 + 2 * i, 31 - 4 * i, 1);
    for (int i = 0; i < 8; i++) shad(T, S, i, 8 + i);
    for (int i = 0; i < 4; i++) sbfly(S, T, 8 + 2 * i, 9 + 2 * i, 28 - 16 * i, 1);
    for (int i = 0; i < 4; i++) shad(T, S, 8 + i, 12 + i);
    for (int i = 0; i < 4; i++) had(T, i, 4 + i, 0);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) sbfly(S, T, 4 + 8 * i + 3 * j, 5 + 8 * i + j, 24 - 16 * j, 1);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) shad(T, S, 4 + 8 * j + i, 6 + 8 * j + i);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) had(T, 8 * j + i, 2 + 8 * j + i, 0);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) bfly(T, 2 + 4 * j + 8 * i, 3 + 4 * j + 8 * i, 48 + 64 * (i ^ j), 0);
    adst_out_perm(T, 4);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) T[1 + 12 * j + 2 * i] = -T[1 + 12 * j + 2 * i];
}

static void iadst(int32_t* T, int n) {
    if (n == 2) iadst4(T);
    else if (n == 3) iadst8(T);
    else iadst16(T);
}

static void iwht(int32_t* T, int shift) {
    int32_t a = T[0] >> shift, c = T[1] >> shift, dd = T[2] >> shift, b = T[3] >> shift;
    a += c;
    dd -= b;
    int32_t e = (a - dd) >> 1;
    b = e - b;
    c = e - c;
    a -= b;
    dd += c;
    T[0] = a;
    T[1] = b;
    T[2] = c;
    T[3] = dd;
}

void vp9_reconstruct(struct vp9_dec* d, int plane, int x, int y, int tx_sz, int eob) {
    (void)eob;
    int n = 2 + tx_sz;
    int n0 = 1 << n;
    int32_t* D = d->coefs;                                  // Dequant[i][j] = D[i * n0 + j]
    int32_t T[32];
    int tx_type = d->tx_type;

    for (int i = 0; i < n0; i++) {
        int32_t* row = D + i * n0;
        int any = 0;
        for (int j = 0; j < n0; j++) any |= row[j];
        if (!any) continue;
        for (int j = 0; j < n0; j++) T[j] = row[j];
        if (d->lossless) iwht(T, 2);
        else if (tx_type == DCT_DCT || tx_type == ADST_DCT) idct(T, n);
        else iadst(T, n);
        for (int j = 0; j < n0; j++) row[j] = T[j];
    }
    struct vp9_buf* b = d->cur;
    pel* dst = b->plane[plane] + (size_t)y * b->stride[plane] + x;
    int stride = b->stride[plane];
    int maxv = (1 << d->bit_depth) - 1;
    int shift = vp9_imin(6, n + 2);
    for (int j = 0; j < n0; j++) {
        for (int i = 0; i < n0; i++) T[i] = D[i * n0 + j];
        if (d->lossless) iwht(T, 0);
        else if (tx_type == DCT_DCT || tx_type == DCT_ADST) idct(T, n);
        else iadst(T, n);
        for (int i = 0; i < n0; i++) {
            int r = d->lossless ? T[i] : vp9_round2(T[i], shift);
            pel* o = dst + (size_t)i * stride + j;
            *o = (pel)vp9_clip3(0, maxv, *o + r);
        }
    }
    memset(D, 0, (size_t)n0 * n0 * sizeof(int32_t));
}

// ---------------------------------------------------------------------------
// reference borders

void vp9_extend_borders(struct vp9_buf* b) {
    for (int p = 0; p < 3; p++) {
        int pw = p ? b->cw : b->width, ph = p ? b->ch : b->height;
        int ew = b->ext_w[p], eh = b->ext_h[p];
        int stride = b->stride[p];
        pel* base = b->plane[p];
        for (int y = 0; y < ph; y++) {
            pel* row = base + (ptrdiff_t)y * stride;
            pel l = row[0], r = row[pw - 1];
            for (int x = 1; x <= VP9_BORDER; x++) row[-x] = l;
            for (int x = pw; x < ew + VP9_BORDER; x++) row[x] = r;
        }
        for (int y = ph; y < eh + VP9_BORDER; y++)
            memcpy(base + (ptrdiff_t)y * stride - VP9_BORDER, base + (ptrdiff_t)(ph - 1) * stride - VP9_BORDER,
                   (size_t)(ew + 2 * VP9_BORDER) * sizeof(pel));
        for (int y = 1; y <= VP9_BORDER; y++)
            memcpy(base - (ptrdiff_t)y * stride - VP9_BORDER, base - VP9_BORDER, (size_t)(ew + 2 * VP9_BORDER) * sizeof(pel));
    }
}

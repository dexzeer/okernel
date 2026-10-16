// VP9 loop filter (spec section 8.8). The specification describes the decisions per sample;
// every decision is constant over runs of 8 samples along an edge, so they are taken once per
// run and the sample filters are applied in a loop.

#include "vp9_int.h"
#include "vp9_simd.h"

static void loop_filter_frame_init(struct vp9_dec* d) {
    int n_shift = d->loop_filter_level >> 5;
    int scale = 1 << n_shift;
    for (int seg = 0; seg < MAX_SEGMENTS; seg++) {
        int lvl_seg = d->loop_filter_level;
        if (d->seg_enabled && d->feature_enabled[seg][SEG_LVL_ALT_L]) {
            int data = d->feature_data[seg][SEG_LVL_ALT_L];
            lvl_seg = d->seg_abs_or_delta_update ? data : lvl_seg + data;
            lvl_seg = vp9_clip3(0, MAX_LOOP_FILTER, lvl_seg);
        }
        if (!d->loop_filter_delta_enabled) {
            memset(d->lvl_lookup[seg], lvl_seg, sizeof d->lvl_lookup[seg]);
        } else {
            int intra = lvl_seg + d->loop_filter_ref_deltas[INTRA_FRAME] * scale;
            d->lvl_lookup[seg][INTRA_FRAME][0] = (uint8_t)vp9_clip3(0, MAX_LOOP_FILTER, intra);
            d->lvl_lookup[seg][INTRA_FRAME][1] = d->lvl_lookup[seg][INTRA_FRAME][0];
            for (int ref = LAST_FRAME; ref < MAX_REF_FRAMES; ref++)
                for (int mode = 0; mode < MAX_MODE_LF_DELTAS; mode++) {
                    int inter = lvl_seg + d->loop_filter_ref_deltas[ref] * scale + d->loop_filter_mode_deltas[mode] * scale;
                    d->lvl_lookup[seg][ref][mode] = (uint8_t)vp9_clip3(0, MAX_LOOP_FILTER, inter);
                }
        }
    }
}

// sample filtering (8.8.5); `q` points at q0 and `step` is the distance between samples across the edge.
// The thresholds are scaled to the bit depth once per edge run (lf_params), the function is inlined into one loop per filter
// size (so the size tests fold away), and the wide filters use running sums instead of re-adding 15 taps per output.
#define lf_params vp9_lf_p

#define ABSD(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))

static inline __attribute__((always_inline)) void sample_filter(pel* q, ptrdiff_t step, int filter_size, const struct lf_params* P) {
    int q0 = q[0], q1 = q[step], q2 = q[2 * step], q3 = q[3 * step];
    int p0 = q[-step], p1 = q[-2 * step], p2 = q[-3 * step], p3 = q[-4 * step];
    int lim = P->limit, one = P->one;
    int ap1p0 = ABSD(p1, p0), aq1q0 = ABSD(q1, q0);
    if (ABSD(p3, p2) > lim || ABSD(p2, p1) > lim || ap1p0 > lim || aq1q0 > lim || ABSD(q2, q1) > lim || ABSD(q3, q2) > lim ||
        ABSD(p0, q0) * 2 + ABSD(p1, q1) / 2 > P->blimit)
        return;                                                    // filterMask == 0
    int hev = ap1p0 > P->thresh || aq1q0 > P->thresh;
    int flat = 0, flat2 = 0;
    if (filter_size >= TX_8X8)
        flat = !(ap1p0 > one || aq1q0 > one || ABSD(p2, p0) > one || ABSD(q2, q0) > one || ABSD(p3, p0) > one || ABSD(q3, q0) > one);
    if (filter_size >= TX_16X16 && flat) {
        int q4 = q[4 * step], q5 = q[5 * step], q6 = q[6 * step], q7 = q[7 * step];
        int p4 = q[-5 * step], p5 = q[-6 * step], p6 = q[-7 * step], p7 = q[-8 * step];
        flat2 = !(ABSD(p7, p0) > one || ABSD(q7, q0) > one || ABSD(p6, p0) > one || ABSD(q6, q0) > one ||
                  ABSD(p5, p0) > one || ABSD(q5, q0) > one || ABSD(p4, p0) > one || ABSD(q4, q0) > one);
    }

    if (filter_size == TX_4X4 || !flat) {
        // narrow filter (8.8.5.2)
        int lo = P->lo, hi = P->hi, off = P->off;
        int ps1 = p1 - off, ps0 = p0 - off, qs0 = q0 - off, qs1 = q1 - off;
        int filter = hev ? vp9_clip3(lo, hi, ps1 - qs1) : 0;
        filter = vp9_clip3(lo, hi, filter + 3 * (qs0 - ps0));
        int filter1 = vp9_clip3(lo, hi, filter + 4) >> 3;
        int filter2 = vp9_clip3(lo, hi, filter + 3) >> 3;
        q[0] = (pel)(vp9_clip3(lo, hi, qs0 - filter1) + off);
        q[-step] = (pel)(vp9_clip3(lo, hi, ps0 + filter2) + off);
        if (!hev) {
            filter = vp9_round2(filter1, 1);
            q[step] = (pel)(vp9_clip3(lo, hi, qs1 - filter) + off);
            q[-2 * step] = (pel)(vp9_clip3(lo, hi, ps1 + filter) + off);
        }
        return;
    }
    // wide filter (8.8.5.3): F[i] = round2(v[i] + sum_{j=-n..n} v[clip(i + j)], log2), clip to [-(n+1), n]
    int log2 = (filter_size == TX_8X8 || !flat2) ? 3 : 4;
    int n = (1 << (log2 - 1)) - 1;
    int v[16];                                                     // v[k + 8] = sample at offset k (-8 .. 7)
    for (int k = -(n + 1); k <= n; k++) v[k + 8] = q[k * step];
    int lo_i = -(n + 1);
    // window for i = -n: offsets -2n .. 0 clipped
    int sum = 0;
    for (int j = -n; j <= n; j++) { int p = -n + j; if (p < lo_i) p = lo_i; if (p > n) p = n; sum += v[p + 8]; }
    int rnd = 1 << (log2 - 1);
    for (int i = -n; i < n; i++) {
        q[i * step] = (pel)((sum + v[i + 8] + rnd) >> log2);
        int add = i + n + 1; if (add > n) add = n;                  // the sample entering the window
        int sub = i - n; if (sub < lo_i) sub = lo_i;                // the sample leaving it
        sum += v[add + 8] - v[sub + 8];
    }
}

static void superblock_filter(struct vp9_dec* d, int plane, int pass, int row, int col, int simd) {
    struct vp9_buf* b = d->cur;
    int sub_x = plane ? d->subsampling_x : 0, sub_y = plane ? d->subsampling_y : 0;
    int dx = pass == 0;
    int sub = pass == 0 ? sub_x : sub_y;
    int edge_len = pass == 0 ? 64 >> sub_y : 64 >> sub_x;
    pel* base = b->plane[plane];
    int stride = b->stride[plane];
    ptrdiff_t step = dx ? 1 : stride;                              // across the edge
    ptrdiff_t along = dx ? stride : 1;                             // along the edge
    int sharp = d->loop_filter_sharpness;
    int shift = sharp > 4 ? 2 : sharp > 0 ? 1 : 0;

    for (int edge = 0; edge < (16 >> sub); edge++) {
        for (int i0 = 0; i0 < edge_len; i0 += 8) {
            int x, y;
            if (pass == 0) { x = col * 8 + edge * (4 << sub_x); y = row * 8 + (i0 << sub_y); }
            else { x = col * 8 + (i0 << sub_x); y = row * 8 + edge * (4 << sub_y); }
            // on screen?
            if (x >= 8 * d->mi_cols || y >= 8 * d->mi_rows) continue;
            if (pass == 0 && x == 0) continue;
            if (pass == 1 && y == 0) continue;
            int loop_col = ((x >> 3) >> sub_x) << sub_x;
            int loop_row = ((y >> 3) >> sub_y) << sub_y;
            const struct mi* m = d->mi + (size_t)loop_row * d->mi_cols + loop_col;
            int mi_size = m->sb_type;
            int tx_size = m->tx_size;
            int tx_sz;
            if (plane > 0) {
                if (mi_size < BLOCK_8X8) tx_sz = TX_4X4;
                else tx_sz = vp9_imin(tx_size, vp9_max_txsize[vp9_ss_size[mi_size][d->subsampling_x][d->subsampling_y]]);
            } else {
                tx_sz = tx_size;
            }
            int sb_size = sub ? vp9_imax(BLOCK_16X16, mi_size) : mi_size;
            int skip = m->skip;
            int is_intra = m->ref[0] <= INTRA_FRAME;
            int is_block_edge;
            if (pass == 0) is_block_edge = (x % (8 * vp9_num_8x8_wide[sb_size])) == 0;
            else is_block_edge = (y % (8 * vp9_num_8x8_high[sb_size])) == 0;
            int is_tx_edge;
            if (pass == 1 && sub_x == 1 && (d->mi_cols & 1) && (edge & 1) && (x + 8) >= d->mi_cols * 8) is_tx_edge = 0;
            else is_tx_edge = (edge % (1 << tx_sz)) == 0;
            int is32 = (edge % 8) == 0;
            int apply = is_block_edge || (is_tx_edge && (is_intra || !skip));
            if (!apply) continue;

            // filter size (8.8.3)
            int base_size = (tx_sz == TX_4X4 && is32) ? TX_8X8 : vp9_imin(TX_16X16, tx_sz);
            int fsize;
            if (pass == 0 && sub_x == 1 && base_size == TX_16X16 && (x >> 3) == d->mi_cols - 1) fsize = TX_8X8;
            else if (pass == 1 && sub_y == 1 && base_size == TX_16X16 && (y >> 3) == d->mi_rows - 1) fsize = TX_8X8;
            else fsize = base_size;

            // adaptive filter strength (8.8.4)
            int mode_type = (m->y_mode == NEARESTMV || m->y_mode == NEARMV || m->y_mode == NEWMV) ? 1 : 0;
            int lvl = d->lvl_lookup[m->seg_id][m->ref[0]][mode_type];
            if (lvl == 0) continue;
            int limit = sharp > 0 ? vp9_clip3(1, 9 - sharp, lvl >> shift) : vp9_imax(1, lvl >> shift);
            int blimit = 2 * (lvl + 2) + limit;
            int thresh = lvl >> 4;

            // number of samples of this run that lie inside the frame
            int count = 8;
            if (pass == 0) {
                int rem = (8 * d->mi_rows - y + (1 << sub_y) - 1) >> sub_y;
                if (rem < count) count = rem;
            } else {
                int rem = (8 * d->mi_cols - x + (1 << sub_x) - 1) >> sub_x;
                if (rem < count) count = rem;
            }
            pel* p = base + (ptrdiff_t)(y >> sub_y) * stride + (x >> sub_x);
            int bsh = d->bit_depth - 8;
            struct lf_params P = { limit << bsh, blimit << bsh, thresh << bsh, 1 << bsh, -(1 << (d->bit_depth - 1)),
                                   (1 << (d->bit_depth - 1)) - 1, 0x80 << bsh };
            if (count == 8 && simd) vp9_simd_lf_run(p, step, along, fsize, &P);          // eight lines at once (the frame edge leaves shorter runs to the scalar loops)
            else if (fsize == TX_4X4) { for (int k = 0; k < count; k++) sample_filter(p + k * along, step, TX_4X4, &P); }
            else if (fsize == TX_8X8) { for (int k = 0; k < count; k++) sample_filter(p + k * along, step, TX_8X8, &P); }
            else { for (int k = 0; k < count; k++) sample_filter(p + k * along, step, TX_16X16, &P); }
        }
    }
}

void vp9_loop_filter_frame(struct vp9_dec* d) {
    loop_filter_frame_init(d);
    int simd = vp9_simd_ok();
    for (int row = 0; row < d->mi_rows; row += 8)
        for (int col = 0; col < d->mi_cols; col += 8)
            for (int plane = 0; plane < 3; plane++)
                for (int pass = 0; pass < 2; pass++) superblock_filter(d, plane, pass, row, col, simd);
}

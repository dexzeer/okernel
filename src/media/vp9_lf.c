// VP9 loop filter (spec section 8.8). The specification describes the decisions per sample;
// every decision is constant over runs of 8 samples along an edge, so they are taken once per
// run and the sample filters are applied in a loop.

#include "vp9_int.h"

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

// sample filtering (8.8.5); `q` points at q0 and `step` is the distance between samples across the edge
static void sample_filter(pel* q, ptrdiff_t step, int filter_size, int limit, int blimit, int thresh, int bd) {
    int q0 = q[0], q1 = q[step], q2 = q[2 * step], q3 = q[3 * step];
    int p0 = q[-step], p1 = q[-2 * step], p2 = q[-3 * step], p3 = q[-4 * step];
    int sh = bd - 8;

    int hev = abs(p1 - p0) > (thresh << sh) || abs(q1 - q0) > (thresh << sh);
    int limit_bd = limit << sh, blimit_bd = blimit << sh;
    if (abs(p3 - p2) > limit_bd || abs(p2 - p1) > limit_bd || abs(p1 - p0) > limit_bd || abs(q1 - q0) > limit_bd ||
        abs(q2 - q1) > limit_bd || abs(q3 - q2) > limit_bd || abs(p0 - q0) * 2 + abs(p1 - q1) / 2 > blimit_bd)
        return;                                                    // filterMask == 0
    int one = 1 << sh;
    int flat = 0, flat2 = 0;
    if (filter_size >= TX_8X8)
        flat = !(abs(p1 - p0) > one || abs(q1 - q0) > one || abs(p2 - p0) > one || abs(q2 - q0) > one ||
                 abs(p3 - p0) > one || abs(q3 - q0) > one);
    if (filter_size >= TX_16X16) {
        int q4 = q[4 * step], q5 = q[5 * step], q6 = q[6 * step], q7 = q[7 * step];
        int p4 = q[-5 * step], p5 = q[-6 * step], p6 = q[-7 * step], p7 = q[-8 * step];
        flat2 = !(abs(p7 - p0) > one || abs(q7 - q0) > one || abs(p6 - p0) > one || abs(q6 - q0) > one ||
                  abs(p5 - p0) > one || abs(q5 - q0) > one || abs(p4 - p0) > one || abs(q4 - q0) > one);
    }

    if (filter_size == TX_4X4 || !flat) {
        // narrow filter (8.8.5.2)
        int lo = -(1 << (bd - 1)), hi = (1 << (bd - 1)) - 1;
        int off = 0x80 << sh;
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
    // wide filter (8.8.5.3)
    int log2 = (filter_size == TX_8X8 || !flat2) ? 3 : 4;
    int n = (1 << (log2 - 1)) - 1;
    int v[16];                                                     // v[k + 8] = sample at offset k (-8 .. 7)
    for (int k = -(n + 1); k <= n; k++) v[k + 8] = q[k * step];
    int F[16];
    for (int i = -n; i < n; i++) {
        int t = v[i + 8];
        for (int j = -n; j <= n; j++) {
            int p = vp9_clip3(-(n + 1), n, i + j);
            t += v[p + 8];
        }
        F[i + 8] = vp9_round2(t, log2);
    }
    for (int i = -n; i < n; i++) q[i * step] = (pel)F[i + 8];
}

static void superblock_filter(struct vp9_dec* d, int plane, int pass, int row, int col) {
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
            for (int k = 0; k < count; k++)
                sample_filter(p + k * along, step, fsize, limit, blimit, thresh, d->bit_depth);
        }
    }
}

void vp9_loop_filter_frame(struct vp9_dec* d) {
    loop_filter_frame_init(d);
    for (int row = 0; row < d->mi_rows; row += 8)
        for (int col = 0; col < d->mi_cols; col += 8)
            for (int plane = 0; plane < 3; plane++)
                for (int pass = 0; pass < 2; pass++) superblock_filter(d, plane, pass, row, col);
}

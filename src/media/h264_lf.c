// H.264 deblocking filter (8.7), progressive frames, 4:2:0.
#include "h264_int.h"

static inline int iabs(int x) { return x < 0 ? -x : x; }
static inline int clip3(int lo, int hi, int x) { return x < lo ? lo : x > hi ? hi : x; }
static inline uint8_t clip1(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

// Filter `n` sample lines across one edge. pix points at q0 of the first line, xs steps across the
// edge (towards q), ys steps along it.
static void filter_luma(uint8_t* pix, int xs, int ys, int n, int bs, int alpha, int beta, int tc0) {
    for (int k = 0; k < n; k++, pix += ys) {
        int p0 = pix[-xs], p1 = pix[-2 * xs], q0 = pix[0], q1 = pix[xs];
        if (!(iabs(p0 - q0) < alpha && iabs(p1 - p0) < beta && iabs(q1 - q0) < beta)) continue;
        int p2 = pix[-3 * xs], q2 = pix[2 * xs];
        int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
        if (bs < 4) {
            int tc = tc0 + (ap < beta) + (aq < beta);
            int delta = clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
            pix[-xs] = clip1(p0 + delta);
            pix[0] = clip1(q0 - delta);
            if (ap < beta) pix[-2 * xs] = (uint8_t)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 * 2)) >> 1));
            if (aq < beta) pix[xs] = (uint8_t)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 * 2)) >> 1));
        } else {
            int p3 = pix[-4 * xs], q3 = pix[3 * xs];
            int small = iabs(p0 - q0) < ((alpha >> 2) + 2);
            if (ap < beta && small) {
                pix[-xs] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                pix[-2 * xs] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                pix[-3 * xs] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else pix[-xs] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            if (aq < beta && small) {
                pix[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                pix[xs] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                pix[2 * xs] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static void filter_chroma(uint8_t* pix, int xs, int ys, int n, int bs, int alpha, int beta, int tc0) {
    for (int k = 0; k < n; k++, pix += ys) {
        int p0 = pix[-xs], p1 = pix[-2 * xs], q0 = pix[0], q1 = pix[xs];
        if (!(iabs(p0 - q0) < alpha && iabs(p1 - p0) < beta && iabs(q1 - q0) < beta)) continue;
        if (bs < 4) {
            int tc = tc0 + 1;
            int delta = clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
            pix[-xs] = clip1(p0 + delta);
            pix[0] = clip1(q0 - delta);
        } else {
            pix[-xs] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

// motion of one 4x4 block (picture coordinates in 4x4 units) for list l: reference uid (-1 none) and vector
static inline void motion(const struct h264_frame* f, int bx, int by, int l, int* uid, int* mx, int* my) {
    int w4 = f->mb_width * 4, w8 = f->mb_width * 2;
    *uid = f->ref_uid[l][(by >> 1) * w8 + (bx >> 1)];
    const int16_t* v = f->mv[l] + (by * w4 + bx) * 2;
    *mx = v[0]; *my = v[1];
}

static int mv_differs(int ax, int ay, int bx, int by) { return iabs(ax - bx) >= 4 || iabs(ay - by) >= 4; }

// bS = 1 test between two inter blocks (8.7.2.1)
static int motion_bs(const struct h264_frame* f, int pbx, int pby, int qbx, int qby) {
    int pu[2], pmx[2], pmy[2], qu[2], qmx[2], qmy[2];
    for (int l = 0; l < 2; l++) { motion(f, pbx, pby, l, &pu[l], &pmx[l], &pmy[l]); motion(f, qbx, qby, l, &qu[l], &qmx[l], &qmy[l]); }
    int pc = (pu[0] >= 0) + (pu[1] >= 0), qc = (qu[0] >= 0) + (qu[1] >= 0);
    if (pc != qc) return 1;
    if (pc == 1) {
        int a = pu[0] >= 0 ? 0 : 1, b = qu[0] >= 0 ? 0 : 1;
        if (pu[a] != qu[b]) return 1;
        return mv_differs(pmx[a], pmy[a], qmx[b], qmy[b]);
    }
    if (pc == 0) return 0;
    // two vectors each
    if (!((pu[0] == qu[0] && pu[1] == qu[1]) || (pu[0] == qu[1] && pu[1] == qu[0]))) return 1;
    if (pu[0] != pu[1]) {
        if (pu[0] == qu[0]) return mv_differs(pmx[0], pmy[0], qmx[0], qmy[0]) || mv_differs(pmx[1], pmy[1], qmx[1], qmy[1]);
        return mv_differs(pmx[0], pmy[0], qmx[1], qmy[1]) || mv_differs(pmx[1], pmy[1], qmx[0], qmy[0]);
    }
    // both vectors refer to the same picture
    int straight = mv_differs(pmx[0], pmy[0], qmx[0], qmy[0]) || mv_differs(pmx[1], pmy[1], qmx[1], qmy[1]);
    int crossed = mv_differs(pmx[0], pmy[0], qmx[1], qmy[1]) || mv_differs(pmx[1], pmy[1], qmx[0], qmy[0]);
    return straight && crossed;
}

static int chroma_qp(int qpy, int off) {
    int qpi = clip3(0, 51, qpy + off);
    return qpi < 30 ? qpi : h264_qpc_tab[qpi - 30];
}

void h264_deblock_picture(struct h264_dec* d) {
    const struct h264_sps* sps = d->asps;
    struct h264_frame* f = d->cur;
    int mbw = sps->mb_width, mbh = sps->mb_height;
    int w4 = mbw * 4;
    for (int my = 0; my < mbh; my++) {
        for (int mx = 0; mx < mbw; mx++) {
            int idx = my * mbw + mx;
            struct h264_mbinfo* q = &d->mb[idx];
            if (q->slice_num < 0) continue;
            const struct h264_sliceinfo* si = &d->sinfo[q->slice_num];
            if (si->disable_idc == 1) continue;
            int left = mx > 0 && !(si->disable_idc == 2 && d->mb[idx - 1].slice_num != q->slice_num) && d->mb[idx - 1].slice_num >= 0;
            int top = my > 0 && !(si->disable_idc == 2 && d->mb[idx - mbw].slice_num != q->slice_num) && d->mb[idx - mbw].slice_num >= 0;
            // boundary strengths: bs[dir][edge][segment]
            uint8_t bs[2][4][4];
            int q_intra = q->type != MBK_INTER;
            for (int dir = 0; dir < 2; dir++) {
                for (int e = 0; e < 4; e++) {
                    int skip_edge = (e == 0 && !(dir == 0 ? left : top)) || (q->t8x8 && (e & 1));
                    for (int s = 0; s < 4; s++) {
                        if (skip_edge) { bs[dir][e][s] = 0; continue; }
                        int qx = dir == 0 ? e : s, qy = dir == 0 ? s : e;       // 4x4 block of q inside the macroblock
                        int px, py, pidx = idx;
                        if (dir == 0) { px = qx - 1; py = qy; if (px < 0) { px = 3; pidx = idx - 1; } }
                        else { px = qx; py = qy - 1; if (py < 0) { py = 3; pidx = idx - mbw; } }
                        const struct h264_mbinfo* p = &d->mb[pidx];
                        int v;
                        if (e == 0 && (p->type != MBK_INTER || q_intra)) v = 4;
                        else if (p->type != MBK_INTER || q_intra) v = 3;
                        else if ((p->nzdb >> (py * 4 + px)) & 1 || (q->nzdb >> (qy * 4 + qx)) & 1) v = 2;
                        else {
                            int pbx = (pidx % mbw) * 4 + px, pby = (pidx / mbw) * 4 + py;
                            v = motion_bs(f, pbx, pby, mx * 4 + qx, my * 4 + qy);
                        }
                        bs[dir][e][s] = (uint8_t)v;
                    }
                }
            }
            (void)w4;
            // ---- luma
            uint8_t* base = f->plane[0] + (size_t)my * 16 * f->stride[0] + mx * 16;
            int stride = f->stride[0];
            for (int dir = 0; dir < 2; dir++) {
                for (int e = 0; e < 4; e++) {
                    int pidx = e == 0 ? (dir == 0 ? idx - 1 : idx - mbw) : idx;
                    int any = 0;
                    for (int s = 0; s < 4; s++) any |= bs[dir][e][s];
                    if (!any) continue;
                    int qpav = (d->mb[pidx].qp + q->qp + 1) >> 1;
                    int ia = clip3(0, 51, qpav + si->alpha_off), ib = clip3(0, 51, qpav + si->beta_off);
                    int alpha = h264_alpha_tab[ia], beta = h264_beta_tab[ib];
                    if (!alpha || !beta) continue;
                    for (int s = 0; s < 4; s++) {
                        int b = bs[dir][e][s];
                        if (!b) continue;
                        int tc0 = b < 4 ? h264_tc0_tab[b - 1][ia] : 0;
                        if (dir == 0) filter_luma(base + s * 4 * stride + e * 4, 1, stride, 4, b, alpha, beta, tc0);
                        else filter_luma(base + e * 4 * stride + s * 4, stride, 1, 4, b, alpha, beta, tc0);
                    }
                }
            }
            // ---- chroma (4:2:0): edges at 0 and 4 samples, bS taken from the luma edges 0 and 2
            for (int c = 0; c < 2; c++) {
                int cs = f->stride[1 + c];
                uint8_t* cb = f->plane[1 + c] + (size_t)my * 8 * cs + mx * 8;
                int off = si->chroma_qp_off[c];
                for (int dir = 0; dir < 2; dir++) {
                    for (int ce = 0; ce < 2; ce++) {
                        int e = ce * 2;
                        int pidx = ce == 0 ? (dir == 0 ? idx - 1 : idx - mbw) : idx;
                        int any = 0;
                        for (int s = 0; s < 4; s++) any |= bs[dir][e][s];
                        if (!any) continue;
                        int qpav = (chroma_qp(d->mb[pidx].qp, off) + chroma_qp(q->qp, off) + 1) >> 1;
                        int ia = clip3(0, 51, qpav + si->alpha_off), ib = clip3(0, 51, qpav + si->beta_off);
                        int alpha = h264_alpha_tab[ia], beta = h264_beta_tab[ib];
                        if (!alpha || !beta) continue;
                        for (int s = 0; s < 4; s++) {
                            int b = bs[dir][e][s];
                            if (!b) continue;
                            int tc0 = b < 4 ? h264_tc0_tab[b - 1][ia] : 0;
                            // luma segment s covers two chroma samples along the edge
                            if (dir == 0) filter_chroma(cb + s * 2 * cs + ce * 4, 1, cs, 2, b, alpha, beta, tc0);
                            else filter_chroma(cb + ce * 4 * cs + s * 2, cs, 1, 2, b, alpha, beta, tc0);
                        }
                    }
                }
            }
        }
    }
}

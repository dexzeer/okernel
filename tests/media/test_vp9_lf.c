// The SSE2 loop filter (vp9_simd_lf_run) against an independent scalar transcription of spec 8.8.5: random edge runs at 8/10/12 bits, all three
// filter sizes, both orientations, noise levels picked to reach every branch (masked out, narrow with and without hev, flat, flat2).
// Every byte of the buffer must match, including the guard samples around the run (nothing outside the taps may change).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../../src/media/vp9_simd.h"

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 16); }

static int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static int absd(int a, int b) { return a > b ? a - b : b - a; }

// one sample line (8.8.5): q points at q0
static void ref_line(uint16_t* q, ptrdiff_t step, int fsize, const struct vp9_lf_p* P) {
    int q0 = q[0], q1 = q[step], q2 = q[2 * step], q3 = q[3 * step];
    int p0 = q[-step], p1 = q[-2 * step], p2 = q[-3 * step], p3 = q[-4 * step];
    int hevmask = absd(p1, p0) > P->thresh || absd(q1, q0) > P->thresh;
    int mask = !(absd(p3, p2) > P->limit || absd(p2, p1) > P->limit || absd(p1, p0) > P->limit || absd(q1, q0) > P->limit ||
                 absd(q2, q1) > P->limit || absd(q3, q2) > P->limit || absd(p0, q0) * 2 + absd(p1, q1) / 2 > P->blimit);
    if (!mask) return;
    int flat = 0, flat2 = 0;
    if (fsize >= 1) flat = !(absd(p1, p0) > P->one || absd(q1, q0) > P->one || absd(p2, p0) > P->one || absd(q2, q0) > P->one || absd(p3, p0) > P->one || absd(q3, q0) > P->one);
    if (fsize >= 2 && flat) {
        flat2 = 1;
        for (int k = 4; k <= 7; k++) if (absd(q[-(k + 1) * step], p0) > P->one || absd(q[k * step], q0) > P->one) flat2 = 0;
    }
    if (fsize == 0 || !flat) {
        int ps1 = p1 - P->off, ps0 = p0 - P->off, qs0 = q0 - P->off, qs1 = q1 - P->off;
        int filter = hevmask ? clip3(P->lo, P->hi, ps1 - qs1) : 0;
        filter = clip3(P->lo, P->hi, filter + 3 * (qs0 - ps0));
        int filter1 = clip3(P->lo, P->hi, filter + 4) >> 3;
        int filter2 = clip3(P->lo, P->hi, filter + 3) >> 3;
        q[0] = (uint16_t)(clip3(P->lo, P->hi, qs0 - filter1) + P->off);
        q[-step] = (uint16_t)(clip3(P->lo, P->hi, ps0 + filter2) + P->off);
        if (!hevmask) {
            filter = (filter1 + 1) >> 1;
            q[step] = (uint16_t)(clip3(P->lo, P->hi, qs1 - filter) + P->off);
            q[-2 * step] = (uint16_t)(clip3(P->lo, P->hi, ps1 + filter) + P->off);
        }
        return;
    }
    int n = (fsize >= 2 && flat2) ? 7 : 3, log2 = (fsize >= 2 && flat2) ? 4 : 3;
    int v[16];
    for (int k = -(n + 1); k <= n; k++) v[k + 8] = q[k * step];
    for (int i = -n; i < n; i++) {                                     // straight from the definition: no running sum
        int sum = v[i + 8];
        for (int j = -n; j <= n; j++) { int p = i + j; if (p < -(n + 1)) p = -(n + 1); if (p > n) p = n; sum += v[p + 8]; }
        q[i * step] = (uint16_t)((sum + (1 << (log2 - 1))) >> log2);
    }
}

int main(void) {
    if (!vp9_simd_ok()) { printf("no SSE2: skipped\n"); return 0; }
    static const int noise[] = { 0, 1, 2, 3, 6, 12, 40, 200, 1000, 4000 };
    long runs = 0, bad = 0;
    for (long it = 0; it < 3000000 && bad < 10; it++) {
        int bd = 8 + 2 * (int)(rnd() % 3), bsh = bd - 8, maxv = (1 << bd) - 1;
        int fsize = (int)(rnd() % 3), vertical = (int)(rnd() & 1);
        int lvl = 1 + (int)(rnd() % 63), sharp = (int)(rnd() % 8), shift = sharp > 4 ? 2 : sharp > 0 ? 1 : 0;
        int limit = sharp > 0 ? clip3(1, 9 - sharp, lvl >> shift) : (1 > (lvl >> shift) ? 1 : (lvl >> shift));
        int blimit = 2 * (lvl + 2) + limit, thresh = lvl >> 4;
        struct vp9_lf_p P = { limit << bsh, blimit << bsh, thresh << bsh, 1 << bsh, -(1 << (bd - 1)), (1 << (bd - 1)) - 1, 0x80 << bsh };
        enum { W = 64 };                                              // a 64x64 sample window with the edge in the middle
        uint16_t a[W * W], b[W * W];
        int base = (int)(rnd() % (unsigned)(maxv + 1));
        int amp_in = noise[rnd() % 10] << bsh;                          // noise inside the taps; the far taps get their own amplitude (flat2)
        int amp_far = noise[rnd() % 10] << bsh;
        int step_edge = (rnd() & 1) ? 0 : (int)(rnd() % (unsigned)(8 << bsh));   // a step across the edge
        for (int y = 0; y < W; y++)
            for (int x = 0; x < W; x++) {
                int across = vertical ? x : y;                          // the coordinate across the edge (edge at 32)
                int dist = across - 32; if (dist < 0) dist = -dist;
                int amp = dist <= 4 ? amp_in : amp_far;
                int v = base + (across >= 32 ? step_edge : 0) + (amp ? (int)(rnd() % (unsigned)(2 * amp + 1)) - amp : 0);
                a[y * W + x] = (uint16_t)clip3(0, maxv, v);
            }
        memcpy(b, a, sizeof a);
        ptrdiff_t step = vertical ? 1 : W, along = vertical ? W : 1;
        uint16_t* qa = a + 28 * W + 32;                                 // vertical: q0 of line 0 at (x=32, y=28); the eight lines are y = 28..35
        uint16_t* qb = b + 28 * W + 32;
        if (!vertical) { qa = a + 32 * W + 28; qb = b + 32 * W + 28; }   // horizontal: q0 of line 0 at (x=28, y=32)
        for (int k = 0; k < 8; k++) ref_line(qa + k * along, step, fsize, &P);
        vp9_simd_lf_run(qb, step, along, fsize, &P);
        runs++;
        if (memcmp(a, b, sizeof a)) {
            bad++;
            int first = -1;
            for (int i = 0; i < W * W; i++) if (a[i] != b[i]) { first = i; break; }
            printf("MISMATCH it=%ld bd=%d fsize=%d %s lvl=%d sharp=%d at (%d,%d): ref %d simd %d\n", it, bd, fsize, vertical ? "vertical" : "horizontal", lvl, sharp, first % W, first / W, a[first], b[first]);
        }
    }
    printf("%ld runs, %ld mismatches\n%s\n", runs, bad, bad ? "VP9 LF TESTS FAIL" : "VP9 LF TESTS PASS");
    return bad != 0;
}

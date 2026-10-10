// H.264: intra prediction (8.3) and inverse transforms (8.5).
#include "h264_int.h"

static inline uint8_t clip255(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

// ---------------------------------------------------------------------------------------------
// Intra 4x4 (8.3.1.2). p[x,-1] = T(x), p[-1,y] = L(y), p[-1,-1] = TL.

void h264_intra4x4_pred(uint8_t* dst, int stride, int mode, int al, int at, int atr, int atl) {
    int T[8], L[4], TL = 0;
    if (at) {
        for (int x = 0; x < 4; x++) T[x] = dst[x - stride];
        for (int x = 4; x < 8; x++) T[x] = atr ? dst[x - stride] : T[3];
    }
    if (al) for (int y = 0; y < 4; y++) L[y] = dst[y * stride - 1];
    if (atl) TL = dst[-stride - 1];
#define P(x, y) dst[(y) * stride + (x)]
    switch (mode) {
    case 0:                                         // vertical
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (uint8_t)T[x];
        break;
    case 1:                                         // horizontal
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (uint8_t)L[y];
        break;
    case 2: {                                       // DC
        int v;
        if (at && al) v = (T[0] + T[1] + T[2] + T[3] + L[0] + L[1] + L[2] + L[3] + 4) >> 3;
        else if (al) v = (L[0] + L[1] + L[2] + L[3] + 2) >> 2;
        else if (at) v = (T[0] + T[1] + T[2] + T[3] + 2) >> 2;
        else v = 128;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (uint8_t)v;
        break;
    }
    case 3:                                         // diagonal down left
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
            P(x, y) = (x == 3 && y == 3) ? (uint8_t)((T[6] + 3 * T[7] + 2) >> 2)
                                         : (uint8_t)((T[x + y] + 2 * T[x + y + 1] + T[x + y + 2] + 2) >> 2);
        break;
    case 4:                                         // diagonal down right
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int v;
            if (x > y) v = ((x - y - 2 < 0 ? TL : T[x - y - 2]) + 2 * T[x - y - 1] + T[x - y] + 2) >> 2;
            else if (x < y) v = ((y - x - 2 < 0 ? TL : L[y - x - 2]) + 2 * L[y - x - 1] + L[y - x] + 2) >> 2;
            else v = (T[0] + 2 * TL + L[0] + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 5:                                         // vertical right
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = 2 * x - y, v;
            int i = x - (y >> 1);
            if (z >= 0 && !(z & 1)) v = ((i - 1 < 0 ? TL : T[i - 1]) + T[i] + 1) >> 1;
            else if (z >= 0) v = ((i - 2 < 0 ? TL : T[i - 2]) + 2 * (i - 1 < 0 ? TL : T[i - 1]) + T[i] + 2) >> 2;
            else if (z == -1) v = (L[0] + 2 * TL + T[0] + 2) >> 2;
            else v = (L[y - 1] + 2 * (y - 2 < 0 ? TL : L[y - 2]) + (y - 3 < 0 ? TL : L[y - 3]) + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 6:                                         // horizontal down
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = 2 * y - x, v;
            int i = y - (x >> 1);
            if (z >= 0 && !(z & 1)) v = ((i - 1 < 0 ? TL : L[i - 1]) + L[i] + 1) >> 1;
            else if (z >= 0) v = ((i - 2 < 0 ? TL : L[i - 2]) + 2 * (i - 1 < 0 ? TL : L[i - 1]) + L[i] + 2) >> 2;
            else if (z == -1) v = (L[0] + 2 * TL + T[0] + 2) >> 2;
            else v = (T[x - 1] + 2 * (x - 2 < 0 ? TL : T[x - 2]) + (x - 3 < 0 ? TL : T[x - 3]) + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 7:                                         // vertical left
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int i = x + (y >> 1);
            P(x, y) = (y & 1) ? (uint8_t)((T[i] + 2 * T[i + 1] + T[i + 2] + 2) >> 2) : (uint8_t)((T[i] + T[i + 1] + 1) >> 1);
        }
        break;
    default:                                        // 8: horizontal up
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = x + 2 * y, i = y + (x >> 1), v;
            if (z > 5) v = L[3];
            else if (z == 5) v = (L[2] + 3 * L[3] + 2) >> 2;
            else if (!(z & 1)) v = (L[i] + L[i + 1] + 1) >> 1;
            else v = (L[i] + 2 * L[i + 1] + L[i + 2] + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    }
#undef P
}

// ---------------------------------------------------------------------------------------------
// Intra 8x8 (8.3.2.2) with the reference sample filtering of 8.3.2.2.1

void h264_intra8x8_pred(uint8_t* dst, int stride, int mode, int al, int at, int atr, int atl) {
    int T[16], L[8], TL = 0;            // unfiltered
    int fT[16], fL[8], fTL = 0;         // filtered p'
    if (at) {
        for (int x = 0; x < 8; x++) T[x] = dst[x - stride];
        for (int x = 8; x < 16; x++) T[x] = atr ? dst[x - stride] : T[7];
    }
    if (al) for (int y = 0; y < 8; y++) L[y] = dst[y * stride - 1];
    if (atl) TL = dst[-stride - 1];
    if (at) {
        fT[0] = atl ? (TL + 2 * T[0] + T[1] + 2) >> 2 : (3 * T[0] + T[1] + 2) >> 2;
        for (int x = 1; x < 15; x++) fT[x] = (T[x - 1] + 2 * T[x] + T[x + 1] + 2) >> 2;
        fT[15] = (T[14] + 3 * T[15] + 2) >> 2;
    }
    if (atl) {
        if (!at || !al) {
            if (at) fTL = (3 * TL + T[0] + 2) >> 2;
            else if (al) fTL = (3 * TL + L[0] + 2) >> 2;
            else fTL = TL;
        } else fTL = (T[0] + 2 * TL + L[0] + 2) >> 2;
    }
    if (al) {
        fL[0] = atl ? (TL + 2 * L[0] + L[1] + 2) >> 2 : (3 * L[0] + L[1] + 2) >> 2;
        for (int y = 1; y < 7; y++) fL[y] = (L[y - 1] + 2 * L[y] + L[y + 1] + 2) >> 2;
        fL[7] = (L[6] + 3 * L[7] + 2) >> 2;
    }
#define P(x, y) dst[(y) * stride + (x)]
#define TT(i) ((i) < 0 ? fTL : fT[i])           // p'[i, -1], i = -1 is the corner
#define LL(i) ((i) < 0 ? fTL : fL[i])           // p'[-1, i]
    switch (mode) {
    case 0:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) P(x, y) = (uint8_t)fT[x];
        break;
    case 1:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) P(x, y) = (uint8_t)fL[y];
        break;
    case 2: {
        int v, st = 0, sl = 0;
        if (at) for (int i = 0; i < 8; i++) st += fT[i];
        if (al) for (int i = 0; i < 8; i++) sl += fL[i];
        if (at && al) v = (st + sl + 8) >> 4;
        else if (al) v = (sl + 4) >> 3;
        else if (at) v = (st + 4) >> 3;
        else v = 128;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) P(x, y) = (uint8_t)v;
        break;
    }
    case 3:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
            P(x, y) = (x == 7 && y == 7) ? (uint8_t)((fT[14] + 3 * fT[15] + 2) >> 2)
                                         : (uint8_t)((fT[x + y] + 2 * fT[x + y + 1] + fT[x + y + 2] + 2) >> 2);
        break;
    case 4:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int v;
            if (x > y) v = (TT(x - y - 2) + 2 * TT(x - y - 1) + TT(x - y) + 2) >> 2;
            else if (x < y) v = (LL(y - x - 2) + 2 * LL(y - x - 1) + LL(y - x) + 2) >> 2;
            else v = (fT[0] + 2 * fTL + fL[0] + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 5:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int z = 2 * x - y, v, i = x - (y >> 1);
            if (z >= 0 && !(z & 1)) v = (TT(i - 1) + TT(i) + 1) >> 1;
            else if (z >= 0) v = (TT(i - 2) + 2 * TT(i - 1) + TT(i) + 2) >> 2;
            else if (z == -1) v = (fL[0] + 2 * fTL + fT[0] + 2) >> 2;
            else v = (LL(y - 2 * x - 1) + 2 * LL(y - 2 * x - 2) + LL(y - 2 * x - 3) + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 6:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int z = 2 * y - x, v, i = y - (x >> 1);
            if (z >= 0 && !(z & 1)) v = (LL(i - 1) + LL(i) + 1) >> 1;
            else if (z >= 0) v = (LL(i - 2) + 2 * LL(i - 1) + LL(i) + 2) >> 2;
            else if (z == -1) v = (fL[0] + 2 * fTL + fT[0] + 2) >> 2;
            else v = (TT(x - 2 * y - 1) + 2 * TT(x - 2 * y - 2) + TT(x - 2 * y - 3) + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 7:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int i = x + (y >> 1);
            P(x, y) = (y & 1) ? (uint8_t)((fT[i] + 2 * fT[i + 1] + fT[i + 2] + 2) >> 2) : (uint8_t)((fT[i] + fT[i + 1] + 1) >> 1);
        }
        break;
    default:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int z = x + 2 * y, i = y + (x >> 1), v;
            if (z > 13) v = fL[7];
            else if (z == 13) v = (fL[6] + 3 * fL[7] + 2) >> 2;
            else if (!(z & 1)) v = (fL[i] + fL[i + 1] + 1) >> 1;
            else v = (fL[i] + 2 * fL[i + 1] + fL[i + 2] + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    }
#undef P
#undef TT
#undef LL
}

// ---------------------------------------------------------------------------------------------
// Intra 16x16 (8.3.3) and chroma (8.3.4)

void h264_intra16_pred(uint8_t* dst, int stride, int mode, int al, int at, int atl) {
#define P(x, y) dst[(y) * stride + (x)]
    switch (mode) {
    case 0:
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) P(x, y) = dst[x - stride];
        break;
    case 1:
        for (int y = 0; y < 16; y++) { uint8_t v = dst[y * stride - 1]; for (int x = 0; x < 16; x++) P(x, y) = v; }
        break;
    case 2: {
        int st = 0, sl = 0, v;
        if (at) for (int i = 0; i < 16; i++) st += dst[i - stride];
        if (al) for (int i = 0; i < 16; i++) sl += dst[i * stride - 1];
        if (at && al) v = (st + sl + 16) >> 5;
        else if (al) v = (sl + 8) >> 4;
        else if (at) v = (st + 8) >> 4;
        else v = 128;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) P(x, y) = (uint8_t)v;
        break;
    }
    default: {                                      // plane
        (void)atl;
        int H = 0, V = 0;
        for (int i = 0; i < 8; i++) {
            int a = dst[(8 + i) - stride], b = (6 - i) < 0 ? dst[-stride - 1] : dst[(6 - i) - stride];
            H += (i + 1) * (a - b);
            int c = dst[(8 + i) * stride - 1], e = (6 - i) < 0 ? dst[-stride - 1] : dst[(6 - i) * stride - 1];
            V += (i + 1) * (c - e);
        }
        int a = 16 * (dst[15 * stride - 1] + dst[15 - stride]);
        int b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) P(x, y) = clip255((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        break;
    }
    }
#undef P
}

// chroma 8x8 block (4:2:0); mode: 0 DC, 1 horizontal, 2 vertical, 3 plane
void h264_intra_chroma_pred(uint8_t* dst, int stride, int mode, int al, int at, int atl) {
#define P(x, y) dst[(y) * stride + (x)]
    (void)atl;
    switch (mode) {
    case 0:
        for (int by = 0; by < 2; by++) for (int bx = 0; bx < 2; bx++) {
            int xo = bx * 4, yo = by * 4, st = 0, sl = 0, v;
            if (at) for (int i = 0; i < 4; i++) st += dst[(xo + i) - stride];
            if (al) for (int i = 0; i < 4; i++) sl += dst[(yo + i) * stride - 1];
            if ((bx == 0 && by == 0) || (bx > 0 && by > 0)) {
                if (at && al) v = (st + sl + 4) >> 3;
                else if (al) v = (sl + 2) >> 2;
                else if (at) v = (st + 2) >> 2;
                else v = 128;
            } else if (bx > 0 && by == 0) {
                if (at) v = (st + 2) >> 2; else if (al) v = (sl + 2) >> 2; else v = 128;
            } else {
                if (al) v = (sl + 2) >> 2; else if (at) v = (st + 2) >> 2; else v = 128;
            }
            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(xo + x, yo + y) = (uint8_t)v;
        }
        break;
    case 1:
        for (int y = 0; y < 8; y++) { uint8_t v = dst[y * stride - 1]; for (int x = 0; x < 8; x++) P(x, y) = v; }
        break;
    case 2:
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) P(x, y) = dst[x - stride];
        break;
    default: {
        int H = 0, V = 0;
        for (int i = 0; i < 4; i++) {
            int a = dst[(4 + i) - stride], b = (2 - i) < 0 ? dst[-stride - 1] : dst[(2 - i) - stride];
            H += (i + 1) * (a - b);
            int c = dst[(4 + i) * stride - 1], e = (2 - i) < 0 ? dst[-stride - 1] : dst[(2 - i) * stride - 1];
            V += (i + 1) * (c - e);
        }
        int a = 16 * (dst[7 * stride - 1] + dst[7 - stride]);
        int b = (34 * H + 32) >> 6, c = (34 * V + 32) >> 6;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) P(x, y) = clip255((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
        break;
    }
    }
#undef P
}

// ---------------------------------------------------------------------------------------------
// transforms (8.5.12.2, 8.5.13.2). `c` holds scaled coefficients in raster order; the residual
// is added to dst with clipping.

void h264_idct4_add(uint8_t* dst, int stride, int32_t* c) {
    int32_t t[16];
    for (int i = 0; i < 4; i++) {
        int32_t d0 = c[i * 4], d1 = c[i * 4 + 1], d2 = c[i * 4 + 2], d3 = c[i * 4 + 3];
        int32_t e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        t[i * 4] = e0 + e3; t[i * 4 + 1] = e1 + e2; t[i * 4 + 2] = e1 - e2; t[i * 4 + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        int32_t f0 = t[j], f1 = t[4 + j], f2 = t[8 + j], f3 = t[12 + j];
        int32_t g0 = f0 + f2, g1 = f0 - f2, g2 = (f1 >> 1) - f3, g3 = f1 + (f3 >> 1);
        int32_t h[4] = { g0 + g3, g1 + g2, g1 - g2, g0 - g3 };
        for (int i = 0; i < 4; i++) {
            uint8_t* p = dst + i * stride + j;
            *p = clip255(*p + ((h[i] + 32) >> 6));
        }
    }
}

static void idct8_1d(const int32_t* in, int32_t* out, int is, int os) {
    int32_t d0 = in[0], d1 = in[is], d2 = in[2 * is], d3 = in[3 * is], d4 = in[4 * is], d5 = in[5 * is], d6 = in[6 * is], d7 = in[7 * is];
    int32_t e0 = d0 + d4, e1 = -d3 + d5 - d7 - (d7 >> 1), e2 = d0 - d4, e3 = d1 + d7 - d3 - (d3 >> 1);
    int32_t e4 = (d2 >> 1) - d6, e5 = -d1 + d7 + d5 + (d5 >> 1), e6 = d2 + (d6 >> 1), e7 = d3 + d5 + d1 + (d1 >> 1);
    int32_t f0 = e0 + e6, f1 = e1 + (e7 >> 2), f2 = e2 + e4, f3 = e3 + (e5 >> 2);
    int32_t f4 = e2 - e4, f5 = (e3 >> 2) - e5, f6 = e0 - e6, f7 = e7 - (e1 >> 2);
    out[0] = f0 + f7; out[os] = f2 + f5; out[2 * os] = f4 + f3; out[3 * os] = f6 + f1;
    out[4 * os] = f6 - f1; out[5 * os] = f4 - f3; out[6 * os] = f2 - f5; out[7 * os] = f0 - f7;
}

void h264_idct8_add(uint8_t* dst, int stride, int32_t* c) {
    int32_t t[64];
    for (int i = 0; i < 8; i++) idct8_1d(c + i * 8, t + i * 8, 1, 1);
    int32_t u[64];
    for (int j = 0; j < 8; j++) idct8_1d(t + j, u + j, 8, 8);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            uint8_t* p = dst + i * stride + j;
            *p = clip255(*p + ((u[i * 8 + j] + 32) >> 6));
        }
}

// DC only (all other coefficients zero): the residual is the same everywhere
void h264_dc_add(uint8_t* dst, int stride, int size, int32_t dc) {
    int r = (dc + 32) >> 6;
    if (!r) return;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) { uint8_t* p = dst + y * stride + x; *p = clip255(*p + r); }
}

// 4x4 Hadamard of the Intra16x16 luma DC levels (8.5.10): c raster 4x4 in, f out (not yet scaled)
void h264_luma_dc_transform(const int32_t* c, int32_t* f) {
    int32_t t[16];
    for (int i = 0; i < 4; i++) {
        const int32_t* r = c + i * 4;
        t[i * 4 + 0] = r[0] + r[1] + r[2] + r[3];
        t[i * 4 + 1] = r[0] + r[1] - r[2] - r[3];
        t[i * 4 + 2] = r[0] - r[1] - r[2] + r[3];
        t[i * 4 + 3] = r[0] - r[1] + r[2] - r[3];
    }
    for (int j = 0; j < 4; j++) {
        f[0 * 4 + j] = t[0 * 4 + j] + t[1 * 4 + j] + t[2 * 4 + j] + t[3 * 4 + j];
        f[1 * 4 + j] = t[0 * 4 + j] + t[1 * 4 + j] - t[2 * 4 + j] - t[3 * 4 + j];
        f[2 * 4 + j] = t[0 * 4 + j] - t[1 * 4 + j] - t[2 * 4 + j] + t[3 * 4 + j];
        f[3 * 4 + j] = t[0 * 4 + j] - t[1 * 4 + j] + t[2 * 4 + j] - t[3 * 4 + j];
    }
}

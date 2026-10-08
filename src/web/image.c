// Image decoders: inflate (zlib/gzip/raw), PNG, JPEG (baseline +
// progressive), GIF (first frame), BMP. Integer-only. Output: straight ARGB.
#include "image.h"
#include "wcommon.h"

// =====================================================================
// inflate (RFC 1951), canonical-Huffman decoding in the style of puff
// =====================================================================

struct inf {
    const uint8_t* in;
    int inlen, inpos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t* out;
    int outlen, outcap, outmax;
    int err;
};

struct huff { int16_t count[16]; int16_t symbol[320]; };

static int inf_bits(struct inf* s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int out_byte(struct inf* s, uint8_t b) {
    if (s->outlen >= s->outcap) {
        if (s->outcap >= s->outmax) { s->err = 2; return 0; }
        int nc = s->outcap ? s->outcap * 2 : 65536;
        if (nc > s->outmax) nc = s->outmax;
        uint8_t* n = (uint8_t*)w_realloc(s->out, nc);
        if (!n) { s->err = 3; return 0; }
        s->out = n;
        s->outcap = nc;
    }
    s->out[s->outlen++] = b;
    return 1;
}

static int huff_decode(struct inf* s, const struct huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 4;
    return -1;
}

static int huff_build(struct huff* h, const uint8_t* length, int n) {
    int16_t offs[16];
    for (int len = 0; len < 16; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (int16_t)sym;
    return left;
}

static const int16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int16_t LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const int16_t DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inf_codes(struct inf* s, const struct huff* lc, const struct huff* dc) {
    for (;;) {
        int sym = huff_decode(s, lc);
        if (sym < 0 || s->err) return -1;
        if (sym < 256) { if (!out_byte(s, (uint8_t)sym)) return -1; continue; }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -1;
        int len = LBASE[sym] + inf_bits(s, LEXT[sym]);
        int ds = huff_decode(s, dc);
        if (ds < 0 || ds >= 30 || s->err) return -1;
        int dist = DBASE[ds] + inf_bits(s, DEXT[ds]);
        if (s->err || dist > s->outlen) return -1;
        while (len--) if (!out_byte(s, s->out[s->outlen - dist])) return -1;
    }
}

static int inf_fixed(struct inf* s) {
    static struct huff lc, dc;
    static int ready;
    if (!ready) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dc, l, 30);
        ready = 1;
    }
    return inf_codes(s, &lc, &dc);
}

static int inf_dynamic(struct inf* s) {
    static const uint8_t ORD[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    struct huff lc, dc;
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    int idx;
    for (idx = 0; idx < ncode; idx++) lengths[ORD[idx]] = (uint8_t)inf_bits(s, 3);
    for (; idx < 19; idx++) lengths[ORD[idx]] = 0;
    if (s->err || huff_build(&lc, lengths, 19) != 0) return -1;
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = huff_decode(&*s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) { lengths[idx++] = (uint8_t)sym; continue; }
        int len = 0, rep;
        if (sym == 16) {
            if (idx == 0) return -1;
            len = lengths[idx - 1];
            rep = 3 + inf_bits(s, 2);
        } else if (sym == 17) rep = 3 + inf_bits(s, 3);
        else rep = 11 + inf_bits(s, 7);
        if (idx + rep > nlen + ndist) return -1;
        while (rep--) lengths[idx++] = (uint8_t)len;
    }
    if (lengths[256] == 0) return -1;
    int err = huff_build(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    err = huff_build(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -1;
    return inf_codes(s, &lc, &dc);
}

static int inf_stored(struct inf* s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    s->inpos += 4;
    if (s->inpos + (int)len > s->inlen) return -1;
    while (len--) if (!out_byte(s, s->in[s->inpos++])) return -1;
    return 0;
}

uint8_t* winflate_raw(const uint8_t* in, int len, int* out_len, int max_out) {
    struct inf s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.inlen = len;
    s.outmax = max_out > 0 ? max_out : 64 * 1024 * 1024;
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.err) break;
        int r = type == 0 ? inf_stored(&s) : type == 1 ? inf_fixed(&s) : type == 2 ? inf_dynamic(&s) : -1;
        if (r < 0) {
            // keep what decoded so far (truncated streams still render)
            if (!s.outlen) { w_free(s.out); return 0; }
            break;
        }
    } while (!last);
    *out_len = s.outlen;
    if (!s.out) s.out = (uint8_t*)w_malloc(1);
    return s.out;
}

uint8_t* winflate_zlib(const uint8_t* in, int len, int* out_len, int max_out) {
    if (len < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31) return 0;
    return winflate_raw(in + 2, len - 2, out_len, max_out);
}

uint8_t* winflate_gzip(const uint8_t* in, int len, int* out_len, int max_out) {
    if (len < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return 0;
    int flg = in[3], p = 10;
    if (flg & 4) { if (p + 2 > len) return 0; p += 2 + (in[p] | (in[p + 1] << 8)); }
    if (flg & 8) { while (p < len && in[p]) p++; p++; }
    if (flg & 16) { while (p < len && in[p]) p++; p++; }
    if (flg & 2) p += 2;
    if (p >= len) return 0;
    return winflate_raw(in + p, len - p, out_len, max_out);
}

// =====================================================================
// PNG
// =====================================================================

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static int png_unfilter(uint8_t* data, int w, int h, int bpp_bits, int stride_in, uint8_t* out) {
    int stride = (w * bpp_bits + 7) / 8;
    int bpp = (bpp_bits + 7) / 8;
    (void)stride_in;
    uint8_t* prev = 0;
    for (int y = 0; y < h; y++) {
        uint8_t* src = data + y * (stride + 1);
        int ft = src[0];
        src++;
        uint8_t* dst = out + y * stride;
        for (int x = 0; x < stride; x++) {
            int a = x >= bpp ? dst[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = src[x];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += paeth(a, b, c); break;
            case 0: break;
            default: return 0;
            }
            dst[x] = (uint8_t)v;
        }
        prev = dst;
    }
    return 1;
}

static int png_decode(const uint8_t* d, int len, int* W, int* H, uint32_t** out) {
    if (len < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return 0;
    int p = 8;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t pal[256][4];
    int npal = 0;
    int trns_gray = -1, trns_r = -1, trns_g = -1, trns_b = -1;
    for (int i = 0; i < 256; i++) { pal[i][0] = pal[i][1] = pal[i][2] = 0; pal[i][3] = 255; }
    uint8_t* idat = 0;
    int idat_len = 0, idat_cap = 0;
    while (p + 8 <= len) {
        uint32_t cl = be32(d + p);
        const uint8_t* ty = d + p + 4;
        const uint8_t* cd = d + p + 8;
        if (cl > (uint32_t)(len - p - 12)) cl = (uint32_t)(len - p - 12 > 0 ? len - p - 12 : 0);
        if (!memcmp(ty, "IHDR", 4) && cl >= 13) {
            w = (int)be32(cd); h = (int)be32(cd + 4); depth = cd[8]; ctype = cd[9]; interlace = cd[12];
        } else if (!memcmp(ty, "PLTE", 4)) {
            npal = (int)cl / 3;
            if (npal > 256) npal = 256;
            for (int i = 0; i < npal; i++) { pal[i][0] = cd[i * 3]; pal[i][1] = cd[i * 3 + 1]; pal[i][2] = cd[i * 3 + 2]; }
        } else if (!memcmp(ty, "tRNS", 4)) {
            if (ctype == 3) for (uint32_t i = 0; i < cl && i < 256; i++) pal[i][3] = cd[i];
            else if (ctype == 0 && cl >= 2) trns_gray = (cd[0] << 8) | cd[1];
            else if (ctype == 2 && cl >= 6) { trns_r = (cd[0] << 8) | cd[1]; trns_g = (cd[2] << 8) | cd[3]; trns_b = (cd[4] << 8) | cd[5]; }
        } else if (!memcmp(ty, "IDAT", 4)) {
            if (idat_len + (int)cl > idat_cap) {
                int nc = idat_cap ? idat_cap * 2 : 65536;
                while (nc < idat_len + (int)cl) nc *= 2;
                uint8_t* n = (uint8_t*)w_realloc(idat, nc);
                if (!n) { w_free(idat); return 0; }
                idat = n;
                idat_cap = nc;
            }
            memcpy(idat + idat_len, cd, cl);
            idat_len += (int)cl;
        } else if (!memcmp(ty, "IEND", 4)) break;
        p += 12 + (int)cl;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !idat || (int64_t)w * h > 40000000) { w_free(idat); return 0; }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!ch) { w_free(idat); return 0; }
    int bpp_bits = ch * depth;
    int rawlen;
    uint8_t* raw = winflate_zlib(idat, idat_len, &rawlen, (w * bpp_bits / 8 + 2) * h * 2 + 1024);
    w_free(idat);
    if (!raw) return 0;
    uint32_t* px = (uint32_t*)w_malloc((size_t)w * h * 4);
    if (!px) { w_free(raw); return 0; }
    memset(px, 0, (size_t)w * h * 4);
    static const int AX[7] = { 0, 4, 0, 2, 0, 1, 0 }, AY[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int DX[7] = { 8, 8, 4, 4, 2, 2, 1 }, DY[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;
    int off = 0;
    for (int pass = 0; pass < passes; pass++) {
        int ax = interlace ? AX[pass] : 0, ay = interlace ? AY[pass] : 0;
        int dx = interlace ? DX[pass] : 1, dy = interlace ? DY[pass] : 1;
        int pw = (w - ax + dx - 1) / dx, ph = (h - ay + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        int stride = (pw * bpp_bits + 7) / 8;
        int need = (stride + 1) * ph;
        if (off + need > rawlen) {
            // truncated: decode the complete rows only
            ph = (rawlen - off) / (stride + 1);
            if (ph <= 0) break;
            need = (stride + 1) * ph;
        }
        uint8_t* un = (uint8_t*)w_malloc((size_t)stride * ph + 1);
        if (!un) break;
        if (!png_unfilter(raw + off, pw, ph, bpp_bits, stride, un)) { w_free(un); break; }
        off += need;
        for (int y = 0; y < ph; y++) {
            const uint8_t* row = un + y * stride;
            for (int x = 0; x < pw; x++) {
                int r, g, b, a = 255;
                if (depth == 16) {
                    const uint8_t* q = row + x * ch * 2;
                    int v0 = (q[0] << 8) | q[1];
                    if (ctype == 0) { r = g = b = q[0]; if (v0 == trns_gray) a = 0; }
                    else if (ctype == 2) {
                        r = q[0]; g = q[2]; b = q[4];
                        if (v0 == trns_r && ((q[2] << 8) | q[3]) == trns_g && ((q[4] << 8) | q[5]) == trns_b) a = 0;
                    } else if (ctype == 4) { r = g = b = q[0]; a = q[2]; }
                    else { r = q[0]; g = q[2]; b = q[4]; a = q[6]; }
                } else if (depth == 8) {
                    const uint8_t* q = row + x * ch;
                    if (ctype == 0) { r = g = b = q[0]; if (q[0] == trns_gray) a = 0; }
                    else if (ctype == 2) { r = q[0]; g = q[1]; b = q[2]; if (r == trns_r && g == trns_g && b == trns_b) a = 0; }
                    else if (ctype == 3) { r = pal[q[0]][0]; g = pal[q[0]][1]; b = pal[q[0]][2]; a = pal[q[0]][3]; }
                    else if (ctype == 4) { r = g = b = q[0]; a = q[1]; }
                    else { r = q[0]; g = q[1]; b = q[2]; a = q[3]; }
                } else {
                    // 1/2/4-bit gray or palette
                    int bit = x * depth;
                    int v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
                    if (ctype == 3) { r = pal[v][0]; g = pal[v][1]; b = pal[v][2]; a = pal[v][3]; }
                    else {
                        int s = v * 255 / ((1 << depth) - 1);
                        r = g = b = s;
                        if (v == trns_gray) a = 0;
                    }
                }
                int X = ax + x * dx, Y = ay + y * dy;
                if (X < w && Y < h) px[Y * w + X] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
        w_free(un);
    }
    w_free(raw);
    *W = w;
    *H = h;
    *out = px;
    return 1;
}

// =====================================================================
// JPEG
// =====================================================================

struct jhuff {
    uint8_t bits[17];
    uint8_t vals[256];
    int32_t maxcode[18];
    int32_t valptr[17];
    int32_t mincode[17];
    uint8_t look_len[512];   // 9-bit fast lookup: code length (0 = slow path)
    uint8_t look_val[512];
};

struct jcomp {
    int id, h, v, tq, td, ta;
    int bw, bh;              // blocks wide/high (padded to MCU)
    int16_t* coef;           // bw*bh*64 coefficients (natural order)
    int dcpred;
    uint8_t* plane;          // decoded samples (bw*8 x bh*8)
};

struct jdec {
    const uint8_t* d;
    int len, pos;
    uint32_t bitbuf;
    int bitcnt;
    int marker_hit;
    uint16_t qt[4][64];
    struct jhuff hd[4], ha[4];
    struct jcomp c[4];
    int nc;
    int w, h;
    int hmax, vmax;
    int mcux, mcuy;
    int progressive;
    int restart;
    int eobrun;
    int adobe_transform;
};

static const uint8_t ZZ[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63
};

static void jh_build(struct jhuff* h) {
    int code = 0, k = 0;
    memset(h->look_len, 0, sizeof h->look_len);
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    // fast table
    k = 0;
    code = 0;
    for (int l = 1; l <= 9; l++) {
        for (int i = 0; i < h->bits[l]; i++, k++, code++) {
            int shift = 9 - l;
            for (int j = 0; j < (1 << shift); j++) {
                h->look_len[(code << shift) | j] = (uint8_t)l;
                h->look_val[(code << shift) | j] = h->vals[k];
            }
        }
        code <<= 1;
    }
}

static void jfill(struct jdec* j) {
    while (j->bitcnt <= 24) {
        int b = 0;
        if (!j->marker_hit && j->pos < j->len) {
            b = j->d[j->pos];
            if (b == 0xFF) {
                int n = j->pos + 1 < j->len ? j->d[j->pos + 1] : 0;
                if (n == 0) j->pos += 2;
                else { j->marker_hit = 1; b = 0; }
            } else j->pos++;
        }
        j->bitbuf |= (uint32_t)b << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}

static int jbits(struct jdec* j, int n) {
    if (!n) return 0;
    if (j->bitcnt < n) jfill(j);
    int v = (int)(j->bitbuf >> (32 - n));
    j->bitbuf <<= n;
    j->bitcnt -= n;
    return v;
}

static int jbit(struct jdec* j) { return jbits(j, 1); }

static int jdecode(struct jdec* j, const struct jhuff* h) {
    if (j->bitcnt < 16) jfill(j);
    int peek = (int)(j->bitbuf >> 23);
    int l = h->look_len[peek];
    if (l) {
        j->bitbuf <<= l;
        j->bitcnt -= l;
        return h->look_val[peek];
    }
    int code = 0;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | jbit(j);
        if (code <= h->maxcode[l] && h->maxcode[l] >= 0) {
            int idx = h->valptr[l] + code - h->mincode[l];
            return idx < 256 ? h->vals[idx] : 0;
        }
    }
    return 0;
}

static int jextend(int v, int t) { return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

static void jreset(struct jdec* j) {
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->marker_hit = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dcpred = 0;
    j->eobrun = 0;
    // skip the RSTn marker
    while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) {
        if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && j->d[j->pos + 1] != 0xFF) return;
        j->pos++;
    }
    if (j->pos + 1 < j->len) j->pos += 2;
}

// Inverse DCT, straight from the T.81 definition (A.3.3):
//   s(y,x) = 1/4 sum_v sum_u C(u) C(v) S(v,u) cos((2x+1)u pi/16) cos((2y+1)v pi/16)
// with C(0) = 1/sqrt(2), C(k>0) = 1. It is separable: an 8-point 1-D
// transform s(x) = sum_u K(x,u) S(u), K(x,u) = C(u)/2 cos((2x+1)u pi/16),
// over the columns, then over the rows. cos((2(7-x)+1)u pi/16) =
// (-1)^u cos((2x+1)u pi/16), so for x < 4 the even-u terms (e) give both
// s(x) and s(7-x) unchanged and the odd-u terms (o) with a sign flip:
//   s(x) = e + o,  s(7-x) = e - o.
// IDCT_K[x][u] = round(K(x,u) * 2^14), x = 0..3 (computed offline; the web
// engine is integer-only).
#define IDCT_KB 14
static const int16_t IDCT_K[4][8] = {
    {5793, 8035, 7568, 6811, 5793, 4551, 3135, 1598},
    {5793, 6811, 3135, -1598, -5793, -8035, -7568, -4551},
    {5793, 4551, -3135, -8035, -5793, 1598, 7568, 6811},
    {5793, 1598, -7568, -4551, 5793, 6811, -3135, -8035},
};
#define IDCT_FB 8   // fraction bits kept between the column and row passes

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

// d: dequantized coefficients, natural (row-major) order. Writes the 8x8
// samples with the +128 level shift.
static void idct_block(uint8_t* out, int stride, const int16_t* d) {
    int32_t mid[64];
    // columns: |S| <= 32768, |K| <= 8035 -> |sum of 8| < 2^31
    for (int col = 0; col < 8; col++) {
        const int16_t* in = d + col;
        int ac = 0;
        for (int v = 1; v < 8; v++) ac |= in[v * 8];
        if (!ac) {   // DC only: flat column, K(x,0) is the same for every x
            int32_t f = (int32_t)in[0] * IDCT_K[0][0];
            f = (f + (1 << (IDCT_KB - IDCT_FB - 1))) >> (IDCT_KB - IDCT_FB);
            for (int y = 0; y < 8; y++) mid[y * 8 + col] = f;
            continue;
        }
        for (int y = 0; y < 4; y++) {
            const int16_t* k = IDCT_K[y];
            int32_t e = in[0] * k[0] + in[16] * k[2] + in[32] * k[4] + in[48] * k[6];
            int32_t o = in[8] * k[1] + in[24] * k[3] + in[40] * k[5] + in[56] * k[7];
            int32_t r = 1 << (IDCT_KB - IDCT_FB - 1);
            mid[y * 8 + col] = (e + o + r) >> (IDCT_KB - IDCT_FB);
            mid[(7 - y) * 8 + col] = (e - o + r) >> (IDCT_KB - IDCT_FB);
        }
    }
    // rows: 64-bit sums (a corrupt stream can push the columns far out of
    // the range a real image produces)
    const int sh = IDCT_KB + IDCT_FB;
    const int64_t rnd = ((int64_t)1 << (sh - 1)) + ((int64_t)128 << sh);
    for (int y = 0; y < 8; y++, out += stride) {
        const int32_t* in = mid + y * 8;
        if (!(in[1] | in[2] | in[3] | in[4] | in[5] | in[6] | in[7])) {   // flat row
            int64_t a = ((int64_t)in[0] * IDCT_K[0][0] + rnd) >> sh;
            uint8_t v = clamp8(a < -1 ? -1 : a > 256 ? 256 : (int)a);
            for (int x = 0; x < 8; x++) out[x] = v;
            continue;
        }
        for (int x = 0; x < 4; x++) {
            const int16_t* k = IDCT_K[x];
            int64_t e = (int64_t)in[0] * k[0] + (int64_t)in[2] * k[2] +
                        (int64_t)in[4] * k[4] + (int64_t)in[6] * k[6];
            int64_t o = (int64_t)in[1] * k[1] + (int64_t)in[3] * k[3] +
                        (int64_t)in[5] * k[5] + (int64_t)in[7] * k[7];
            int64_t a = (e + o + rnd) >> sh, b = (e - o + rnd) >> sh;
            out[x] = clamp8(a < -1 ? -1 : a > 256 ? 256 : (int)a);
            out[7 - x] = clamp8(b < -1 ? -1 : b > 256 ? 256 : (int)b);
        }
    }
}

// decode one block (baseline): coefficients into coef (natural order, not dequantized)
static void j_block_baseline(struct jdec* j, struct jcomp* c, int16_t* coef) {
    int t = jdecode(j, &j->hd[c->td]);
    int diff = t ? jextend(jbits(j, t), t) : 0;
    c->dcpred += diff;
    coef[0] = (int16_t)c->dcpred;
    for (int k = 1; k < 64;) {
        int rs = jdecode(j, &j->ha[c->ta]);
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15) break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) break;
        coef[ZZ[k]] = (int16_t)jextend(jbits(j, s), s);
        k++;
    }
}

// progressive scans
static void j_dc_first(struct jdec* j, struct jcomp* c, int16_t* coef, int al) {
    int t = jdecode(j, &j->hd[c->td]);
    int diff = t ? jextend(jbits(j, t), t) : 0;
    c->dcpred += diff;
    coef[0] = (int16_t)(c->dcpred * (1 << al));
}

static void j_dc_refine(struct jdec* j, int16_t* coef, int al) {
    if (jbit(j)) coef[0] |= (int16_t)(1 << al);
}

static void j_ac_first(struct jdec* j, struct jcomp* c, int16_t* coef, int ss, int se, int al) {
    if (j->eobrun > 0) { j->eobrun--; return; }
    for (int k = ss; k <= se;) {
        int rs = jdecode(j, &j->ha[c->ta]);
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) j->eobrun += jbits(j, r);
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) break;
        coef[ZZ[k]] = (int16_t)(jextend(jbits(j, s), s) * (1 << al));
        k++;
    }
}

static void j_ac_refine(struct jdec* j, struct jcomp* c, int16_t* coef, int ss, int se, int al) {
    int p1 = 1 << al, m1 = -1 * (1 << al);
    int k = ss;
    if (j->eobrun <= 0) {
        for (; k <= se;) {
            int rs = jdecode(j, &j->ha[c->ta]);
            int r = rs >> 4, s = rs & 15;
            int val = 0;
            if (!s) {
                if (r < 15) {
                    j->eobrun = (1 << r);
                    if (r) j->eobrun += jbits(j, r);
                    break;
                }
            } else {
                val = jbit(j) ? p1 : m1;
            }
            while (k <= se) {
                int16_t* z = &coef[ZZ[k]];
                if (*z) {
                    if (jbit(j) && !(*z & p1)) *z = (int16_t)(*z >= 0 ? *z + p1 : *z + m1);
                } else {
                    if (r == 0) {
                        if (val) *z = (int16_t)val;
                        k++;
                        break;
                    }
                    r--;
                }
                k++;
            }
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            int16_t* z = &coef[ZZ[k]];
            if (*z && jbit(j) && !(*z & p1)) *z = (int16_t)(*z >= 0 ? *z + p1 : *z + m1);
        }
        j->eobrun--;
    }
}

static int j_scan(struct jdec* j, const uint8_t* sos, int sl) {
    int ns = sos[0];
    if (ns < 1 || ns > 4 || sl < 1 + ns * 2 + 3) return 0;
    struct jcomp* comps[4];
    for (int i = 0; i < ns; i++) {
        int id = sos[1 + i * 2], tables = sos[2 + i * 2];
        comps[i] = 0;
        for (int k = 0; k < j->nc; k++) if (j->c[k].id == id) comps[i] = &j->c[k];
        if (!comps[i]) return 0;
        comps[i]->td = tables >> 4;
        comps[i]->ta = tables & 15;
        if (comps[i]->td > 3 || comps[i]->ta > 3) return 0;
    }
    int ss = sos[1 + ns * 2], se = sos[2 + ns * 2], ah = sos[3 + ns * 2] >> 4, al = sos[3 + ns * 2] & 15;
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->marker_hit = 0;
    j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dcpred = 0;
    int mcus = 0;
    if (ns == 1) {
        // non-interleaved: iterate the component's own blocks
        struct jcomp* c = comps[0];
        int bw = (j->w * c->h / j->hmax + 7) / 8, bh = (j->h * c->v / j->vmax + 7) / 8;
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                int16_t* coef = c->coef + (by * c->bw + bx) * 64;
                if (!j->progressive) j_block_baseline(j, c, coef);
                else if (ss == 0) { if (!ah) j_dc_first(j, c, coef, al); else j_dc_refine(j, coef, al); }
                else if (!ah) j_ac_first(j, c, coef, ss, se, al);
                else j_ac_refine(j, c, coef, ss, se, al);
                if (j->restart && ++mcus % j->restart == 0) jreset(j);
            }
    } else {
        for (int my = 0; my < j->mcuy; my++)
            for (int mx = 0; mx < j->mcux; mx++) {
                for (int i = 0; i < ns; i++) {
                    struct jcomp* c = comps[i];
                    for (int yy = 0; yy < c->v; yy++)
                        for (int xx = 0; xx < c->h; xx++) {
                            int bx = mx * c->h + xx, by = my * c->v + yy;
                            int16_t* coef = c->coef + (by * c->bw + bx) * 64;
                            if (!j->progressive) j_block_baseline(j, c, coef);
                            else if (!ah) j_dc_first(j, c, coef, al);
                            else j_dc_refine(j, coef, al);
                        }
                }
                if (j->restart && ++mcus % j->restart == 0) jreset(j);
            }
    }
    // advance to the next marker
    while (j->pos + 1 < j->len) {
        if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && !(j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) break;
        j->pos++;
    }
    return 1;
}

static int jpeg_decode(const uint8_t* d, int len, int* W, int* H, uint32_t** out) {
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    struct jdec* j = (struct jdec*)w_calloc(1, sizeof(struct jdec));
    if (!j) return 0;
    j->d = d;
    j->len = len;
    j->pos = 2;
    j->adobe_transform = -1;
    int ok = 0, have_frame = 0;
    while (j->pos + 4 <= len) {
        if (d[j->pos] != 0xFF) { j->pos++; continue; }
        int m = d[j->pos + 1];
        j->pos += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) { if (m == 0xFF) j->pos--; continue; }
        if (m == 0xD9) break;
        if (j->pos + 2 > len) break;
        int sl = (d[j->pos] << 8) | d[j->pos + 1];
        if (sl < 2 || j->pos + sl > len) break;
        const uint8_t* s = d + j->pos + 2;
        int n = sl - 2;
        j->pos += sl;
        switch (m) {
        case 0xDB: { // DQT
            int p = 0;
            while (p < n) {
                int pq = s[p] >> 4, tq = s[p] & 3;
                p++;
                for (int i = 0; i < 64 && p < n; i++) {
                    j->qt[tq][ZZ[i]] = pq ? (uint16_t)((s[p] << 8) | s[p + 1]) : s[p];
                    p += pq ? 2 : 1;
                }
            }
            break;
        }
        case 0xC4: { // DHT
            int p = 0;
            while (p + 17 <= n) {
                int tc = s[p] >> 4, th = s[p] & 3;
                struct jhuff* h = tc ? &j->ha[th] : &j->hd[th];
                int tot = 0;
                h->bits[0] = 0;
                for (int i = 1; i <= 16; i++) { h->bits[i] = s[p + i]; tot += s[p + i]; }
                p += 17;
                if (tot > 256 || p + tot > n) break;
                memcpy(h->vals, s + p, tot);
                p += tot;
                jh_build(h);
            }
            break;
        }
        case 0xC0: case 0xC1: case 0xC2: { // SOF
            if (n < 6) break;
            j->progressive = m == 0xC2;
            j->h = (s[1] << 8) | s[2];
            j->w = (s[3] << 8) | s[4];
            j->nc = s[5];
            if (j->nc < 1 || j->nc > 4 || n < 6 + j->nc * 3 || j->w <= 0 || j->h <= 0 ||
                (int64_t)j->w * j->h > 40000000) goto done;
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->nc; i++) {
                j->c[i].id = s[6 + i * 3];
                j->c[i].h = s[7 + i * 3] >> 4;
                j->c[i].v = s[7 + i * 3] & 15;
                j->c[i].tq = s[8 + i * 3] & 3;
                if (j->c[i].h < 1 || j->c[i].h > 4 || j->c[i].v < 1 || j->c[i].v > 4) goto done;
                if (j->c[i].h > j->hmax) j->hmax = j->c[i].h;
                if (j->c[i].v > j->vmax) j->vmax = j->c[i].v;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->nc; i++) {
                j->c[i].bw = j->mcux * j->c[i].h;
                j->c[i].bh = j->mcuy * j->c[i].v;
                j->c[i].coef = (int16_t*)w_calloc((size_t)j->c[i].bw * j->c[i].bh * 64, sizeof(int16_t));
                if (!j->c[i].coef) goto done;
            }
            have_frame = 1;
            break;
        }
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD:
        case 0xCE: case 0xCF:
            goto done; // lossless / arithmetic: unsupported
        case 0xDD:
            if (n >= 2) j->restart = (s[0] << 8) | s[1];
            break;
        case 0xEE:
            if (n >= 12 && !memcmp(s, "Adobe", 5)) j->adobe_transform = s[11];
            break;
        case 0xDA:
            if (!have_frame) goto done;
            j_scan(j, s, n);
            break;
        default:
            break;
        }
    }
    if (!have_frame) goto done;
    // dequantize + IDCT into planes
    for (int i = 0; i < j->nc; i++) {
        struct jcomp* c = &j->c[i];
        int pw = c->bw * 8, ph = c->bh * 8;
        c->plane = (uint8_t*)w_malloc((size_t)pw * ph);
        if (!c->plane) goto done;
        int16_t blk[64];
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++) {
                const int16_t* co = c->coef + (by * c->bw + bx) * 64;
                for (int k = 0; k < 64; k++) blk[k] = (int16_t)(co[k] * j->qt[c->tq][k]);
                idct_block(c->plane + by * 8 * pw + bx * 8, pw, blk);
            }
    }
    {
        uint32_t* px = (uint32_t*)w_malloc((size_t)j->w * j->h * 4);
        if (!px) goto done;
        for (int y = 0; y < j->h; y++) {
            for (int x = 0; x < j->w; x++) {
                int v[4];
                for (int i = 0; i < j->nc; i++) {
                    struct jcomp* c = &j->c[i];
                    int sx = x * c->h / j->hmax, sy = y * c->v / j->vmax;
                    v[i] = c->plane[sy * c->bw * 8 + sx];
                }
                int r, g, b;
                if (j->nc == 1) r = g = b = v[0];
                else if (j->nc == 3 && j->adobe_transform != 0) {
                    int Y = v[0] << 16, cb = v[1] - 128, cr = v[2] - 128;
                    r = (Y + 91881 * cr + 32768) >> 16;
                    g = (Y - 22554 * cb - 46802 * cr + 32768) >> 16;
                    b = (Y + 116130 * cb + 32768) >> 16;
                } else if (j->nc == 4) {
                    int cc = v[0], mm = v[1], yy = v[2], kk = v[3];
                    if (j->adobe_transform == 2) {
                        int Y = cc << 16, cb = mm - 128, cr = yy - 128;
                        cc = 255 - ((Y + 91881 * cr + 32768) >> 16);
                        mm = 255 - ((Y - 22554 * cb - 46802 * cr + 32768) >> 16);
                        yy = 255 - ((Y + 116130 * cb + 32768) >> 16);
                    }
                    // Adobe CMYK is stored inverted
                    r = cc * kk / 255; g = mm * kk / 255; b = yy * kk / 255;
                } else { r = v[0]; g = v[1]; b = v[2]; }
                px[y * j->w + x] = 0xFF000000u | ((uint32_t)clamp8(r) << 16) | ((uint32_t)clamp8(g) << 8) | clamp8(b);
            }
        }
        *W = j->w;
        *H = j->h;
        *out = px;
        ok = 1;
    }
done:
    for (int i = 0; i < 4; i++) { w_free(j->c[i].coef); w_free(j->c[i].plane); }
    w_free(j);
    return ok;
}

// =====================================================================
// GIF (first frame)
// =====================================================================

static int gif_decode(const uint8_t* d, int len, int* W, int* H, uint32_t** out) {
    if (len < 13 || memcmp(d, "GIF8", 4)) return 0;
    int w = d[6] | (d[7] << 8), h = d[8] | (d[9] << 8);
    int flags = d[10];
    int p = 13;
    uint8_t gct[256][3];
    int gct_n = 0;
    if (flags & 0x80) {
        gct_n = 2 << (flags & 7);
        if (p + gct_n * 3 > len) return 0;
        memcpy(gct, d + p, gct_n * 3);
        p += gct_n * 3;
    }
    if (w <= 0 || h <= 0 || (int64_t)w * h > 40000000) return 0;
    int transparent = -1;
    while (p < len) {
        int b = d[p++];
        if (b == 0x3B) break;
        if (b == 0x21) { // extension
            if (p >= len) break;
            int label = d[p++];
            if (label == 0xF9 && p + 5 <= len && d[p] == 4) {
                if (d[p + 1] & 1) transparent = d[p + 4];
            }
            while (p < len && d[p]) p += d[p] + 1;
            p++;
            continue;
        }
        if (b != 0x2C || p + 9 > len) break;
        int fx = d[p] | (d[p + 1] << 8), fy = d[p + 2] | (d[p + 3] << 8);
        int fw = d[p + 4] | (d[p + 5] << 8), fh = d[p + 6] | (d[p + 7] << 8);
        int lf = d[p + 8];
        p += 9;
        uint8_t (*ct)[3] = gct;
        uint8_t lct[256][3];
        int ct_n = gct_n;
        if (lf & 0x80) {
            ct_n = 2 << (lf & 7);
            if (p + ct_n * 3 > len) return 0;
            memcpy(lct, d + p, ct_n * 3);
            p += ct_n * 3;
            ct = lct;
        }
        int interlaced = (lf & 0x40) != 0;
        if (p >= len) return 0;
        int minsize = d[p++];
        if (minsize < 1 || minsize > 11) return 0;
        // gather data sub-blocks
        int dl = 0;
        for (int q = p; q < len && d[q]; q += d[q] + 1) dl += d[q];
        uint8_t* data = (uint8_t*)w_malloc(dl + 1);
        if (!data) return 0;
        int o = 0;
        while (p < len && d[p]) {
            int n = d[p];
            if (p + 1 + n > len) n = len - p - 1;
            memcpy(data + o, d + p + 1, n);
            o += n;
            p += d[p] + 1;
        }
        p++;
        uint32_t* px = (uint32_t*)w_calloc((size_t)w * h, 4);
        uint8_t* idx = (uint8_t*)w_malloc((size_t)fw * fh + 1);
        uint16_t* prefix = (uint16_t*)w_malloc(4096 * sizeof(uint16_t));
        uint8_t* suffix = (uint8_t*)w_malloc(4096);
        uint8_t* stack = (uint8_t*)w_malloc(4097);
        if (!px || !idx || !prefix || !suffix || !stack) {
            w_free(px); w_free(idx); w_free(prefix); w_free(suffix); w_free(stack); w_free(data);
            return 0;
        }
        int clear = 1 << minsize, eoi = clear + 1;
        int codesize = minsize + 1, next = clear + 2, old = -1;
        uint32_t bits = 0;
        int nbits = 0, bp = 0, outn = 0, total = fw * fh;
        uint8_t first = 0;
        for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
        while (outn < total) {
            while (nbits < codesize && bp < o) { bits |= (uint32_t)data[bp++] << nbits; nbits += 8; }
            if (nbits < codesize) break;
            int code = (int)(bits & ((1u << codesize) - 1));
            bits >>= codesize;
            nbits -= codesize;
            if (code == clear) { codesize = minsize + 1; next = clear + 2; old = -1; continue; }
            if (code == eoi) break;
            int sp = 0, c = code;
            if (old < 0) {
                if (code >= clear) break;
                idx[outn++] = (uint8_t)code;
                first = (uint8_t)code;
                old = code;
                continue;
            }
            if (code >= next) { stack[sp++] = first; c = old; }
            while (c >= clear && sp < 4096) { stack[sp++] = suffix[c]; c = prefix[c]; }
            if (c >= clear) break;
            stack[sp++] = (uint8_t)c;
            first = (uint8_t)c;
            while (sp && outn < total) idx[outn++] = stack[--sp];
            if (next < 4096) { prefix[next] = (uint16_t)old; suffix[next] = first; next++; }
            if (next == (1 << codesize) && codesize < 12) codesize++;
            old = code;
        }
        // compose onto the canvas
        for (int i = 0; i < outn; i++) {
            int row = i / fw, col = i % fw;
            if (interlaced) {
                int r = row;
                int p1 = (fh + 7) / 8, p2 = (fh + 3) / 8, p3 = (fh + 1) / 4;
                if (r < p1) row = r * 8;
                else if (r < p1 + p2) row = (r - p1) * 8 + 4;
                else if (r < p1 + p2 + p3) row = (r - p1 - p2) * 4 + 2;
                else row = (r - p1 - p2 - p3) * 2 + 1;
            }
            int X = fx + col, Y = fy + row;
            if (X >= w || Y >= h) continue;
            int v = idx[i];
            if (v == transparent || v >= ct_n) continue;
            px[Y * w + X] = 0xFF000000u | ((uint32_t)ct[v][0] << 16) | ((uint32_t)ct[v][1] << 8) | ct[v][2];
        }
        w_free(idx); w_free(prefix); w_free(suffix); w_free(stack); w_free(data);
        *W = w;
        *H = h;
        *out = px;
        return 1;
    }
    return 0;
}

// =====================================================================
// BMP
// =====================================================================

static int bmp_decode(const uint8_t* d, int len, int* W, int* H, uint32_t** out) {
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return 0;
    uint32_t off = d[10] | (d[11] << 8) | (d[12] << 16) | ((uint32_t)d[13] << 24);
    int32_t w = (int32_t)(d[18] | (d[19] << 8) | (d[20] << 16) | ((uint32_t)d[21] << 24));
    int32_t h = (int32_t)(d[22] | (d[23] << 8) | (d[24] << 16) | ((uint32_t)d[25] << 24));
    int bpp = d[28] | (d[29] << 8);
    int comp = d[30];
    int top_down = h < 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || (int64_t)w * h > 40000000 || (comp != 0 && comp != 3)) return 0;
    if (bpp != 24 && bpp != 32 && bpp != 8) return 0;
    int stride = ((w * bpp + 31) / 32) * 4;
    if (off + (uint32_t)stride * h > (uint32_t)len) return 0;
    uint32_t* px = (uint32_t*)w_malloc((size_t)w * h * 4);
    if (!px) return 0;
    const uint8_t* pal = d + 14 + (d[14] | (d[15] << 8));
    for (int y = 0; y < h; y++) {
        const uint8_t* row = d + off + (top_down ? y : h - 1 - y) * stride;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 24) c = 0xFF000000u | ((uint32_t)row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
            else if (bpp == 32) c = ((uint32_t)(comp == 3 ? 255 : row[x * 4 + 3] ? row[x * 4 + 3] : 255) << 24) |
                                   ((uint32_t)row[x * 4 + 2] << 16) | (row[x * 4 + 1] << 8) | row[x * 4];
            else { const uint8_t* q = pal + row[x] * 4; c = 0xFF000000u | ((uint32_t)q[2] << 16) | (q[1] << 8) | q[0]; }
            px[y * w + x] = c;
        }
    }
    *W = w;
    *H = h;
    *out = px;
    return 1;
}

// =====================================================================
// front end
// =====================================================================

static uint32_t* downscale(uint32_t* src, int w, int h, int f, int* ow, int* oh) {
    int nw = (w + f - 1) / f, nh = (h + f - 1) / f;
    uint32_t* dst = (uint32_t*)w_malloc((size_t)nw * nh * 4);
    if (!dst) return 0;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int yy = y * f; yy < y * f + f && yy < h; yy++)
                for (int xx = x * f; xx < x * f + f && xx < w; xx++) {
                    uint32_t p = src[yy * w + xx], pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 255) * pa;
                    g += ((p >> 8) & 255) * pa;
                    b += (p & 255) * pa;
                    n++;
                }
            dst[y * nw + x] = a ? ((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a) : 0;
        }
    *ow = nw;
    *oh = nh;
    return dst;
}

int wimage_decode(const uint8_t* data, int len, int max_dim, int* w, int* h, uint32_t** px) {
    int ok = 0;
    *px = 0;
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P') ok = png_decode(data, len, w, h, px);
    else if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8) ok = jpeg_decode(data, len, w, h, px);
    else if (len >= 6 && !memcmp(data, "GIF8", 4)) ok = gif_decode(data, len, w, h, px);
    else if (len >= 2 && data[0] == 'B' && data[1] == 'M') ok = bmp_decode(data, len, w, h, px);
    if (!ok) return 0;
    if (max_dim > 0 && (*w > max_dim || *h > max_dim)) {
        int big = *w > *h ? *w : *h;
        int f = (big + max_dim - 1) / max_dim;
        int nw, nh;
        uint32_t* s = downscale(*px, *w, *h, f, &nw, &nh);
        if (s) { w_free(*px); *px = s; *w = nw; *h = nh; }
    }
    return 1;
}

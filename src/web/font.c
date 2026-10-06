#include "font.h"
#include "raster.h"
#include "wcommon.h"
#include "../cjk.h"

// ---- Embedded faces (src/web/fontdata.asm, incbin of fonts/*.ttf) --------
extern const uint8_t wfd_sans_r[], wfd_sans_r_end[];
extern const uint8_t wfd_sans_b[], wfd_sans_b_end[];
extern const uint8_t wfd_sans_i[], wfd_sans_i_end[];
extern const uint8_t wfd_sans_bi[], wfd_sans_bi_end[];
extern const uint8_t wfd_serif_r[], wfd_serif_r_end[];
extern const uint8_t wfd_serif_b[], wfd_serif_b_end[];
extern const uint8_t wfd_serif_i[], wfd_serif_i_end[];
extern const uint8_t wfd_mono_r[], wfd_mono_r_end[];
extern const uint8_t wfd_mono_b[], wfd_mono_b_end[];
extern const uint8_t wfd_sym[], wfd_sym_end[];

enum { F_SANS_R, F_SANS_B, F_SANS_I, F_SANS_BI, F_SERIF_R, F_SERIF_B, F_SERIF_I,
       F_MONO_R, F_MONO_B, F_SYM, F_COUNT };

struct face {
    const uint8_t* d;
    uint32_t size;
    uint32_t cmap_sub;   // offset of the chosen cmap subtable
    int cmap_fmt;        // 4 or 12
    uint32_t glyf, loca, hmtx;
    int loca_long, num_hmetrics, num_glyphs, upem;
    int ascent, descent, line_gap, xheight, capheight; // font units
    uint16_t* gid_cache; // cp < GID_CACHE_N -> gid+1 (0 = unknown)
    int ok;
};

#define GID_CACHE_N 0x3000

static struct face faces[F_COUNT];
static int fonts_ready;

static inline uint32_t u16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline int32_t i16(const uint8_t* p) { return (int16_t)(((uint32_t)p[0] << 8) | p[1]); }
static inline uint32_t u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t find_table(const struct face* f, const char* tag) {
    uint32_t n = u16(f->d + 4);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* r = f->d + 12 + i * 16;
        if (r[0] == (uint8_t)tag[0] && r[1] == (uint8_t)tag[1] &&
            r[2] == (uint8_t)tag[2] && r[3] == (uint8_t)tag[3]) {
            uint32_t off = u32(r + 8), len = u32(r + 12);
            if (off + len > f->size || off + len < off) return 0;
            return off;
        }
    }
    return 0;
}

static void face_load(struct face* f, const uint8_t* d, const uint8_t* end) {
    memset(f, 0, sizeof(*f));
    f->d = d;
    f->size = (uint32_t)(end - d);
    if (f->size < 12) return;
    uint32_t head = find_table(f, "head"), hhea = find_table(f, "hhea");
    uint32_t maxp = find_table(f, "maxp"), cmap = find_table(f, "cmap");
    uint32_t os2 = find_table(f, "OS/2");
    f->glyf = find_table(f, "glyf");
    f->loca = find_table(f, "loca");
    f->hmtx = find_table(f, "hmtx");
    if (!head || !hhea || !maxp || !cmap || !f->glyf || !f->loca || !f->hmtx) return;
    f->upem = (int)u16(d + head + 18);
    f->loca_long = i16(d + head + 50) != 0;
    f->num_glyphs = (int)u16(d + maxp + 4);
    f->ascent = i16(d + hhea + 4);
    f->descent = -i16(d + hhea + 6);
    f->line_gap = i16(d + hhea + 8);
    f->num_hmetrics = (int)u16(d + hhea + 34);
    f->xheight = f->upem / 2;
    f->capheight = f->upem * 7 / 10;
    if (os2 && u16(d + os2) >= 2) {
        f->xheight = i16(d + os2 + 86);
        f->capheight = i16(d + os2 + 88);
    }
    // cmap: prefer (3,10) format 12, then (3,1)/(0,x) format 4
    uint32_t n = u16(d + cmap + 2), best = 0;
    int best_rank = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* r = d + cmap + 4 + i * 8;
        uint32_t pid = u16(r), eid = u16(r + 2), off = cmap + u32(r + 4);
        if (off + 4 > f->size) continue;
        int fmt = (int)u16(d + off), rank = 0;
        if (fmt == 12 && (pid == 3 || pid == 0)) rank = 3;
        else if (fmt == 4 && pid == 3 && eid == 1) rank = 2;
        else if (fmt == 4 && pid == 0) rank = 1;
        if (rank > best_rank) { best_rank = rank; best = off; }
    }
    if (!best) return;
    f->cmap_sub = best;
    f->cmap_fmt = (int)u16(d + best);
    f->ok = f->upem > 0;
}

void wfont_init(void) {
    if (fonts_ready) return;
    face_load(&faces[F_SANS_R], wfd_sans_r, wfd_sans_r_end);
    face_load(&faces[F_SANS_B], wfd_sans_b, wfd_sans_b_end);
    face_load(&faces[F_SANS_I], wfd_sans_i, wfd_sans_i_end);
    face_load(&faces[F_SANS_BI], wfd_sans_bi, wfd_sans_bi_end);
    face_load(&faces[F_SERIF_R], wfd_serif_r, wfd_serif_r_end);
    face_load(&faces[F_SERIF_B], wfd_serif_b, wfd_serif_b_end);
    face_load(&faces[F_SERIF_I], wfd_serif_i, wfd_serif_i_end);
    face_load(&faces[F_MONO_R], wfd_mono_r, wfd_mono_r_end);
    face_load(&faces[F_MONO_B], wfd_mono_b, wfd_mono_b_end);
    face_load(&faces[F_SYM], wfd_sym, wfd_sym_end);
    fonts_ready = 1;
}

// Face index + whether to shear (synthetic oblique) for a font spec.
static int pick_face(const struct wfont* f, int* synth) {
    *synth = 0;
    switch (f->family) {
    case WF_FAMILY_SERIF:
        if (f->italic && !f->bold) return F_SERIF_I;
        if (f->italic) *synth = 1;
        return f->bold ? F_SERIF_B : F_SERIF_R;
    case WF_FAMILY_MONO:
        if (f->italic) *synth = 1;
        return f->bold ? F_MONO_B : F_MONO_R;
    default:
        return f->bold ? (f->italic ? F_SANS_BI : F_SANS_B)
                       : (f->italic ? F_SANS_I : F_SANS_R);
    }
}

static uint32_t cmap_lookup(const struct face* f, uint32_t cp) {
    const uint8_t* d = f->d;
    uint32_t t = f->cmap_sub;
    if (f->cmap_fmt == 4) {
        if (cp > 0xFFFF) return 0;
        uint32_t segx2 = u16(d + t + 6);
        uint32_t ends = t + 14, starts = ends + segx2 + 2;
        uint32_t deltas = starts + segx2, ranges = deltas + segx2;
        uint32_t lo = 0, hi = segx2 / 2;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (u16(d + ends + mid * 2) < cp) lo = mid + 1; else hi = mid;
        }
        if (lo >= segx2 / 2) return 0;
        uint32_t start = u16(d + starts + lo * 2);
        if (cp < start) return 0;
        uint32_t delta = u16(d + deltas + lo * 2), roff = u16(d + ranges + lo * 2);
        if (!roff) return (cp + delta) & 0xFFFF;
        uint32_t ga = ranges + lo * 2 + roff + (cp - start) * 2;
        if (ga + 2 > f->size) return 0;
        uint32_t g = u16(d + ga);
        return g ? ((g + delta) & 0xFFFF) : 0;
    }
    if (f->cmap_fmt == 12) {
        uint32_t ng = u32(d + t + 12), lo = 0, hi = ng;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            const uint8_t* g = d + t + 16 + mid * 12;
            if (u32(g + 4) < cp) lo = mid + 1; else hi = mid;
        }
        if (lo >= ng) return 0;
        const uint8_t* g = d + t + 16 + lo * 12;
        uint32_t s = u32(g);
        if (cp < s) return 0;
        return u32(g + 8) + (cp - s);
    }
    return 0;
}

static uint32_t face_gid(struct face* f, uint32_t cp) {
    if (!f->ok) return 0;
    if (cp < GID_CACHE_N) {
        if (!f->gid_cache) {
            f->gid_cache = (uint16_t*)w_calloc(GID_CACHE_N, sizeof(uint16_t));
            if (!f->gid_cache) return cmap_lookup(f, cp);
        }
        uint16_t v = f->gid_cache[cp];
        if (v) return v - 1u;
        uint32_t g = cmap_lookup(f, cp);
        f->gid_cache[cp] = (uint16_t)(g + 1);
        return g;
    }
    return cmap_lookup(f, cp);
}

static int32_t face_adv_fu(const struct face* f, uint32_t gid) {
    if (gid >= (uint32_t)f->num_hmetrics) gid = (uint32_t)f->num_hmetrics - 1;
    return (int32_t)u16(f->d + f->hmtx + gid * 4);
}

// Scale font units to LU at pixel size px.
static inline int32_t fu_lu(const struct face* f, int32_t v, int px) {
    return w_muldiv(v, px * LU, f->upem);
}

// Resolve cp to (face, gid). Fallback chain: chosen face -> symbols face.
// Returns face index or -1 (no outline anywhere).
static int resolve(int fi, uint32_t cp, uint32_t* gid) {
    uint32_t g = face_gid(&faces[fi], cp);
    if (g) { *gid = g; return fi; }
    if (cp < 0x20) { *gid = 0; return fi; }
    g = face_gid(&faces[F_SYM], cp);
    if (g) { *gid = g; return F_SYM; }
    if (fi != F_SANS_R) {
        g = face_gid(&faces[F_SANS_R], cp);
        if (g) { *gid = g; return F_SANS_R; }
    }
    *gid = 0;
    return -1;
}

void wfont_metrics(const struct wfont* f, struct wfmetrics* m) {
    wfont_init();
    int synth;
    int fi = pick_face(f, &synth);
    const struct face* fc = &faces[fi];
    int px = f->px ? f->px : 16;
    if (!fc->ok) {
        m->ascent = PX(px) * 8 / 10; m->descent = PX(px) / 5; m->line_gap = 0;
        m->xheight = PX(px) / 2; m->capheight = PX(px) * 7 / 10;
        return;
    }
    m->ascent = fu_lu(fc, fc->ascent, px);
    m->descent = fu_lu(fc, fc->descent, px);
    m->line_gap = fu_lu(fc, fc->line_gap, px);
    m->xheight = fu_lu(fc, fc->xheight, px);
    m->capheight = fu_lu(fc, fc->capheight, px);
}

// Width (LU) of codepoints with no outline: CJK bitmaps are square (1em),
// everything else draws a tofu box 0.6em wide.
static int32_t missing_adv(int px, uint32_t cp) {
    if (cjk_glyph_for(cp)) return PX(px);
    return PX(px) * 6 / 10;
}

static int is_zero_width(uint32_t cp) {
    return (cp >= 0x300 && cp <= 0x36F) || cp == 0x200B || cp == 0x200C ||
           cp == 0x200D || cp == 0xFEFF || cp == 0xAD || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           cp == 0x200E || cp == 0x200F;
}

int32_t wfont_advance(const struct wfont* f, uint32_t cp) {
    wfont_init();
    int synth;
    int fi = pick_face(f, &synth);
    int px = f->px ? f->px : 16;
    if (cp == 0xA0) cp = ' ';
    if (is_zero_width(cp)) {
        uint32_t g;
        int r = resolve(fi, cp, &g);
        if (r < 0 || cp == 0xAD) return 0;
    }
    uint32_t gid;
    int r = resolve(fi, cp, &gid);
    if (r < 0) return missing_adv(px, cp);
    return fu_lu(&faces[r], face_adv_fu(&faces[r], gid), px);
}

int32_t wfont_measure(const struct wfont* f, const char* s, int len) {
    int32_t w = 0;
    for (int i = 0; i < len;) {
        uint32_t cp;
        i += w_utf8_dec(s + i, len - i, &cp);
        w += wfont_advance(f, cp);
    }
    return w;
}

// ---- Glyph outlines -------------------------------------------------------

struct xf {             // font units -> composed transform (2.14 matrix + offset)
    int32_t a, b, c, d; // x' = (a*x + c*y) >> 14 + dx ; y' = (b*x + d*y) >> 14 + dy
    int32_t dx, dy;
};

struct gctx {
    struct wraster* r;
    const struct face* f;
    int px;
    int synth;
    int32_t ox, oy;     // subpx origin: glyph (0,0) maps to (ox, oy) in mask space
};

// Map a point in font units through xf into mask subpx coordinates.
static void map_pt(const struct gctx* g, const struct xf* t, int32_t x, int32_t y,
                   int32_t* X, int32_t* Y) {
    int32_t tx = ((t->a * x + t->c * y) >> 14) + t->dx;
    int32_t ty = ((t->b * x + t->d * y) >> 14) + t->dy;
    int32_t sx = w_muldiv(tx, g->px * WR_ONE, g->f->upem);
    int32_t sy = w_muldiv(ty, g->px * WR_ONE, g->f->upem);
    if (g->synth) sx += (sy * 54) >> 8; // ~12 degree oblique
    *X = g->ox + sx;
    *Y = g->oy - sy;
}

#define MAXPTS 1024
static int16_t gx[MAXPTS], gy[MAXPTS];
static uint8_t gflag[MAXPTS];
static uint16_t gend[256];

static int glyph_loc(const struct face* f, uint32_t gid, uint32_t* off, uint32_t* len) {
    if (gid >= (uint32_t)f->num_glyphs) return 0;
    uint32_t a, b;
    if (f->loca_long) { a = u32(f->d + f->loca + gid * 4); b = u32(f->d + f->loca + gid * 4 + 4); }
    else { a = u16(f->d + f->loca + gid * 2) * 2; b = u16(f->d + f->loca + gid * 2 + 2) * 2; }
    if (b <= a || f->glyf + b > f->size) return 0;
    *off = f->glyf + a;
    *len = b - a;
    return 1;
}

static void emit_simple(struct gctx* g, const struct xf* t, const uint8_t* p, uint32_t len,
                        int ncont) {
    const uint8_t* end = p + len;
    const uint8_t* q = p + 10;
    if (ncont > 256) return;
    for (int i = 0; i < ncont; i++) { gend[i] = (uint16_t)u16(q); q += 2; }
    int npts = ncont ? gend[ncont - 1] + 1 : 0;
    if (npts > MAXPTS || npts <= 0) return;
    uint32_t ilen = u16(q);
    q += 2 + ilen;
    // flags
    for (int i = 0; i < npts;) {
        if (q >= end) return;
        uint8_t fl = *q++;
        gflag[i++] = fl;
        if (fl & 8) {
            if (q >= end) return;
            int rep = *q++;
            while (rep-- > 0 && i < npts) gflag[i++] = fl;
        }
    }
    int32_t v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = gflag[i];
        if (fl & 2) { if (q >= end) return; int dv = *q++; v += (fl & 16) ? dv : -dv; }
        else if (!(fl & 16)) { if (q + 2 > end) return; v += i16(q); q += 2; }
        gx[i] = (int16_t)v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = gflag[i];
        if (fl & 4) { if (q >= end) return; int dv = *q++; v += (fl & 32) ? dv : -dv; }
        else if (!(fl & 32)) { if (q + 2 > end) return; v += i16(q); q += 2; }
        gy[i] = (int16_t)v;
    }
    int s = 0;
    for (int c = 0; c < ncont; c++) {
        int e = gend[c];
        if (e < s || e >= npts) return;
        int n = e - s + 1;
        // Start at an on-curve point, or the midpoint of the first two
        // off-curve points when the contour has none on the curve.
        int start = -1;
        for (int k = 0; k < n; k++) if (gflag[s + k] & 1) { start = k; break; }
        int32_t X, Y, X0, Y0;
        if (start >= 0) {
            map_pt(g, t, gx[s + start], gy[s + start], &X0, &Y0);
        } else {
            int32_t ax, ay, bx, by;
            map_pt(g, t, gx[s], gy[s], &ax, &ay);
            map_pt(g, t, gx[s + (n > 1)], gy[s + (n > 1)], &bx, &by);
            X0 = (ax + bx) / 2; Y0 = (ay + by) / 2;
            start = 0;
        }
        wr_move(g->r, X0, Y0);
        int32_t cx = 0, cy = 0;
        int have_ctl = 0;
        for (int k = 1; k <= n; k++) {
            int idx = s + (start + k) % n;
            map_pt(g, t, gx[idx], gy[idx], &X, &Y);
            if (gflag[idx] & 1) {
                if (have_ctl) wr_quad(g->r, cx, cy, X, Y);
                else wr_line(g->r, X, Y);
                have_ctl = 0;
            } else {
                if (have_ctl) wr_quad(g->r, cx, cy, (cx + X) / 2, (cy + Y) / 2);
                cx = X; cy = Y;
                have_ctl = 1;
            }
        }
        if (have_ctl) wr_quad(g->r, cx, cy, X0, Y0);
        wr_close(g->r);
        s = e + 1;
    }
}

static void emit_glyph(struct gctx* g, uint32_t gid, const struct xf* t, int depth) {
    uint32_t off, len;
    if (depth > 6 || !glyph_loc(g->f, gid, &off, &len) || len < 10) return;
    const uint8_t* p = g->f->d + off;
    int ncont = i16(p);
    if (ncont >= 0) { emit_simple(g, t, p, len, ncont); return; }
    const uint8_t* q = p + 10;
    const uint8_t* end = p + len;
    for (;;) {
        if (q + 4 > end) return;
        uint32_t fl = u16(q), cg = u16(q + 2);
        q += 4;
        int32_t a1, a2;
        if (fl & 1) { if (q + 4 > end) return; a1 = i16(q); a2 = i16(q + 2); q += 4; }
        else { if (q + 2 > end) return; a1 = (int8_t)q[0]; a2 = (int8_t)q[1]; q += 2; }
        int32_t ma = 1 << 14, mb = 0, mc = 0, md = 1 << 14;
        if (fl & 8) { if (q + 2 > end) return; ma = md = i16(q); q += 2; }
        else if (fl & 0x40) { if (q + 4 > end) return; ma = i16(q); md = i16(q + 2); q += 4; }
        else if (fl & 0x80) {
            if (q + 8 > end) return;
            ma = i16(q); mb = i16(q + 2); mc = i16(q + 4); md = i16(q + 6); q += 8;
        }
        struct xf ct;
        // compose: parent(t) o child(m, offset)
        ct.a = (t->a * ma + t->c * mb) >> 14;
        ct.b = (t->b * ma + t->d * mb) >> 14;
        ct.c = (t->a * mc + t->c * md) >> 14;
        ct.d = (t->b * mc + t->d * md) >> 14;
        int32_t ox = (fl & 2) ? a1 : 0, oy = (fl & 2) ? a2 : 0; // point matching: ignored
        ct.dx = ((t->a * ox + t->c * oy) >> 14) + t->dx;
        ct.dy = ((t->b * ox + t->d * oy) >> 14) + t->dy;
        emit_glyph(g, cg, &ct, depth + 1);
        if (!(fl & 0x20)) break;
    }
}

// ---- Glyph cache ------------------------------------------------------------

struct gent {
    uint32_t key;
    struct gent* next;
    int16_t w, h, left, top;   // bitmap size; left/top offsets from pen/baseline
    uint8_t bm[];
};

#define GHASH 4096
#define GCACHE_BUDGET (6 * 1024 * 1024)
static struct gent* ghash[GHASH];
static int gcount, gbytes;
static struct wraster g_r;
static int g_r_init;

void wfont_cache_flush(void) {
    for (int i = 0; i < GHASH; i++) {
        struct gent* e = ghash[i];
        while (e) { struct gent* n = e->next; w_free(e); e = n; }
        ghash[i] = 0;
    }
    gcount = 0; gbytes = 0;
}

void wfont_cache_stats(int* glyphs, int* bytes) { *glyphs = gcount; *bytes = gbytes; }

// Coverage boost: unhinted AA text reads thin; lift mid-tones slightly.
static uint8_t gamma_lut[256];
static int gamma_ready;
static void gamma_init(void) {
    // a' = a + a*(255-a)/(255*3)  (cheap concave curve, monotonic)
    for (int a = 0; a < 256; a++) {
        int v = a + (a * (255 - a)) / 600;
        gamma_lut[a] = (uint8_t)(v > 255 ? 255 : v);
    }
    gamma_ready = 1;
}

struct mask_ctx { uint8_t* bm; int w, h; };
static void mask_span(void* ctx, int y, int x, int len, const uint8_t* cov) {
    struct mask_ctx* m = (struct mask_ctx*)ctx;
    if (y < 0 || y >= m->h) return;
    for (int i = 0; i < len; i++) {
        int xx = x + i;
        if (xx >= 0 && xx < m->w) m->bm[y * m->w + xx] = gamma_lut[cov[i]];
    }
}

static struct gent* glyph_get(int fi, int synth, int px, uint32_t gid, int phase) {
    uint32_t key = ((uint32_t)fi << 28) | ((uint32_t)synth << 27) | ((uint32_t)phase << 25) |
                   ((uint32_t)(px & 0x1FF) << 16) | (gid & 0xFFFF);
    uint32_t h = (key * 2654435761u) >> 20;
    for (struct gent* e = ghash[h]; e; e = e->next)
        if (e->key == key) return e;
    const struct face* f = &faces[fi];
    uint32_t off, len;
    int32_t xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    int empty = !glyph_loc(f, gid, &off, &len) || len < 10;
    if (!empty) {
        const uint8_t* p = f->d + off;
        xmin = i16(p + 2); ymin = i16(p + 4); xmax = i16(p + 6); ymax = i16(p + 8);
    }
    int left = 0, top = 0, w = 0, hh = 0;
    if (!empty) {
        int32_t sx0 = w_muldiv(xmin, px * WR_ONE, f->upem);
        int32_t sx1 = w_muldiv(xmax, px * WR_ONE, f->upem);
        int32_t sy0 = w_muldiv(ymin, px * WR_ONE, f->upem);
        int32_t sy1 = w_muldiv(ymax, px * WR_ONE, f->upem);
        if (synth) { sx0 += (sy0 * 54) >> 8; sx1 += (sy1 * 54) >> 8; }
        left = (sx0 >> WR_SHIFT) - 1;
        int right = ((sx1 + WR_ONE - 1) >> WR_SHIFT) + 2;
        top = ((sy1 + WR_ONE - 1) >> WR_SHIFT) + 1;
        int bottom = (sy0 >> WR_SHIFT) - 1;
        w = right - left;
        hh = top - bottom;
        if (w <= 0 || hh <= 0 || w > 2048 || hh > 2048) { w = hh = 0; empty = 1; }
    }
    struct gent* e = (struct gent*)w_malloc(sizeof(struct gent) + (size_t)(w * hh));
    if (!e) return 0;
    e->key = key; e->w = (int16_t)w; e->h = (int16_t)hh;
    e->left = (int16_t)left; e->top = (int16_t)top;
    if (w * hh) {
        memset(e->bm, 0, (size_t)(w * hh));
        if (!g_r_init) { wr_init(&g_r); g_r_init = 1; }
        if (!gamma_ready) gamma_init();
        wr_reset(&g_r);
        struct gctx gc;
        gc.r = &g_r; gc.f = f; gc.px = px; gc.synth = synth;
        gc.ox = -left * WR_ONE + phase * (WR_ONE / 4);
        gc.oy = top * WR_ONE;
        struct xf t = { 1 << 14, 0, 0, 1 << 14, 0, 0 };
        emit_glyph(&gc, gid, &t, 0);
        struct mask_ctx mc = { e->bm, w, hh };
        wr_fill(&g_r, 0, 0, w, hh, 0, mask_span, &mc);
    }
    if (gbytes > GCACHE_BUDGET) wfont_cache_flush();
    e->next = ghash[h];
    ghash[h] = e;
    gcount++;
    gbytes += (int)sizeof(struct gent) + w * hh;
    return e;
}

// ---- Drawing ----------------------------------------------------------------

void ws_fill_rect(struct wsurf* s, int x, int y, int w, int h, uint32_t argb) {
    int x0 = W_MAX(x, s->cx0), y0 = W_MAX(y, s->cy0);
    int x1 = W_MIN(x + w, s->cx1), y1 = W_MIN(y + h, s->cy1);
    unsigned a = argb >> 24;
    if (x0 >= x1 || y0 >= y1 || a == 0) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = s->px + yy * s->stride;
        if (a >= 255) { uint32_t c = argb & 0xFFFFFF; for (int xx = x0; xx < x1; xx++) row[xx] = c; }
        else for (int xx = x0; xx < x1; xx++) row[xx] = ws_blend(row[xx], argb, a);
    }
}

void ws_mask(struct wsurf* s, int x, int y, int w, int h,
             const uint8_t* mask, int mstride, uint32_t argb) {
    int x0 = W_MAX(x, s->cx0), y0 = W_MAX(y, s->cy0);
    int x1 = W_MIN(x + w, s->cx1), y1 = W_MIN(y + h, s->cy1);
    unsigned ca = argb >> 24;
    if (x0 >= x1 || y0 >= y1 || ca == 0) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = s->px + yy * s->stride;
        const uint8_t* m = mask + (yy - y) * mstride - x;
        for (int xx = x0; xx < x1; xx++) {
            unsigned a = m[xx];
            if (!a) continue;
            if (ca < 255) a = (a * ca + 127) / 255;
            row[xx] = ws_blend(row[xx], argb, a);
        }
    }
}

static void draw_tofu(struct wsurf* s, int x, int y_base, int px, uint32_t argb) {
    int w = px * 6 / 10 - 2, h = px * 7 / 10;
    if (w < 3) w = 3;
    int x0 = x + 1, y0 = y_base - h;
    ws_fill_rect(s, x0, y0, w, 1, argb);
    ws_fill_rect(s, x0, y_base - 1, w, 1, argb);
    ws_fill_rect(s, x0, y0, 1, h, argb);
    ws_fill_rect(s, x0 + w - 1, y0, 1, h, argb);
}

static void draw_cjk(struct wsurf* s, int x, int y_base, int px, const uint8_t* bits, uint32_t argb) {
    // 16x16 bitmap scaled to an em box sitting on the baseline (box bottom
    // ~0.12em below it, like the ideographic baseline offset).
    int top = y_base - px * 88 / 100;
    for (int yy = 0; yy < px; yy++) {
        int sy = yy * 16 / px;
        for (int xx = 0; xx < px; xx++) {
            int sx = xx * 16 / px;
            if (bits[sy * 2 + (sx >> 3)] & (0x80 >> (sx & 7)))
                ws_fill_rect(s, x + xx, top + yy, 1, 1, argb);
        }
    }
}

int32_t wfont_draw(struct wsurf* s, const struct wfont* f, int32_t x, int32_t y,
                   uint32_t argb, const char* str, int len,
                   int32_t letter_sp, int32_t word_sp) {
    wfont_init();
    int synth;
    int fi = pick_face(f, &synth);
    int px = f->px ? f->px : 16;
    int base_y = LU_ROUND(y);
    int32_t pen = x;
    for (int i = 0; i < len;) {
        uint32_t cp;
        i += w_utf8_dec(str + i, len - i, &cp);
        if (cp == 0xA0) cp = ' ';
        uint32_t gid;
        int r = resolve(fi, cp, &gid);
        int32_t adv;
        if (r < 0) {
            if (is_zero_width(cp)) continue;
            const uint8_t* bits = cjk_glyph_for(cp);
            int pxx = LU_FLOOR(pen);
            if (bits) draw_cjk(s, pxx, base_y, px, bits, argb);
            else draw_tofu(s, pxx, base_y, px, argb);
            adv = missing_adv(px, cp);
        } else {
            adv = (cp == 0xAD) ? 0 : fu_lu(&faces[r], face_adv_fu(&faces[r], gid), px);
            if (cp > ' ' && cp != 0xAD) {
                int32_t pp = pen; // LU; quarter-pixel phase
                int ipx = pp >> 6;
                int phase = (pp & 63) >> 4;
                struct gent* g = glyph_get(r, (r == fi) ? synth : 0, px, gid, phase);
                if (g && g->w)
                    ws_mask(s, ipx + g->left, base_y - g->top, g->w, g->h, g->bm, g->w, argb);
            }
        }
        pen += adv + letter_sp;
        if (cp == ' ') pen += word_sp;
    }
    return pen - x;
}

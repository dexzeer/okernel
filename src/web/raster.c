#include "raster.h"
#include "wcommon.h"

#define BAND 64

void wr_init(struct wraster* r) { memset(r, 0, sizeof(*r)); wr_reset(r); }

void wr_free(struct wraster* r) {
    w_free(r->e); w_free(r->cover); w_free(r->area); w_free(r->row);
    memset(r, 0, sizeof(*r));
}

void wr_reset(struct wraster* r) {
    r->ne = 0;
    r->has_sub = 0;
    r->minx = r->miny = INT32_MAX;
    r->maxx = r->maxy = INT32_MIN;
}

static void add_edge(struct wraster* r, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (y0 == y1) return; // horizontal edges carry no cover
    if (r->ne == r->cap) {
        int nc = r->cap ? r->cap * 2 : 256;
        struct wr_edge* ne = (struct wr_edge*)w_realloc(r->e, nc * sizeof(struct wr_edge));
        if (!ne) return;
        r->e = ne; r->cap = nc;
    }
    struct wr_edge* e = &r->e[r->ne++];
    e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1;
    if (x0 < r->minx) r->minx = x0;
    if (x1 < r->minx) r->minx = x1;
    if (x0 > r->maxx) r->maxx = x0;
    if (x1 > r->maxx) r->maxx = x1;
    if (y0 < r->miny) r->miny = y0;
    if (y1 < r->miny) r->miny = y1;
    if (y0 > r->maxy) r->maxy = y0;
    if (y1 > r->maxy) r->maxy = y1;
}

void wr_move(struct wraster* r, int32_t x, int32_t y) {
    if (r->has_sub) wr_close(r);
    r->cx = r->sx = x; r->cy = r->sy = y;
    r->has_sub = 1;
}

void wr_line(struct wraster* r, int32_t x, int32_t y) {
    if (!r->has_sub) { wr_move(r, x, y); return; }
    add_edge(r, r->cx, r->cy, x, y);
    r->cx = x; r->cy = y;
}

void wr_close(struct wraster* r) {
    if (!r->has_sub) return;
    if (r->cx != r->sx || r->cy != r->sy) add_edge(r, r->cx, r->cy, r->sx, r->sy);
    r->cx = r->sx; r->cy = r->sy;
    r->has_sub = 0;
}

static uint32_t isqrt32(uint32_t v) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

// num / d for |num| < d * 2^31 without libgcc.
static int32_t div64(int64_t num, int32_t d) {
    int neg = 0;
    if (num < 0) { num = -num; neg = 1; }
    uint64_t u = (uint64_t)num;
    uint32_t hi = (uint32_t)(u >> 32), lo = (uint32_t)u, q, rr;
    if (hi >= (uint32_t)d) return neg ? INT32_MIN + 1 : INT32_MAX;
    __asm__("divl %4" : "=a"(q), "=d"(rr) : "a"(lo), "d"(hi), "rm"((uint32_t)d));
    (void)rr;
    return neg ? -(int32_t)q : (int32_t)q;
}

void wr_quad(struct wraster* r, int32_t cx, int32_t cy, int32_t x, int32_t y) {
    if (!r->has_sub) wr_move(r, r->cx, r->cy);
    int32_t x0 = r->cx, y0 = r->cy;
    uint32_t dd = (uint32_t)(iabs32(x0 - 2 * cx + x) + iabs32(y0 - 2 * cy + y));
    // error ~ dd / (8 n^2) <= 0.1px (25.6 subpx)  =>  n^2 >= dd / 205
    int n = (int)isqrt32(dd / 205) + 1;
    if (n > 64) n = 64;
    int32_t n2 = n * n;
    for (int i = 1; i <= n; i++) {
        int64_t a = (int64_t)(n - i) * (n - i), b = (int64_t)2 * i * (n - i), c = (int64_t)i * i;
        int32_t px = div64(a * x0 + b * cx + c * x, n2);
        int32_t py = div64(a * y0 + b * cy + c * y, n2);
        if (i == n) { px = x; py = y; }
        wr_line(r, px, py);
    }
}

void wr_cubic(struct wraster* r, int32_t c1x, int32_t c1y, int32_t c2x, int32_t c2y,
              int32_t x, int32_t y) {
    if (!r->has_sub) wr_move(r, r->cx, r->cy);
    int32_t x0 = r->cx, y0 = r->cy;
    uint32_t d1 = (uint32_t)(iabs32(x0 - 2 * c1x + c2x) + iabs32(y0 - 2 * c1y + c2y));
    uint32_t d2 = (uint32_t)(iabs32(c1x - 2 * c2x + x) + iabs32(c1y - 2 * c2y + y));
    uint32_t dd = d1 > d2 ? d1 : d2;
    int n = (int)isqrt32((dd * 3) / 205) + 1;
    if (n > 96) n = 96;
    int64_t n3 = (int64_t)n * n * n;
    for (int i = 1; i <= n; i++) {
        int64_t t = i, s = n - i;
        int64_t a = s * s * s, b = 3 * s * s * t, c = 3 * s * t * t, d = t * t * t;
        // n3 <= 884736 fits int32; numerators stay within int64.
        int32_t px = div64(a * x0 + b * c1x + c * c2x + d * x, (int32_t)n3);
        int32_t py = div64(a * y0 + b * c1y + c * c2y + d * y, (int32_t)n3);
        if (i == n) { px = x; py = y; }
        wr_line(r, px, py);
    }
}

void wr_rect(struct wraster* r, int32_t x, int32_t y, int32_t w, int32_t h) {
    wr_move(r, x, y);
    wr_line(r, x + w, y);
    wr_line(r, x + w, y + h);
    wr_line(r, x, y + h);
    wr_close(r);
}

// Cubic kappa for a quarter ellipse: 0.5523 in 16.16
#define KAPPA 36195

void wr_rrect(struct wraster* r, int32_t x, int32_t y, int32_t w, int32_t h,
              const int32_t rad[4]) {
    int32_t tl = rad[0], tr = rad[1], br = rad[2], bl = rad[3];
    // Clamp radii so adjacent corners never overlap (CSS: scale all down).
    int32_t m = w < h ? w : h;
    if (tl + tr > w || bl + br > w || tl + bl > h || tr + br > h) {
        int32_t s = m / 2;
        if (tl > s) tl = s;
        if (tr > s) tr = s;
        if (br > s) br = s;
        if (bl > s) bl = s;
    }
    if (tl <= 0 && tr <= 0 && br <= 0 && bl <= 0) { wr_rect(r, x, y, w, h); return; }
    int32_t k;
    wr_move(r, x + tl, y);
    wr_line(r, x + w - tr, y);
    if (tr > 0) {
        k = (int32_t)(((int64_t)tr * KAPPA) >> 16);
        wr_cubic(r, x + w - tr + k, y, x + w, y + tr - k, x + w, y + tr);
    }
    wr_line(r, x + w, y + h - br);
    if (br > 0) {
        k = (int32_t)(((int64_t)br * KAPPA) >> 16);
        wr_cubic(r, x + w, y + h - br + k, x + w - br + k, y + h, x + w - br, y + h);
    }
    wr_line(r, x + bl, y + h);
    if (bl > 0) {
        k = (int32_t)(((int64_t)bl * KAPPA) >> 16);
        wr_cubic(r, x + bl - k, y + h, x, y + h - bl + k, x, y + h - bl);
    }
    wr_line(r, x, y + tl);
    if (tl > 0) {
        k = (int32_t)(((int64_t)tl * KAPPA) >> 16);
        wr_cubic(r, x, y + tl - k, x + tl - k, y, x + tl, y);
    }
    wr_close(r);
}

void wr_ellipse(struct wraster* r, int32_t cx, int32_t cy, int32_t rx, int32_t ry) {
    int32_t kx = (int32_t)(((int64_t)rx * KAPPA) >> 16);
    int32_t ky = (int32_t)(((int64_t)ry * KAPPA) >> 16);
    wr_move(r, cx + rx, cy);
    wr_cubic(r, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    wr_cubic(r, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    wr_cubic(r, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    wr_cubic(r, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    wr_close(r);
}

int wr_bounds(const struct wraster* r, int* x0, int* y0, int* x1, int* y1) {
    if (r->ne == 0) return 0;
    *x0 = r->minx >> WR_SHIFT;
    *y0 = r->miny >> WR_SHIFT;
    *x1 = (r->maxx + WR_ONE - 1) >> WR_SHIFT;
    *y1 = (r->maxy + WR_ONE - 1) >> WR_SHIFT;
    return 1;
}

// ---- Cell accumulation (one band) -----------------------------------------

struct band {
    int32_t* cover;
    int32_t* area;
    int cw, h;           // cell grid width (box_w + 2) and band height
};

static inline void cell_add(struct band* b, int ex, int ey, int32_t cov, int32_t area) {
    if ((unsigned)ey >= (unsigned)b->h || (unsigned)ex >= (unsigned)b->cw) return;
    int i = ey * b->cw + ex;
    b->cover[i] += cov;
    b->area[i] += area;
}

static void render_hline(struct band* b, int ey, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    int ex1 = x1 >> 8, ex2 = x2 >> 8;
    int32_t fx1 = x1 & 255, fx2 = x2 & 255;
    if (y1 == y2) return;
    if (ex1 == ex2) { cell_add(b, ex1, ey, y2 - y1, (fx1 + fx2) * (y2 - y1)); return; }
    int32_t p = (256 - fx1) * (y2 - y1), first = 256, incr = 1, dx = x2 - x1;
    if (dx < 0) { p = fx1 * (y2 - y1); first = 0; incr = -1; dx = -dx; }
    int32_t delta = p / dx, mod = p % dx;
    if (mod < 0) { delta--; mod += dx; }
    cell_add(b, ex1, ey, delta, (fx1 + first) * delta);
    ex1 += incr;
    y1 += delta;
    if (ex1 != ex2) {
        p = 256 * (y2 - y1 + delta);
        int32_t lift = p / dx, rem = p % dx;
        if (rem < 0) { lift--; rem += dx; }
        mod -= dx;
        while (ex1 != ex2) {
            delta = lift;
            mod += rem;
            if (mod >= 0) { mod -= dx; delta++; }
            cell_add(b, ex1, ey, delta, 256 * delta);
            y1 += delta;
            ex1 += incr;
        }
    }
    delta = y2 - y1;
    cell_add(b, ex1, ey, delta, (fx2 + 256 - first) * delta);
}

static void render_line(struct band* b, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    int32_t dx = x2 - x1, dy = y2 - y1;
    int ey1 = y1 >> 8, ey2 = y2 >> 8;
    int32_t fy1 = y1 & 255, fy2 = y2 & 255;
    if (ey1 == ey2) { render_hline(b, ey1, x1, fy1, x2, fy2); return; }
    int incr = 1;
    if (dx == 0) {
        int ex = x1 >> 8;
        int32_t two_fx = (x1 - (ex << 8)) << 1;
        int32_t first = 256;
        if (dy < 0) { first = 0; incr = -1; }
        int32_t delta = first - fy1;
        cell_add(b, ex, ey1, delta, two_fx * delta);
        ey1 += incr;
        delta = first + first - 256;
        int32_t area = two_fx * delta;
        while (ey1 != ey2) { cell_add(b, ex, ey1, delta, area); ey1 += incr; }
        delta = fy2 - 256 + first;
        cell_add(b, ex, ey1, delta, two_fx * delta);
        return;
    }
    int32_t p = (256 - fy1) * dx, first = 256;
    if (dy < 0) { p = fy1 * dx; first = 0; incr = -1; dy = -dy; }
    int32_t delta = p / dy, mod = p % dy;
    if (mod < 0) { delta--; mod += dy; }
    int32_t x_from = x1 + delta;
    render_hline(b, ey1, x1, fy1, x_from, first);
    ey1 += incr;
    if (ey1 != ey2) {
        p = 256 * dx;
        int32_t lift = p / dy, rem = p % dy;
        if (rem < 0) { lift--; rem += dy; }
        mod -= dy;
        while (ey1 != ey2) {
            delta = lift;
            mod += rem;
            if (mod >= 0) { mod -= dy; delta++; }
            int32_t x_to = x_from + delta;
            render_hline(b, ey1, x_from, 256 - first, x_to, first);
            x_from = x_to;
            ey1 += incr;
        }
    }
    render_hline(b, ey1, x_from, 256 - first, x2, fy2);
}

// x where the segment crosses y = yt (Y0 != Y1).
static int32_t lerp_x(int32_t X0, int32_t Y0, int32_t X1, int32_t Y1, int32_t yt) {
    int64_t num = (int64_t)(X1 - X0) * (yt - Y0);
    int32_t d = Y1 - Y0;
    if (d < 0) { d = -d; num = -num; }
    return X0 + div64(num, d);
}

// y where the segment crosses x = xt (X0 != X1).
static int32_t lerp_y(int32_t X0, int32_t Y0, int32_t X1, int32_t Y1, int32_t xt) {
    return lerp_x(Y0, X0, Y1, X1, xt);
}

// Clip a band-relative edge horizontally into [0, xmax] by splitting at the
// boundaries; parts outside are clamped onto the boundary (left-clamped
// parts keep their winding contribution for the pixels to their right).
static void xclip_line(struct band* b, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       int32_t xmax, int depth) {
    if (y0 == y1) return;
    if (x0 <= 0 && x1 <= 0) { render_line(b, 0, y0, 0, y1); return; }
    if (x0 >= xmax && x1 >= xmax) { render_line(b, xmax, y0, xmax, y1); return; }
    if (depth < 3) {
        if ((x0 < 0) != (x1 < 0)) {
            int32_t ym = lerp_y(x0, y0, x1, y1, 0);
            xclip_line(b, x0, y0, 0, ym, xmax, depth + 1);
            xclip_line(b, 0, ym, x1, y1, xmax, depth + 1);
            return;
        }
        if ((x0 > xmax) != (x1 > xmax)) {
            int32_t ym = lerp_y(x0, y0, x1, y1, xmax);
            xclip_line(b, x0, y0, xmax, ym, xmax, depth + 1);
            xclip_line(b, xmax, ym, x1, y1, xmax, depth + 1);
            return;
        }
    }
    if (x0 < 0) x0 = 0;
    if (x1 < 0) x1 = 0;
    if (x0 > xmax) x0 = xmax;
    if (x1 > xmax) x1 = xmax;
    render_line(b, x0, y0, x1, y1);
}

int wr_fill(struct wraster* r, int x0, int y0, int x1, int y1, int evenodd,
            wr_span_fn fn, void* ctx) {
    if (r->has_sub) wr_close(r);
    if (r->ne == 0) return 0;
    int bx0, by0, bx1, by1;
    wr_bounds(r, &bx0, &by0, &bx1, &by1);
    if (by0 < y0) by0 = y0;
    if (by1 > y1) by1 = y1;
    // x extent: draw only inside [x0,x1) but the path may start further left
    int dx0 = bx0 > x0 ? bx0 : x0;
    int dx1 = bx1 < x1 ? bx1 : x1;
    if (by0 >= by1 || dx0 >= dx1) return 0;
    int bw = dx1 - dx0;
    int cw = bw + 2;
    int cells = cw * BAND;
    if (cells > r->scratch_cells) {
        w_free(r->cover); w_free(r->area);
        r->cover = (int32_t*)w_malloc(cells * sizeof(int32_t));
        r->area = (int32_t*)w_malloc(cells * sizeof(int32_t));
        if (!r->cover || !r->area) { r->scratch_cells = 0; return 0; }
        r->scratch_cells = cells;
    }
    if (bw > r->scratch_w) {
        w_free(r->row);
        r->row = (uint8_t*)w_malloc(bw);
        if (!r->row) { r->scratch_w = 0; return 0; }
        r->scratch_w = bw;
    }
    struct band b;
    b.cover = r->cover; b.area = r->area; b.cw = cw;
    int32_t ox = dx0 << WR_SHIFT;
    int32_t xmax = (bw + 1) << WR_SHIFT;
    int drew = 0;
    for (int band_y = by0; band_y < by1; band_y += BAND) {
        int bh = by1 - band_y < BAND ? by1 - band_y : BAND;
        b.h = bh;
        memset(b.cover, 0, cw * bh * sizeof(int32_t));
        memset(b.area, 0, cw * bh * sizeof(int32_t));
        int32_t oy = band_y << WR_SHIFT;
        int32_t hs = bh << WR_SHIFT;
        int any = 0;
        for (int i = 0; i < r->ne; i++) {
            struct wr_edge* e = &r->e[i];
            int32_t X0 = e->x0 - ox, Y0 = e->y0 - oy, X1 = e->x1 - ox, Y1 = e->y1 - oy;
            if ((Y0 <= 0 && Y1 <= 0) || (Y0 >= hs && Y1 >= hs)) continue;
            int32_t ex0 = X0, ey0 = Y0, ex1 = X1, ey1 = Y1;
            // vertical clip to [0, hs]
            if (ey0 < 0) { ex0 = lerp_x(X0, Y0, X1, Y1, 0); ey0 = 0; }
            else if (ey0 > hs) { ex0 = lerp_x(X0, Y0, X1, Y1, hs); ey0 = hs; }
            if (ey1 < 0) { ex1 = lerp_x(X0, Y0, X1, Y1, 0); ey1 = 0; }
            else if (ey1 > hs) { ex1 = lerp_x(X0, Y0, X1, Y1, hs); ey1 = hs; }
            xclip_line(&b, ex0, ey0, ex1, ey1, xmax, 0);
            any = 1;
        }
        if (!any) continue;
        for (int yy = 0; yy < bh; yy++) {
            int32_t* cv = b.cover + yy * cw;
            int32_t* ar = b.area + yy * cw;
            int32_t acc = 0;
            int first = -1, last = -1;
            for (int xx = 0; xx < bw; xx++) {
                acc += cv[xx];
                int32_t a = ((acc << 9) - ar[xx]) >> 9;
                if (a < 0) a = -a;
                if (evenodd) {
                    a &= 511;
                    if (a > 256) a = 512 - a;
                }
                if (a > 255) a = 255;
                r->row[xx] = (uint8_t)a;
                if (a) { if (first < 0) first = xx; last = xx; }
            }
            if (first >= 0) {
                fn(ctx, band_y + yy, dx0 + first, last - first + 1, r->row + first);
                drew = 1;
            }
        }
    }
    return drew;
}

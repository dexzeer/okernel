// Inline SVG renderer (static subset): shapes + paths with transforms,
// fill/stroke from computed CSS (presentation attributes are mapped to CSS
// by the cascade), viewBox mapping. Integer math only.
#include "paint.h"
#include "raster.h"
#include "css.h"
#include "wcommon.h"
#include "colortab.h"
#include "css_int.h"

// fixed 16.16 affine: x' = a*x + c*y + e ; y' = b*x + d*y + f
struct mat { int32_t a, b, c, d, e, f; };

static int32_t fmul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }

static struct mat mat_mul(struct mat m, struct mat n) { // m * n (apply n first)
    struct mat r;
    r.a = fmul(m.a, n.a) + fmul(m.c, n.b);
    r.b = fmul(m.b, n.a) + fmul(m.d, n.b);
    r.c = fmul(m.a, n.c) + fmul(m.c, n.d);
    r.d = fmul(m.b, n.c) + fmul(m.d, n.d);
    r.e = fmul(m.a, n.e) + fmul(m.c, n.f) + m.e;
    r.f = fmul(m.b, n.e) + fmul(m.d, n.f) + m.f;
    return r;
}

static struct mat mat_id(void) { struct mat m = { 65536, 0, 0, 65536, 0, 0 }; return m; }

static int32_t sin16(int32_t deg_milli) {
    int32_t d = deg_milli / 1000;
    d %= 360;
    if (d < 0) d += 360;
    return sin_deg[d];
}
static int32_t cos16(int32_t deg_milli) { return sin16(deg_milli + 90000); }

// number parser for SVG lists (commas/space separated; "1-2" -> 1, -2; ".5.5")
static int num(const char* s, int len, int* i, int32_t* out16) {
    int p = *i;
    while (p < len && (w_isspace((unsigned char)s[p]) || s[p] == ',')) p++;
    if (p >= len) return 0;
    char c = s[p];
    if (!(w_isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.')) return 0;
    int st = p;
    if (s[p] == '-' || s[p] == '+') p++;
    int dot = 0;
    while (p < len && (w_isdigit((unsigned char)s[p]) || (s[p] == '.' && !dot))) { if (s[p] == '.') dot = 1; p++; }
    if (p < len && (s[p] == 'e' || s[p] == 'E') && p + 1 < len &&
        (w_isdigit((unsigned char)s[p + 1]) || s[p + 1] == '-' || s[p + 1] == '+')) {
        p++;
        if (s[p] == '-' || s[p] == '+') p++;
        while (p < len && w_isdigit((unsigned char)s[p])) p++;
    }
    int32_t m;
    int u;
    if (!cv_number(s + st, p - st, &m, &u)) return 0;
    *out16 = w_muldiv(m, 65536, 1000);
    *i = p;
    return 1;
}

static int32_t attr_num(struct wdom* d, int el, int atom, int32_t dflt, int32_t pct_basis16) {
    int vl;
    const char* v = wdom_attr(d, el, atom, &vl);
    if (!v) return dflt;
    int i = 0;
    int32_t r;
    if (!num(v, vl, &i, &r)) return dflt;
    while (i < vl && w_isspace((unsigned char)v[i])) i++;
    if (i < vl && v[i] == '%') return fmul(pct_basis16, r / 100);
    return r;
}

static struct mat parse_transform(const char* s, int len) {
    struct mat m = mat_id();
    int i = 0;
    while (i < len) {
        while (i < len && (w_isspace((unsigned char)s[i]) || s[i] == ',')) i++;
        int ns = i;
        while (i < len && w_isalpha((unsigned char)s[i])) i++;
        int nl = i - ns;
        while (i < len && s[i] != '(') i++;
        if (i >= len) break;
        i++;
        int32_t a[6];
        int na = 0;
        while (na < 6 && num(s, len, &i, &a[na])) na++;
        while (i < len && s[i] != ')') i++;
        i++;
        struct mat t = mat_id();
        const char* nm = s + ns;
        if (w_ieq(nm, nl, "matrix") && na == 6) {
            t.a = a[0]; t.b = a[1]; t.c = a[2]; t.d = a[3]; t.e = a[4]; t.f = a[5];
        } else if (w_ieq(nm, nl, "translate") && na >= 1) {
            t.e = a[0]; t.f = na > 1 ? a[1] : 0;
        } else if (w_ieq(nm, nl, "scale") && na >= 1) {
            t.a = a[0]; t.d = na > 1 ? a[1] : a[0];
        } else if (w_ieq(nm, nl, "rotate") && na >= 1) {
            int32_t deg = w_muldiv(a[0], 1000, 65536);
            int32_t sn = sin16(deg), cs = cos16(deg);
            t.a = cs; t.b = sn; t.c = -sn; t.d = cs;
            if (na >= 3) {
                struct mat t1 = mat_id(), t2 = mat_id();
                t1.e = a[1]; t1.f = a[2];
                t2.e = -a[1]; t2.f = -a[2];
                t = mat_mul(mat_mul(t1, t), t2);
            }
        } else if (w_ieq(nm, nl, "skewx") && na >= 1) {
            int32_t deg = w_muldiv(a[0], 1000, 65536);
            int32_t cs = cos16(deg);
            t.c = cs ? w_div64((int64_t)sin16(deg) * 65536, cs) : 0;
        } else if (w_ieq(nm, nl, "skewy") && na >= 1) {
            int32_t deg = w_muldiv(a[0], 1000, 65536);
            int32_t cs = cos16(deg);
            t.b = cs ? w_div64((int64_t)sin16(deg) * 65536, cs) : 0;
        }
        m = mat_mul(m, t);
    }
    return m;
}

// ---- path building (flattened polylines in device subpixels) -----------------

struct pbuf {
    int32_t* x;
    int32_t* y;
    uint8_t* start;     // 1 = this point starts a subpath
    uint8_t* closed;    // subpath closed flag at its start point
    int n, cap;
    struct mat m;       // user -> device subpixel (24.8) transform (16.16)
};

static void pb_point(struct pbuf* p, int32_t ux, int32_t uy, int start) {
    if (p->n >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        int32_t* nx = (int32_t*)w_realloc(p->x, nc * sizeof(int32_t));
        if (!nx) return;
        p->x = nx;
        int32_t* ny = (int32_t*)w_realloc(p->y, nc * sizeof(int32_t));
        if (!ny) return;
        p->y = ny;
        uint8_t* ns = (uint8_t*)w_realloc(p->start, nc);
        if (!ns) return;
        p->start = ns;
        uint8_t* ncl = (uint8_t*)w_realloc(p->closed, nc);
        if (!ncl) return;
        p->closed = ncl;
        p->cap = nc;
    }
    int64_t X = (int64_t)p->m.a * ux + (int64_t)p->m.c * uy;
    int64_t Y = (int64_t)p->m.b * ux + (int64_t)p->m.d * uy;
    p->x[p->n] = (int32_t)(X >> 24) + (p->m.e >> 8);
    p->y[p->n] = (int32_t)(Y >> 24) + (p->m.f >> 8);
    p->start[p->n] = (uint8_t)start;
    p->closed[p->n] = 0;
    p->n++;
}

static void pb_close(struct pbuf* p) {
    for (int i = p->n - 1; i >= 0; i--) if (p->start[i]) { p->closed[i] = 1; return; }
}

static void pb_quad(struct pbuf* p, int32_t x0, int32_t y0, int32_t cx, int32_t cy, int32_t x, int32_t y) {
    int n = 12;
    for (int i = 1; i <= n; i++) {
        int64_t t = i, s = n - i;
        int32_t px = w_div64(s * s * x0 + 2 * s * t * cx + t * t * x, n * n);
        int32_t py = w_div64(s * s * y0 + 2 * s * t * cy + t * t * y, n * n);
        pb_point(p, px, py, 0);
    }
}

static int32_t div_n(int64_t v, int n) { return w_div64(v, n); }

static void pb_cubic(struct pbuf* p, int32_t x0, int32_t y0, int32_t c1x, int32_t c1y, int32_t c2x, int32_t c2y,
                     int32_t x, int32_t y) {
    int n = 16;
    for (int i = 1; i <= n; i++) {
        int64_t t = i, s = n - i;
        // coordinates are 16.16; keep products in int64 (n^3 = 4096)
        int64_t px = (s * s * s) * (int64_t)x0 + 3 * s * s * t * (int64_t)c1x + 3 * s * t * t * (int64_t)c2x + t * t * t * (int64_t)x;
        int64_t py = (s * s * s) * (int64_t)y0 + 3 * s * s * t * (int64_t)c1y + 3 * s * t * t * (int64_t)c2y + t * t * t * (int64_t)y;
        pb_point(p, div_n(px, n * n * n), div_n(py, n * n * n), 0);
    }
}

static void fix_quad_division(void) {}

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

// angle (degrees*1000) of vector (x, y)
static int32_t atan2_deg(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;
    int64_t len = isqrt64((uint64_t)((int64_t)x * x + (int64_t)y * y));
    if (!len) return 0;
    int32_t sn = w_div64((int64_t)y << 16, (int32_t)len);
    int32_t cs = w_div64((int64_t)x << 16, (int32_t)len);
    // search the sin table
    int best = 0;
    int32_t bd = INT32_MAX;
    for (int d = 0; d < 360; d++) {
        int32_t e = (sin_deg[d] - sn);
        int32_t f = (sin_deg[(d + 90) % 360] - cs);
        int32_t dd = (e < 0 ? -e : e) + (f < 0 ? -f : f);
        if (dd < bd) { bd = dd; best = d; }
    }
    return best * 1000;
}

// SVG elliptical arc (endpoint parameterization) -> polyline. Solved in
// normalized space (x/rx, y/ry) where the ellipse is the unit circle, so all
// intermediate values stay small 16.16 fixed-point numbers.
static void pb_arc(struct pbuf* p, int32_t x0, int32_t y0, int32_t rx, int32_t ry, int32_t rot_deg16,
                   int large, int sweep, int32_t x, int32_t y) {
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (rx < 16 || ry < 16 || (x0 == x && y0 == y)) { pb_point(p, x, y, 0); return; }
    int32_t phi = w_muldiv(rot_deg16, 1000, 65536);
    int32_t sp = sin16(phi), cp = cos16(phi);
    int32_t dx2 = (x0 - x) / 2, dy2 = (y0 - y) / 2;
    int32_t x1p = fmul(cp, dx2) + fmul(sp, dy2);
    int32_t y1p = -fmul(sp, dx2) + fmul(cp, dy2);
    int32_t X = w_div64((int64_t)x1p << 16, rx);
    int32_t Y = w_div64((int64_t)y1p << 16, ry);
    int32_t dd = fmul(X, X) + fmul(Y, Y);
    if (dd > 65536) {
        // radii too small: scale them up so the arc just fits
        int32_t sq = (int32_t)isqrt64((uint64_t)dd << 16);
        rx = fmul(rx, sq);
        ry = fmul(ry, sq);
        X = w_div64((int64_t)X << 16, sq);
        Y = w_div64((int64_t)Y << 16, sq);
        dd = 65536;
    }
    if (dd <= 0) { pb_point(p, x, y, 0); return; }
    int32_t ratio = w_div64((int64_t)(65536 - dd) << 16, dd);
    if (ratio < 0) ratio = 0;
    int32_t coef = (int32_t)isqrt64((uint64_t)ratio << 16);
    if (large == sweep) coef = -coef;
    int32_t cxn = fmul(coef, Y), cyn = -fmul(coef, X);
    int32_t a1 = atan2_deg(Y - cyn, X - cxn);
    int32_t a2 = atan2_deg(-Y - cyn, -X - cxn);
    int32_t da = a2 - a1;
    if (sweep && da < 0) da += 360000;
    if (!sweep && da > 0) da -= 360000;
    // ellipse center in user space
    int32_t ccx = fmul(cxn, rx), ccy = fmul(cyn, ry);
    int32_t mx = (x0 + x) / 2, my = (y0 + y) / 2;
    int32_t cx = fmul(cp, ccx) - fmul(sp, ccy) + mx;
    int32_t cy = fmul(sp, ccx) + fmul(cp, ccy) + my;
    int steps = (da < 0 ? -da : da) / 10000 + 2;
    for (int i = 1; i <= steps; i++) {
        int32_t ang = a1 + w_div64((int64_t)da * i, steps);
        int32_t ex = fmul(rx, cos16(ang)), ey = fmul(ry, sin16(ang));
        int32_t px = fmul(cp, ex) - fmul(sp, ey) + cx;
        int32_t py = fmul(sp, ex) + fmul(cp, ey) + cy;
        if (i == steps) { px = x; py = y; }
        pb_point(p, px, py, 0);
    }
}

static void build_path_d(struct pbuf* p, const char* s, int len) {
    int i = 0;
    char cmd = 0;
    int32_t cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; // current, subpath start, last control
    char last = 0;
    while (i < len) {
        while (i < len && (w_isspace((unsigned char)s[i]) || s[i] == ',')) i++;
        if (i >= len) break;
        if (w_isalpha((unsigned char)s[i])) { cmd = s[i++]; }
        else if (!cmd) break;
        int rel = cmd >= 'a';
        char C = (char)(cmd & ~32);
        int32_t a[7];
        int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 :
                   C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if (C == 'Z') {
            pb_close(p);
            cx = sx; cy = sy;
            last = 'Z';
            cmd = 0;
            continue;
        }
        int k = 0;
        for (; k < need; k++) {
            if (C == 'A' && (k == 3 || k == 4)) {
                // flags may be written without separators ("a1 1 0 01.5.5")
                while (i < len && (w_isspace((unsigned char)s[i]) || s[i] == ',')) i++;
                if (i < len && (s[i] == '0' || s[i] == '1')) { a[k] = (s[i] - '0') << 16; i++; continue; }
                break;
            }
            if (!num(s, len, &i, &a[k])) break;
        }
        if (k < need) break;
        int32_t ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (C) {
        case 'M':
            cx = a[0] + ox; cy = a[1] + oy;
            sx = cx; sy = cy;
            pb_point(p, cx, cy, 1);
            cmd = rel ? 'l' : 'L';
            break;
        case 'L': cx = a[0] + ox; cy = a[1] + oy; pb_point(p, cx, cy, 0); break;
        case 'H': cx = a[0] + (rel ? cx : 0); pb_point(p, cx, cy, 0); break;
        case 'V': cy = a[0] + (rel ? cy : 0); pb_point(p, cx, cy, 0); break;
        case 'C': {
            int32_t c1x = a[0] + ox, c1y = a[1] + oy, c2x = a[2] + ox, c2y = a[3] + oy;
            int32_t ex = a[4] + ox, ey = a[5] + oy;
            pb_cubic(p, cx, cy, c1x, c1y, c2x, c2y, ex, ey);
            lcx = c2x; lcy = c2y;
            cx = ex; cy = ey;
            break;
        }
        case 'S': {
            int32_t c1x = cx, c1y = cy;
            if (last == 'C' || last == 'S') { c1x = 2 * cx - lcx; c1y = 2 * cy - lcy; }
            int32_t c2x = a[0] + ox, c2y = a[1] + oy, ex = a[2] + ox, ey = a[3] + oy;
            pb_cubic(p, cx, cy, c1x, c1y, c2x, c2y, ex, ey);
            lcx = c2x; lcy = c2y;
            cx = ex; cy = ey;
            break;
        }
        case 'Q': {
            int32_t qx = a[0] + ox, qy = a[1] + oy, ex = a[2] + ox, ey = a[3] + oy;
            pb_quad(p, cx, cy, qx, qy, ex, ey);
            lcx = qx; lcy = qy;
            cx = ex; cy = ey;
            break;
        }
        case 'T': {
            int32_t qx = cx, qy = cy;
            if (last == 'Q' || last == 'T') { qx = 2 * cx - lcx; qy = 2 * cy - lcy; }
            int32_t ex = a[0] + ox, ey = a[1] + oy;
            pb_quad(p, cx, cy, qx, qy, ex, ey);
            lcx = qx; lcy = qy;
            cx = ex; cy = ey;
            break;
        }
        case 'A': {
            int32_t ex = a[5] + ox, ey = a[6] + oy;
            pb_arc(p, cx, cy, a[0], a[1], a[2], a[3] != 0, a[4] != 0, ex, ey);
            cx = ex; cy = ey;
            break;
        }
        default:
            i = len;
            break;
        }
        last = C;
    }
}

static void build_ellipse(struct pbuf* p, int32_t cx, int32_t cy, int32_t rx, int32_t ry) {
    int n = 48;
    for (int i = 0; i <= n; i++) {
        int32_t a = i * 360000 / n;
        pb_point(p, cx + fmul(rx, cos16(a)), cy + fmul(ry, sin16(a)), i == 0);
    }
    pb_close(p);
}

static void build_rect(struct pbuf* p, int32_t x, int32_t y, int32_t w, int32_t h, int32_t rx, int32_t ry) {
    if (rx > w / 2) rx = w / 2;
    if (ry > h / 2) ry = h / 2;
    if (rx <= 0 || ry <= 0) {
        pb_point(p, x, y, 1);
        pb_point(p, x + w, y, 0);
        pb_point(p, x + w, y + h, 0);
        pb_point(p, x, y + h, 0);
        pb_close(p);
        return;
    }
    // corners as quarter ellipses
    const int n = 8;
    pb_point(p, x + rx, y, 1);
    pb_point(p, x + w - rx, y, 0);
    for (int i = 1; i <= n; i++) { int32_t a = 270000 + i * 90000 / n; pb_point(p, x + w - rx + fmul(rx, cos16(a)), y + ry + fmul(ry, sin16(a)), 0); }
    pb_point(p, x + w, y + h - ry, 0);
    for (int i = 1; i <= n; i++) { int32_t a = i * 90000 / n; pb_point(p, x + w - rx + fmul(rx, cos16(a)), y + h - ry + fmul(ry, sin16(a)), 0); }
    pb_point(p, x + rx, y + h, 0);
    for (int i = 1; i <= n; i++) { int32_t a = 90000 + i * 90000 / n; pb_point(p, x + rx + fmul(rx, cos16(a)), y + h - ry + fmul(ry, sin16(a)), 0); }
    pb_point(p, x, y + ry, 0);
    for (int i = 1; i <= n; i++) { int32_t a = 180000 + i * 90000 / n; pb_point(p, x + rx + fmul(rx, cos16(a)), y + ry + fmul(ry, sin16(a)), 0); }
    pb_close(p);
}

static void build_points(struct pbuf* p, const char* s, int len, int close) {
    int i = 0, first = 1;
    int32_t x, y;
    while (num(s, len, &i, &x) && num(s, len, &i, &y)) { pb_point(p, x, y, first); first = 0; }
    if (close) pb_close(p);
}

// ---- fill / stroke -------------------------------------------------------------

struct sfill { struct wsurf* s; uint32_t color; };
static void span_blend(void* ctx, int y, int x, int len, const uint8_t* cov) {
    struct sfill* f = (struct sfill*)ctx;
    struct wsurf* s = f->s;
    if (y < s->cy0 || y >= s->cy1) return;
    unsigned ca = f->color >> 24;
    uint32_t* row = s->px + y * s->stride;
    for (int i = 0; i < len; i++) {
        int xx = x + i;
        if (xx < s->cx0 || xx >= s->cx1 || !cov[i]) continue;
        row[xx] = ws_blend(row[xx], f->color, (cov[i] * ca + 127) / 255);
    }
}

static struct wraster SR;
static int sr_ready;

static void fill_pbuf(struct wsurf* s, const struct pbuf* p, uint32_t color, int evenodd) {
    if (!sr_ready) { wr_init(&SR); sr_ready = 1; }
    wr_reset(&SR);
    for (int i = 0; i < p->n; i++) {
        if (p->start[i]) wr_move(&SR, p->x[i], p->y[i]);
        else wr_line(&SR, p->x[i], p->y[i]);
    }
    struct sfill f = { s, color };
    wr_fill(&SR, s->cx0, s->cy0, s->cx1, s->cy1, evenodd, span_blend, &f);
}

static void quad_seg(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t hw) {
    int32_t dx = x1 - x0, dy = y1 - y0;
    uint32_t len = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
    if (!len) return;
    if (len > 0x7FFFFFFF) return;
    int32_t nx = w_div64((int64_t)-dy * hw, (int32_t)len), ny = w_div64((int64_t)dx * hw, (int32_t)len);
    // consistent winding for nonzero union
    wr_move(&SR, x0 + nx, y0 + ny);
    wr_line(&SR, x1 + nx, y1 + ny);
    wr_line(&SR, x1 - nx, y1 - ny);
    wr_line(&SR, x0 - nx, y0 - ny);
    wr_close(&SR);
}

static void stroke_pbuf(struct wsurf* s, const struct pbuf* p, uint32_t color, int32_t width_sub) {
    if (!sr_ready) { wr_init(&SR); sr_ready = 1; }
    wr_reset(&SR);
    int32_t hw = width_sub / 2;
    if (hw < 64) hw = 64; // >= 0.25px half width (keeps hairlines visible)
    int start = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->start[i]) start = i;
        if (i + 1 < p->n && !p->start[i + 1]) quad_seg(p->x[i], p->y[i], p->x[i + 1], p->y[i + 1], hw);
        int end_of_sub = (i + 1 >= p->n) || p->start[i + 1];
        if (end_of_sub && p->closed[start]) quad_seg(p->x[i], p->y[i], p->x[start], p->y[start], hw);
        // round joins
        if (hw >= 192) wr_ellipse(&SR, p->x[i], p->y[i], hw, hw);
    }
    struct sfill f = { s, color };
    wr_fill(&SR, s->cx0, s->cy0, s->cx1, s->cy1, 0, span_blend, &f);
}

static uint32_t paint_of(const struct wstyle* st, int stroke, struct wdom* d, int el, int* none) {
    uint32_t c = stroke ? st->stroke : st->fill;
    *none = stroke ? st->stroke_none : st->fill_none;
    if (*none && !stroke) {
        // fill="url(#gradient)": use the gradient's first stop color
        int vl;
        const char* v = wdom_attr(d, el, A_fill, &vl);
        if (v && vl > 5 && w_ieq_prefix(v, vl, "url(#")) {
            char id[64];
            int n = 0;
            for (int i = 5; i < vl && v[i] != ')' && n < 63; i++) id[n++] = v[i];
            id[n] = 0;
            int g = wdom_find_id(d, id);
            if (g >= 0) {
                for (int c2 = d->n[g].first; c2 >= 0; c2 = d->n[c2].next) {
                    if (d->n[c2].type != WN_ELEM || d->n[c2].tag != T_stop) continue;
                    char col[64];
                    if (wdom_attr_copy(d, c2, A_stop_color, col, sizeof col) > 0) {
                        uint32_t cc;
                        if (css_parse_color(col, (int)strlen(col), &cc)) { *none = 0; return cc; }
                    }
                    int sl;
                    const char* sv = wdom_attr(d, c2, A_style, &sl);
                    if (sv) {
                        for (int k = 0; k + 11 < sl; k++)
                            if (w_ieq_prefix(sv + k, sl - k, "stop-color:")) {
                                int e = k + 11;
                                while (e < sl && sv[e] != ';') e++;
                                uint32_t cc;
                                if (css_parse_color(sv + k + 11, e - k - 11, &cc)) { *none = 0; return cc; }
                            }
                    }
                    break;
                }
            }
        }
    }
    return c;
}

static int32_t opacity_attr(struct wdom* d, int el, int atom) {
    int vl;
    const char* v = wdom_attr(d, el, atom, &vl);
    if (!v) return 255;
    int32_t m; int u;
    if (!cv_number(v, vl, &m, &u)) return 255;
    if (u < vl && v[u] == '%') m /= 100;
    return W_CLAMP((m * 255 + 500) / 1000, 0, 255);
}

struct sctx {
    struct wsurf* s;
    struct wdom* d;
    const struct wstyleset* ss;
    uint32_t current;
    uint8_t alpha;
    int depth;
};

static void render_el(struct sctx* c, int el, struct mat m, int in_use);

static void render_children(struct sctx* c, int el, struct mat m) {
    for (int k = c->d->n[el].first; k >= 0; k = c->d->n[k].next)
        if (c->d->n[k].type == WN_ELEM) render_el(c, k, m, 0);
}

static void render_el(struct sctx* c, int el, struct mat m, int in_use) {
    struct wdom* d = c->d;
    if (++c->depth > 64) { c->depth--; return; }
    const struct wnode* n = &d->n[el];
    const struct wstyle* st = css_style_of(c->ss, el);
    if (st && (st->display == D_NONE || st->visibility)) { c->depth--; return; }
    int t = n->tag;
    if (t == T_defs || t == T_title || t == T_desc || t == T_mask || t == T_clippath ||
        t == T_lineargradient || t == T_radialgradient || t == T_stop || (t == T_symbol && !in_use)) {
        c->depth--;
        return;
    }
    int tl;
    const char* tv = wdom_attr(d, el, A_transform, &tl);
    if (tv) m = mat_mul(m, parse_transform(tv, tl));
    int32_t op = opacity_attr(d, el, A_opacity);
    if (st && st->opacity < 255) op = op * st->opacity / 255;
    uint8_t saved = c->alpha;
    c->alpha = (uint8_t)(c->alpha * op / 255);
    if (t == T_g || t == T_svg || t == T_symbol || t == T_a) {
        if ((t == T_svg && in_use) || t == T_symbol) {
            // nested viewport: viewBox mapping relative to its width/height
        }
        render_children(c, el, m);
        c->alpha = saved;
        c->depth--;
        return;
    }
    if (t == T_use) {
        int hl;
        const char* h = wdom_attr(d, el, A_href, &hl);
        if (!h) h = wdom_attr_s(d, el, "xlink:href", &hl);
        if (h && hl > 1 && h[0] == '#') {
            char id[96];
            int k = hl - 1 < 95 ? hl - 1 : 95;
            memcpy(id, h + 1, k);
            id[k] = 0;
            int target = wdom_find_id(d, id);
            if (target >= 0 && target != el) {
                struct mat tm = mat_id();
                tm.e = attr_num(d, el, A_x, 0, 0);
                tm.f = attr_num(d, el, A_y, 0, 0);
                struct mat um = mat_mul(m, tm);
                if (d->n[target].tag == T_symbol) {
                    // symbol viewBox -> use width/height (default 100%)
                    int vl;
                    const char* vb = wdom_attr(d, target, A_viewbox, &vl);
                    int32_t v4[4];
                    int vi = 0, k2 = 0;
                    while (vb && k2 < 4 && num(vb, vl, &vi, &v4[k2])) k2++;
                    if (vb && k2 == 4 && v4[2] > 0 && v4[3] > 0) {
                        int32_t uw = attr_num(d, el, A_width, v4[2], 0), uh = attr_num(d, el, A_height, v4[3], 0);
                        int32_t sx = w_div64((int64_t)uw * 65536, v4[2]), sy = w_div64((int64_t)uh * 65536, v4[3]);
                        int32_t sc = sx < sy ? sx : sy;
                        struct mat vm = mat_id();
                        vm.a = sc; vm.d = sc;
                        vm.e = -fmul(v4[0], sc) + (uw - fmul(v4[2], sc)) / 2;
                        vm.f = -fmul(v4[1], sc) + (uh - fmul(v4[3], sc)) / 2;
                        um = mat_mul(um, vm);
                    }
                }
                render_el(c, target, um, 1);
            }
        }
        c->alpha = saved;
        c->depth--;
        return;
    }
    struct pbuf p;
    memset(&p, 0, sizeof p);
    // user units (16.16) -> device subpixels: matrix maps to px*65536; the
    // pbuf divides by 2^24 to get 24.8, so prescale the matrix by 2^8
    p.m = m;
    p.m.a = m.a; // (a*x) is 16.16*16.16 -> >>24 gives (px * 256): exactly 24.8
    p.m.e = m.e;
    switch (t) {
    case T_path: {
        int dl;
        const char* dv = wdom_attr(d, el, A_d, &dl);
        if (dv) build_path_d(&p, dv, dl);
        break;
    }
    case T_rect: {
        int32_t x = attr_num(d, el, A_x, 0, 0), y = attr_num(d, el, A_y, 0, 0);
        int32_t w = attr_num(d, el, A_width, 0, 0), h = attr_num(d, el, A_height, 0, 0);
        int32_t rx = attr_num(d, el, A_rx, -1, 0), ry = attr_num(d, el, A_ry, -1, 0);
        if (rx < 0) rx = ry > 0 ? ry : 0;
        if (ry < 0) ry = rx;
        if (w > 0 && h > 0) build_rect(&p, x, y, w, h, rx, ry);
        break;
    }
    case T_circle: {
        int32_t r = attr_num(d, el, A_r, 0, 0);
        if (r > 0) build_ellipse(&p, attr_num(d, el, A_cx, 0, 0), attr_num(d, el, A_cy, 0, 0), r, r);
        break;
    }
    case T_ellipse: {
        int32_t rx = attr_num(d, el, A_rx, 0, 0), ry = attr_num(d, el, A_ry, 0, 0);
        if (rx > 0 && ry > 0) build_ellipse(&p, attr_num(d, el, A_cx, 0, 0), attr_num(d, el, A_cy, 0, 0), rx, ry);
        break;
    }
    case T_line:
        pb_point(&p, attr_num(d, el, A_x1, 0, 0), attr_num(d, el, A_y1, 0, 0), 1);
        pb_point(&p, attr_num(d, el, A_x2, 0, 0), attr_num(d, el, A_y2, 0, 0), 0);
        break;
    case T_polyline: case T_polygon: {
        int pl;
        const char* pv = wdom_attr(d, el, A_points, &pl);
        if (pv) build_points(&p, pv, pl, t == T_polygon);
        break;
    }
    default:
        break;
    }
    if (p.n > 0 && st) {
        int none;
        uint32_t fc = paint_of(st, 0, d, el, &none);
        int evenodd = 0;
        int fl;
        const char* fr = wdom_attr(d, el, A_fill_rule, &fl);
        if (fr && w_ieq(fr, fl, "evenodd")) evenodd = 1;
        if (!none && t != T_line && t != T_polyline) {
            uint32_t a = (fc >> 24) * c->alpha / 255 * opacity_attr(d, el, A_fill_opacity) / 255;
            if (a) fill_pbuf(c->s, &p, (a << 24) | (fc & 0xFFFFFF), evenodd);
        } else if (!none && t == T_polyline) {
            uint32_t a = (fc >> 24) * c->alpha / 255;
            if (a) fill_pbuf(c->s, &p, (a << 24) | (fc & 0xFFFFFF), evenodd);
        }
        uint32_t sc = paint_of(st, 1, d, el, &none);
        if (!none) {
            // stroke width in user units -> device (scale by the matrix's x scale)
            int32_t sw_user = w_muldiv(st->stroke_width, 65536, 64); // LU -> 16.16 user units
            int64_t scale = (int64_t)(m.a < 0 ? -m.a : m.a) + (m.c < 0 ? -m.c : m.c);
            int32_t sw_sub = (int32_t)(((int64_t)sw_user * scale) >> 24);
            uint32_t a = (sc >> 24) * c->alpha / 255 * opacity_attr(d, el, A_stroke_opacity) / 255;
            if (a) stroke_pbuf(c->s, &p, (a << 24) | (sc & 0xFFFFFF), sw_sub);
        }
    }
    w_free(p.x); w_free(p.y); w_free(p.start); w_free(p.closed);
    c->alpha = saved;
    c->depth--;
}

void wsvg_paint(struct wsurf* s, struct wdom* d, const struct wstyleset* ss, int node,
                int x, int y, int w, int h, uint32_t current_color, uint8_t alpha) {
    (void)fix_quad_division;
    if (node < 0 || w <= 0 || h <= 0) return;
    // viewBox -> viewport (xMidYMid meet)
    struct mat m = mat_id();
    m.e = x << 16;
    m.f = y << 16;
    int vl;
    const char* vb = wdom_attr(d, node, A_viewbox, &vl);
    int32_t v4[4];
    int vi = 0, k = 0;
    while (vb && k < 4 && num(vb, vl, &vi, &v4[k])) k++;
    if (vb && k == 4 && v4[2] > 0 && v4[3] > 0) {
        // scale = viewport px / viewBox user units, in 16.16
        int32_t sx = w_div64((int64_t)w * 65536 * 65536, v4[2]);
        int32_t sy = w_div64((int64_t)h * 65536 * 65536, v4[3]);
        int32_t sc = sx < sy ? sx : sy;
        int pl;
        const char* par = wdom_attr(d, node, A_preserveaspectratio, &pl);
        if (par && w_ieq_prefix(par, pl, "none")) { m.a = sx; m.d = sy; }
        else { m.a = sc; m.d = sc; }
        m.e += -fmul(v4[0], m.a) + ((w << 16) - fmul(v4[2], m.a)) / 2;
        m.f += -fmul(v4[1], m.d) + ((h << 16) - fmul(v4[3], m.d)) / 2;
    }
    int old[4];
    ws_push_clip(s, x, y, x + w, y + h, old);
    struct sctx c;
    c.s = s;
    c.d = d;
    c.ss = ss;
    c.current = current_color;
    c.alpha = alpha;
    c.depth = 0;
    render_children(&c, node, m);
    ws_pop_clip(s, old);
}

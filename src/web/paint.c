// Display list -> pixels.
#include "paint.h"
#include "raster.h"
#include "font.h"
#include "wcommon.h"
#include "colortab.h"

static struct wraster PR;
static int pr_ready;

struct fillctx { struct wsurf* s; uint32_t color; };

static void span_fill(void* ctx, int y, int x, int len, const uint8_t* cov) {
    struct fillctx* f = (struct fillctx*)ctx;
    struct wsurf* s = f->s;
    if (y < s->cy0 || y >= s->cy1) return;
    unsigned ca = f->color >> 24;
    uint32_t* row = s->px + y * s->stride;
    for (int i = 0; i < len; i++) {
        int xx = x + i;
        if (xx < s->cx0 || xx >= s->cx1) continue;
        unsigned a = cov[i];
        if (!a) continue;
        a = (a * ca + 127) / 255;
        row[xx] = ws_blend(row[xx], f->color, a);
    }
}

static void pr_init(void) { if (!pr_ready) { wr_init(&PR); pr_ready = 1; } }

// AA rounded rect (px coordinates in 1/256 subpixel)
static void fill_rrect_sub(struct wsurf* s, int32_t x, int32_t y, int32_t w, int32_t h, const int32_t rad[4],
                           uint32_t color) {
    if ((color >> 24) == 0 || w <= 0 || h <= 0) return;
    pr_init();
    wr_reset(&PR);
    wr_rrect(&PR, x, y, w, h, rad);
    struct fillctx f = { s, color };
    wr_fill(&PR, s->cx0, s->cy0, s->cx1, s->cy1, 0, span_fill, &f);
}

// LU -> subpixel (1/256)
#define SUB(v) ((v) * 4)

static uint32_t apply_alpha(uint32_t c, uint8_t a) {
    if (a >= 255) return c;
    uint32_t ca = c >> 24;
    return (((ca * a + 127) / 255) << 24) | (c & 0xFFFFFF);
}

static uint32_t shade(uint32_t c, int pct) {
    // pct < 100 darker, > 100 lighter
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (pct < 100) { r = r * pct / 100; g = g * pct / 100; b = b * pct / 100; }
    else {
        r = r + (255 - r) * (pct - 100) / 100;
        g = g + (255 - g) * (pct - 100) / 100;
        b = b + (255 - b) * (pct - 100) / 100;
    }
    return (c & 0xFF000000) | (r << 16) | (g << 8) | b;
}

static void side_rect(struct wsurf* s, int x, int y, int w, int h, uint32_t c, int style, int horiz, int bw) {
    if (w <= 0 || h <= 0) return;
    if (style == BS_DASHED || style == BS_DOTTED) {
        int seg = style == BS_DOTTED ? (bw > 0 ? bw : 1) : (bw > 0 ? bw * 3 : 3);
        int len = horiz ? w : h;
        for (int o = 0; o < len; o += seg * 2) {
            int l = seg < len - o ? seg : len - o;
            if (horiz) ws_fill_rect(s, x + o, y, l, h, c);
            else ws_fill_rect(s, x, y + o, w, l, c);
        }
        return;
    }
    if (style == BS_DOUBLE && bw >= 3) {
        int t = bw / 3;
        if (horiz) { ws_fill_rect(s, x, y, w, t, c); ws_fill_rect(s, x, y + h - t, w, t, c); }
        else { ws_fill_rect(s, x, y, t, h, c); ws_fill_rect(s, x + w - t, y, t, h, c); }
        return;
    }
    ws_fill_rect(s, x, y, w, h, c);
}

static void paint_border(struct wsurf* s, int x, int y, int w, int h, const struct dborder* b, uint8_t alpha,
                         int32_t sub_x, int32_t sub_y, int32_t sub_w, int32_t sub_h) {
    int bt = LU_ROUND(b->w[0]), br = LU_ROUND(b->w[1]), bb = LU_ROUND(b->w[2]), bl = LU_ROUND(b->w[3]);
    if (b->w[0] > 0 && bt == 0) bt = 1;
    if (b->w[1] > 0 && br == 0) br = 1;
    if (b->w[2] > 0 && bb == 0) bb = 1;
    if (b->w[3] > 0 && bl == 0) bl = 1;
    uint32_t c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = apply_alpha(b->c[i], alpha);
        int st = b->s[i];
        if (st == BS_INSET) c[i] = (i == 0 || i == 3) ? shade(c[i], 60) : shade(c[i], 140);
        else if (st == BS_OUTSET) c[i] = (i == 0 || i == 3) ? shade(c[i], 140) : shade(c[i], 60);
        else if (st == BS_GROOVE) c[i] = (i == 0 || i == 3) ? shade(c[i], 70) : shade(c[i], 130);
        else if (st == BS_RIDGE) c[i] = (i == 0 || i == 3) ? shade(c[i], 130) : shade(c[i], 70);
    }
    int has_r = b->r[0] | b->r[1] | b->r[2] | b->r[3];
    if (has_r) {
        // ring = outer rrect minus inner rrect (even-odd), in the top color
        pr_init();
        wr_reset(&PR);
        int32_t ro[4] = { SUB(b->r[0]), SUB(b->r[1]), SUB(b->r[2]), SUB(b->r[3]) };
        wr_rrect(&PR, sub_x, sub_y, sub_w, sub_h, ro);
        int32_t ix = sub_x + SUB(b->w[3]), iy = sub_y + SUB(b->w[0]);
        int32_t iw = sub_w - SUB(b->w[1] + b->w[3]), ih = sub_h - SUB(b->w[0] + b->w[2]);
        if (iw > 0 && ih > 0) {
            int32_t ri[4];
            for (int i = 0; i < 4; i++) {
                int32_t bwv = SUB((i == 0 || i == 1) ? b->w[0] : b->w[2]);
                ri[i] = ro[i] - bwv;
                if (ri[i] < 0) ri[i] = 0;
            }
            wr_rrect(&PR, ix, iy, iw, ih, ri);
        }
        uint32_t col = c[0];
        if (!b->w[0]) col = c[3] ? c[3] : c[2];
        struct fillctx f = { s, col };
        wr_fill(&PR, s->cx0, s->cy0, s->cx1, s->cy1, 1, span_fill, &f);
        return;
    }
    if (bt) side_rect(s, x, y, w, bt, c[0], b->s[0], 1, bt);
    if (bb) side_rect(s, x, y + h - bb, w, bb, c[2], b->s[2], 1, bb);
    if (bl) side_rect(s, x, y + bt, bl, h - bt - bb, c[3], b->s[3], 0, bl);
    if (br) side_rect(s, x + w - br, y + bt, br, h - bt - bb, c[1], b->s[1], 0, br);
}

// ---- images ---------------------------------------------------------------------------

void wpaint_image(struct wsurf* s, const struct wimg* im, int dx, int dy, int dw, int dh, uint8_t alpha) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0 || dw <= 0 || dh <= 0) return;
    int x0 = W_MAX(dx, s->cx0), y0 = W_MAX(dy, s->cy0);
    int x1 = W_MIN(dx + dw, s->cx1), y1 = W_MIN(dy + dh, s->cy1);
    if (x0 >= x1 || y0 >= y1) return;
    int sw = im->w, sh = im->h;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = s->px + y * s->stride;
        // source rows covered by this destination row (16.16)
        int32_t sy0 = w_div64((int64_t)(y - dy) * sh * 65536, dh);
        int32_t sy1 = w_div64((int64_t)(y - dy + 1) * sh * 65536, dh);
        int ry0 = (int)(sy0 >> 16), ry1 = (int)((sy1 + 65535) >> 16);
        if (ry1 <= ry0) ry1 = ry0 + 1;
        if (ry1 > sh) ry1 = sh;
        if (ry0 >= sh) ry0 = sh - 1;
        int down_y = ry1 - ry0 > 1;
        for (int x = x0; x < x1; x++) {
            int32_t sx0 = w_div64((int64_t)(x - dx) * sw * 65536, dw);
            int32_t sx1 = w_div64((int64_t)(x - dx + 1) * sw * 65536, dw);
            int rx0 = (int)(sx0 >> 16), rx1 = (int)((sx1 + 65535) >> 16);
            if (rx1 <= rx0) rx1 = rx0 + 1;
            if (rx1 > sw) rx1 = sw;
            if (rx0 >= sw) rx0 = sw - 1;
            uint32_t pxv;
            if (down_y || rx1 - rx0 > 1) {
                // box filter over the covered source pixels (premultiplied sum)
                uint32_t sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
                int stepx = (rx1 - rx0 > 8) ? (rx1 - rx0) / 8 : 1;
                int stepy = (ry1 - ry0 > 8) ? (ry1 - ry0) / 8 : 1;
                for (int yy = ry0; yy < ry1; yy += stepy) {
                    const uint32_t* sr_ = im->px + yy * sw;
                    for (int xx = rx0; xx < rx1; xx += stepx) {
                        uint32_t p = sr_[xx];
                        uint32_t a = p >> 24;
                        sa += a;
                        sr += ((p >> 16) & 255) * a;
                        sg += ((p >> 8) & 255) * a;
                        sb += (p & 255) * a;
                        n++;
                    }
                }
                if (!n || !sa) continue;
                pxv = ((sa / n) << 24) | ((sr / sa) << 16) | ((sg / sa) << 8) | (sb / sa);
            } else {
                pxv = im->px[ry0 * sw + rx0];
            }
            unsigned a = pxv >> 24;
            if (alpha < 255) a = (a * alpha + 127) / 255;
            if (a) row[x] = ws_blend(row[x], pxv, a);
        }
    }
}

// ---- gradients --------------------------------------------------------------------------

static uint32_t grad_color(const struct wgrad* g, const int32_t* pos, int32_t t) {
    // t in 0..65536
    int n = g->nstops;
    if (n == 1) return g->color[0];
    if (t <= pos[0]) return g->color[0];
    for (int i = 1; i < n; i++) {
        if (t <= pos[i]) {
            int32_t span = pos[i] - pos[i - 1];
            int32_t f = span > 0 ? w_muldiv(t - pos[i - 1], 256, span) : 256;
            uint32_t a = g->color[i - 1], b = g->color[i];
            uint32_t out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                int32_t ca = (a >> sh) & 255, cb = (b >> sh) & 255;
                out |= (uint32_t)((ca * (256 - f) + cb * f) >> 8) << sh;
            }
            return out;
        }
    }
    return g->color[n - 1];
}

static void paint_gradient(struct wsurf* s, const struct wgrad* g, int x, int y, int w, int h, uint8_t alpha) {
    if (!g || g->nstops < 1 || w <= 0 || h <= 0) return;
    int32_t pos[CSS_MAX_STOPS];
    // resolve stop positions to 0..65536 along the gradient line
    int32_t deg = (g->angle / 1000) % 360;
    if (deg < 0) deg += 360;
    int32_t sn = sin_deg[deg], cs = sin_deg[(deg + 90) % 360];
    // gradient line length (px * 65536)
    // gradient line length in px * 65536 (fits int32 for boxes < 16000px)
    int32_t ww = w > 16000 ? 16000 : w, hh = h > 16000 ? 16000 : h;
    int32_t glen = (int32_t)(((int64_t)ww * (sn < 0 ? -sn : sn) + (int64_t)hh * (cs < 0 ? -cs : cs)) >> 2) ;
    // (>> 2 keeps headroom: glen is px * 16384)
    int32_t rad_half = (ww > hh ? ww : hh) * 7 / 10 * 2; // radial radius in half-pixels
    if (rad_half <= 0) rad_half = 1;
    if (glen <= 0) glen = 16384;
    for (int i = 0; i < g->nstops; i++) {
        int32_t p = g->pos[i];
        if (p == -1) pos[i] = -1;
        else if (p >= 0) pos[i] = w_muldiv(p, 65536, 10000);
        else {
            int32_t lu = -(p + 2);
            // lu/64 px along a line of glen/16384 px -> * 65536
            pos[i] = w_div64((int64_t)lu * 16384 * 1024, glen);
        }
    }
    if (pos[0] < 0) pos[0] = 0;
    if (pos[g->nstops - 1] < 0) pos[g->nstops - 1] = 65536;
    for (int i = 1; i < g->nstops; i++) {
        if (pos[i] >= 0) { if (pos[i] < pos[i - 1]) pos[i] = pos[i - 1]; continue; }
        int j = i;
        while (j < g->nstops && pos[j] < 0) j++;
        int32_t a = pos[i - 1], b = pos[j];
        for (int k = i; k < j; k++) pos[k] = a + (b - a) * (k - i + 1) / (j - i + 1);
        i = j - 1;
    }
    int x0 = W_MAX(x, s->cx0), y0 = W_MAX(y, s->cy0);
    int x1 = W_MIN(x + w, s->cx1), y1 = W_MIN(y + h, s->cy1);
    if (x0 >= x1 || y0 >= y1) return;
    int64_t cxp = (int64_t)x * 2 + w, cyp = (int64_t)y * 2 + h; // center * 2
    int vertical = !g->radial && (deg == 0 || deg == 180);
    int horizontal = !g->radial && (deg == 90 || deg == 270);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = s->px + yy * s->stride;
        uint32_t rowc = 0;
        if (vertical) {
            int32_t t = w_div64(((int64_t)(yy * 2 + 1) - cyp) * 32768 * (deg == 180 ? 1 : -1), h > 0 ? h : 1) + 32768;
            rowc = grad_color(g, pos, W_CLAMP(t, 0, 65536));
            uint32_t c = apply_alpha(rowc, alpha);
            unsigned a = c >> 24;
            for (int xx = x0; xx < x1; xx++) row[xx] = ws_blend(row[xx], c, a);
            continue;
        }
        for (int xx = x0; xx < x1; xx++) {
            int32_t t;
            int64_t dx = (int64_t)(xx * 2 + 1) - cxp, dy = (int64_t)(yy * 2 + 1) - cyp;
            if (g->radial) {
                // distance from center / radius
                int64_t d2 = dx * dx + dy * dy;
                uint32_t r = 0, bit = 1u << 30;
                uint64_t v = (uint64_t)d2;
                if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
                uint32_t vv = (uint32_t)v;
                while (bit > vv) bit >>= 2;
                while (bit) { if (vv >= r + bit) { vv -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
                t = w_div64((int64_t)r * 65536, rad_half);
            } else if (horizontal) {
                t = w_div64(dx * 32768 * (deg == 90 ? 1 : -1), w > 0 ? w : 1) + 32768;
            } else {
                int64_t proj = dx * sn - dy * cs; // half-pixels * 65536 (sin scale)
                // t = proj / (2 * len_px) with len_px = glen / 16384
                t = w_div64(proj * 8192, glen) + 32768;
            }
            uint32_t c = apply_alpha(grad_color(g, pos, W_CLAMP(t, 0, 65536)), alpha);
            row[xx] = ws_blend(row[xx], c, c >> 24);
        }
    }
}

// ---- shadows ------------------------------------------------------------------------

static void paint_shadow(struct wsurf* s, int32_t x, int32_t y, int32_t w, int32_t h, const struct dshadow* d,
                         uint32_t color) {
    int32_t blur = LU_ROUND(d->blur), spread = d->spread;
    int32_t bx = x + d->ox - spread, by = y + d->oy - spread;
    int32_t bw = w + spread * 2, bh = h + spread * 2;
    int layers = blur > 0 ? (blur < 6 ? blur : 6) : 1;
    uint32_t a0 = color >> 24;
    for (int i = 0; i < layers; i++) {
        // expansion from +blur/2 (faint) down to -blur/2 (dense)
        int32_t e = blur > 0 ? PX(blur) / 2 - PX(blur) * i / layers : 0;
        uint32_t a = blur > 0 ? a0 / layers + 1 : a0;
        if (a > 255) a = 255;
        int32_t r[4];
        for (int k = 0; k < 4; k++) { r[k] = SUB(d->r[k] + (e > 0 ? e : 0)); }
        fill_rrect_sub(s, SUB(bx - e), SUB(by - e), SUB(bw + 2 * e), SUB(bh + 2 * e), r, (a << 24) | (color & 0xFFFFFF));
    }
}

// ---- widgets -------------------------------------------------------------------------

static void paint_widget(struct wsurf* s, int x, int y, int w, int h, int kind, int state, uint32_t color) {
    if (kind == 1 && state) { // checkbox checked
        ws_fill_rect(s, x, y, w, h, color);
        pr_init();
        wr_reset(&PR);
        int32_t X = WR_FIX(x), Y = WR_FIX(y), W = WR_FIX(w), H = WR_FIX(h);
        wr_move(&PR, X + W * 20 / 100, Y + H * 50 / 100);
        wr_line(&PR, X + W * 42 / 100, Y + H * 72 / 100);
        wr_line(&PR, X + W * 80 / 100, Y + H * 28 / 100);
        wr_line(&PR, X + W * 88 / 100, Y + H * 36 / 100);
        wr_line(&PR, X + W * 42 / 100, Y + H * 88 / 100);
        wr_line(&PR, X + W * 12 / 100, Y + H * 58 / 100);
        wr_close(&PR);
        struct fillctx f = { s, 0xFFFFFFFF };
        wr_fill(&PR, s->cx0, s->cy0, s->cx1, s->cy1, 0, span_fill, &f);
    } else if (kind == 2 && state) { // radio dot
        pr_init();
        wr_reset(&PR);
        wr_ellipse(&PR, WR_FIX(x) + WR_FIX(w) / 2, WR_FIX(y) + WR_FIX(h) / 2, WR_FIX(w) / 4, WR_FIX(h) / 4);
        struct fillctx f = { s, color };
        wr_fill(&PR, s->cx0, s->cy0, s->cx1, s->cy1, 0, span_fill, &f);
    } else if (kind == 3) { // select arrow
        pr_init();
        wr_reset(&PR);
        int32_t cx = WR_FIX(x) + WR_FIX(w) / 2, cy = WR_FIX(y) + WR_FIX(h) / 2;
        wr_move(&PR, cx - WR_FIX(4), cy - WR_FIX(2));
        wr_line(&PR, cx + WR_FIX(4), cy - WR_FIX(2));
        wr_line(&PR, cx, cy + WR_FIX(3));
        wr_close(&PR);
        struct fillctx f = { s, color };
        wr_fill(&PR, s->cx0, s->cy0, s->cx1, s->cy1, 0, span_fill, &f);
    } else if (kind == 4) { // meter / progress
        int32_t r[4] = { WR_FIX(h) / 2, WR_FIX(h) / 2, WR_FIX(h) / 2, WR_FIX(h) / 2 };
        fill_rrect_sub(s, WR_FIX(x), WR_FIX(y), WR_FIX(w), WR_FIX(h), r, 0xFFE6E6E6);
        int fw = w * state / 1000;
        if (fw > 0) fill_rrect_sub(s, WR_FIX(x), WR_FIX(y), WR_FIX(fw), WR_FIX(h), r, color);
    }
}

// ---- main ---------------------------------------------------------------------------------

void wpaint(const struct wlayout* L, struct wsurf* s, int scroll_y, const struct wpaint_env* env) {
    uint32_t bg = wlay_canvas_bg(L);
    int old[4];
    ws_push_clip(s, 0, 0, s->w, s->h, old);
    ws_fill_rect(s, s->cx0, s->cy0, s->cx1 - s->cx0, s->cy1 - s->cy0, bg | 0xFF000000);
    const struct ditem* it;
    int n = wlay_items(L, &it);
    const char* text = wlay_text(L);
    int clips[64][4];
    int nclip = 0;
    int32_t scroll_lu = PX(scroll_y);
    for (int i = 0; i < n; i++) {
        const struct ditem* d = &it[i];
        int32_t oy = d->fixed ? 0 : scroll_lu;
        if (d->kind == DI_UNCLIP) {
            if (nclip > 0) { nclip--; ws_pop_clip(s, clips[nclip]); }
            continue;
        }
        int x = LU_ROUND(d->x), y = LU_ROUND(d->y - oy);
        int w = LU_ROUND(d->x + d->w) - x, h = LU_ROUND(d->y - oy + d->h) - y;
        if (d->kind == DI_CLIP) {
            if (nclip < 64) {
                ws_push_clip(s, x, y, x + w, y + h, clips[nclip]);
                nclip++;
            }
            continue;
        }
        // vertical culling (shadows / text may overhang a little)
        if (y > s->cy1 + 64 || y + h < s->cy0 - 64) continue;
        if (s->cx0 >= s->cx1 || s->cy0 >= s->cy1) continue;
        switch (d->kind) {
        case DI_RECT: {
            uint32_t c = apply_alpha(d->color, d->alpha);
            if (d->ref >= 0) {
                const int32_t* r = wlay_radii(L, d->ref);
                int32_t rs[4] = { SUB(r[0]), SUB(r[1]), SUB(r[2]), SUB(r[3]) };
                fill_rrect_sub(s, SUB(d->x), SUB(d->y - oy), SUB(d->w), SUB(d->h), rs, c);
            } else ws_fill_rect(s, x, y, w, h, c);
            break;
        }
        case DI_LINE:
            ws_fill_rect(s, x, y, w > 0 ? w : 1, h > 0 ? h : 1, apply_alpha(d->color, d->alpha));
            break;
        case DI_BORDER:
            paint_border(s, x, y, w, h, wlay_border(L, d->ref), d->alpha, SUB(d->x), SUB(d->y - oy), SUB(d->w), SUB(d->h));
            break;
        case DI_TEXT: {
            const struct dtext* t = wlay_text_ref(L, d->ref);
            struct wfont f;
            f.family = t->family; f.bold = t->bold; f.italic = t->italic; f.px = t->px;
            int32_t base = d->a - oy;
            uint32_t c = apply_alpha(d->color, d->alpha);
            if (y > s->cy1 || y + h < s->cy0) break;
            wfont_draw(s, &f, d->x, base, c, text + t->t0, (int)t->tl, t->letter_sp, t->word_sp);
            if (t->deco) {
                struct wfmetrics m;
                wfont_metrics(&f, &m);
                int th = t->px / 14;
                if (th < 1) th = 1;
                uint32_t dc = apply_alpha(t->deco_color ? t->deco_color : d->color, d->alpha);
                int bx = LU_ROUND(d->x), bw = LU_ROUND(d->x + t->width) - bx;
                int by = LU_ROUND(base);
                if (t->deco & DECO_UNDERLINE) {
                    int uy = by + (t->px + 7) / 9;
                    if (uy <= by) uy = by + 1;
                    if (t->deco_style == 2 || t->deco_style == 3) side_rect(s, bx, uy, bw, th, dc, t->deco_style == 2 ? BS_DOTTED : BS_DASHED, 1, th);
                    else ws_fill_rect(s, bx, uy, bw, th, dc);
                }
                if (t->deco & DECO_LINE_THROUGH) ws_fill_rect(s, bx, by - LU_ROUND(m.xheight) / 2 - th / 2, bw, th, dc);
                if (t->deco & DECO_OVERLINE) ws_fill_rect(s, bx, by - LU_ROUND(m.ascent), bw, th, dc);
            }
            break;
        }
        case DI_IMAGE: {
            const struct wimg* im = env && env->image ? env->image(env->ctx, d->a) : 0;
            if (im) wpaint_image(s, im, x, y, w, h, d->alpha);
            break;
        }
        case DI_BGIMG: {
            const struct dbgimg* bgi = wlay_bgimg(L, d->ref);
            const struct wimg* im = env && env->image ? env->image(env->ctx, bgi->img) : 0;
            if (!im) break;
            int o2[4];
            ws_push_clip(s, x, y, x + w, y + h, o2);
            int tw = LU_ROUND(bgi->iw), th = LU_ROUND(bgi->ih);
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
            int ix = LU_ROUND(bgi->ix), iy = LU_ROUND(bgi->iy - oy);
            int rx = bgi->repeat == BG_REPEAT || bgi->repeat == BG_REPEAT_X;
            int ry = bgi->repeat == BG_REPEAT || bgi->repeat == BG_REPEAT_Y;
            int sx = ix, sy = iy;
            if (rx) while (sx > s->cx0) sx -= tw;
            if (ry) while (sy > s->cy0) sy -= th;
            int count = 0;
            for (int yy = sy; yy < (ry ? s->cy1 : sy + 1); yy += th)
                for (int xx = sx; xx < (rx ? s->cx1 : sx + 1); xx += tw) {
                    if (++count > 4000) break;
                    wpaint_image(s, im, xx, yy, tw, th, d->alpha);
                }
            ws_pop_clip(s, o2);
            break;
        }
        case DI_GRAD: {
            const struct wgrad* g = env && env->ss ? css_grad(env->ss, d->a) : 0;
            if (!g) break;
            if (d->ref >= 0) {
                // rounded: clip to the rect, approximate corners by the rect itself
                paint_gradient(s, g, x, y, w, h, d->alpha);
            } else paint_gradient(s, g, x, y, w, h, d->alpha);
            break;
        }
        case DI_SHADOW: {
            // an outer shadow is only visible outside the element's border box
            int o2[4];
            int bands[4][4] = {
                { s->cx0, s->cy0, s->cx1, y },          // above
                { s->cx0, y + h, s->cx1, s->cy1 },      // below
                { s->cx0, y, x, y + h },                // left
                { x + w, y, s->cx1, y + h },            // right
            };
            for (int k = 0; k < 4; k++) {
                ws_push_clip(s, bands[k][0], bands[k][1], bands[k][2], bands[k][3], o2);
                if (s->cx0 < s->cx1 && s->cy0 < s->cy1)
                    paint_shadow(s, d->x, d->y - oy, d->w, d->h, wlay_shadow(L, d->ref), apply_alpha(d->color, d->alpha));
                ws_pop_clip(s, o2);
            }
            break;
        }
        case DI_WIDGET:
            paint_widget(s, x, y, w, h, d->a, d->b, apply_alpha(d->color, d->alpha));
            break;
        case DI_SVG:
            if (env && env->d) wsvg_paint(s, env->d, env->ss, d->a, x, y, w, h, d->color, d->alpha);
            break;
        }
    }
    while (nclip > 0) { nclip--; ws_pop_clip(s, clips[nclip]); }
    ws_pop_clip(s, old);
}

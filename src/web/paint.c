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
    if (im->opaque && alpha == 255 && sw == dw && sh == dh) {        // video frame at its own size: copy rows
        for (int y = y0; y < y1; y++)
            memcpy(s->px + y * s->stride + x0, im->px + (y - dy) * sw + (x0 - dx), (size_t)(x1 - x0) * 4);
        return;
    }
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
        W_PROGRESS();
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
            f.webfont = t->webfont;
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
        case DI_MARKS: {
            const struct dtext* t = wlay_text_ref(L, d->ref);
            if (!t || !t->emph_cp) break;
            if (y > s->cy1 || y + h < s->cy0) break;
            struct wfont f;
            f.family = t->family; f.bold = t->bold; f.italic = t->italic; f.px = t->px;
            f.webfont = t->webfont;
            struct wfont m50 = f;
            m50.px = (uint16_t)(t->px / 2);
            struct wfmetrics m5;
            wfont_metrics(&m50, &m5);
            char mb[4];
            int ml = w_utf8_enc(t->emph_cp, mb);
            int32_t mark_adv = wfont_advance(&m50, t->emph_cp);
            uint32_t c = apply_alpha(d->color, d->alpha);
            const char* txt = text + t->t0;
            if (t->vmode) {
                // vertical: the item rect is the annotation band (LU); one mark
                // per base glyph cell along the band, centered across it
                int32_t bx = d->x, by = d->y - oy, bw = d->w, bh = d->h;
                int32_t em5 = m5.ascent + m5.descent;
                int32_t at = t->vup ? by + bh : by;
                for (uint32_t i = 0; i < t->tl;) {
                    uint32_t cp;
                    uint32_t n = (uint32_t)w_utf8_dec(txt + i, (int)(t->tl - i), &cp);
                    if (!n) break;
                    int32_t adv = wfont_advance(&f, cp) + t->letter_sp;
                    if (t->vup) at -= adv;
                    if (cp != ' ' && cp != 0x3000 && cp != '\n' && cp != '\t' && adv > 0) {
                        int32_t top = at + (adv - em5) / 2;
                        int32_t mx = bx + (bw - mark_adv) / 2;
                        wfont_draw(s, &m50, mx, top + m5.ascent, c, mb, ml, 0, 0);
                    }
                    if (!t->vup) at += adv;
                    i += n;
                }
                break;
            }
            int32_t base = d->a - oy;
            int32_t at = d->x;
            for (uint32_t i = 0; i < t->tl;) {
                uint32_t cp;
                uint32_t n = (uint32_t)w_utf8_dec(txt + i, (int)(t->tl - i), &cp);
                if (!n) break;
                int32_t adv = wfont_advance(&f, cp) + t->letter_sp;
                if (cp != ' ' && cp != 0x3000 && cp != '\n' && cp != '\t')
                    wfont_draw(s, &m50, at + (adv - mark_adv) / 2, base, c, mb, ml, 0, 0);
                at += adv;
                i += n;
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
            struct wdom* sdom = 0; const struct wstyleset* sss = 0; int sroot = -1;
            int is_svg = env && env->svg && env->svg(env->ctx, bgi->img, &sdom, &sss, &sroot);
            const struct wimg* im = !is_svg && env && env->image ? env->image(env->ctx, bgi->img) : 0;
            if (!im && !is_svg) break;
            int o2[4];
            ws_push_clip(s, x, y, x + w, y + h, o2);
            int32_t tw = bgi->iw, th = bgi->ih, ix = bgi->ix, iy = bgi->iy - oy;
            int32_t stx = bgi->stepx > 0 ? bgi->stepx : tw, sty = bgi->stepy > 0 ? bgi->stepy : th;
            if (stx < 1) stx = 1;
            if (sty < 1) sty = 1;
            int32_t cx0 = s->cx0 << 6, cy0 = s->cy0 << 6, cx1 = s->cx1 << 6, cy1 = s->cy1 << 6;
            // the first tile that can reach the clip
            int32_t sx = ix, sy = iy;
            if (bgi->rx && sx + tw > cx0) sx -= ((sx + tw - cx0 - 1) / stx) * stx;
            else if (bgi->rx && sx + tw <= cx0) sx += ((cx0 - sx - tw) / stx) * stx;
            if (bgi->ry && sy + th > cy0) sy -= ((sy + th - cy0 - 1) / sty) * sty;
            else if (bgi->ry && sy + th <= cy0) sy += ((cy0 - sy - th) / sty) * sty;
            int count = 0;
            for (int32_t yy = sy; yy < (bgi->ry ? cy1 : sy + 1); yy += sty) {
                for (int32_t xx = sx; xx < (bgi->rx ? cx1 : sx + 1); xx += stx) {
                    if (++count > 4000) break;
                    int X = LU_ROUND(xx), Y = LU_ROUND(yy);
                    int W = LU_ROUND(xx + tw) - X, H = LU_ROUND(yy + th) - Y;
                    if (W < 1) W = 1;
                    if (H < 1) H = 1;
                    if (is_svg) wsvg_paint(s, sdom, sss, sroot, X, Y, W, H, 0xFF000000, d->alpha);
                    else wpaint_image(s, im, X, Y, W, H, d->alpha);
                }
                if (count > 4000) break;
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

// ---- damage-based repaint ----------------------------------------------------------------------------------------------------------
// A page that changes a little (a spinner, a progress bar, a counter) used to be repainted completely: 155 repaints of a 1916x936 view
// took 6.4 s of a 18 s YouTube load (41 ms each). The painter keeps a signature per display item from its last paint; when the
// surface and the scroll position are the same, only the rectangles of items that appeared, vanished or changed are repainted (every item
// that touches such a rectangle is drawn again, clipped to it, so the result is identical to a full paint). Anything not understood
// (reordered items, a changed background, too much damage) falls back to the full paint.
int wpaint_inc_enabled = 1;
int wpaint_dbg;

static uint32_t hm(uint32_t h, uint32_t v) { h ^= v; h *= 16777619u; h ^= h >> 15; return h; }

static uint32_t item_sig(const struct wlayout* L, const struct ditem* d, const char* text, const struct wpaint_env* env, int* always, int* margin) {
    uint32_t h = 2166136261u;
    h = hm(h, d->kind | (uint32_t)d->fixed << 8 | (uint32_t)d->alpha << 16);
    h = hm(h, (uint32_t)d->x); h = hm(h, (uint32_t)d->y); h = hm(h, (uint32_t)d->w); h = hm(h, (uint32_t)d->h);
    h = hm(h, d->color); h = hm(h, (uint32_t)d->a); h = hm(h, (uint32_t)d->b); h = hm(h, (uint32_t)d->ref);
    *always = 0;
    *margin = 2;
    switch (d->kind) {
    case DI_RECT:
        if (d->ref >= 0) { const int32_t* r = wlay_radii(L, d->ref); for (int i = 0; i < 4; i++) h = hm(h, (uint32_t)r[i]); }
        break;
    case DI_BORDER: {
        const struct dborder* b = wlay_border(L, d->ref);
        if (b) { for (int i = 0; i < 4; i++) { h = hm(h, (uint32_t)b->w[i]); h = hm(h, b->c[i]); h = hm(h, b->s[i]); h = hm(h, (uint32_t)b->r[i]); } }
        break;
    }
    case DI_TEXT: {
        const struct dtext* t = wlay_text_ref(L, d->ref);
        if (t) {
            h = hm(h, t->family | (uint32_t)t->bold << 8 | (uint32_t)t->italic << 16 | (uint32_t)t->deco << 24);
            h = hm(h, t->px | (uint32_t)t->deco_style << 16);
            h = hm(h, (uint32_t)t->letter_sp); h = hm(h, (uint32_t)t->word_sp); h = hm(h, t->deco_color); h = hm(h, (uint32_t)t->width);
            for (uint32_t i = 0; i < t->tl; i++) h = hm(h, (uint8_t)text[t->t0 + i]);
        }
        *margin = 4 + (t ? t->px / 4 : 0);                 // italic overhang, descenders, decorations
        break;
    }
    case DI_MARKS: {
        const struct dtext* t = wlay_text_ref(L, d->ref);
        if (t) {
            h = hm(h, t->family | (uint32_t)t->bold << 8 | (uint32_t)t->italic << 16);
            h = hm(h, t->px | (uint32_t)t->emph_cp << 16);
            h = hm(h, (uint32_t)t->letter_sp); h = hm(h, (uint32_t)t->width);
            for (uint32_t i = 0; i < t->tl; i++) h = hm(h, (uint8_t)text[t->t0 + i]);
        }
        *margin = 4 + (t ? t->px / 4 : 0);
        break;
    }
    case DI_IMAGE:
    case DI_BGIMG: {
        int id = d->a;
        if (d->kind == DI_BGIMG) {
            const struct dbgimg* g = wlay_bgimg(L, d->ref);
            if (g) { id = g->img; h = hm(h, (uint32_t)g->ix); h = hm(h, (uint32_t)g->iy); h = hm(h, (uint32_t)g->iw); h = hm(h, (uint32_t)g->ih); h = hm(h, (uint32_t)g->stepx); h = hm(h, (uint32_t)g->stepy); h = hm(h, (uint32_t)g->rx | (uint32_t)g->ry << 1); }
        }
        uint32_t s = env && env->image_sig ? env->image_sig(env->ctx, id) : 0;
        if (!s) *always = 1; else h = hm(h, s);
        break;
    }
    case DI_GRAD: {
        const struct wgrad* g = env && env->ss ? css_grad(env->ss, d->a) : 0;
        if (g) {
            h = hm(h, (uint32_t)g->angle); h = hm(h, g->nstops | (uint32_t)g->repeating << 8 | (uint32_t)g->radial << 16);
            for (int i = 0; i < g->nstops && i < CSS_MAX_STOPS; i++) { h = hm(h, g->color[i]); h = hm(h, (uint32_t)g->pos[i]); }
        }
        break;
    }
    case DI_SHADOW: {
        const struct dshadow* sh = wlay_shadow(L, d->ref);
        if (sh) {
            h = hm(h, (uint32_t)sh->blur); h = hm(h, (uint32_t)sh->spread); h = hm(h, (uint32_t)sh->ox); h = hm(h, (uint32_t)sh->oy);
            for (int i = 0; i < 4; i++) h = hm(h, (uint32_t)sh->r[i]);
            int e = LU_ROUND(sh->blur) * 2 + LU_ROUND(sh->spread > 0 ? sh->spread : 0) + LU_ROUND(sh->ox < 0 ? -sh->ox : sh->ox) + LU_ROUND(sh->oy < 0 ? -sh->oy : sh->oy);
            *margin = e + 4;
        }
        break;
    }
    case DI_SVG:
        *always = 1;                                          // its picture depends on DOM state the list does not carry
        break;
    case DI_CLIP:
        if (d->ref >= 0) { const int32_t* r = wlay_radii(L, d->ref); for (int i = 0; i < 4; i++) h = hm(h, (uint32_t)r[i]); }
        break;
    default:
        break;
    }
    return h;
}

struct drect { int x0, y0, x1, y1; };

// Which cache painted last into a surface: tabs share one page surface, so another document's picture may be there.
static struct { const uint32_t* px; const void* owner; } surf_owner[16];
static int owner_ok(const uint32_t* px, const void* pc) {
    for (int i = 0; i < 16; i++) if (surf_owner[i].px == px) return surf_owner[i].owner == pc;
    return 0;
}
static void owner_set(const uint32_t* px, const void* pc) {
    static int next;
    for (int i = 0; i < 16; i++) if (surf_owner[i].px == px) { surf_owner[i].owner = pc; return; }
    surf_owner[next & 15].px = px; surf_owner[next & 15].owner = pc; next++;
}

static void item_rect(const struct ditem* d, int32_t scroll_lu, int margin, int sw, int sh, struct drect* r) {
    int32_t oy = d->fixed ? 0 : scroll_lu;
    int x = LU_ROUND(d->x), y = LU_ROUND(d->y - oy);
    int w = LU_ROUND(d->x + d->w) - x, h = LU_ROUND(d->y - oy + d->h) - y;
    r->x0 = x - margin; r->y0 = y - margin; r->x1 = x + (w > 0 ? w : 1) + margin; r->y1 = y + (h > 0 ? h : 1) + margin;
    if (d->kind == DI_TEXT || d->kind == DI_MARKS) { r->y0 -= margin; r->y1 += margin; }   // the item's own box is the line box; glyphs may leave it a little
    if (r->x0 < 0) r->x0 = 0;
    if (r->y0 < 0) r->y0 = 0;
    if (r->x1 > sw) r->x1 = sw;
    if (r->y1 > sh) r->y1 = sh;
}

static void pc_store(struct wpaint_cache* pc, int n, const uint32_t* sig, const uint8_t* alw, const struct drect* rc, const struct wsurf* s, int scroll, uint32_t bg) {
    if (n > pc->cap) {
        int nc = n + n / 2 + 64;
        uint32_t* a = (uint32_t*)w_realloc(pc->sig, (size_t)nc * 4);
        if (a) pc->sig = a;
        uint8_t* b = (uint8_t*)w_realloc(pc->always, (size_t)nc);
        if (b) pc->always = b;
        struct drect* c = (struct drect*)w_realloc(pc->rect, (size_t)nc * sizeof(struct drect));
        if (c) pc->rect = (void*)c;
        if (!a || !b || !c) { pc->valid = 0; return; }
        pc->cap = nc;
    }
    memcpy(pc->sig, sig, (size_t)n * 4);
    memcpy(pc->always, alw, (size_t)n);
    memcpy(pc->rect, rc, (size_t)n * sizeof(struct drect));
    pc->n = n; pc->scroll = scroll; pc->w = s->w; pc->h = s->h; pc->px = s->px; pc->bg = bg; pc->valid = 1;
}

void wpaint_cache_free(struct wpaint_cache* pc) {
    w_free(pc->sig); w_free(pc->always); w_free(pc->rect);
    memset(pc, 0, sizeof *pc);
}

struct sref { uint32_t sig; int idx; };
// insertion/shell sort (no libc qsort in the kernel)
static void sref_sort(struct sref* a, int n) {
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++) {
            struct sref t = a[i];
            int j = i;
            while (j >= gap && (a[j - gap].sig > t.sig || (a[j - gap].sig == t.sig && a[j - gap].idx > t.idx))) { a[j] = a[j - gap]; j -= gap; }
            a[j] = t;
        }
}

#define MAX_DMG 48
static void dmg_add(struct drect* dm, int* nd, const struct drect* r) {
    if (r->x0 >= r->x1 || r->y0 >= r->y1) return;
    for (int i = 0; i < *nd; i++) {                               // merge with a rectangle it touches (8 px slack)
        struct drect* q = &dm[i];
        if (r->x0 <= q->x1 + 8 && r->x1 >= q->x0 - 8 && r->y0 <= q->y1 + 8 && r->y1 >= q->y0 - 8) {
            if (r->x0 < q->x0) q->x0 = r->x0;
            if (r->y0 < q->y0) q->y0 = r->y0;
            if (r->x1 > q->x1) q->x1 = r->x1;
            if (r->y1 > q->y1) q->y1 = r->y1;
            return;
        }
    }
    if (*nd < MAX_DMG) { dm[(*nd)++] = *r; return; }
    dm[0].x0 = dm[0].x0 < r->x0 ? dm[0].x0 : r->x0;               // out of slots: grow the first one over everything
    dm[0].y0 = dm[0].y0 < r->y0 ? dm[0].y0 : r->y0;
    dm[0].x1 = dm[0].x1 > r->x1 ? dm[0].x1 : r->x1;
    dm[0].y1 = dm[0].y1 > r->y1 ? dm[0].y1 : r->y1;
}

// Returns the number of surface pixels repainted (the whole surface for a full paint).
long wpaint_inc(const struct wlayout* L, struct wsurf* s, int scroll_y, const struct wpaint_env* env, struct wpaint_cache* pc) {
    const long total = (long)s->w * s->h;
    const struct ditem* it;
    int n = wlay_items(L, &it);
    const char* text = wlay_text(L);
    uint32_t bg = wlay_canvas_bg(L);
    int32_t scroll_lu = PX(scroll_y);
    int whole_clip = s->cx0 == 0 && s->cy0 == 0 && s->cx1 == s->w && s->cy1 == s->h;
    uint32_t* sig = (uint32_t*)w_malloc((size_t)(n + 1) * 4);
    uint8_t* alw = (uint8_t*)w_malloc((size_t)n + 1);
    struct drect* rc = (struct drect*)w_malloc((size_t)(n + 1) * sizeof(struct drect));
    if (!sig || !alw || !rc) { w_free(sig); w_free(alw); w_free(rc); pc->valid = 0; wpaint(L, s, scroll_y, env); return total; }
    for (int i = 0; i < n; i++) {
        int always, margin;
        sig[i] = item_sig(L, &it[i], text, env, &always, &margin);
        alw[i] = (uint8_t)always;
        item_rect(&it[i], scroll_lu, margin, s->w, s->h, &rc[i]);
    }
    long painted = total;
    int incremental = 0;
    if (wpaint_dbg) w_log("[paint] call: n=%d cache valid=%d n=%d scroll %d/%d size %dx%d/%dx%d same_px=%d clip_whole=%d\n", n, pc->valid, pc->n, pc->scroll, scroll_y, pc->w, pc->h, s->w, s->h, pc->px == s->px, whole_clip);
    if (wpaint_inc_enabled && pc->valid && owner_ok(s->px, pc) && whole_clip && pc->scroll == scroll_y && pc->w == s->w && pc->h == s->h && pc->px == s->px && pc->bg == bg && n > 0) {
        // match the items of the last paint with the new ones (equal signature, k-th with k-th), stable order required
        struct sref* o = (struct sref*)w_malloc((size_t)(pc->n + 1) * sizeof(struct sref));
        struct sref* nw = (struct sref*)w_malloc((size_t)(n + 1) * sizeof(struct sref));
        uint8_t* om = (uint8_t*)w_calloc((size_t)pc->n + 1, 1);
        uint8_t* nm = (uint8_t*)w_calloc((size_t)n + 1, 1);
        int* map = (int*)w_malloc((size_t)(pc->n + 1) * sizeof(int));          // old index -> new index
        const struct drect* orc = (const struct drect*)pc->rect;
        if (o && nw && om && nm && map) {
            int no = 0, nn = 0;
            for (int i = 0; i < pc->n; i++) if (!pc->always[i]) { o[no].sig = pc->sig[i]; o[no].idx = i; no++; }
            for (int i = 0; i < n; i++) if (!alw[i]) { nw[nn].sig = sig[i]; nw[nn].idx = i; nn++; }
            sref_sort(o, no);
            sref_sort(nw, nn);
            int a = 0, b = 0, ordered = 1;
            for (int i = 0; i < pc->n; i++) map[i] = -1;
            while (a < no && b < nn) {
                if (o[a].sig < nw[b].sig) a++;
                else if (o[a].sig > nw[b].sig) b++;
                else { om[o[a].idx] = 1; nm[nw[b].idx] = 1; map[o[a].idx] = nw[b].idx; a++; b++; }
            }
            int last = -1;
            for (int i = 0; i < pc->n; i++) if (map[i] >= 0) { if (map[i] < last) { ordered = 0; break; } last = map[i]; }
            if (wpaint_dbg) { int uo = 0, un = 0; for (int i = 0; i < pc->n; i++) if (!om[i]) uo++; for (int i = 0; i < n; i++) if (!nm[i]) un++; w_log("[paint] diff: unmatched old %d new %d, ordered %d\n", uo, un, ordered); }
            if (ordered) {
                struct drect dm[MAX_DMG];
                int nd = 0;
                for (int i = 0; i < pc->n; i++) if (!om[i]) dmg_add(dm, &nd, &orc[i]);
                for (int i = 0; i < n; i++) if (!nm[i]) dmg_add(dm, &nd, &rc[i]);
                long area = 0;
                for (int i = 0; i < nd; i++) area += (long)(dm[i].x1 - dm[i].x0) * (dm[i].y1 - dm[i].y0);
                if (area * 10 < total * 6) {                                    // less than 60% of the surface: repaint just those
                    if (wpaint_dbg) for (int i = 0; i < nd; i++) w_log("[paint] damage rect %d: %d,%d - %d,%d\n", i, dm[i].x0, dm[i].y0, dm[i].x1, dm[i].y1);
                    for (int i = 0; i < nd; i++) {
                        s->cx0 = dm[i].x0; s->cy0 = dm[i].y0; s->cx1 = dm[i].x1; s->cy1 = dm[i].y1;
                        wpaint(L, s, scroll_y, env);
                    }
                    ws_reset_clip(s);
                    painted = area;
                    incremental = 1;
                }
            }
        }
        w_free(o); w_free(nw); w_free(om); w_free(nm); w_free(map);
    }
    if (!incremental) wpaint(L, s, scroll_y, env);
    if (whole_clip) { pc_store(pc, n, sig, alw, rc, s, scroll_y, bg); owner_set(s->px, pc); }
    else pc->valid = 0;
    w_free(sig); w_free(alw); w_free(rc);
    return painted;
}

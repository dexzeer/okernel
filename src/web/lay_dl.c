// Display list emission (paint order) + hit regions.
#include "lay_int.h"

#define GROWARR(arr, n, cap, type) do { \
    if ((n) >= (cap)) { \
        int nc_ = (cap) ? (cap) * 2 : 256; \
        type* na_ = (type*)w_realloc((arr), (size_t)nc_ * sizeof(type)); \
        if (!na_) return -1; \
        (arr) = na_; (cap) = nc_; \
    } } while (0)

static int di_add(struct wlayout* L, int kind, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    GROWARR(L->di, L->ndi, L->capdi, struct ditem);
    struct ditem* d = &L->di[L->ndi];
    d->kind = (uint8_t)kind;
    d->fixed = (uint8_t)L->fixed_ctx;
    d->alpha = L->alpha;
    d->_pad = 0;
    if (L->psx != 65536 || L->psy != 65536 || L->poff_x || L->poff_y) {
        x = w_muldiv(x, L->psx, 65536) + L->poff_x;
        y = w_muldiv(y, L->psy, 65536) + L->poff_y;
        w = w_muldiv(w, L->psx, 65536);
        h = w_muldiv(h, L->psy, 65536);
    }
    d->x = x; d->y = y; d->w = w; d->h = h;
    d->color = color;
    d->a = d->b = 0;
    d->ref = -1;
    return L->ndi++;
}

static int rad_add(struct wlayout* L, const int32_t r[4]) {
    typedef int32_t rad4_t[4];
    GROWARR(L->rad, L->nrad, L->caprad, rad4_t);
    for (int i = 0; i < 4; i++) L->rad[L->nrad][i] = r[i];
    return L->nrad++;
}

static int hit_add(struct wlayout* L, int32_t x, int32_t y, int32_t w, int32_t h, int node, int kind) {
    if (w <= 0 || h <= 0) return -1;
    GROWARR(L->hits, L->nhits, L->caphits, struct dhit);
    struct dhit* t = &L->hits[L->nhits];
    t->x = x; t->y = y; t->w = w; t->h = h; t->node = node; t->kind = (uint8_t)kind;
    t->fixed = (uint8_t)L->fixed_ctx;
    return L->nhits++;
}

static uint32_t with_alpha(struct wlayout* L, uint32_t c) {
    uint32_t a = c >> 24;
    if (L->alpha < 255) a = (a * L->alpha + 127) / 255;
    return (a << 24) | (c & 0xFFFFFF);
}

static int link_ancestor(struct wlayout* L, int node) {
    struct wdom* d = L->d;
    for (int n = node, k = 0; n > 0 && k < 64; n = d->n[n].parent, k++)
        if (d->n[n].type == WN_ELEM && (wdom_is(d, n, T_a) || wdom_is(d, n, T_area)) && wdom_has_attr(d, n, A_href))
            return n;
    return -1;
}

static void radii_of(struct lbox* b, int32_t r[4]) {
    for (int i = 0; i < 4; i++) {
        r[i] = wl_resolve(b->pst->radius[i], b->w < b->h ? b->w : b->h, 0);
        if (r[i] < 0) r[i] = 0;
    }
}

// overflow-clip-margin (overflow: clip only): the clip edge moves out by a length, from the padding / content / border edge
static void ocm_adjust(const struct lbox* b, const struct wstyle* s, int32_t* cx, int32_t* cy, int32_t* cw, int32_t* ch) {
    if (!(s->overflow_x == OV_CLIP && s->overflow_y == OV_CLIP && (s->ocm_len || s->ocm_box))) return;
    int32_t l = 0, t = 0, r = 0, bt = 0;
    if (s->ocm_box == 1) { l = b->p[3]; t = b->p[0]; r = b->p[1]; bt = b->p[2]; }
    else if (s->ocm_box == 2) { l = -b->b[3]; t = -b->b[0]; r = -b->b[1]; bt = -b->b[2]; }
    *cx += l - s->ocm_len; *cy += t - s->ocm_len;
    *cw += -l - r + 2 * s->ocm_len; *ch += -t - bt + 2 * s->ocm_len;
}

// ---- backgrounds / borders --------------------------------------------------------

// the border / padding / content box of b (BGB_*), x y w h
static void bg_box(const struct lbox* b, int kind, int32_t x, int32_t y, int32_t w, int32_t h, int32_t out[4]) {
    int32_t l = 0, t = 0, r = 0, bt = 0;
    if (kind >= BGB_PADDING) { l = b->b[3]; t = b->b[0]; r = b->b[1]; bt = b->b[2]; }
    if (kind == BGB_CONTENT) { l += b->p[3]; t += b->p[0]; r += b->p[1]; bt += b->p[2]; }
    out[0] = x + l; out[1] = y + t; out[2] = w - l - r; out[3] = h - t - bt;
    if (out[2] < 0) out[2] = 0;
    if (out[3] < 0) out[3] = 0;
}

// CSS Images 3 default sizing algorithm for a background tile. iw/ih: intrinsic size in LU (-1 = none), ratio = w/h * 65536 (0 = none),
// pw/ph: the positioning area.
static void bg_tile_size(const struct wstyle* s, int32_t iw, int32_t ih, int32_t ratio, int32_t pw, int32_t ph, int32_t* tw, int32_t* th) {
    if (s->bg_size_kind == BGS_COVER || s->bg_size_kind == BGS_CONTAIN) {
        if (!ratio) { *tw = pw; *th = ph; return; }
        int32_t wh = (int32_t)w_div64((int64_t)ph * ratio, 65536);      // width when the height fills the area
        int fit_h = s->bg_size_kind == BGS_CONTAIN ? (wh <= pw) : (wh >= pw);
        if (fit_h) { *tw = wh; *th = ph; }
        else { *tw = pw; *th = (int32_t)w_div64((int64_t)pw * 65536, ratio); }
        return;
    }
    int32_t W = -1, H = -1;
    if (s->bg_size_kind == BGS_LEN) {
        if (s->bg_size[0].t == WL_LEN) W = wl_resolve(s->bg_size[0], pw, 0);
        if (s->bg_size[1].t == WL_LEN) H = wl_resolve(s->bg_size[1], ph, 0);
    }
    if (W >= 0 && H >= 0) { *tw = W; *th = H; return; }
    if (W >= 0) {
        *tw = W;
        *th = ratio ? (int32_t)w_div64((int64_t)W * 65536, ratio) : (ih > 0 ? ih : ph);
        return;
    }
    if (H >= 0) {
        *th = H;
        *tw = ratio ? (int32_t)w_div64((int64_t)H * ratio, 65536) : (iw > 0 ? iw : pw);
        return;
    }
    if (iw > 0 && ih > 0) { *tw = iw; *th = ih; return; }
    if (ratio) {
        if (iw > 0) { *tw = iw; *th = (int32_t)w_div64((int64_t)iw * 65536, ratio); return; }
        if (ih > 0) { *th = ih; *tw = (int32_t)w_div64((int64_t)ih * ratio, 65536); return; }
        int32_t wh = (int32_t)w_div64((int64_t)ph * ratio, 65536);       // contain
        if (wh <= pw) { *tw = wh; *th = ph; }
        else { *tw = pw; *th = (int32_t)w_div64((int64_t)pw * 65536, ratio); }
        return;
    }
    *tw = iw > 0 ? iw : pw;
    *th = ih > 0 ? ih : ph;
}

// ---- border-image ----------------------------------------------------------------------------------------------------

// One piece of the source image drawn into a rectangle: the whole image is placed and scaled so that the source rectangle
// (sx, sy, sw, sh; LU of image pixels) lands on (dx, dy, dw, dh), and the item is clipped to (cx, cy, cw, ch).
static void bi_piece(struct wlayout* L, int img, int32_t IW, int32_t IH, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                     int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t cx, int32_t cy, int32_t cw, int32_t ch) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    int32_t x0 = W_MAX(dx, cx), y0 = W_MAX(dy, cy), x1 = W_MIN(dx + dw, cx + cw), y1 = W_MIN(dy + dh, cy + ch);
    if (x1 <= x0 || y1 <= y0) return;
    if (L->ndbg >= L->capdbg) {
        int nc = L->capdbg ? L->capdbg * 2 : 64;
        struct dbgimg* n = (struct dbgimg*)w_realloc(L->dbg, nc * sizeof(struct dbgimg));
        if (!n) return;
        L->dbg = n; L->capdbg = nc;
    }
    struct dbgimg* bg = &L->dbg[L->ndbg];
    int32_t tw = w_muldiv(IW, dw, sw), th = w_muldiv(IH, dh, sh);
    bg->img = img;
    bg->ix = dx - w_muldiv(sx, dw, sw); bg->iy = dy - w_muldiv(sy, dh, sh);
    bg->iw = tw; bg->ih = th; bg->stepx = tw; bg->stepy = th; bg->rx = 0; bg->ry = 0;
    int k = di_add(L, DI_BGIMG, x0, y0, x1 - x0, y1 - y0, 0xFF000000);
    if (k >= 0) L->di[k].ref = L->ndbg++;
}

// Tiles of length tl along a patch [p0, p0 + plen): positions (and the tile length) for stretch / repeat / round / space.
static int bi_axis(int mode, int32_t p0, int32_t plen, int32_t tl, int32_t* pos, int32_t* len, int max) {
    int n = 0;
    if (plen <= 0) return 0;
    if (mode == BIR_STRETCH || tl <= 0) { pos[0] = p0; len[0] = plen; return 1; }
    if (mode == BIR_REPEAT) {
        int32_t cnt = (plen + tl - 1) / tl;
        if (cnt > max) cnt = max;
        int32_t start = p0 + (plen - cnt * tl) / 2;
        for (int i = 0; i < cnt; i++) { pos[n] = start + i * tl; len[n] = tl; n++; }
    } else if (mode == BIR_ROUND) {
        int32_t cnt = (plen + tl / 2) / tl;
        if (cnt < 1) cnt = 1;
        if (cnt > max) cnt = max;
        int32_t t2 = plen / cnt;
        for (int i = 0; i < cnt; i++) { pos[n] = p0 + i * t2; len[n] = t2; n++; }
    } else {
        int32_t cnt = plen / tl;
        if (cnt > max) cnt = max;
        if (cnt <= 0) return 0;
        int32_t gap = (plen - cnt * tl) / (cnt + 1);
        for (int i = 0; i < cnt; i++) { pos[n] = p0 + gap + i * (tl + gap); len[n] = tl; n++; }
    }
    return n;
}

// Paints the border image; returns 0 when there is nothing to draw (the normal border is drawn instead).
static int emit_border_image(struct wlayout* L, const struct lbox* b, const struct wstyle* s, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!s->bi_src || !L->imgs || !L->imgs->for_url) return 0;
    int iw = 0, ih = 0;
    int img = L->imgs->for_url(L->imgs->ctx, css_str(L->ss, s->bi_src, s->bi_src_len), &iw, &ih);
    if (img < 0) return 0;
    int dw = iw, dh = ih; int32_t ratio = 0;
    if (L->imgs->info) L->imgs->info(L->imgs->ctx, img, &dw, &dh, &ratio);
    if (dw <= 0 || dh <= 0) { dw = iw; dh = ih; }
    if (dw <= 0 || dh <= 0) return 0;
    int32_t IW = PX(dw), IH = PX(dh);
    // the border image area: the border box grown by the outset
    int32_t os[4];
    for (int k = 0; k < 4; k++) {
        const struct wlen* o = &s->bi_outset[k];
        os[k] = o->t == WL_NONE ? w_muldiv(o->px, b->b[k], 1000) : o->px;
    }
    int32_t ax = x - os[3], ay = y - os[0], aw = w + os[1] + os[3], ah = h + os[0] + os[2];
    if (aw <= 0 || ah <= 0) return 0;
    // slices (T R B L), in LU of image pixels
    int32_t sl[4];
    for (int k = 0; k < 4; k++) {
        int32_t full = (k & 1) ? IW : IH;
        const struct wlen* o = &s->bi_slice[k];
        sl[k] = o->pct ? w_muldiv(full, o->pct, 10000) : o->px;
        if (sl[k] > full) sl[k] = full;
        if (sl[k] < 0) sl[k] = 0;
    }
    // widths
    int32_t bw[4];
    for (int k = 0; k < 4; k++) {
        const struct wlen* o = &s->bi_width[k];
        if (o->t == WL_AUTO) bw[k] = sl[k];
        else if (o->t == WL_NONE) bw[k] = w_muldiv(o->px, b->b[k], 1000);
        else bw[k] = o->px + w_muldiv((k & 1) ? aw : ah, o->pct, 10000);
        if (bw[k] < 0) bw[k] = 0;
    }
    {
        int64_t sh_ = (int64_t)bw[1] + bw[3], sv = (int64_t)bw[0] + bw[2];
        int32_t fn = 1, fd = 1;                                  // the factor aw / sh_ or ah / sv, whichever shrinks more
        if (sh_ > aw) { fn = aw; fd = (int32_t)sh_; }
        if (sv > ah && (int64_t)ah * fd < (int64_t)fn * sv) { fn = ah; fd = (int32_t)sv; }
        if (fn != fd) for (int k = 0; k < 4; k++) bw[k] = w_muldiv(bw[k], fn, fd);
    }
    int32_t midw = aw - bw[3] - bw[1], midh = ah - bw[0] - bw[2];
    int32_t mw = IW - sl[3] - sl[1], mh = IH - sl[0] - sl[2];          // the source's middle
    // corners
    bi_piece(L, img, IW, IH, 0, 0, sl[3], sl[0], ax, ay, bw[3], bw[0], ax, ay, bw[3], bw[0]);
    bi_piece(L, img, IW, IH, IW - sl[1], 0, sl[1], sl[0], ax + aw - bw[1], ay, bw[1], bw[0], ax + aw - bw[1], ay, bw[1], bw[0]);
    bi_piece(L, img, IW, IH, 0, IH - sl[2], sl[3], sl[2], ax, ay + ah - bw[2], bw[3], bw[2], ax, ay + ah - bw[2], bw[3], bw[2]);
    bi_piece(L, img, IW, IH, IW - sl[1], IH - sl[2], sl[1], sl[2], ax + aw - bw[1], ay + ah - bw[2], bw[1], bw[2], ax + aw - bw[1], ay + ah - bw[2], bw[1], bw[2]);
    int32_t pos[160], len[160];
    // top and bottom edges: stretched across their height, repeated along their length
    for (int side = 0; side < 2; side++) {
        int k = side ? 2 : 0;
        int32_t sy0 = side ? IH - sl[2] : 0, shh = sl[k], dy0 = side ? ay + ah - bw[2] : ay, dhh = bw[k];
        if (mw <= 0 || shh <= 0 || dhh <= 0 || midw <= 0) continue;
        int32_t tl = w_muldiv(mw, dhh, shh);
        int n = bi_axis(s->bi_rep[0], ax + bw[3], midw, tl, pos, len, 160);
        for (int i = 0; i < n; i++)
            bi_piece(L, img, IW, IH, sl[3], sy0, mw, shh, pos[i], dy0, len[i], dhh, ax + bw[3], dy0, midw, dhh);
    }
    // left and right edges
    for (int side = 0; side < 2; side++) {
        int k = side ? 1 : 3;
        int32_t sx0 = side ? IW - sl[1] : 0, sww = sl[k], dx0 = side ? ax + aw - bw[1] : ax, dww = bw[k];
        if (mh <= 0 || sww <= 0 || dww <= 0 || midh <= 0) continue;
        int32_t tl = w_muldiv(mh, dww, sww);
        int n = bi_axis(s->bi_rep[1], ay + bw[0], midh, tl, pos, len, 160);
        for (int i = 0; i < n; i++)
            bi_piece(L, img, IW, IH, sx0, sl[0], sww, mh, dx0, pos[i], dww, len[i], dx0, ay + bw[0], dww, midh);
    }
    // the middle, only with fill
    if (s->bi_fill && mw > 0 && mh > 0 && midw > 0 && midh > 0) {
        int32_t kxn = sl[0] > 0 && bw[0] > 0 ? bw[0] : 1, kxd = sl[0] > 0 && bw[0] > 0 ? sl[0] : 1;
        int32_t kyn = sl[3] > 0 && bw[3] > 0 ? bw[3] : 1, kyd = sl[3] > 0 && bw[3] > 0 ? sl[3] : 1;
        int32_t tlx = s->bi_rep[0] == BIR_STRETCH ? midw : w_muldiv(mw, kxn, kxd);
        int32_t tly = s->bi_rep[1] == BIR_STRETCH ? midh : w_muldiv(mh, kyn, kyd);
        int32_t px[160], lx[160], py[40], ly[40];
        int nx = bi_axis(s->bi_rep[0], ax + bw[3], midw, tlx, px, lx, 160);
        int ny = bi_axis(s->bi_rep[1], ay + bw[0], midh, tly, py, ly, 40);
        if (nx * ny > 2000) { nx = nx > 40 ? 40 : nx; ny = ny > 40 ? 40 : ny; }
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++)
                bi_piece(L, img, IW, IH, sl[3], sl[0], mw, mh, px[i], py[j], lx[i], ly[j], ax + bw[3], ay + bw[0], midw, midh);
    }
    return 1;
}

static void emit_bg_border(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->pst;
    if (!s || s->visibility || (b->flags & BF_CLAMPED)) return;
    int32_t x = b->ax, y = b->ay, w = b->w, h = b->h;
    if (w <= 0 || h <= 0) return;
    int32_t r[4];
    radii_of(b, r);
    int has_r = r[0] | r[1] | r[2] | r[3];
    int rref = has_r ? rad_add(L, r) : -1;
    if (s->has_shadow && (s->shadow_color >> 24)) {
        if (L->ndsh >= L->capdsh) {
            int nc = L->capdsh ? L->capdsh * 2 : 64;
            struct dshadow* n = (struct dshadow*)w_realloc(L->dsh, nc * sizeof(struct dshadow));
            if (n) { L->dsh = n; L->capdsh = nc; }
        }
        if (L->ndsh < L->capdsh) {
            struct dshadow* sh = &L->dsh[L->ndsh];
            sh->blur = s->shadow_blur; sh->spread = s->shadow_spread;
            sh->ox = s->shadow_x; sh->oy = s->shadow_y;
            for (int i = 0; i < 4; i++) sh->r[i] = r[i];
            int k = di_add(L, DI_SHADOW, x, y, w, h, with_alpha(L, s->shadow_color));
            if (k >= 0) L->di[k].ref = L->ndsh++;
        }
    }
    // The root element's background is the canvas; body's when propagated
    // (html has none). Its color is the canvas fill (wlay_canvas_bg); its
    // gradient / image paint over the whole canvas, positioned against the
    // element's own box.
    int skip_bg = 0, canvas = 0;
    if (b->node >= 0 && b->node == L->d->html) skip_bg = canvas = 1;
    if (b->node >= 0 && b->node == L->d->body && L->d->html >= 0) {
        const struct wstyle* hs = css_style_of(L->ss, L->d->html);
        if (hs && !(hs->bg_color >> 24) && hs->bg_grad < 0 && !hs->bg_image) skip_bg = canvas = 1;
    }
    if (s->has_mask) skip_bg = 1; // mask-image unsupported: never paint an unmasked fill
    // painting area: the border box, or the whole canvas for the root background
    int32_t ax = x, ay = y, aw = w, ah = h;
    if (canvas) {
        const struct lbox* root = &L->b[L->root];
        ax = 0;
        ay = 0;
        aw = W_MAX(root->w, x + w);
        ah = W_MAX(root->h, y + h);
    }
    if (!skip_bg && (s->bg_color >> 24)) {
        int k = di_add(L, DI_RECT, x, y, w, h, with_alpha(L, s->bg_color));
        if (k >= 0) L->di[k].ref = rref;
    }
    if ((!skip_bg || canvas) && !s->has_mask && s->bg_grad >= 0) {
        int k = di_add(L, DI_GRAD, ax, ay, aw, ah, 0xFF000000);
        if (k >= 0) { L->di[k].a = s->bg_grad; L->di[k].ref = canvas ? -1 : rref; }
    }
    if (s->bg_image && !s->has_mask && L->imgs && L->imgs->for_url) {
        int iw = 0, ih = 0;
        int img = L->imgs->for_url(L->imgs->ctx, css_str(L->ss, s->bg_image, s->bg_image_len), &iw, &ih);
        if (img >= 0 && iw > 0 && ih > 0) {
            int32_t ob[4], cb[4];
            bg_box(b, s->bg_origin, x, y, w, h, ob);
            if (s->bg_attach == 2 && (b->sw || b->sh)) {            // local: the image belongs to the scrolling content
                ob[0] -= b->sox; ob[1] -= b->soy;
                if (b->sw > ob[2]) ob[2] = b->sw;
                if (b->sh > ob[3]) ob[3] = b->sh;
            }
            if (canvas) { cb[0] = ax; cb[1] = ay; cb[2] = aw; cb[3] = ah; }
            else bg_box(b, s->bg_clip, x, y, w, h, cb);
            if (s->bg_attach == 2 && (b->sw || b->sh) && !canvas && s->bg_clip != BGB_BORDER) {
                int32_t pw0 = w - b->b[1] - b->b[3], ph0 = h - b->b[0] - b->b[2];
                int32_t ex = b->sw > pw0 ? b->sw - pw0 : 0, ey = b->sh > ph0 ? b->sh - ph0 : 0;
                int32_t nx0 = cb[0] - b->sox, ny0 = cb[1] - b->soy, nx1 = cb[0] + cb[2] + ex - b->sox, ny1 = cb[1] + cb[3] + ey - b->soy;
                int32_t vx0 = x + b->b[3], vy0 = y + b->b[0], vx1 = x + w - b->b[1], vy1 = y + h - b->b[2];
                if (nx0 < vx0) nx0 = vx0;
                if (ny0 < vy0) ny0 = vy0;
                if (nx1 > vx1) nx1 = vx1;
                if (ny1 > vy1) ny1 = vy1;
                cb[0] = nx0; cb[1] = ny0; cb[2] = nx1 > nx0 ? nx1 - nx0 : 0; cb[3] = ny1 > ny0 ? ny1 - ny0 : 0;
            }
            int32_t px = ob[0], py = ob[1], pw = ob[2], ph = ob[3];
            int dw = iw, dh = ih;
            int32_t ratio = 0;
            if (L->imgs->info) L->imgs->info(L->imgs->ctx, img, &dw, &dh, &ratio);
            if (!ratio && dw > 0 && dh > 0) ratio = (int32_t)w_div64((int64_t)dw * 65536, dh);
            int32_t tw, th;
            bg_tile_size(s, dw > 0 ? PX(dw) : -1, dh > 0 ? PX(dh) : -1, ratio, pw, ph, &tw, &th);
            if (tw > PX(30000)) tw = PX(30000);       // extreme ratios (a viewBox of 2^31 x 1): the visible part is what matters
            if (th > PX(30000)) th = PX(30000);
            int rxm = s->bg_repeat & 3, rym = (s->bg_repeat >> 2) & 3;
            if (tw > 0 && th > 0 && pw > 0 && ph > 0) {
                // round: the tile is stretched so a whole number of them fills the area; an auto size on the other axis follows
                int32_t otw = tw, oth = th;
                int auto_w = s->bg_size_kind == BGS_AUTO || (s->bg_size_kind == BGS_LEN && s->bg_size[0].t != WL_LEN);
                int auto_h = s->bg_size_kind == BGS_AUTO || (s->bg_size_kind == BGS_LEN && s->bg_size[1].t != WL_LEN);
                if (rxm == BGR_ROUND) { int32_t n = (pw + tw / 2) / tw; if (n < 1) n = 1; tw = pw / n; }
                if (rym == BGR_ROUND) { int32_t n = (ph + th / 2) / th; if (n < 1) n = 1; th = ph / n; }
                if (rxm == BGR_ROUND && rym != BGR_ROUND && auto_h) th = w_muldiv(oth, tw, otw);
                if (rym == BGR_ROUND && rxm != BGR_ROUND && auto_w) tw = w_muldiv(otw, th, oth);
                int32_t ox = px + wl_resolve(s->bg_pos[0], pw - tw, 0);
                int32_t oy = py + wl_resolve(s->bg_pos[1], ph - th, 0);
                int32_t stx = tw, sty = th;
                int draw = 1, repx = rxm != BGR_NO_REPEAT, repy = rym != BGR_NO_REPEAT;
                if (rxm == BGR_SPACE) {
                    int32_t n = pw / tw;
                    if (n == 0) draw = 0;
                    else if (n == 1) repx = 0;
                    else { stx = tw + (pw - n * tw) / (n - 1); ox = px; }
                }
                if (rym == BGR_SPACE) {
                    int32_t n = ph / th;
                    if (n == 0) draw = 0;
                    else if (n == 1) repy = 0;
                    else { sty = th + (ph - n * th) / (n - 1); oy = py; }
                }
                if (draw) {
                    if (L->ndbg >= L->capdbg) {
                        int nc = L->capdbg ? L->capdbg * 2 : 64;
                        struct dbgimg* n = (struct dbgimg*)w_realloc(L->dbg, nc * sizeof(struct dbgimg));
                        if (n) { L->dbg = n; L->capdbg = nc; }
                    }
                    if (L->ndbg < L->capdbg) {
                        struct dbgimg* bg = &L->dbg[L->ndbg];
                        bg->img = img; bg->ix = ox; bg->iy = oy; bg->iw = tw; bg->ih = th;
                        bg->stepx = stx; bg->stepy = sty; bg->rx = (uint8_t)repx; bg->ry = (uint8_t)repy;
                        int k = di_add(L, DI_BGIMG, cb[0], cb[1], cb[2], cb[3], 0xFF000000);
                        if (k >= 0) L->di[k].ref = L->ndbg++;
                    }
                }
            }
        }
    }
    int bi_done = emit_border_image(L, b, s, x, y, w, h);        // painted even where border-style none made the border widths 0
    if ((b->b[0] | b->b[1] | b->b[2] | b->b[3]) && !bi_done) {
        if (L->nbord >= L->capbord) {
            int nc = L->capbord ? L->capbord * 2 : 256;
            struct dborder* n = (struct dborder*)w_realloc(L->bord, nc * sizeof(struct dborder));
            if (!n) return;
            L->bord = n;
            L->capbord = nc;
        }
        struct dborder* d = &L->bord[L->nbord];
        for (int i = 0; i < 4; i++) {
            d->w[i] = b->b[i];
            d->c[i] = with_alpha(L, s->bc[i]);
            d->s[i] = s->bs[i];
            d->r[i] = r[i];
        }
        int k = di_add(L, DI_BORDER, x, y, w, h, 0);
        if (k >= 0) L->di[k].ref = L->nbord++;
    }
}

// ---- text ------------------------------------------------------------------------

static void emit_text(struct wlayout* L, const struct wstyle* s, int32_t x, int32_t base, uint32_t t0,
                      uint32_t tl, int32_t width, int32_t word_extra) {
    if (!tl || s->visibility) return;
    if (L->ndtx >= L->capdtx) {
        int nc = L->capdtx ? L->capdtx * 2 : 1024;
        struct dtext* n = (struct dtext*)w_realloc(L->dtx, nc * sizeof(struct dtext));
        if (!n) return;
        L->dtx = n;
        L->capdtx = nc;
    }
    struct dtext* t = &L->dtx[L->ndtx];
    struct wfont f;
    lb_font(s, &f);
    if (L->psx != 65536 || L->psy != 65536) {
        int32_t px = w_muldiv(f.px, (L->psx + L->psy) >> 1, 65536);
        if (px > 65535) px = 65535;
        f.px = (uint16_t)px;
    }
    t->t0 = t0; t->tl = tl;
    t->family = f.family; t->bold = f.bold; t->italic = f.italic; t->px = f.px;
    t->webfont = f.webfont;
    t->deco = s->deco_line | s->deco_inh;
    t->deco_style = s->deco_style;
    t->deco_color = with_alpha(L, s->deco_line ? s->deco_color : s->deco_inh_color);
    t->letter_sp = w_muldiv(s->letter_spacing, (L->psx + L->psy) >> 1, 65536);
    t->word_sp = w_muldiv(s->word_spacing + word_extra, (L->psx + L->psy) >> 1, 65536);
    t->width = w_muldiv(width, (L->psx + L->psy) >> 1, 65536);
    struct wfmetrics m;
    wfont_metrics(&f, &m);
    int k = di_add(L, DI_TEXT, x, base - m.ascent, width, m.ascent + m.descent, with_alpha(L, s->color));
    if (k >= 0) { L->di[k].ref = L->ndtx++; L->di[k].a = base; }
}


// text-emphasis marks over a text run: the paint side walks the glyphs and
// draws the mark codepoint at half the font size, centered on each one.
// Horizontal: (x, base) is the run start and its baseline; the item rect is
// derived from the mark font. Vertical (vert): (x, y, w, h) is the already
// transformed annotation band and the marks walk it like emit_text_vert.
static void emit_marks(struct wlayout* L, const struct wstyle* s, int32_t x, int32_t y, int32_t w, int32_t h,
                       int32_t base, uint32_t t0, uint32_t tl, int vert) {
    if (!tl || !s->emph_cp || s->visibility) return;
    if (L->ndtx >= L->capdtx) {
        int nc = L->capdtx ? L->capdtx * 2 : 1024;
        struct dtext* n = (struct dtext*)w_realloc(L->dtx, nc * sizeof(struct dtext));
        if (!n) return;
        L->dtx = n;
        L->capdtx = nc;
    }
    struct dtext* t = &L->dtx[L->ndtx];
    memset(t, 0, sizeof *t);
    struct wfont f;
    lb_font(s, &f);
    t->t0 = t0; t->tl = tl;
    t->family = f.family; t->bold = f.bold; t->italic = f.italic; t->px = f.px;
    t->webfont = f.webfont;
    t->letter_sp = s->letter_spacing;
    t->width = w;
    t->emph_cp = s->emph_cp;
    int k;
    if (vert) {
        t->vmode = 1;
        t->vup = (uint8_t)((s->wmode == WM_SLR) != (s->direction_rtl != 0));
        k = di_add(L, DI_MARKS, x, y, w, h, with_alpha(L, s->color));
        if (k >= 0) { L->di[k].ref = L->ndtx++; L->di[k].a = 0; }
        return;
    }
    struct wfont f50 = f;
    f50.px = (uint16_t)(f.px / 2);
    struct wfmetrics m;
    wfont_metrics(&f50, &m);
    k = di_add(L, DI_MARKS, x, base - m.ascent, w, m.ascent + m.descent, with_alpha(L, s->color));
    if (k >= 0) { L->di[k].ref = L->ndtx++; L->di[k].a = base; }
}


// Text in a vertical writing mode: the fragment rectangle (x, y, w = column width, h = run length) holds the glyphs one below the other,
// upright (sideways Latin text is not rotated). Each glyph sits centred in a cell as wide as its inline advance.
static void emit_text_vert(struct wlayout* L, const struct wstyle* s, int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t t0, uint32_t tl) {
    if (!tl || s->visibility) return;
    struct wfont f;
    lb_font(s, &f);
    struct wfmetrics m;
    wfont_metrics(&f, &m);
    const char* txt = L->text.p + t0;
    int32_t total = 0;
    for (uint32_t i = 0; i < tl;) { uint32_t cp; i += (uint32_t)w_utf8_dec(txt + i, (int)(tl - i), &cp); total += wfont_advance(&f, cp); }
    int up = (s->wmode == WM_SLR) != (s->direction_rtl != 0);       // sideways-lr and rtl run bottom to top (both: top to bottom)
    (void)total;
    int32_t at = up ? y + h : y;
    int32_t colx = x + (w - (m.ascent + m.descent)) / 2;
    for (uint32_t i = 0; i < tl;) {
        uint32_t cp;
        int n = w_utf8_dec(txt + i, (int)(tl - i), &cp);
        int32_t a = wfont_advance(&f, cp);
        if (up) at -= a;
        if (cp != ' ' && a > 0) {
            int32_t top = at + (a - (m.ascent + m.descent)) / 2;
            emit_text(L, s, colx + (m.ascent + m.descent - a) / 2, top + m.ascent, t0 + i, (uint32_t)n, a, 0);
        }
        if (!up) at += a;
        i += (uint32_t)n;
    }
}

static void emit_atomic_or_block(struct wlayout* L, int bi);

// One line of a text field. With text-overflow, a line wider than the field ends in the ellipsis (or string) instead of being cut.
static void emit_field_line(struct wlayout* L, const struct wstyle* tmp, const struct wfont* f, const char* val, uint32_t off, int len,
                            int32_t x, int32_t base, int32_t cw, int32_t tw, int focused) {
    if (len <= 0) return;
    if (tmp->text_overflow_ellipsis && tw > cw && !focused) {       // a focused field shows its text (and caret), not the ellipsis
        const char* es = "â¦"; int el = 3;
        if (tmp->text_overflow_ellipsis == 2) { es = css_str(L->ss, tmp->text_overflow_str, tmp->text_overflow_len); el = tmp->text_overflow_len; }
        int32_t ew = el ? wfont_measure(f, es, el) : 0;
        int keep = len;
        int32_t kw = tw;
        while (keep > 0 && kw + ew > cw) {
            keep--;
            while (keep > 0 && ((unsigned char)val[keep] & 0xC0) == 0x80) keep--;
            kw = keep ? wfont_measure(f, val, keep) : 0;
        }
        emit_text(L, tmp, x, base, off, (uint32_t)keep, kw, 0);
        if (el) {
            uint32_t eo = (uint32_t)L->text.len;
            wbuf_put(&L->text, es, el);
            emit_text(L, tmp, x + kw, base, eo, (uint32_t)el, ew, 0);
        }
        return;
    }
    emit_text(L, tmp, x, base, off, (uint32_t)len, tw, 0);
}

// replaced element content
static void emit_replaced(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->pst;
    int32_t cx = b->ax + b->b[3] + b->p[3], cy = b->ay + b->b[0] + b->p[0];
    int32_t cw = b->w - b->b[1] - b->b[3] - b->p[1] - b->p[3];
    int32_t ch = b->h - b->b[0] - b->b[2] - b->p[0] - b->p[2];
    if (s->visibility) return;
    switch (b->rk) {
    case RK_IMG: case RK_VIDEO: case RK_CANVAS: case RK_IFRAME: case RK_OBJECT:
        if (b->img >= 0 && b->iw > 0 && b->ih > 0 && cw > 0 && ch > 0) {
            int32_t dx = cx, dy = cy, dw = cw, dh = ch;
            if (s->object_fit == OF_CONTAIN || s->object_fit == OF_COVER || s->object_fit == OF_SCALE_DOWN || s->object_fit == OF_NONE) {
                int32_t sx = w_muldiv(cw, 1000, PX(b->iw)), sy = w_muldiv(ch, 1000, PX(b->ih));
                int32_t sc = s->object_fit == OF_COVER ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
                if (s->object_fit == OF_SCALE_DOWN && sc > 1000) sc = 1000;
                if (s->object_fit == OF_NONE) sc = 1000;
                dw = w_muldiv(PX(b->iw), sc, 1000);
                dh = w_muldiv(PX(b->ih), sc, 1000);
                dx = cx + wl_resolve(s->object_pos[0], cw - dw, 0);
                dy = cy + wl_resolve(s->object_pos[1], ch - dh, 0);
            }
            int clip = s->object_fit == OF_COVER || s->object_fit == OF_NONE || s->object_fit == OF_SCALE_DOWN;
            if (clip) di_add(L, DI_CLIP, cx, cy, cw, ch, 0);
            int k = di_add(L, DI_IMAGE, dx, dy, dw, dh, 0xFF000000);
            if (k >= 0) L->di[k].a = b->img;
            if (clip) di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
        } else if (b->rk == RK_IMG && b->node >= 0 && cw > PX(4) && ch > PX(4)) {
            // broken / loading image with a reserved box: show alt text
            char alt[256];
            int n = wdom_attr_copy(L->d, b->node, A_alt, alt, sizeof alt);
            if (n > 0) {
                uint32_t off = (uint32_t)L->text.len;
                wbuf_put(&L->text, alt, n);
                struct wfont f; lb_font(s, &f);
                struct wfmetrics m; wfont_metrics(&f, &m);
                di_add(L, DI_CLIP, cx, cy, cw, ch, 0);
                emit_text(L, s, cx + PX(2), cy + m.ascent + PX(2), off, (uint32_t)n, wfont_measure(&f, alt, n), 0);
                di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
            }
        }
        break;
    case RK_SVG: {
        int k = di_add(L, DI_SVG, cx, cy, cw, ch, with_alpha(L, s->color));
        if (k >= 0) L->di[k].a = b->node;
        break;
    }
    case RK_INPUT_TEXT: case RK_TEXTAREA: case RK_SELECT: case RK_INPUT_BUTTON: {
        char val[1024];
        int n = -1, placeholder = 0;
        if (b->rk == RK_TEXTAREA) {
            n = wdom_attr_copy(L->d, b->node, A_value, val, sizeof val);
            if (n < 0) n = wdom_text_content(L->d, b->node, val, sizeof val);
        } else if (b->rk == RK_SELECT) {
            int first = -1, sel = -1;
            for (int c = L->d->n[b->node].first; c >= 0; c = wdom_next(L->d, c, b->node)) {
                if (!wdom_is(L->d, c, T_option)) continue;
                if (first < 0) first = c;
                if (wdom_has_attr(L->d, c, A_selected)) { sel = c; break; }
            }
            if (sel < 0) sel = first;
            n = sel >= 0 ? wdom_text_content(L->d, sel, val, sizeof val) : 0;
        } else {
            n = wdom_attr_copy(L->d, b->node, A_value, val, sizeof val);
            if (b->rk == RK_INPUT_BUTTON && n < 0) {
                int tl;
                const char* t = wdom_attr(L->d, b->node, A_type, &tl);
                const char* d = (t && w_ieq(t, tl, "reset")) ? "Reset" : (t && w_ieq(t, tl, "file")) ? "Choose File" :
                                (t && w_ieq(t, tl, "button")) ? "" : "Submit";
                n = (int)strlen(d);
                memcpy(val, d, n);
            }
            if (b->rk == RK_INPUT_TEXT) {
                int tl;
                const char* t = wdom_attr(L->d, b->node, A_type, &tl);
                if (t && w_ieq(t, tl, "password") && n > 0) {
                    int k = 0;
                    for (int i = 0; i < n && k < 300; i++) k += w_utf8_enc(0x2022, val + k);
                    n = k;
                }
                if (n <= 0) {
                    n = wdom_attr_copy(L->d, b->node, A_placeholder, val, sizeof val);
                    placeholder = 1;
                }
            }
        }
        if (n < 0) n = 0;
        // collapse newlines for single-line controls
        if (b->rk != RK_TEXTAREA) for (int i = 0; i < n; i++) if (val[i] == '\n' || val[i] == '\r' || val[i] == '\t') val[i] = ' ';
        struct wfont f; lb_font(s, &f);
        struct wfmetrics m; wfont_metrics(&f, &m);
        int32_t lh = lb_line_height(s);
        int focused = (L->d->n[b->node].flags & WNF_FOCUSED) != 0;
        di_add(L, DI_CLIP, cx, cy, cw, ch, 0);
        if (n > 0) {
            uint32_t off = (uint32_t)L->text.len;
            wbuf_put(&L->text, val, n);
            struct wstyle tmp = *s;
            if (placeholder) tmp.color = 0xFF757575;
            tmp.deco_line = 0;
            tmp.deco_inh = 0;
            if (b->rk == RK_TEXTAREA) {
                int32_t yy = cy + (lh - m.ascent - m.descent) / 2 + m.ascent;
                int st = 0;
                for (int i = 0; i <= n; i++) {
                    if (i == n || val[i] == '\n') {
                        emit_field_line(L, &tmp, &f, val + st, off + st, i - st, cx, yy, cw, wfont_measure(&f, val + st, i - st), focused);
                        yy += lh;
                        st = i + 1;
                    }
                }
            } else {
                int32_t tw = wfont_measure(&f, val, n);
                int32_t tx = cx;
                if (b->rk == RK_INPUT_BUTTON) tx = cx + (cw - tw) / 2;
                int32_t base = cy + (ch - lh) / 2 + (lh - m.ascent - m.descent) / 2 + m.ascent;
                emit_field_line(L, &tmp, &f, val, off, n, tx, base, cw, tw, focused);
                if (focused && !placeholder && b->rk == RK_INPUT_TEXT) {
                    int32_t caret = tx + tw;
                    di_add(L, DI_LINE, caret, base - m.ascent, PX(1), m.ascent + m.descent, with_alpha(L, s->color));
                }
            }
        } else if (focused && b->rk == RK_INPUT_TEXT) {
            int32_t base = cy + (ch - lh) / 2 + (lh - m.ascent - m.descent) / 2 + m.ascent;
            di_add(L, DI_LINE, cx, base - m.ascent, PX(1), m.ascent + m.descent, with_alpha(L, s->color));
        }
        di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
        if (b->rk == RK_SELECT) {
            int k = di_add(L, DI_WIDGET, cx + cw - PX(14), cy, PX(12), ch, with_alpha(L, s->color));
            if (k >= 0) L->di[k].a = 3;
        }
        break;
    }
    case RK_CHECKBOX: case RK_RADIO: {
        int checked = wdom_has_attr(L->d, b->node, A_checked);
        int k = di_add(L, DI_WIDGET, b->ax, b->ay, b->w, b->h, with_alpha(L, 0xFF0075FF));
        if (k >= 0) { L->di[k].a = b->rk == RK_CHECKBOX ? 1 : 2; L->di[k].b = checked; }
        break;
    }
    case RK_METER: {
        int32_t frac = 500;
        char v[32], mx[32];
        if (wdom_attr_copy(L->d, b->node, A_value, v, sizeof v) > 0) {
            int32_t a, z = 1000; int u;
            if (cv_number(v, (int)strlen(v), &a, &u)) {
                int ml = wdom_attr_s(L->d, b->node, "max", 0) ? wdom_attr_copy(L->d, b->node, watom_find(&L->d->atoms, "max", 3), mx, sizeof mx) : -1;
                if (ml > 0) cv_number(mx, ml, &z, &u);
                if (z > 0) frac = w_muldiv(a, 1000, z);
            }
        }
        if (frac < 0) frac = 0;
        if (frac > 1000) frac = 1000;
        int k = di_add(L, DI_WIDGET, b->ax, b->ay, b->w, b->h, with_alpha(L, 0xFF0075FF));
        if (k >= 0) { L->di[k].a = 4; L->di[k].b = frac; }
        break;
    }
    default:
        break;
    }
}

static void emit_marker(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->pst;
    if (!(b->flags & BF_MARKER) || s->visibility) return;
    const struct wstyle* ms = (b->node >= 0) ? css_pseudo_of(L->ss, b->node, 3) : 0;
    if (!ms) ms = s;
    struct wfont f; lb_font(ms, &f);
    struct wfmetrics m; wfont_metrics(&f, &m);
    const char* t = L->text.p + b->t0;
    int32_t tw = wfont_measure(&f, t, (int)b->tl);
    int32_t base;
    if (b->baseline >= 0) base = b->ay + b->baseline;
    else base = b->ay + b->b[0] + b->p[0] + m.ascent + (lb_line_height(s) - m.ascent - m.descent) / 2;
    int32_t x = b->ax + b->b[3] + b->p[3] - tw;
    emit_text(L, ms, x, base, b->t0, b->tl, tw, 0);
}

// ---- layers (positioned descendants) ------------------------------------------------

static int is_layer(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    return b->pst && (b->kind == LB_BLOCK || b->kind == LB_REPLACED) && b->pst->position != POS_STATIC;
}

// Does this positioned box start a stacking context? (z-index other than auto, opacity < 1, fixed/sticky.) One that does not
// paints at the z=0 step of its parent context, and its own positioned descendants belong to THAT context, not to it: a
// dialog with z-index 2202 inside an absolutely positioned (z-index: auto) app root still beats a backdrop at 2201 outside.
static int creates_ctx(struct wlayout* L, int bi) {
    const struct wstyle* s = L->b[bi].st;
    return !s->z_auto || s->opacity < 255 || s->position == POS_FIXED || s->position == POS_STICKY;
}

struct layers { int* v; int n, cap; };
static void layers_push(struct layers* ls, int bi) {
    if (ls->n >= ls->cap) {
        int nc = ls->cap ? ls->cap * 2 : 32;
        int* n = (int*)w_realloc(ls->v, nc * sizeof(int));
        if (!n) return;
        ls->v = n;
        ls->cap = nc;
    }
    ls->v[ls->n++] = bi;
}

static void collect_layers(struct wlayout* L, int bi, struct layers* ls, int depth) {
    if (depth > 400) return;
    for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) {
        if (!(L->b[c].flags & BF_LAID) && L->b[c].kind != LB_TEXT && L->b[c].kind != LB_INLINE &&
            L->b[c].kind != LB_BR && L->b[c].kind != LB_WBR && L->b[c].kind != LB_MARKER) continue;
        if (is_layer(L, c)) {
            layers_push(ls, c);
            if (!creates_ctx(L, c)) collect_layers(L, c, ls, depth + 1);     // its layers join this context, in tree order
            continue;
        }
        collect_layers(L, c, ls, depth + 1);
    }
}

static void emit_flow(struct wlayout* L, int bi, int depth);

static void emit_ifc(struct wlayout* L, int bi, int depth) {
    struct lbox* b = &L->b[bi];
    int32_t ox = b->ax + b->b[3] + b->p[3], oy = b->ay + b->b[0] + b->p[0];
    for (int f = b->fr0; f < b->fr0 + b->nfr; f++) {
        struct lfrag* fr = &L->fr[f];
        struct lbox* fb = &L->b[fr->box];
        switch (fr->kind) {
        case FR_IBOX: {
            const struct wstyle* s = fb->pst;
            if (s->visibility || is_layer(L, fr->box)) break;
            int32_t x = ox + fr->x, y = oy + fr->y;
            if (s->bg_color >> 24) di_add(L, DI_RECT, x, y, fr->w, fr->h, with_alpha(L, s->bg_color));
            if (s->bg_grad >= 0) { int k = di_add(L, DI_GRAD, x, y, fr->w, fr->h, 0xFF000000); if (k >= 0) L->di[k].a = s->bg_grad; }
            int32_t bw[4] = { fb->b[0], fr->last ? fb->b[1] : 0, fb->b[2], fr->first ? fb->b[3] : 0 };
            if (s->direction_rtl && s->wmode == WM_HTB) { bw[1] = fr->first ? fb->b[1] : 0; bw[3] = fr->last ? fb->b[3] : 0; }
            if (s->wmode == WM_SLR) { bw[0] = fr->last ? fb->b[0] : 0; bw[1] = fb->b[1]; bw[2] = fr->first ? fb->b[2] : 0; bw[3] = fb->b[3]; }
            else if (s->wmode != WM_HTB) { bw[0] = fr->first ? fb->b[0] : 0; bw[1] = fb->b[1]; bw[2] = fr->last ? fb->b[2] : 0; bw[3] = fb->b[3]; }
            if (bw[0] | bw[1] | bw[2] | bw[3]) {
                if (L->nbord >= L->capbord) {
                    int nc = L->capbord ? L->capbord * 2 : 256;
                    struct dborder* n = (struct dborder*)w_realloc(L->bord, nc * sizeof(struct dborder));
                    if (!n) break;
                    L->bord = n;
                    L->capbord = nc;
                }
                struct dborder* d = &L->bord[L->nbord];
                int32_t r[4];
                radii_of(fb, r);
                for (int i = 0; i < 4; i++) { d->w[i] = bw[i]; d->c[i] = with_alpha(L, s->bc[i]); d->s[i] = s->bs[i]; d->r[i] = r[i]; }
                int k = di_add(L, DI_BORDER, x, y, fr->w, fr->h, 0);
                if (k >= 0) L->di[k].ref = L->nbord++;
            }
            int ln = fb->node >= 0 ? link_ancestor(L, fb->node) : -1;
            if (ln >= 0) hit_add(L, x, y, fr->w, fr->h, ln, HIT_LINK);
            break;
        }
        case FR_TEXT: case FR_MARKER: {
            const struct wstyle* s = fb->pst;
            int32_t x = ox + fr->x, base = oy + fr->y;
            if (fr->_p & 1) {              // rectangle form (the fragment went through a writing mode transform)
                if (s->wmode != WM_HTB) { emit_text_vert(L, s, x, base, fr->w, fr->h, fr->t0, fr->tl); break; }
                struct wfmetrics rm;
                lb_font_metrics(s, &rm);
                base += rm.ascent;
            }
            emit_text(L, s, x, base, fr->t0, fr->tl, fr->w, fr->word_extra);
            int node = fb->node;
            if (node < 0 && fb->parent >= 0) node = L->b[fb->parent].node;
            int ln = node >= 0 ? link_ancestor(L, node) : -1;
            if (ln >= 0) {
                struct wfmetrics m;
                lb_font_metrics(s, &m);
                hit_add(L, x, base - m.ascent, fr->w, m.ascent + m.descent, ln, HIT_LINK);
            }
            break;
        }
        case FR_MARKS: {
            const struct wstyle* s = fb->pst;
            int32_t x = ox + fr->x, base = oy + fr->y;
            if ((fr->_p & 1) && s->wmode != WM_HTB) {
                emit_marks(L, s, x, base, fr->w, fr->h, 0, fr->t0, fr->tl, 1);
            } else {
                int32_t b = base;
                if (fr->_p & 1) {                    // transformed back to horizontal: the rect's bottom is the annotation descent line
                    struct wfmetrics rm;
                    lb_font_metrics(s, &rm);
                    b = base + fr->h - rm.descent / 2;
                }
                emit_marks(L, s, x, 0, fr->w, 0, b, fr->t0, fr->tl, 0);
            }
            break;
        }
        case FR_ATOMIC:
            if (is_layer(L, fr->box)) break;
            emit_atomic_or_block(L, fr->box);
            break;
        }
    }
    (void)depth;
}

// Paint a non-layer box and its in-flow content (block-level, float or atomic).
static void emit_atomic_or_block(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->pst;
    uint8_t saved_alpha = L->alpha;
    int saved_fixed = L->fixed_ctx;
    int32_t saved_pt[4] = { L->poff_x, L->poff_y, L->psx, L->psy };
    if (b->flags & BF_FIXED) L->fixed_ctx = 1;
    if (s && s->opacity < 255) L->alpha = (uint8_t)((L->alpha * s->opacity + 127) / 255);
    if (L->alpha == 0) { L->alpha = saved_alpha; L->fixed_ctx = saved_fixed; return; }
    if (L->rank && bi < L->nrank) L->rank[bi] = ++L->rank_n;       // paint order, for hit testing
    if (s && (s->scale_x != 65536 || s->scale_y != 65536)) {
        // compose this box's scale around its transform-origin into the paint
        // transform (the origin maps to itself: off1 = T0(o) - o*S1)
        int32_t o = b->ax + wl_resolve(s->tfo_x, b->w, 0);
        int32_t oy = b->ay + wl_resolve(s->tfo_y, b->h, 0);
        int32_t nsx = w_muldiv(L->psx, s->scale_x, 65536);
        int32_t nsy = w_muldiv(L->psy, s->scale_y, 65536);
        L->poff_x = w_muldiv(o, L->psx, 65536) + L->poff_x - w_muldiv(o, nsx, 65536);
        L->poff_y = w_muldiv(oy, L->psy, 65536) + L->poff_y - w_muldiv(oy, nsy, 65536);
        L->psx = nsx; L->psy = nsy;
    }
    emit_bg_border(L, bi);
    b = &L->b[bi];
    if (b->flags & BF_MARKER) emit_marker(L, bi);
    // hit regions for controls and block-level links
    if (b->node >= 0 && L->d->n[b->node].type == WN_ELEM) {
        int rk_field = b->kind == LB_REPLACED && (b->rk == RK_INPUT_TEXT || b->rk == RK_TEXTAREA || b->rk == RK_SELECT);
        int rk_btn = (b->kind == LB_REPLACED && (b->rk == RK_INPUT_BUTTON || b->rk == RK_CHECKBOX || b->rk == RK_RADIO)) ||
                     wdom_is(L->d, b->node, T_button);
        if (rk_field) hit_add(L, b->ax, b->ay, b->w, b->h, b->node, HIT_FIELD);
        else if (rk_btn) hit_add(L, b->ax, b->ay, b->w, b->h, b->node, HIT_BUTTON);
        else if (b->kind == LB_BLOCK || b->kind == LB_REPLACED) {
            int ln = link_ancestor(L, b->node);
            if (ln >= 0) hit_add(L, b->ax, b->ay, b->w, b->h, ln, HIT_LINK);
        }
    }
    if (b->kind == LB_REPLACED) {
        emit_replaced(L, bi);
    } else {
        int paint_contain = s && (s->contain & CT_PAINT);
        int clip = s && (s->overflow_x != OV_VISIBLE || s->overflow_y != OV_VISIBLE || paint_contain);
        if (clip) {
            int32_t cx = b->ax + b->b[3], cy = b->ay + b->b[0];
            int32_t cw = b->w - b->b[1] - b->b[3], ch = b->h - b->b[0] - b->b[2];
            int32_t big = PX(1000000);
            // overflow-x visible + y hidden still clips both axes in practice
            if (!paint_contain) {
                if (s->overflow_x == OV_VISIBLE && s->overflow_y != OV_VISIBLE) { cx -= big / 2; cw += big; }
                if (s->overflow_y == OV_VISIBLE && s->overflow_x != OV_VISIBLE) { cy -= big / 2; ch += big; }
                ocm_adjust(b, s, &cx, &cy, &cw, &ch);
            }
            di_add(L, DI_CLIP, cx, cy, cw, ch, 0);
        }
        if (b->flags & BF_IFC) emit_ifc(L, bi, 0);
        else emit_flow(L, bi, 0);
        if (clip) di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
    }
    L->poff_x = saved_pt[0]; L->poff_y = saved_pt[1]; L->psx = saved_pt[2]; L->psy = saved_pt[3];
    L->alpha = saved_alpha;
    L->fixed_ctx = saved_fixed;
}

static void emit_flow(struct wlayout* L, int bi, int depth) {
    if (depth > 400) return;
    // block-level in-flow children first, then floats (atomic), per CSS 2 E.2
    for (int pass = 0; pass < 2; pass++) {
        for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) {
            struct lbox* cb = &L->b[c];
            if (cb->kind == LB_TEXT || cb->kind == LB_INLINE || cb->kind == LB_BR || cb->kind == LB_WBR ||
                cb->kind == LB_MARKER)
                continue;
            if (!(cb->flags & BF_LAID)) continue;
            if (is_layer(L, c)) continue;
            int is_float = (cb->flags & BF_FLOAT) != 0;
            if (is_float != pass) continue;
            if (cb->flags & BF_INLINE_LEVEL) continue; // painted by its IFC
            emit_atomic_or_block(L, c);
        }
    }
}

static int z_of(struct wlayout* L, int bi) {
    const struct wstyle* s = L->b[bi].st;
    return s->z_auto ? 0 : s->z_index;
}

static void sort_layers(struct wlayout* L, int* v, int n) {
    for (int i = 1; i < n; i++) {
        int k = v[i], kz = z_of(L, k), j = i - 1;
        while (j >= 0 && z_of(L, v[j]) > kz) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}

static void emit_layer(struct wlayout* L, int bi, int depth) {
    if (depth > 64) return;
    struct layers ls = { 0, 0, 0 };
    if (bi == L->root || creates_ctx(L, bi)) collect_layers(L, bi, &ls, 0);       // (a box that starts no context: its layers were hoisted)
    sort_layers(L, ls.v, ls.n);
    uint8_t saved_alpha = L->alpha;
    int saved_fixed = L->fixed_ctx;
    struct lbox* b = &L->b[bi];
    if (b->flags & BF_FIXED) L->fixed_ctx = 1;
    if (b->pst && b->pst->opacity < 255) L->alpha = (uint8_t)((L->alpha * b->pst->opacity + 127) / 255);
    // a positioned box is clipped by overflow != visible ancestors up to and including
    // its containing block (the nearest positioned one): above that it escapes the clip.
    // this paints the hoisted positioned descendants of an overflow-clipped layer inside it
    int nclip = 0;
    for (int a = b->parent; a >= 0 && nclip < 6; a = L->b[a].parent) {
        struct lbox* ab = &L->b[a];
        const struct wstyle* as = ab->pst;
        if (!as) break;
        int a_paint = (as->contain & CT_PAINT) != 0;
        if ((as->overflow_x != OV_VISIBLE || as->overflow_y != OV_VISIBLE || a_paint) && (ab->flags & BF_LAID)) {
            int32_t cx = ab->ax + ab->b[3], cy = ab->ay + ab->b[0];
            int32_t cw = ab->w - ab->b[1] - ab->b[3], ch = ab->h - ab->b[0] - ab->b[2];
            int32_t big = PX(1000000);
            if (!a_paint) {
                if (as->overflow_x == OV_VISIBLE && as->overflow_y != OV_VISIBLE) { cx -= big / 2; cw += big; }
                if (as->overflow_y == OV_VISIBLE && as->overflow_x != OV_VISIBLE) { cy -= big / 2; ch += big; }
                ocm_adjust(ab, as, &cx, &cy, &cw, &ch);
            }
            di_add(L, DI_CLIP, cx, cy, cw, ch, 0);
            nclip++;
        }
        if (as->position != POS_STATIC) break;   // the containing block is the last clipper
    }
    // negative z
    for (int i = 0; i < ls.n; i++) if (z_of(L, ls.v[i]) < 0) emit_layer(L, ls.v[i], depth + 1);
    L->alpha = saved_alpha;
    L->fixed_ctx = saved_fixed;
    if (bi == L->root) emit_flow(L, bi, 0);
    else emit_atomic_or_block(L, bi);
    if (b->flags & BF_FIXED) L->fixed_ctx = 1;
    if (b->pst && b->pst->opacity < 255) L->alpha = (uint8_t)((L->alpha * b->pst->opacity + 127) / 255);
    // the layer's own overflow clip applies to its positioned descendants too
    int lpc = b->pst && (b->pst->contain & CT_PAINT);
    int clip = bi != L->root && b->pst && (b->pst->overflow_x != OV_VISIBLE || b->pst->overflow_y != OV_VISIBLE || lpc);
    if (clip) {
        int32_t cx = b->ax + b->b[3], cy = b->ay + b->b[0], cw = b->w - b->b[1] - b->b[3], ch = b->h - b->b[0] - b->b[2];
        if (!lpc) ocm_adjust(b, b->pst, &cx, &cy, &cw, &ch);
        di_add(L, DI_CLIP, cx, cy, cw, ch, 0);
    }
    for (int i = 0; i < ls.n; i++) if (z_of(L, ls.v[i]) >= 0) emit_layer(L, ls.v[i], depth + 1);
    if (clip) di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
    for (int i = 0; i < nclip; i++) di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
    L->alpha = saved_alpha;
    L->fixed_ctx = saved_fixed;
    w_free(ls.v);
}

void lay_emit(struct wlayout* L) {
    w_free(L->rank);
    L->nrank = L->nb;
    L->rank_n = 0;
    L->rank = (int32_t*)w_malloc((size_t)(L->nb + 1) * sizeof(int32_t));
    if (L->rank) memset(L->rank, 0, (size_t)(L->nb + 1) * sizeof(int32_t));
    L->ndi = 0;
    L->alpha = 255;
    L->fixed_ctx = 0;
    emit_layer(L, L->root, 0);
}

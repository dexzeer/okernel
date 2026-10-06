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
        r[i] = wl_resolve(b->st->radius[i], b->w < b->h ? b->w : b->h, 0);
        if (r[i] < 0) r[i] = 0;
    }
}

// ---- backgrounds / borders --------------------------------------------------------

static void emit_bg_border(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    if (!s || s->visibility) return;
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
    // the root element's background is the canvas; body's when propagated
    int skip_bg = 0;
    if (b->node >= 0 && b->node == L->d->html) skip_bg = 1;
    if (b->node >= 0 && b->node == L->d->body && L->d->html >= 0) {
        const struct wstyle* hs = css_style_of(L->ss, L->d->html);
        if (hs && !(hs->bg_color >> 24) && hs->bg_grad < 0 && !hs->bg_image) skip_bg = 1;
    }
    if (s->has_mask) skip_bg = 1; // mask-image unsupported: never paint an unmasked fill
    if (!skip_bg && (s->bg_color >> 24)) {
        int k = di_add(L, DI_RECT, x, y, w, h, with_alpha(L, s->bg_color));
        if (k >= 0) L->di[k].ref = rref;
    }
    if (!skip_bg && s->bg_grad >= 0) {
        int k = di_add(L, DI_GRAD, x, y, w, h, 0xFF000000);
        if (k >= 0) { L->di[k].a = s->bg_grad; L->di[k].ref = rref; }
    }
    if (s->bg_image && !s->has_mask && L->imgs && L->imgs->for_url) {
        int iw = 0, ih = 0;
        int img = L->imgs->for_url(L->imgs->ctx, css_str(L->ss, s->bg_image, s->bg_image_len), &iw, &ih);
        if (img >= 0 && iw > 0 && ih > 0) {
            // positioning area = padding box
            int32_t px = x + b->b[3], py = y + b->b[0];
            int32_t pw = w - b->b[1] - b->b[3], ph = h - b->b[0] - b->b[2];
            int32_t tw = PX(iw), th = PX(ih);
            if (s->bg_size_kind == BGS_COVER || s->bg_size_kind == BGS_CONTAIN) {
                int32_t sx = w_muldiv(pw, 1000, tw), sy = w_muldiv(ph, 1000, th);
                int32_t sc = (s->bg_size_kind == BGS_COVER) ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
                tw = w_muldiv(tw, sc, 1000);
                th = w_muldiv(th, sc, 1000);
            } else if (s->bg_size_kind == BGS_LEN) {
                int32_t W = s->bg_size[0].t == WL_LEN ? wl_resolve(s->bg_size[0], pw, 0) : -1;
                int32_t H = s->bg_size[1].t == WL_LEN ? wl_resolve(s->bg_size[1], ph, 0) : -1;
                if (W >= 0 && H < 0) { H = w_muldiv(th, W, tw > 0 ? tw : 1); }
                else if (H >= 0 && W < 0) { W = w_muldiv(tw, H, th > 0 ? th : 1); }
                if (W >= 0) tw = W;
                if (H >= 0) th = H;
            }
            if (tw > 0 && th > 0) {
                int32_t ox = px + wl_resolve(s->bg_pos[0], pw - tw, 0);
                int32_t oy = py + wl_resolve(s->bg_pos[1], ph - th, 0);
                if (L->ndbg >= L->capdbg) {
                    int nc = L->capdbg ? L->capdbg * 2 : 64;
                    struct dbgimg* n = (struct dbgimg*)w_realloc(L->dbg, nc * sizeof(struct dbgimg));
                    if (n) { L->dbg = n; L->capdbg = nc; }
                }
                if (L->ndbg < L->capdbg) {
                    struct dbgimg* bg = &L->dbg[L->ndbg];
                    bg->img = img; bg->ix = ox; bg->iy = oy; bg->iw = tw; bg->ih = th;
                    bg->repeat = s->bg_repeat;
                    int k = di_add(L, DI_BGIMG, x, y, w, h, 0xFF000000);
                    if (k >= 0) L->di[k].ref = L->ndbg++;
                }
            }
        }
    }
    if (b->b[0] | b->b[1] | b->b[2] | b->b[3]) {
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
    t->t0 = t0; t->tl = tl;
    t->family = f.family; t->bold = f.bold; t->italic = f.italic; t->px = f.px;
    t->deco = s->deco_line | s->deco_inh;
    t->deco_style = s->deco_style;
    t->deco_color = with_alpha(L, s->deco_line ? s->deco_color : s->deco_inh_color);
    t->letter_sp = s->letter_spacing;
    t->word_sp = s->word_spacing + word_extra;
    t->width = width;
    struct wfmetrics m;
    wfont_metrics(&f, &m);
    int k = di_add(L, DI_TEXT, x, base - m.ascent, width, m.ascent + m.descent, with_alpha(L, s->color));
    if (k >= 0) { L->di[k].ref = L->ndtx++; L->di[k].a = base; }
}


static void emit_atomic_or_block(struct wlayout* L, int bi);

// replaced element content
static void emit_replaced(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    const struct wstyle* s = b->st;
    int32_t cx = b->ax + b->b[3] + b->p[3], cy = b->ay + b->b[0] + b->p[0];
    int32_t cw = b->w - b->b[1] - b->b[3] - b->p[1] - b->p[3];
    int32_t ch = b->h - b->b[0] - b->b[2] - b->p[0] - b->p[2];
    if (s->visibility) return;
    switch (b->rk) {
    case RK_IMG: case RK_VIDEO:
        if (b->img >= 0 && b->iw > 0 && b->ih > 0 && cw > 0 && ch > 0) {
            int32_t dx = cx, dy = cy, dw = cw, dh = ch;
            if (s->object_fit == OF_CONTAIN || s->object_fit == OF_COVER || s->object_fit == OF_SCALE_DOWN) {
                int32_t sx = w_muldiv(cw, 1000, PX(b->iw)), sy = w_muldiv(ch, 1000, PX(b->ih));
                int32_t sc = s->object_fit == OF_COVER ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
                if (s->object_fit == OF_SCALE_DOWN && sc > 1000) sc = 1000;
                dw = w_muldiv(PX(b->iw), sc, 1000);
                dh = w_muldiv(PX(b->ih), sc, 1000);
                dx = cx + (cw - dw) / 2;
                dy = cy + (ch - dh) / 2;
            }
            int clip = s->object_fit == OF_COVER;
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
                        emit_text(L, &tmp, cx, yy, off + st, (uint32_t)(i - st), wfont_measure(&f, val + st, i - st), 0);
                        yy += lh;
                        st = i + 1;
                    }
                }
            } else {
                int32_t tw = wfont_measure(&f, val, n);
                int32_t tx = cx;
                if (b->rk == RK_INPUT_BUTTON) tx = cx + (cw - tw) / 2;
                int32_t base = cy + (ch - lh) / 2 + (lh - m.ascent - m.descent) / 2 + m.ascent;
                emit_text(L, &tmp, tx, base, off, (uint32_t)n, tw, 0);
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
    const struct wstyle* s = b->st;
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
    return b->st && (b->kind == LB_BLOCK || b->kind == LB_REPLACED) && b->st->position != POS_STATIC;
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
        if (is_layer(L, c)) { layers_push(ls, c); continue; }
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
            const struct wstyle* s = fb->st;
            if (s->visibility || is_layer(L, fr->box)) break;
            int32_t x = ox + fr->x, y = oy + fr->y;
            if (s->bg_color >> 24) di_add(L, DI_RECT, x, y, fr->w, fr->h, with_alpha(L, s->bg_color));
            if (s->bg_grad >= 0) { int k = di_add(L, DI_GRAD, x, y, fr->w, fr->h, 0xFF000000); if (k >= 0) L->di[k].a = s->bg_grad; }
            int32_t bw[4] = { fb->b[0], fr->last ? fb->b[1] : 0, fb->b[2], fr->first ? fb->b[3] : 0 };
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
            const struct wstyle* s = fb->st;
            int32_t x = ox + fr->x, base = oy + fr->y;
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
    const struct wstyle* s = b->st;
    uint8_t saved_alpha = L->alpha;
    int saved_fixed = L->fixed_ctx;
    if (b->flags & BF_FIXED) L->fixed_ctx = 1;
    if (s && s->opacity < 255) L->alpha = (uint8_t)((L->alpha * s->opacity + 127) / 255);
    if (L->alpha == 0) { L->alpha = saved_alpha; L->fixed_ctx = saved_fixed; return; }
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
        int clip = s && (s->overflow_x != OV_VISIBLE || s->overflow_y != OV_VISIBLE);
        if (clip) {
            int32_t cx = b->ax + b->b[3], cy = b->ay + b->b[0];
            int32_t cw = b->w - b->b[1] - b->b[3], ch = b->h - b->b[0] - b->b[2];
            int32_t big = PX(1000000);
            // overflow-x visible + y hidden still clips both axes in practice
            if (s->overflow_x == OV_VISIBLE && s->overflow_y != OV_VISIBLE) { cx -= big / 2; cw += big; }
            if (s->overflow_y == OV_VISIBLE && s->overflow_x != OV_VISIBLE) { cy -= big / 2; ch += big; }
            di_add(L, DI_CLIP, cx, cy, cw, ch, 0);
        }
        if (b->flags & BF_IFC) emit_ifc(L, bi, 0);
        else emit_flow(L, bi, 0);
        if (clip) di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
    }
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
    collect_layers(L, bi, &ls, 0);
    sort_layers(L, ls.v, ls.n);
    uint8_t saved_alpha = L->alpha;
    int saved_fixed = L->fixed_ctx;
    struct lbox* b = &L->b[bi];
    if (b->flags & BF_FIXED) L->fixed_ctx = 1;
    if (b->st && b->st->opacity < 255) L->alpha = (uint8_t)((L->alpha * b->st->opacity + 127) / 255);
    // negative z
    for (int i = 0; i < ls.n; i++) if (z_of(L, ls.v[i]) < 0) emit_layer(L, ls.v[i], depth + 1);
    L->alpha = saved_alpha;
    L->fixed_ctx = saved_fixed;
    if (bi == L->root) emit_flow(L, bi, 0);
    else emit_atomic_or_block(L, bi);
    if (b->flags & BF_FIXED) L->fixed_ctx = 1;
    if (b->st && b->st->opacity < 255) L->alpha = (uint8_t)((L->alpha * b->st->opacity + 127) / 255);
    // the layer's own overflow clip applies to its positioned descendants too
    int clip = bi != L->root && b->st && (b->st->overflow_x != OV_VISIBLE || b->st->overflow_y != OV_VISIBLE);
    if (clip) di_add(L, DI_CLIP, b->ax + b->b[3], b->ay + b->b[0], b->w - b->b[1] - b->b[3], b->h - b->b[0] - b->b[2], 0);
    for (int i = 0; i < ls.n; i++) if (z_of(L, ls.v[i]) >= 0) emit_layer(L, ls.v[i], depth + 1);
    if (clip) di_add(L, DI_UNCLIP, 0, 0, 0, 0, 0);
    L->alpha = saved_alpha;
    L->fixed_ctx = saved_fixed;
    w_free(ls.v);
}

void lay_emit(struct wlayout* L) {
    L->ndi = 0;
    L->alpha = 255;
    L->fixed_ctx = 0;
    emit_layer(L, L->root, 0);
}

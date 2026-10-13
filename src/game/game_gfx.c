// The renderer: flat, bold, cartoony shapes straight into an ARGB pixel buffer. No anti-aliasing on purpose (it is the look,
// and a 1.6 GHz Atom has better things to do). The view is 40 pixels per meter at 960x540 and scales with the buffer.
#include "game_internal.h"

double sin(double);
double cos(double);

#define OPAQUE 0xFF000000u

struct surf { uint32_t* px; int w, h, stride; };

static inline int fl(double x) { int i = (int)x; return i - (x < (double)i); }

static inline uint32_t mix(uint32_t a, uint32_t b, int t256) {     // a at (256 - t), b at t
    uint32_t r = (((a >> 16) & 255) * (256 - t256) + ((b >> 16) & 255) * t256) >> 8;
    uint32_t g = (((a >> 8) & 255) * (256 - t256) + ((b >> 8) & 255) * t256) >> 8;
    uint32_t bl = ((a & 255) * (256 - t256) + (b & 255) * t256) >> 8;
    return OPAQUE | (r << 16) | (g << 8) | bl;
}

static inline void put(struct surf* s, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[y * s->stride + x] = OPAQUE | c;
}

static void hspan(struct surf* s, int x0, int x1, int y, uint32_t c) {
    if ((unsigned)y >= (unsigned)s->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    uint32_t* row = s->px + y * s->stride;
    for (int x = x0; x <= x1; x++) row[x] = OPAQUE | c;
}

static void rect(struct surf* s, int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) hspan(s, x, x + w - 1, y + j, c);
}

static void rect_blend(struct surf* s, int x, int y, int w, int h, uint32_t c, int t256) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= (unsigned)s->h) continue;
        for (int i = x; i < x + w; i++) if ((unsigned)i < (unsigned)s->w) s->px[j * s->stride + i] = mix(s->px[j * s->stride + i], c, t256);
    }
}

static void line(struct surf* s, int x0, int y0, int x1, int y1, uint32_t c, int thick) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    int guard = dx + dy + 2;
    int half = thick / 2;
    while (guard-- > 0) {
        for (int j = -half; j < thick - half; j++) for (int i = -half; i < thick - half; i++) put(s, x0 + i, y0 + j, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void fill_poly(struct surf* s, const int* xs, const int* ys, int n, uint32_t c) {      // convex
    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) { if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    if (ymin < 0) ymin = 0;
    if (ymax >= s->h) ymax = s->h - 1;
    for (int y = ymin; y <= ymax; y++) {
        int lo = 1 << 30, hi = -(1 << 30);
        for (int i = 0; i < n; i++) {
            int x0 = xs[i], y0 = ys[i], x1 = xs[(i + 1) % n], y1 = ys[(i + 1) % n];
            if ((y0 > y && y1 > y) || (y0 < y && y1 < y)) continue;
            if (y0 == y1) { if (x0 < lo) lo = x0; if (x1 < lo) lo = x1; if (x0 > hi) hi = x0; if (x1 > hi) hi = x1; continue; }
            int x = x0 + (int)((long long)(x1 - x0) * (y - y0) / (y1 - y0));
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        if (lo <= hi) hspan(s, lo, hi, y, c);
    }
}

static void poly_outline(struct surf* s, const int* xs, const int* ys, int n, uint32_t c, int thick) {
    for (int i = 0; i < n; i++) line(s, xs[i], ys[i], xs[(i + 1) % n], ys[(i + 1) % n], c, thick);
}

static void fill_circle(struct surf* s, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= r * r + r) w++;
        hspan(s, cx - w, cx + w, cy + dy, c);
    }
}

// ---- text: a 5x7 font written for this game ---------------------------------------------------------------------------

static const uint8_t FONT[][7] = {
    /* A */ { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* B */ { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, /* C */ { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    /* D */ { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, /* E */ { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, /* F */ { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    /* G */ { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, /* H */ { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* I */ { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* J */ { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, /* K */ { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* L */ { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    /* M */ { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* N */ { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }, /* O */ { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* P */ { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, /* Q */ { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, /* R */ { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    /* S */ { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, /* T */ { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* U */ { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* V */ { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, /* W */ { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 }, /* X */ { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    /* Y */ { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, /* Z */ { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    /* 0 */ { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 1 */ { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 2 */ { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
    /* 3 */ { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E }, /* 4 */ { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 5 */ { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    /* 6 */ { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 7 */ { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 8 */ { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    /* 9 */ { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    /* . */ { 0, 0, 0, 0, 0, 0x0C, 0x0C }, /* , */ { 0, 0, 0, 0, 0x0C, 0x04, 0x08 }, /* ! */ { 0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04 }, /* ? */ { 0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04 },
    /* - */ { 0, 0, 0, 0x1F, 0, 0, 0 }, /* : */ { 0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0 }, /* / */ { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 }, /* ' */ { 0x04, 0x04, 0x08, 0, 0, 0, 0 },
};

static const uint8_t* glyph(char ch) {
    static const uint8_t blank[7] = { 0 };
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return FONT[ch - 'A'];
    if (ch >= '0' && ch <= '9') return FONT[26 + ch - '0'];
    static const char EXTRA[] = ".,!?-:/'";
    for (int i = 0; EXTRA[i]; i++) if (EXTRA[i] == ch) return FONT[36 + i];
    return blank;
}

static int text_width(const char* t, int scale) { int n = 0; while (*t++) n++; return n * 6 * scale - scale; }

static void draw_text(struct surf* s, int x, int y, const char* t, int scale, uint32_t c) {
    for (; *t; t++, x += 6 * scale) {
        const uint8_t* gl = glyph(*t);
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++)
                if (gl[r] & (0x10 >> col)) rect(s, x + col * scale, y + r * scale, scale, scale, c);
    }
}

static void draw_text_shadow(struct surf* s, int x, int y, const char* t, int scale, uint32_t c) {
    draw_text(s, x + scale, y + scale, t, scale, 0x101018);
    draw_text(s, x, y, t, scale, c);
}

// ---- the view ---------------------------------------------------------------------------------------------------------

struct view { double ppm, cx, cy; int ox, oy; };

static inline int sx(const struct view* v, double wx) { return fl((wx - v->cx) * v->ppm) + v->ox; }
static inline int sy(const struct view* v, double wy) { return fl((wy - v->cy) * v->ppm) + v->oy; }

static void draw_body(struct surf* s, const struct view* v, const struct ph_body* b, uint32_t fill, uint32_t edge) {
    int thick = v->ppm >= 30 ? 2 : 1;
    if (b->shape == SH_CIRCLE) {
        int r = fl(b->radius * v->ppm + 0.5);
        if (r < 1) r = 1;
        int cx = sx(v, b->p.x), cy = sy(v, b->p.y);
        fill_circle(s, cx, cy, r, edge);
        fill_circle(s, cx, cy, r > thick ? r - thick : r, fill);
        line(s, cx, cy, cx + fl(cos(b->a) * r * 0.8), cy + fl(sin(b->a) * r * 0.8), edge, 1);      // a spoke, so you can see it roll
        return;
    }
    int xs[PH_MAX_VERTS], ys[PH_MAX_VERTS];
    for (int i = 0; i < b->nv; i++) {
        v2 p = ph_world_point(b, b->vert[i]);
        xs[i] = sx(v, p.x);
        ys[i] = sy(v, p.y);
    }
    fill_poly(s, xs, ys, b->nv, fill);
    poly_outline(s, xs, ys, b->nv, edge, thick);
}

static v2 local_to_screen_pt(const struct ph_body* b, double lx, double ly) { return ph_world_point(b, (v2){ lx, ly }); }

static void draw_player(struct game* g, struct surf* s, const struct view* v, struct g_ent* E) {
    struct ph_world* w = &g->world;
    // back to front: far arm and leg, chair, torso, near arm and leg, head
    static const int ORDER[] = { PART_UARM_R, PART_FARM_R, PART_THIGH_R, PART_SHIN_R, PART_WHEEL, PART_CASTER, PART_CHAIR, PART_THIGH_L, PART_SHIN_L, PART_TORSO,
                                 PART_UARM_L, PART_FARM_L, PART_HEAD };
    for (unsigned i = 0; i < sizeof ORDER / sizeof ORDER[0]; i++) {
        int part = ORDER[i];
        int bi = E->body[part];
        if (bi < 0 || !w->body[bi].used) continue;
        struct ph_body* b = &w->body[bi];
        draw_body(s, v, b, E->part_fill[part], 0x14141c);
        if (part == PART_WHEEL) {                                     // the big wheel's spokes + hand rim
            int cx = sx(v, b->p.x), cy = sy(v, b->p.y), r = fl(b->radius * v->ppm);
            for (int k = 0; k < 3; k++) {
                double a = b->a + k * 1.0471975512;
                line(s, cx - fl(cos(a) * r * 0.85), cy - fl(sin(a) * r * 0.85), cx + fl(cos(a) * r * 0.85), cy + fl(sin(a) * r * 0.85), 0x9aa0a8, 1);
            }
            fill_circle(s, cx, cy, r / 6 + 1, 0x9aa0a8);
        }
        if (part == PART_CHAIR) {                                     // backrest, handle, footplate
            struct ph_body* c = b;
            v2 a1 = local_to_screen_pt(c, -0.20, 0.0), a2 = local_to_screen_pt(c, -0.26, -0.75), a3 = local_to_screen_pt(c, -0.48, -0.75);
            v2 f1 = local_to_screen_pt(c, 0.10, 0.0), f2 = local_to_screen_pt(c, 0.67, 0.58);
            line(s, sx(v, a1.x), sy(v, a1.y), sx(v, a2.x), sy(v, a2.y), 0x8a929c, 3);
            line(s, sx(v, a2.x), sy(v, a2.y), sx(v, a3.x), sy(v, a3.y), 0x8a929c, 3);
            line(s, sx(v, f1.x), sy(v, f1.y), sx(v, f2.x), sy(v, f2.y), 0x8a929c, 2);
        }
        if (part == PART_HEAD) {                                      // a face: eyes forward, X eyes when dead
            struct ph_body* hd = b;
            double r = hd->radius;
            v2 e1 = local_to_screen_pt(hd, r * 0.25, -r * 0.2), e2 = local_to_screen_pt(hd, r * 0.62, -r * 0.2), mo = local_to_screen_pt(hd, r * 0.45, r * 0.35);
            int ex1 = sx(v, e1.x), ey1 = sy(v, e1.y), ex2 = sx(v, e2.x), ey2 = sy(v, e2.y);
            if (E->dead) {
                for (int q = 0; q < 2; q++) {
                    int ex = q ? ex2 : ex1, ey = q ? ey2 : ey1;
                    line(s, ex - 2, ey - 2, ex + 2, ey + 2, 0x14141c, 1);
                    line(s, ex - 2, ey + 2, ex + 2, ey - 2, 0x14141c, 1);
                }
                line(s, sx(v, mo.x) - 3, sy(v, mo.y), sx(v, mo.x) + 3, sy(v, mo.y) + 1, 0x14141c, 1);
            } else {
                fill_circle(s, ex1, ey1, 2, 0xffffff); put(s, ex1 + 1, ey1, 0x14141c);
                fill_circle(s, ex2, ey2, 2, 0xffffff); put(s, ex2 + 1, ey2, 0x14141c);
                fill_circle(s, sx(v, mo.x), sy(v, mo.y), 1, 0x14141c);
            }
        }
    }
}

static void draw_goal(struct game* g, struct surf* s, const struct view* v, struct g_ent* E) {
    struct ph_body* b = &g->world.body[E->body[0]];
    int px0 = sx(v, b->p.x), top = sy(v, b->p.y - 1.2), bot = sy(v, b->p.y + 1.2);
    line(s, px0, top, px0, bot, 0xeeeeee, 3);
    double wave = sin(g->sky_t * 5.0) * 0.12;
    int xs[4] = { px0, sx(v, b->p.x + 0.9), sx(v, b->p.x + 0.8 + wave * 1.5), px0 };
    int ys[4] = { top, sy(v, b->p.y - 1.2 + 0.2 + wave), sy(v, b->p.y - 1.2 + 0.5), sy(v, b->p.y - 1.2 + 0.7) };
    fill_poly(s, xs, ys, 4, 0x4cd264);
    poly_outline(s, xs, ys, 4, 0x14541f, 1);
}

static void draw_hills(struct game* g, struct surf* s, const struct view* v) {
    // sky: a plain vertical gradient, then two layers of rolling hills that move slower than the ground
    for (int y = 0; y < s->h; y++) hspan(s, 0, s->w - 1, y, mix(0x6fc3ff, 0xe6f7ff, y * 256 / (s->h ? s->h : 1) > 255 ? 255 : y * 256 / (s->h ? s->h : 1)));
    for (int layer = 0; layer < 3; layer++) {
        double par = 0.15 + layer * 0.18;
        uint32_t color = layer == 0 ? 0xa9d9c8 : layer == 1 ? 0x86c7a8 : 0x5fae85;
        double base = s->h * (0.55 + layer * 0.12);
        for (int x = 0; x < s->w; x += 2) {
            double wx = (x - v->ox) / v->ppm * 1.0 + v->cx * par;
            double hgt = sin(wx * (0.35 + layer * 0.17) + layer * 2.0) * 22 + sin(wx * 0.9 + layer) * 8;
            int top = fl(base - hgt - (v->cy * v->ppm * par * 0.2));
            for (int xx = x; xx < x + 2 && xx < s->w; xx++) for (int y = top < 0 ? 0 : top; y < s->h; y++) s->px[y * s->stride + xx] = OPAQUE | color;
        }
    }
    // clouds
    for (int i = 0; i < 5; i++) {
        double cx = (i * 9.3 + g->sky_t * 0.25) - v->cx * 0.1;
        cx = cx - 46.0 * fl(cx / 46.0) - 12.0;
        int px0 = fl(cx * v->ppm * 0.35) + s->w / 4, py = fl(s->h * (0.12 + 0.05 * (i % 3)));
        fill_circle(s, px0, py, 18, 0xffffff);
        fill_circle(s, px0 + 18, py + 4, 14, 0xffffff);
        fill_circle(s, px0 - 18, py + 5, 12, 0xffffff);
    }
}

void g_draw(struct game* g, uint32_t* px, int w, int h, int stride) {
    struct surf S = { px, w, h, stride };
    struct surf* s = &S;
    struct view v;
    v.ppm = G_PPM * g->cam_zoom * (h / 540.0);
    double shx = g->shake > 0.01 ? (g_rand(g) - 0.5) * g->shake * v.ppm * 0.4 : 0, shy = g->shake > 0.01 ? (g_rand(g) - 0.5) * g->shake * v.ppm * 0.4 : 0;
    v.cx = g->cam_x;
    v.cy = g->cam_y;
    v.ox = w / 2 + fl(shx);
    v.oy = h / 2 + fl(shy);
    g->sky_t += 1.0 / 60.0;
    draw_hills(g, s, &v);

    // static level first, then loose things, then the player on top
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < G_MAX_ENTS; i++) {
            struct g_ent* E = &g->ent[i];
            if (!E->used || !E->nbody) continue;
            int layer = E->kind == K_PLAYER ? 2 : (E->kind == K_BLOCK || E->kind == K_RAMP || E->kind == K_SPIKES || E->kind == K_GOAL) ? 0 : 1;
            if (layer != pass) continue;
            if (E->kind == K_PLAYER) { draw_player(g, s, &v, E); continue; }
            if (E->kind == K_GOAL) { draw_goal(g, s, &v, E); continue; }
            for (int k = 0; k < E->nbody; k++) {
                if (E->body[k] < 0 || !g->world.body[E->body[k]].used) continue;
                struct ph_body* b = &g->world.body[E->body[k]];
                uint32_t fill = E->fill;
                if (E->kind == K_BLOCK || E->kind == K_RAMP) {
                    // ground: dark earth with a grass lip on the top face
                    draw_body(s, &v, b, fill, E->line);
                    v2 a = ph_world_point(b, (v2){ b->vert[0].x, b->vert[0].y }), c = ph_world_point(b, (v2){ b->vert[1].x, b->vert[1].y });
                    line(s, sx(&v, a.x), sy(&v, a.y) + 1, sx(&v, c.x), sy(&v, c.y) + 1, 0x58b84a, 4);
                    continue;
                }
                draw_body(s, &v, b, fill, E->line);
                if (E->kind == K_CRATE && b->shape == SH_POLY) {          // the X on a crate
                    v2 p0 = ph_world_point(b, b->vert[0]), p1 = ph_world_point(b, b->vert[1]), p2 = ph_world_point(b, b->vert[2]), p3 = ph_world_point(b, b->vert[3]);
                    line(s, sx(&v, p0.x), sy(&v, p0.y), sx(&v, p2.x), sy(&v, p2.y), E->line, 1);
                    line(s, sx(&v, p1.x), sy(&v, p1.y), sx(&v, p3.x), sy(&v, p3.y), E->line, 1);
                }
                if (E->kind == K_BARREL) {
                    int cx = sx(&v, b->p.x), cy = sy(&v, b->p.y), r = fl(b->radius * v.ppm);
                    line(s, cx - r + 2, cy - r / 3, cx + r - 2, cy - r / 3, 0xffe27a, 2);
                    line(s, cx - r + 2, cy + r / 3, cx + r - 2, cy + r / 3, 0xffe27a, 2);
                }
            }
        }
    }

    // particles and floating words
    for (int i = 0; i < g->npart; i++) {
        struct g_particle* p = &g->part[i];
        int x = sx(&v, p->x), y = sy(&v, p->y);
        int size = p->size * (h > 700 ? 2 : 1);
        if (p->shape == 1) { rect(s, x - size / 2, y - 1, size, 2, p->color); rect(s, x - 1, y - size / 2, 2, size, p->color); }
        else rect(s, x - size / 2, y - size / 2, size, size, p->color);
    }
    for (int i = 0; i < G_MAX_TEXTS; i++)
        if (g->text[i].life > 0) draw_text_shadow(s, sx(&v, g->text[i].x) - text_width(g->text[i].s, 2) / 2, sy(&v, g->text[i].y), g->text[i].s, 2, 0xffffff);
    if (g->flash > 0) rect_blend(s, 0, 0, w, h, 0xffffff, 100 * g->flash);

    // heads-up display
    int ts = h >= 800 ? 4 : 2;
    char buf[40] = "TIME ";
    {
        char num[32];
        dm_fmt_num(fl(g->time * 10) / 10.0, num);
        size_t n = strlen(buf);
        for (size_t i = 0; num[i] && n + 1 < sizeof buf; i++) buf[n++] = num[i];
        if (!strchr(num, '.') && n + 3 < sizeof buf) { buf[n++] = '.'; buf[n++] = '0'; }
        buf[n] = 0;
    }
    draw_text_shadow(s, 12, 10, buf, ts, 0xffffff);
    const char* msg = game_message(g);
    if (*msg) {
        int ms = h >= 800 ? 6 : 3;
        while (ms > 1 && text_width(msg, ms) > w - 20) ms--;
        draw_text_shadow(s, (w - text_width(msg, ms)) / 2, h / 6, msg, ms, g->state == GS_DEAD ? 0xff5050 : 0xffe24a);
    }
    if (g->time < 6.0 && g->state == GS_PLAYING) {
        const char* hint = "ARROWS: DRIVE AND LEAN   SPACE: EJECT   R: RESTART";
        int hs = h >= 800 ? 3 : 2;
        while (hs > 1 && text_width(hint, hs) > w - 20) hs--;
        draw_text_shadow(s, (w - text_width(hint, hs)) / 2, h - 14 - 7 * hs, hint, hs, 0xffffff);
    }
}

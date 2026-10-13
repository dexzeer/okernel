// The game world: entities, the level API scripts call (world:, fx:, game: modules), events between physics and dmccl.
//
// A level script builds the world in `when run`, then reacts:
//     when guy touches "spikes" do  fx:blood(guy, 40)  then  guy.kill  endifchain
// Everything a script can name is an entity: a number in the VM, an index here. Entities own one or more physics bodies.
#include "game_internal.h"

double sqrt(double);
double sin(double);
double cos(double);
double floor(double);
double atan2(double, double);

#define STEP (1.0 / 60.0)

// ---- small helpers -----------------------------------------------------------------------------------------------------

static const char* const KIND_NAMES[K_KINDS] = { "block", "ramp", "crate", "ball", "plank", "spikes", "goal", "bridge", "rope", "barrel", "player", "debris" };
static const char* const KIND_TAGS[K_KINDS]  = { "ground", "ground", "crate", "ball", "plank", "spikes", "goal", "bridge", "rope", "barrel", "guy", "debris" };

double g_rand(struct game* g) {
    uint32_t x = g->rng ? g->rng : 0x1234567u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x;
    return (double)(x >> 8) / 16777216.0;
}

static void log_line(struct game* g, const char* s) { if (g->log_fn) g->log_fn(g->log_user, s); }

static void set_text(char* dst, size_t cap, const char* src) {
    size_t n = 0;
    while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

void g_say(struct game* g, const char* msg, double secs) {
    set_text(g->message, sizeof g->message, msg);
    g->message_time = secs;
}

static int ent_valid(const struct game* g, int e) { return e >= 0 && e < G_MAX_ENTS && g->ent[e].used; }

int g_ent_of_body(const struct game* g, int body) {
    if (body < 0 || body >= PH_MAX_BODIES) return -1;
    int e = g->world.body[body].ent;
    return ent_valid(g, e) ? e : -1;
}

int g_ent_new(struct game* g, int kind, const char* tag) {
    for (int i = 0; i < G_MAX_ENTS; i++) {
        if (g->ent[i].used) continue;
        struct g_ent* e = &g->ent[i];
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->kind = (uint8_t)kind;
        set_text(e->tag, sizeof e->tag, tag ? tag : KIND_TAGS[kind]);
        e->parent = e->child = -1;
        e->hp = 100;
        switch (kind) {
        case K_BLOCK: case K_RAMP: e->fill = 0x4a3a30; e->line = 0x1b1411; break;
        case K_CRATE: e->fill = 0xc8913d; e->line = 0x5c3d12; break;
        case K_BALL: e->fill = 0xd2503c; e->line = 0x5c1a12; break;
        case K_PLANK: case K_BRIDGE: e->fill = 0xb07a3a; e->line = 0x4a3010; break;
        case K_SPIKES: e->fill = 0xcfd4da; e->line = 0x3a3f46; break;
        case K_GOAL: e->fill = 0x4cd264; e->line = 0x14541f; break;
        case K_ROPE: e->fill = 0x8a7a5a; e->line = 0x2b2418; break;
        case K_BARREL: e->fill = 0xc83c3c; e->line = 0x501414; break;
        default: e->fill = 0xe0c070; e->line = 0x302010; break;
        }
        return i;
    }
    return -1;
}

static int ent_add_body(struct game* g, int e, int body) {
    if (body < 0 || e < 0 || g->ent[e].nbody >= G_MAX_PARTS) return -1;
    g->ent[e].body[g->ent[e].nbody++] = body;
    g->world.body[body].ent = e;
    return body;
}

static void ent_remove_bodies(struct game* g, int e) {
    struct g_ent* E = &g->ent[e];
    for (int i = 0; i < E->nbody; i++) if (E->body[i] >= 0) ph_remove_body(&g->world, E->body[i]);
    E->nbody = 0;
}

// ---- particles and effects --------------------------------------------------------------------------------------------

void g_particle_add(struct game* g, double x, double y, double vx, double vy, double life, uint32_t color, int size, int shape);
#define particle g_particle_add
void g_particle_add(struct game* g, double x, double y, double vx, double vy, double life, uint32_t color, int size, int shape) {
    if (g->npart >= G_MAX_PARTICLES) g->npart = G_MAX_PARTICLES - 1;       // overwrite the newest: old ones are nearly gone
    struct g_particle* p = &g->part[g->npart++];
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->life = p->max_life = life;
    p->color = color;
    p->size = (uint8_t)size;
    p->shape = (uint8_t)shape;
}

void g_blood(struct game* g, double x, double y, int n) {
    static const uint32_t RED[] = { 0xb0141c, 0xd01c24, 0x8c0c14, 0xe83038 };
    static const uint32_t FUN[] = { 0xffd23c, 0xff6ec7, 0x4cd2ff, 0xffffff, 0xa0ff50 };
    if (n > 80) n = 80;
    for (int i = 0; i < n; i++) {
        double a = g_rand(g) * 6.2831853, sp = 1.5 + g_rand(g) * 7;
        if (g->gore) particle(g, x, y, cos(a) * sp, sin(a) * sp - 2, 0.8 + g_rand(g) * 1.4, RED[(int)(g_rand(g) * 4) & 3], 2 + (int)(g_rand(g) * 2), 0);
        else particle(g, x, y, cos(a) * sp, sin(a) * sp - 3, 0.7 + g_rand(g) * 0.8, FUN[(int)(g_rand(g) * 5) % 5], 3, 1);   // cartoon: sparkly stars
    }
}

void g_boom(struct game* g, double x, double y, double power) {
    double reach = 2.0 + power * 0.12;
    for (int i = 0; i < PH_MAX_BODIES; i++) {
        struct ph_body* b = &g->world.body[i];
        if (!b->used || !b->dynamic) continue;
        double dx = b->p.x - x, dy = b->p.y - y, d = sqrt(dx * dx + dy * dy);
        if (d > reach) continue;
        double k = 1.0 - d / reach;
        double nx = d > 0.01 ? dx / d : 0, ny = d > 0.01 ? dy / d : -1;
        v2 imp = { nx * power * k * b->m * 0.9, ny * power * k * b->m * 0.9 - power * k * b->m * 0.25 };
        ph_apply_impulse(b, imp, b->p);
        int e = g_ent_of_body(g, i);
        if (e >= 0 && g->ent[e].kind == K_PLAYER && !g->ent[e].dead && power >= 60 && k > 0.5) g_player_die(g, e, "KABOOM");     // small blasts throw you; big ones end you
    }
    for (int i = 0; i < 70; i++) {
        double a = g_rand(g) * 6.2831853, sp = 2 + g_rand(g) * (4 + power * 0.25);
        static const uint32_t FIRE[] = { 0xffd23c, 0xff8c1c, 0xff4a14, 0x6a6a6a };
        particle(g, x, y, cos(a) * sp, sin(a) * sp, 0.4 + g_rand(g) * 0.7, FIRE[(int)(g_rand(g) * 4) & 3], 3 + (int)(g_rand(g) * 3), 0);
    }
    g->shake += power * 0.015;
    g->flash = 3;
}

static void add_text(struct game* g, double x, double y, const char* s) {
    for (int i = 0; i < G_MAX_TEXTS; i++)
        if (g->text[i].life <= 0) { g->text[i].x = x; g->text[i].y = y; g->text[i].life = 1.6; set_text(g->text[i].s, sizeof g->text[i].s, s); return; }
}

// ---- killing -----------------------------------------------------------------------------------------------------------

void g_ent_abort(struct game* g, int e) {
    if (!ent_valid(g, e)) return;
    ent_remove_bodies(g, e);
    g->ent[e].used = 0;
    if (e == g->player) g->player = -1;
    dm_ent_gone(g->vm, e);
}

void g_ent_kill(struct game* g, int e, int with_fx) {
    if (!ent_valid(g, e)) return;
    struct g_ent* E = &g->ent[e];
    if (E->kind == K_PLAYER) { g_player_die(g, e, ""); return; }
    char tag[20];
    set_text(tag, sizeof tag, E->tag);
    double x = 0, y = 0;
    if (E->nbody && E->body[0] >= 0) { x = g->world.body[E->body[0]].p.x; y = g->world.body[E->body[0]].p.y; }
    if (with_fx) {
        uint32_t c = E->fill;
        for (int i = 0; i < 14; i++) {
            double a = g_rand(g) * 6.2831853, sp = 1 + g_rand(g) * 5;
            particle(g, x, y, cos(a) * sp, sin(a) * sp - 2, 0.5 + g_rand(g) * 0.7, c, 3, 0);
        }
    }
    if (g->vm) dm_fire_dies(g->vm, e, tag);
    g_ent_abort(g, e);
}

// ---- the script host: entity members ----------------------------------------------------------------------------------

static int h_get(dm_vm* vm, int ent, const char* name, dm_val* out) {
    struct game* g = (struct game*)dm_host_user(vm);
    if (!ent_valid(g, ent)) { *out = dm_nil(); return 1; }                   // dead things read as nil
    struct g_ent* E = &g->ent[ent];
    struct ph_body* b = E->nbody ? &g->world.body[E->body[E->kind == K_PLAYER ? PART_TORSO : 0]] : 0;
    if (!strcmp(name, "alive")) { *out = dm_bool(!E->dead); return 1; }
    if (!strcmp(name, "kind")) { *out = dm_str_new(vm, KIND_NAMES[E->kind]); return 1; }
    if (!strcmp(name, "tag")) { *out = dm_str_new(vm, E->tag); return 1; }
    if (!strcmp(name, "hp")) { *out = dm_num(E->hp); return 1; }
    if (!strcmp(name, "w")) { *out = dm_num(E->w); return 1; }
    if (!strcmp(name, "h")) { *out = dm_num(E->h); return 1; }
    if (!b || !b->used) return 0;
    if (!strcmp(name, "x")) { *out = dm_num(b->p.x); return 1; }
    if (!strcmp(name, "y")) { *out = dm_num(b->p.y); return 1; }
    if (!strcmp(name, "vx")) { *out = dm_num(b->v.x); return 1; }
    if (!strcmp(name, "vy")) { *out = dm_num(b->v.y); return 1; }
    if (!strcmp(name, "speed")) { *out = dm_num(sqrt(b->v.x * b->v.x + b->v.y * b->v.y)); return 1; }
    if (!strcmp(name, "angle")) { *out = dm_num(b->a * 57.29577951308232); return 1; }
    if (!strcmp(name, "mass")) { double m = 0; for (int i = 0; i < E->nbody; i++) m += g->world.body[E->body[i]].m; *out = dm_num(m); return 1; }
    return 0;
}

static int h_set(dm_vm* vm, int ent, const char* name, dm_val v) {
    struct game* g = (struct game*)dm_host_user(vm);
    if (!ent_valid(g, ent)) return 1;
    struct g_ent* E = &g->ent[ent];
    if (!strcmp(name, "tag") && v.t == DM_STR) { set_text(E->tag, sizeof E->tag, dm_text(&v)); return 1; }
    if (v.t != DM_NUM) return 0;
    if (!strcmp(name, "hp")) { E->hp = v.u.n; return 1; }
    int root = E->kind == K_PLAYER ? PART_TORSO : 0;
    if (!E->nbody || !g->world.body[E->body[root]].used) return 0;
    struct ph_body* r = &g->world.body[E->body[root]];
    if (!strcmp(name, "x") || !strcmp(name, "y")) {                          // teleport the whole thing
        double dx = !strcmp(name, "x") ? v.u.n - r->p.x : 0, dy = !strcmp(name, "y") ? v.u.n - r->p.y : 0;
        for (int i = 0; i < E->nbody; i++) if (E->body[i] >= 0 && g->world.body[E->body[i]].used) { g->world.body[E->body[i]].p.x += dx; g->world.body[E->body[i]].p.y += dy; }
        return 1;
    }
    if (!strcmp(name, "vx") || !strcmp(name, "vy")) {
        for (int i = 0; i < E->nbody; i++) if (E->body[i] >= 0 && g->world.body[E->body[i]].used) {
            if (!strcmp(name, "vx")) g->world.body[E->body[i]].v.x = v.u.n; else g->world.body[E->body[i]].v.y = v.u.n;
        }
        return 1;
    }
    if (!strcmp(name, "angle")) { r->a = v.u.n / 57.29577951308232; return 1; }
    return 0;
}

static int h_act(dm_vm* vm, int ent, const char* verb, int argc, const dm_val* argv, dm_val* ret) {
    struct game* g = (struct game*)dm_host_user(vm);
    (void)ret;
    if (!ent_valid(g, ent)) return 1;                                         // verbs on the dead do nothing, politely
    struct g_ent* E = &g->ent[ent];
    if (!strcmp(verb, "abort")) { g_ent_abort(g, ent); return 1; }
    if (!strcmp(verb, "kill")) { g_ent_kill(g, ent, 1); return 1; }
    if (!strcmp(verb, "hurt")) {
        double n = argc > 0 && argv[0].t == DM_NUM ? argv[0].u.n : 10;
        E->hp -= n;
        if (E->hp <= 0) g_ent_kill(g, ent, 1);
        return 1;
    }
    if (!strcmp(verb, "push") || !strcmp(verb, "spin")) {
        for (int i = 0; i < E->nbody; i++) {
            if (E->body[i] < 0 || !g->world.body[E->body[i]].used) continue;
            struct ph_body* b = &g->world.body[E->body[i]];
            if (!strcmp(verb, "spin")) { if (argc > 0 && argv[0].t == DM_NUM) b->w += argv[0].u.n; continue; }
            if (argc >= 2 && argv[0].t == DM_NUM && argv[1].t == DM_NUM) { b->v.x += argv[0].u.n; b->v.y += argv[1].u.n; }
        }
        return 1;
    }
    if (!strcmp(verb, "freeze") || !strcmp(verb, "thaw")) {
        for (int i = 0; i < E->nbody; i++) {
            if (E->body[i] < 0 || !g->world.body[E->body[i]].used) continue;
            struct ph_body* b = &g->world.body[E->body[i]];
            if (!strcmp(verb, "freeze")) { b->v.x = b->v.y = b->w = 0; b->gscale = 0; }
            else b->gscale = 1;
        }
        return 1;
    }
    if (!strcmp(verb, "eject") && E->kind == K_PLAYER) { g_player_eject(g, ent); return 1; }
    if (!strcmp(verb, "boom")) {
        struct ph_body* b = &g->world.body[E->body[E->kind == K_PLAYER ? PART_TORSO : 0]];
        g_boom(g, b->p.x, b->p.y, argc > 0 && argv[0].t == DM_NUM ? argv[0].u.n : 30);
        return 1;
    }
    return 0;
}

static int h_ctx(dm_vm* vm, int me, const char* name, dm_val* out) {
    struct game* g = (struct game*)dm_host_user(vm);
    if (!ent_valid(g, me)) { *out = dm_nil(); return 1; }
    int v = !strcmp(name, "child") ? g->ent[me].child : g->ent[me].parent;
    *out = ent_valid(g, v) ? dm_ent(v) : dm_nil();
    return 1;
}

static int h_alive(dm_vm* vm, int ent) {
    struct game* g = (struct game*)dm_host_user(vm);
    return ent_valid(g, ent) && !g->ent[ent].dead;
}

static void h_suicide(dm_vm* vm, int ent) {
    struct game* g = (struct game*)dm_host_user(vm);
    if (ent >= 0) g_ent_kill(g, ent, 1);
}

static void h_print(void* user, const char* line) { log_line((struct game*)user, line); }

// ---- natives: module world --------------------------------------------------------------------------------------------

static struct game* G(void* ctx) { return (struct game*)ctx; }

static int need(dm_vm* vm, int argc, const dm_val* a, int n, const char* usage) {
    if (argc < n) return dm_fail(vm, usage);
    for (int i = 0; i < n; i++) if (a[i].t != DM_NUM) return dm_fail(vm, usage);
    return 0;
}

static double opt(int argc, const dm_val* a, int i, double dflt) { return i < argc && a[i].t == DM_NUM ? a[i].u.n : dflt; }

// Every entity a script makes remembers who made it (the running thing, if any): `parent` and `child` work off this.
static void adopt(struct game* g, dm_vm* vm, int e) {
    int me = dm_me(vm);
    if (me < 0 || !ent_valid(g, me) || me == e) return;
    g->ent[e].parent = me;
    g->ent[me].child = e;
}

static int body_box(struct game* g, int e, double x, double y, double w, double h, double density, int dynamic, double angle_deg) {
    int b = ph_add_box(&g->world, x, y, w / 2, h / 2, density, dynamic);
    if (b < 0) return -1;
    g->world.body[b].a = angle_deg / 57.29577951308232;
    g->world.body[b].fric = dynamic ? 0.6 : 0.9;
    g->ent[e].w = w;
    g->ent[e].h = h;
    return ent_add_body(g, e, b);
}

static int make_block(struct game* g, dm_vm* vm, int kind, double x, double y, double w, double h, double deg, dm_val* ret) {
    int e = g_ent_new(g, kind, 0);
    if (e < 0 || body_box(g, e, x, y, w, h, 1, 0, deg) < 0) return dm_fail(vm, "the level is full (too many bodies)");
    g->ent[e].fixed = 1;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

static int f_block(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    if (need(vm, argc, a, 4, "world:block(x, y, width, height)")) return -1;
    return make_block(G(ctx), vm, K_BLOCK, a[0].u.n, a[1].u.n, a[2].u.n, a[3].u.n, 0, ret);
}

static int f_ramp(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    if (need(vm, argc, a, 5, "world:ramp(x, y, width, height, degrees)")) return -1;
    return make_block(G(ctx), vm, K_RAMP, a[0].u.n, a[1].u.n, a[2].u.n, a[3].u.n, a[4].u.n, ret);
}

// slope(x1, y1, x2, y2 [, thickness]): solid ground whose TOP runs from the first point to the second. Chain slopes to make hills.
static int f_slope(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 4, "world:slope(x1, y1, x2, y2 [, thickness])")) return -1;
    double x1 = a[0].u.n, y1 = a[1].u.n, x2 = a[2].u.n, y2 = a[3].u.n, t = opt(argc, a, 4, 1.0);
    double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy);
    if (len < 0.05) return dm_fail(vm, "world:slope needs two different points");
    double nx = dy / len, ny = -dx / len;                       // the side that is up
    if (ny > 0) { nx = -nx; ny = -ny; }
    double cx = (x1 + x2) / 2 - nx * t / 2, cy = (y1 + y2) / 2 - ny * t / 2;
    int e = g_ent_new(g, K_RAMP, 0);
    if (e < 0 || body_box(g, e, cx, cy, len, t, 1, 0, atan2(dy, dx) * 57.29577951308232) < 0) return dm_fail(vm, "the level is full (too many bodies)");
    g->ent[e].fixed = 1;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

// bouncy(x, y, width [, height]): a trampoline pad. Anything dropped on it comes back up (higher than it should, even).
static int f_bouncy(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 3, "world:bouncy(x, y, width [, height])")) return -1;
    if (make_block(g, vm, K_BLOCK, a[0].u.n, a[1].u.n, a[2].u.n, opt(argc, a, 3, 0.4), 0, ret)) return -1;
    struct g_ent* E = &g->ent[ret->u.ent];
    g->world.body[E->body[0]].rest = 1.15;
    g->world.body[E->body[0]].fric = 0.3;
    E->fill = 0xff5fb0;
    E->line = 0x7a1f55;
    set_text(E->tag, sizeof E->tag, "bouncy");
    return 0;
}

static int f_crate(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 2, "world:crate(x, y [, size])")) return -1;
    double s = opt(argc, a, 2, 0.8);
    int e = g_ent_new(g, K_CRATE, 0);
    if (e < 0 || body_box(g, e, a[0].u.n, a[1].u.n, s, s, 0.25, 1, 0) < 0) return dm_fail(vm, "the level is full (too many bodies)");
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

static int f_plank(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 4, "world:plank(x, y, width, height)")) return -1;
    int e = g_ent_new(g, K_PLANK, 0);
    if (e < 0 || body_box(g, e, a[0].u.n, a[1].u.n, a[2].u.n, a[3].u.n, 0.4, 1, 0) < 0) return dm_fail(vm, "the level is full (too many bodies)");
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

static int make_ball(struct game* g, dm_vm* vm, int kind, double x, double y, double r, dm_val* ret) {
    int e = g_ent_new(g, kind, 0);
    int b = e >= 0 ? ph_add_circle(&g->world, x, y, r, 0.5, 1) : -1;
    if (b < 0) return dm_fail(vm, "the level is full (too many bodies)");
    g->world.body[b].rest = 0.45;
    g->world.body[b].fric = 0.6;
    ent_add_body(g, e, b);
    g->ent[e].w = g->ent[e].h = r * 2;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

static int f_ball(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    if (need(vm, argc, a, 2, "world:ball(x, y [, radius])")) return -1;
    return make_ball(G(ctx), vm, K_BALL, a[0].u.n, a[1].u.n, opt(argc, a, 2, 0.5), ret);
}

// spikes(x, y, count): a row of count spikes, x/y = the middle of the base. They kill whatever rides into them.
static int f_spikes(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 2, "world:spikes(x, y [, count])")) return -1;
    int n = (int)opt(argc, a, 2, 4);
    if (n < 1) n = 1;
    if (n > 12) n = 12;
    int e = g_ent_new(g, K_SPIKES, 0);
    if (e < 0) return dm_fail(vm, "the level is full (too many bodies)");
    for (int i = 0; i < n; i++) {
        v2 v[3] = { { -0.25, 0.3 }, { 0.25, 0.3 }, { 0, -0.3 } };
        double cx = a[0].u.n + (i - (n - 1) / 2.0) * 0.5;
        int b = ph_add_poly(&g->world, cx, a[1].u.n - 0.3, v, 3, 1, 0);
        if (b < 0) return dm_fail(vm, "the level is full (too many bodies)");
        g->world.body[b].sensor = 1;
        ent_add_body(g, e, b);
    }
    g->ent[e].fixed = 1;
    g->ent[e].w = n * 0.5;
    g->ent[e].h = 0.6;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

static int f_goal(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 2, "world:goal(x, y)")) return -1;
    int e = g_ent_new(g, K_GOAL, 0);
    if (e < 0 || body_box(g, e, a[0].u.n, a[1].u.n - 1.2, 0.5, 2.4, 1, 0, 0) < 0) return dm_fail(vm, "the level is full (too many bodies)");
    g->world.body[g->ent[e].body[0]].sensor = 1;
    g->ent[e].fixed = 1;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

// bridge(x1, y1, x2, y2, planks): planks hinged end to end between two posts. It sags and swings under a wheelchair.
static int f_bridge(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 4, "world:bridge(x1, y1, x2, y2 [, planks])")) return -1;
    int n = (int)opt(argc, a, 4, 8);
    if (n < 2) n = 2;
    if (n > 14) n = 14;
    int e = g_ent_new(g, K_BRIDGE, 0);
    if (e < 0) return dm_fail(vm, "the level is full (too many bodies)");
    double x1 = a[0].u.n, y1 = a[1].u.n, x2 = a[2].u.n, y2 = a[3].u.n;
    double len = sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)) / n;
    int post1 = ph_add_box(&g->world, x1 - 0.2, y1 + 0.3, 0.2, 0.3, 1, 0), post2 = ph_add_box(&g->world, x2 + 0.2, y2 + 0.3, 0.2, 0.3, 1, 0);
    if (post1 < 0 || post2 < 0) return dm_fail(vm, "the level is full (too many bodies)");
    int prev = post1;
    for (int i = 0; i < n; i++) {
        double t = (i + 0.5) / n;
        int b = ph_add_box(&g->world, x1 + (x2 - x1) * t, y1 + 0.1 + (y2 - y1) * t, len / 2, 0.1, 12, 1);   // heavy planks: a light chain cannot carry a rider
        if (b < 0) return dm_fail(vm, "the level is full (too many bodies)");
        g->world.body[b].group = 900 + e;
        g->world.body[b].fric = 0.9;
        if (prev == post1) g->world.body[post1].group = 900 + e;
        double jx = x1 + (x2 - x1) * ((double)i / n), jy = y1 + 0.1 + (y2 - y1) * ((double)i / n);   // planks sit with their TOP on the road
        int j = ph_add_pin(&g->world, prev, b, (v2){ jx, jy });
        (void)j;
        ent_add_body(g, e, b);
        prev = b;
    }
    g->world.body[post2].group = 900 + e;
    ph_add_pin(&g->world, prev, post2, (v2){ x2, y2 + 0.1 });
    ent_add_body(g, e, post1);
    ent_add_body(g, e, post2);
    g->ent[e].fixed = 0;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

// wrecker(x, y [, links, ball radius]): a heavy ball on a chain hanging from a ceiling point. Swing it, dodge it.
static int f_wrecker(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 2, "world:wrecker(x, y [, links, ball_radius])")) return -1;
    int n = (int)opt(argc, a, 2, 6);
    double r = opt(argc, a, 3, 0.8);
    if (n < 2) n = 2;
    if (n > 12) n = 12;
    int e = g_ent_new(g, K_ROPE, "wrecker");
    if (e < 0) return dm_fail(vm, "the level is full (too many bodies)");
    int anchor = ph_add_box(&g->world, a[0].u.n, a[1].u.n, 0.15, 0.15, 1, 0);
    if (anchor < 0) return dm_fail(vm, "the level is full (too many bodies)");
    int prev = anchor;
    for (int i = 0; i < n; i++) {
        int b = ph_add_box(&g->world, a[0].u.n + 0.6 * (i + 0.5), a[1].u.n, 0.3, 0.06, 14, 1);       // starts out sideways: a swing waiting to happen
        if (b < 0) return dm_fail(vm, "the level is full (too many bodies)");
        g->world.body[b].group = 800 + e;
        ph_add_pin(&g->world, prev, b, (v2){ a[0].u.n + 0.6 * i, a[1].u.n });
        ent_add_body(g, e, b);
        prev = b;
    }
    g->world.body[anchor].group = 800 + e;
    int ball = ph_add_circle(&g->world, a[0].u.n + 0.6 * n + r, a[1].u.n, r, 3, 1);
    if (ball < 0) return dm_fail(vm, "the level is full (too many bodies)");
    g->world.body[ball].group = 800 + e;
    g->world.body[ball].rest = 0.2;
    ph_add_pin(&g->world, prev, ball, (v2){ a[0].u.n + 0.6 * n, a[1].u.n });
    ent_add_body(g, e, ball);
    ent_add_body(g, e, anchor);
    g->ent[e].fill = 0x57606b;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

// seesaw(x, y, length): a plank on a pivot that tips both ways.
static int f_seesaw(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (need(vm, argc, a, 2, "world:seesaw(x, y [, length])")) return -1;
    double len = opt(argc, a, 2, 6);
    int e = g_ent_new(g, K_PLANK, "plank");
    int pivot = ph_add_box(&g->world, a[0].u.n, a[1].u.n + 0.4, 0.2, 0.4, 1, 0);
    int board = ph_add_box(&g->world, a[0].u.n, a[1].u.n - 0.05, len / 2, 0.12, 0.6, 1);
    if (e < 0 || pivot < 0 || board < 0) return dm_fail(vm, "the level is full (too many bodies)");
    g->world.body[pivot].group = g->world.body[board].group = 700 + e;
    g->world.body[board].fric = 0.9;
    int j = ph_add_pin(&g->world, pivot, board, (v2){ a[0].u.n, a[1].u.n });
    ph_joint_limit(&g->world, j, -0.42, 0.42);
    ent_add_body(g, e, board);
    ent_add_body(g, e, pivot);
    g->ent[e].w = len;
    adopt(g, vm, e);
    *ret = dm_ent(e);
    return 0;
}

// make("thing", x, y): an entity of a `thing` the script defined; its handlers run with `me` = the new entity.
static int f_make(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (argc < 3 || a[0].t != DM_STR || a[1].t != DM_NUM || a[2].t != DM_NUM) return dm_fail(vm, "world:make(\"thing\", x, y)");
    const char* thing = dm_text(&a[0]);
    const char* base = dm_thing_base(g->prog, thing);
    if (!base) return dm_fail(vm, "there is no `thing` with that name");
    double x = a[1].u.n, y = a[2].u.n;
    dm_val made = dm_nil();
    int e;
    if (!strcmp(base, "ball") || !strcmp(base, "barrel")) {
        if (make_ball(g, vm, !strcmp(base, "barrel") ? K_BARREL : K_BALL, x, y, 0.45, &made)) return -1;
    } else if (!strcmp(base, "crate") || !strcmp(base, "box")) {
        e = g_ent_new(g, K_CRATE, 0);
        if (e < 0 || body_box(g, e, x, y, 0.8, 0.8, 0.5, 1, 0) < 0) return dm_fail(vm, "the level is full (too many bodies)");
        made = dm_ent(e);
    } else if (!strcmp(base, "block")) {
        if (make_block(g, vm, K_BLOCK, x, y, 2, 1, 0, &made)) return -1;
    } else if (!strcmp(base, "plank")) {
        e = g_ent_new(g, K_PLANK, 0);
        if (e < 0 || body_box(g, e, x, y, 2.4, 0.2, 0.4, 1, 0) < 0) return dm_fail(vm, "the level is full (too many bodies)");
        made = dm_ent(e);
    } else return dm_fail(vm, "a thing can be \"ball\", \"barrel\", \"crate\", \"box\", \"block\" or \"plank\"");
    e = made.u.ent;
    set_text(g->ent[e].tag, sizeof g->ent[e].tag, thing);
    if (!strcmp(base, "barrel")) { g->ent[e].fill = 0xc83c3c; g->ent[e].line = 0x501414; }
    adopt(g, vm, e);
    dm_spawn_thing(g->vm, thing, e);
    *ret = made;
    return 0;
}

static int f_spawn(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    if (argc < 3 || a[0].t != DM_STR || a[1].t != DM_NUM || a[2].t != DM_NUM) return dm_fail(vm, "world:spawn(\"wheelchair\", x, y)");
    if (g->player >= 0) return dm_fail(vm, "there is already a player in this level");
    int e = g_spawn_player(g, dm_text(&a[0]), a[1].u.n, a[2].u.n);
    if (e < 0) return dm_fail(vm, "unknown character (try \"wheelchair\")");
    g->player = e;
    *ret = dm_ent(e);
    return 0;
}

// ---- natives: module fx and game --------------------------------------------------------------------------------------

static int entity_pos(struct game* g, const dm_val* v, double* x, double* y) {
    if (v->t != DM_ENT || !ent_valid(g, v->u.ent) || !g->ent[v->u.ent].nbody) return 0;
    struct g_ent* E = &g->ent[v->u.ent];
    int b = E->body[E->kind == K_PLAYER ? PART_TORSO : 0];
    if (b < 0 || !g->world.body[b].used) return 0;
    *x = g->world.body[b].p.x;
    *y = g->world.body[b].p.y;
    return 1;
}

static int f_blood(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    double x, y;
    int n;
    (void)ret;
    if (argc >= 1 && a[0].t == DM_ENT) {
        if (!entity_pos(g, &a[0], &x, &y)) return 0;
        n = (int)opt(argc, a, 1, 20);
    } else {
        if (need(vm, argc, a, 2, "fx:blood(thing [, amount])  or  fx:blood(x, y [, amount])")) return -1;
        x = a[0].u.n; y = a[1].u.n;
        n = (int)opt(argc, a, 2, 20);
    }
    g_blood(g, x, y, n);
    return 0;
}

static int f_boom(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    double x, y, power;
    (void)ret;
    if (argc >= 1 && a[0].t == DM_ENT) {
        if (!entity_pos(g, &a[0], &x, &y)) return 0;
        power = opt(argc, a, 1, 30);
    } else {
        if (need(vm, argc, a, 2, "fx:boom(thing [, power])  or  fx:boom(x, y [, power])")) return -1;
        x = a[0].u.n; y = a[1].u.n;
        power = opt(argc, a, 2, 30);
    }
    g_boom(g, x, y, power);
    return 0;
}

static int f_shake(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    (void)ret;
    if (need(vm, argc, a, 1, "fx:shake(amount)")) return -1;
    G(ctx)->shake += a[0].u.n;
    return 0;
}

static int f_text(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    (void)ret;
    if (argc < 3 || a[0].t != DM_NUM || a[1].t != DM_NUM || a[2].t != DM_STR) return dm_fail(vm, "fx:text(x, y, \"words\")");
    add_text(G(ctx), a[0].u.n, a[1].u.n, dm_text(&a[2]));
    return 0;
}

static void finish_level(struct game* g, int state, const char* msg) {
    if (g->state != GS_PLAYING) return;
    g->state = state;
    g->end_timer = 0;
    g_say(g, msg, 1e9);
}

static void win_level(struct game* g) {
    char num[32], msg[80] = "LEVEL COMPLETE!  ";
    dm_fmt_num(floor(g->time * 10) / 10, num);
    size_t n = strlen(msg);
    for (size_t i = 0; num[i] && n + 3 < sizeof msg; i++) msg[n++] = num[i];
    msg[n++] = 's';
    msg[n] = 0;
    if (g->state != GS_PLAYING) return;
    finish_level(g, GS_WON, msg);
    for (int i = 0; i < 60; i++) {                                           // confetti, always
        static const uint32_t FUN[] = { 0xffd23c, 0xff6ec7, 0x4cd2ff, 0xffffff, 0xa0ff50 };
        double a = g_rand(g) * 6.2831853, sp = 3 + g_rand(g) * 8;
        particle(g, g->cam_x, g->cam_y - 3, cos(a) * sp, sin(a) * sp - 6, 1.5 + g_rand(g), FUN[(int)(g_rand(g) * 5) % 5], 4, 1);
    }
}

static int f_win(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) { (void)vm; (void)argc; (void)a; (void)ret; win_level(G(ctx)); return 0; }

static int f_lose(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    struct game* g = G(ctx);
    (void)vm; (void)ret;
    if (g->player >= 0 && !g->ent[g->player].dead) g_player_die(g, g->player, argc > 0 && a[0].t == DM_STR ? dm_text(&a[0]) : "");
    else finish_level(g, GS_DEAD, argc > 0 && a[0].t == DM_STR ? dm_text(&a[0]) : "You lost");
    return 0;
}

static int f_message(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    (void)ret;
    if (argc < 1 || a[0].t != DM_STR) return dm_fail(vm, "game:message(\"words\" [, seconds])");
    g_say(G(ctx), dm_text(&a[0]), opt(argc, a, 1, 3));
    return 0;
}

static int f_gore(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    (void)ret;
    if (need(vm, argc, a, 1, "game:gore(0 or 1)")) return -1;
    G(ctx)->gore = a[0].u.n > 0;
    return 0;
}

static int f_zoom(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    (void)ret;
    if (need(vm, argc, a, 1, "game:zoom(factor)")) return -1;
    G(ctx)->zoom_goal = a[0].u.n < 0.3 ? 0.3 : a[0].u.n > 3 ? 3 : a[0].u.n;
    return 0;
}

static int f_gravity(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {
    (void)ret;
    if (need(vm, argc, a, 1, "game:gravity(strength)  (14 is normal, 3 is the moon)")) return -1;
    G(ctx)->world.gravity.y = a[0].u.n;
    return 0;
}

static int f_key_held(dm_vm* vm, int argc, const dm_val* a, dm_val* ret, void* ctx) {   // game:held("up") -> true while the key is down
    struct game* g = G(ctx);
    static const char* const NAMES[GK_COUNT] = { "up", "down", "left", "right", "space", "shift", "ctrl" };
    if (argc < 1 || a[0].t != DM_STR) return dm_fail(vm, "game:held(\"up\")");
    for (int i = 0; i < GK_COUNT; i++) if (!strcmp(dm_text(&a[0]), NAMES[i])) { *ret = dm_bool(g->held[i]); return 0; }
    *ret = dm_bool(0);
    return 0;
}

static void register_natives(struct game* g, dm_vm* vm) {
    dm_register_basics(vm);
    dm_register(vm, "world", "block", f_block, g);
    dm_register(vm, "world", "ramp", f_ramp, g);
    dm_register(vm, "world", "slope", f_slope, g);
    dm_register(vm, "world", "bouncy", f_bouncy, g);
    dm_register(vm, "world", "crate", f_crate, g);
    dm_register(vm, "world", "plank", f_plank, g);
    dm_register(vm, "world", "ball", f_ball, g);
    dm_register(vm, "world", "spikes", f_spikes, g);
    dm_register(vm, "world", "goal", f_goal, g);
    dm_register(vm, "world", "bridge", f_bridge, g);
    dm_register(vm, "world", "wrecker", f_wrecker, g);
    dm_register(vm, "world", "seesaw", f_seesaw, g);
    dm_register(vm, "world", "make", f_make, g);
    dm_register(vm, "world", "spawn", f_spawn, g);
    dm_register(vm, "fx", "blood", f_blood, g);
    dm_register(vm, "fx", "boom", f_boom, g);
    dm_register(vm, "fx", "shake", f_shake, g);
    dm_register(vm, "fx", "text", f_text, g);
    dm_register(vm, "game", "win", f_win, g);
    dm_register(vm, "game", "lose", f_lose, g);
    dm_register(vm, "game", "message", f_message, g);
    dm_register(vm, "game", "gore", f_gore, g);
    dm_register(vm, "game", "zoom", f_zoom, g);
    dm_register(vm, "game", "gravity", f_gravity, g);
    dm_register(vm, "game", "held", f_key_held, g);
}

// ---- lifecycle --------------------------------------------------------------------------------------------------------

struct game* game_new(void) {
    struct game* g = (struct game*)DM_ALLOC(sizeof *g);
    if (g) { g->gore = 1; g->player = -1; g->zoom_goal = g->cam_zoom = 1; }
    return g;
}

static void game_clear_level(struct game* g) {
    if (g->vm) { dm_vm_free(g->vm); g->vm = 0; }
    if (g->prog) { dm_program_free(g->prog); g->prog = 0; }
    int gore = g->gore;
    void (*lf)(void*, const char*) = g->log_fn;
    void* lu = g->log_user;
    char* src = g->src;
    size_t sl = g->src_len;
    uint32_t rng = g->rng;
    memset(g, 0, sizeof *g);
    g->gore = gore;
    g->log_fn = lf;
    g->log_user = lu;
    g->src = src;
    g->src_len = sl;
    g->rng = rng ? rng : 0x2545F491u;
    g->player = -1;
    g->zoom_goal = g->cam_zoom = 1;
    ph_init(&g->world);
    g->world.substeps = 4;                       // a ragdoll on a vehicle is a long constraint chain: it needs the extra passes
    g->world.iterations = 14;
}

void game_free(struct game* g) {
    if (!g) return;
    game_clear_level(g);
    DM_FREE(g->src);
    DM_FREE(g);
}

void game_set_log(struct game* g, void (*fn)(void*, const char*), void* user) { g->log_fn = fn; g->log_user = user; }
void game_set_gore(struct game* g, int level) { g->gore = level > 0; }

int game_load(struct game* g, const char* src, size_t len, char* err, size_t err_cap) {
    if (g->src != src) {                         // keep our own copy so `R` can reload
        char* copy = (char*)DM_ALLOC(len + 1);
        if (!copy) { if (err_cap) { strncpy(err, "out of memory", err_cap); err[err_cap - 1] = 0; } return -1; }
        memcpy(copy, src, len);
        DM_FREE(g->src);
        g->src = copy;
        g->src_len = len;
    }
    game_clear_level(g);
    g->prog = dm_compile(g->src, g->src_len, err, err_cap);
    if (!g->prog) return -1;
    struct dm_host h;
    memset(&h, 0, sizeof h);
    h.user = g;
    h.print = h_print;
    h.get = h_get;
    h.set = h_set;
    h.act = h_act;
    h.ctx = h_ctx;
    h.alive = h_alive;
    h.suicide = h_suicide;
    g->vm = dm_vm_new(g->prog, &h);
    if (!g->vm) { if (err_cap) { strncpy(err, "out of memory", err_cap); err[err_cap - 1] = 0; } return -1; }
    register_natives(g, g->vm);
    if (dm_link(g->vm, err, err_cap)) return -1;
    dm_start(g->vm);
    dm_step(g->vm, 0);                           // run `when run` now so the first frame already has the world
    return 0;
}

void game_key(struct game* g, int key, int down) { if (key >= 0 && key < GK_COUNT) g->held[key] = (uint8_t)(down != 0); }

void game_key_name(struct game* g, const char* name) {
    if (!strcmp(name, "r")) {                    // restart is built in: reload the same level
        char err[128];
        if (game_load(g, g->src, g->src_len, err, sizeof err)) log_line(g, err);
        return;
    }
    if (g->vm) dm_fire_key(g->vm, name);
}

int game_state(const struct game* g) { return g->state; }
double game_time(const struct game* g) { return g->time; }
const char* game_message(const struct game* g) { return g->message_time > 0 ? g->message : ""; }

// ---- the frame --------------------------------------------------------------------------------------------------------

static void handle_events(struct game* g) {
    struct ph_world* w = &g->world;
    for (int i = 0; i < w->nev; i++) {
        struct ph_event* ev = &w->ev[i];
        if (ev->kind == PE_JOINT_BREAK) {
            int e = g_ent_of_body(g, ev->a);
            if (e >= 0 && g->ent[e].kind == K_PLAYER) {
                char note[80] = "[game] joint snapped: parts ";
                char num[32];
                dm_fmt_num(g_player_part_of_body(g, e, ev->a), num); strcat(note, num); strcat(note, " and ");
                dm_fmt_num(g_player_part_of_body(g, e, ev->b), num); strcat(note, num); strcat(note, " at force ");
                dm_fmt_num(ev->impulse * 60.0 * g->world.substeps, num); strcat(note, num);
                log_line(g, note);
                g->ent[e].limbs_lost++;
                g_blood(g, ev->pt.x, ev->pt.y, 24);
                g->shake += 0.3;
            }
            continue;
        }
        int ea = g_ent_of_body(g, ev->a), eb = g_ent_of_body(g, ev->b);
        if (ea < 0 || eb < 0 || ea == eb) continue;
        struct g_ent *A = &g->ent[ea], *B = &g->ent[eb];
        if (g->vm) dm_fire_touch(g->vm, ea, A->tag, eb, B->tag);
        // built-in rules: the player against hazards and the goal, and bumps hurting
        for (int s = 0; s < 2; s++) {
            int pe = s ? eb : ea, oe = s ? ea : eb, pb = s ? ev->b : ev->a;
            if (g->ent[pe].kind != K_PLAYER || g->ent[pe].dead || g->state != GS_PLAYING) continue;
            if (g->ent[oe].kind == K_SPIKES) { g_player_die(g, pe, "SPIKED!"); continue; }
            if (g->ent[oe].kind == K_GOAL) { win_level(g); continue; }
            if (g->world.body[s ? ev->a : ev->b].sensor) continue;
            g_player_impact(g, pe, g_player_part_of_body(g, pe, pb), ev->speed);
        }
    }
}

static void update_particles(struct game* g, double dt) {
    int k = 0;
    for (int i = 0; i < g->npart; i++) {
        struct g_particle* p = &g->part[i];
        p->life -= dt;
        if (p->life <= 0) continue;
        p->vy += 14 * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if (p->shape == 0 && p->y > 1e5) continue;
        g->part[k++] = *p;
    }
    g->npart = k;
    for (int i = 0; i < G_MAX_TEXTS; i++) if (g->text[i].life > 0) { g->text[i].life -= dt; g->text[i].y -= dt * 1.2; }
}

static void update_camera(struct game* g, double dt) {
    double tx = g->cam_x, ty = g->cam_y;
    if (g->player >= 0 && g->ent[g->player].used) {
        struct g_ent* P = &g->ent[g->player];
        int tb = P->body[PART_TORSO];
        if (tb >= 0 && g->world.body[tb].used) { tx = g->world.body[tb].p.x + g->world.body[tb].v.x * 0.25; ty = g->world.body[tb].p.y - 1.0; }
    }
    double f = dt * 5.0 > 1 ? 1 : dt * 5.0;
    g->cam_x += (tx - g->cam_x) * f;
    g->cam_y += (ty - g->cam_y) * f;
    g->cam_zoom += (g->zoom_goal - g->cam_zoom) * (dt * 2.0 > 1 ? 1 : dt * 2.0);
    g->shake *= 0.9;
    if (g->flash > 0) g->flash--;
}

static void fixed_step(struct game* g) {
    if (g->state == GS_PLAYING) g->time += STEP;
    else g->end_timer += STEP;
    if (g->message_time > 0) g->message_time -= STEP;
    if (g->player >= 0 && g->ent[g->player].used && !g->ent[g->player].dead) g_player_controls(g, STEP);
    ph_step(&g->world, STEP);
    handle_events(g);
    g->ms_accum += STEP * 1000.0;
    int whole = (int)g->ms_accum;
    g->ms_accum -= whole;
    if (g->vm) dm_step(g->vm, whole);
    update_particles(g, STEP);
    update_camera(g, STEP);
}

void game_update(struct game* g, double dt) {
    if (!g->vm) return;
    if (dt > 0.1) dt = 0.1;
    g->accum += dt;
    int steps = 0;
    while (g->accum >= STEP && steps < 4) { fixed_step(g); g->accum -= STEP; steps++; }
    if (g->accum > STEP) g->accum = STEP;
}

void game_render(struct game* g, uint32_t* px, int w, int h, int stride) { g_draw(g, px, w, h, stride); }

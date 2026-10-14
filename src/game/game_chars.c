// Playable characters: a ragdoll rider on a vehicle. First (and so far only) one: the wheelchair guy.
//
//   keys:   up / W        roll forward         down / S       brake, then reverse
//           left / A      lean back (wheelie)  right / D      lean forward
//           space         eject!  (the rider leaves the chair at speed)
//   ouch:   hard hits on the head or torso end the run; limbs snap off when a joint carries too much.
//
// All sizes are meters, y points down. The rider faces right.
#include "game_internal.h"

double sqrt(double);

// collision layers: the world is layer 1 and collides with everything. The rider's body only meets the world, the legs also
// meet the chair frame (so thighs sit on the seat), and the wheels only meet the world (the rider's shins hang inside them).
#define L_WORLD  1u
#define L_UPPER  2u
#define L_LEGS   4u
#define L_FRAME  8u
#define L_WHEEL  16u

#ifndef ACCEL
#define ACCEL 14.0                    // wheel speed gained per second (rad/s^2)
#endif
#ifndef TORQUE_DRIVE
#define TORQUE_DRIVE 36.0
#endif
#ifndef TORQUE_BRAKE
#define TORQUE_BRAKE 25.0
#endif
#ifndef SPEED_FWD
#define SPEED_FWD 16.0
#endif
#ifndef SPEED_REV
#define SPEED_REV 9.0
#endif

static int box_part(struct game* g, struct g_ent* E, int part, double x, double y, double hw, double hh, double density, uint32_t layer, uint32_t mask, uint32_t color) {
    int b = ph_add_box(&g->world, x, y, hw, hh, density, 1);
    if (b < 0) return -1;
    g->world.body[b].layer = layer;
    g->world.body[b].mask = mask;
    g->world.body[b].ent = (int)(E - g->ent);
    g->world.body[b].damp_ang = 0.15;
    E->body[part] = b;
    E->part_fill[part] = color;
    return b;
}

static int ball_part(struct game* g, struct g_ent* E, int part, double x, double y, double r, double density, uint32_t layer, uint32_t mask, uint32_t color) {
    int b = ph_add_circle(&g->world, x, y, r, density, 1);
    if (b < 0) return -1;
    g->world.body[b].layer = layer;
    g->world.body[b].mask = mask;
    g->world.body[b].ent = (int)(E - g->ent);
    E->body[part] = b;
    E->part_fill[part] = color;
    return b;
}

static int hinge(struct game* g, struct g_ent* E, int a, int b, double x, double y, double lo, double hi, double snap) {
    int j = ph_add_pin(&g->world, a, b, (v2){ x, y });
    if (j < 0) return -1;
    if (lo < hi) ph_joint_limit(&g->world, j, lo, hi);
    g->world.joint[j].break_force = snap;
    if (E->njoint < G_MAX_PARTS + 4) E->joint[E->njoint++] = j;
    return j;
}

int g_spawn_player(struct game* g, const char* kind, double x, double y) {
    if (strcmp(kind, "wheelchair") && strcmp(kind, "guy")) return -1;
    int ei = g_ent_new(g, K_PLAYER, 0);
    if (ei < 0) return -1;
    struct g_ent* E = &g->ent[ei];
    for (int i = 0; i < G_MAX_PARTS; i++) E->body[i] = -1;
    E->nbody = PART_COUNT;
    E->player_kind = 0;
    E->hp = 100;
    E->fill = 0x4aa0e0;
    const uint32_t SKIN = 0xf0c8a0, SHIRT = 0x3a7bd5, PANTS = 0x2b3a55, STEEL = 0x8a929c, TYRE = 0x2a2a30;
    const uint32_t UPPER_MASK = L_WORLD, LEG_MASK = L_WORLD | L_FRAME, FRAME_MASK = L_WORLD | L_LEGS, WHEEL_MASK = L_WORLD;

    // the chair
    int chair = box_part(g, E, PART_CHAIR, x - 0.12, y, 0.22, 0.06, 40, L_FRAME, FRAME_MASK, STEEL);
    int wheel = ball_part(g, E, PART_WHEEL, x - 0.45, y + 0.30, 0.46, 8, L_WHEEL, WHEEL_MASK, TYRE);
    int caster = ball_part(g, E, PART_CASTER, x + 0.55, y + 0.60, 0.15, 6, L_WHEEL, WHEEL_MASK, TYRE);
    // the rider
    int torso = box_part(g, E, PART_TORSO, x - 0.12, y - 0.30, 0.09, 0.22, 70, L_UPPER, UPPER_MASK, SHIRT);
    int head = ball_part(g, E, PART_HEAD, x - 0.12, y - 0.72, 0.17, 70, L_UPPER, UPPER_MASK, SKIN);
    int ua_l = box_part(g, E, PART_UARM_L, x - 0.12, y - 0.34, 0.035, 0.10, 70, L_UPPER, UPPER_MASK, SKIN);
    int fa_l = box_part(g, E, PART_FARM_L, x - 0.02, y - 0.24, 0.10, 0.03, 70, L_UPPER, UPPER_MASK, SKIN);
    int ua_r = box_part(g, E, PART_UARM_R, x - 0.12, y - 0.34, 0.035, 0.10, 70, L_UPPER, UPPER_MASK, SKIN);
    int fa_r = box_part(g, E, PART_FARM_R, x - 0.02, y - 0.24, 0.10, 0.03, 70, L_UPPER, UPPER_MASK, SKIN);
    int th_l = box_part(g, E, PART_THIGH_L, x + 0.02, y - 0.105, 0.14, 0.045, 70, L_LEGS, LEG_MASK, PANTS);
    int sh_l = box_part(g, E, PART_SHIN_L, x + 0.16, y + 0.045, 0.04, 0.15, 70, L_LEGS, LEG_MASK, PANTS);
    int th_r = box_part(g, E, PART_THIGH_R, x + 0.02, y - 0.105, 0.14, 0.045, 70, L_LEGS, LEG_MASK, PANTS);
    int sh_r = box_part(g, E, PART_SHIN_R, x + 0.16, y + 0.045, 0.04, 0.15, 70, L_LEGS, LEG_MASK, PANTS);
    for (int i = 0; i < PART_COUNT; i++)
        if (E->body[i] < 0) {                                       // the world ran out of bodies: undo everything
            for (int k = 0; k < PART_COUNT; k++) if (E->body[k] >= 0) ph_remove_body(&g->world, E->body[k]);
            E->used = 0;
            return -1;
        }
    g->world.body[head].fric = 0.8;
    g->world.body[wheel].fric = 1.3;
    g->world.body[caster].fric = 0.9;
    g->world.body[chair].fric = 0.5;
    g->world.body[chair].damp_ang = 1.4;                 // spins die down by themselves: the chair is a heavy lump, not a top

    // joints: (a, b, x, y, lo, hi, snap force)
    E->njoint = 0;
    int j_wheel = hinge(g, E, chair, wheel, x - 0.45, y + 0.30, 0, 0, 0);
    ph_joint_motor(&g->world, j_wheel, 0, 0);
    hinge(g, E, chair, caster, x + 0.55, y + 0.60, 0, 0, 0);
    hinge(g, E, chair, torso, x - 0.12, y - 0.09, -0.06, 0.7, 0);              // seat belt + backrest (can lean forward, hardly back): leaving it is what space does
    hinge(g, E, torso, head, x - 0.12, y - 0.52, -0.7, 0.7, 6000);
    hinge(g, E, torso, ua_l, x - 0.12, y - 0.44, -2.4, 1.0, 3200);
    hinge(g, E, ua_l, fa_l, x - 0.12, y - 0.24, -0.2, 2.4, 2200);
    hinge(g, E, torso, ua_r, x - 0.12, y - 0.44, -2.4, 1.0, 3200);
    hinge(g, E, ua_r, fa_r, x - 0.12, y - 0.24, -0.2, 2.4, 2200);
    hinge(g, E, torso, th_l, x - 0.12, y - 0.105, -1.4, 0.5, 4200);
    hinge(g, E, th_l, sh_l, x + 0.16, y - 0.105, -0.7, 1.3, 3000);
    hinge(g, E, torso, th_r, x - 0.12, y - 0.105, -1.4, 0.5, 4200);
    hinge(g, E, th_r, sh_r, x + 0.16, y - 0.105, -0.7, 1.3, 3000);
    E->w = 1.1;
    E->h = 1.4;
    return ei;
}

static int joint_alive(const struct game* g, int j) { return j >= 0 && g->world.joint[j].used; }

void g_player_controls(struct game* g, double dt) {
    struct g_ent* E = &g->ent[g->player];
    int chair = E->body[PART_CHAIR], wheel_j = E->joint[0];
    if (chair < 0 || !joint_alive(g, wheel_j)) return;
    struct ph_world* w = &g->world;
    int up = g->held[GK_UP], down = g->held[GK_DOWN], left = g->held[GK_LEFT], right = g->held[GK_RIGHT];
    // grip and weight scale with gravity, so the motor does too: full torque on the moon would stand the chair on its tail
    double grav = w->gravity.y / 14.0;
    grav = grav < 0.25 ? 0.25 : grav > 1 ? 1 : grav;
    double want = 0, torque = 0;
    if (up && !down) { want = SPEED_FWD; torque = TORQUE_DRIVE; }
    else if (down && !up) {
        double vx = w->body[chair].v.x;
        if (vx > 1.0) { want = 0; torque = TORQUE_BRAKE; }                       // still rolling forward: brake first
        else { want = -SPEED_REV; torque = TORQUE_DRIVE * 0.8; }
    }
    // ease the requested wheel speed toward the wanted one: a full-power start would stand the chair up on its back wheel
    double step = ACCEL * grav * dt;
    if (E->drive < want) E->drive = E->drive + step > want ? want : E->drive + step;
    else if (E->drive > want) E->drive = E->drive - step * 2 < want ? want : E->drive - step * 2;
    if (!up && !down) { E->drive = w->joint[wheel_j].used ? 0 : 0; ph_joint_motor(w, wheel_j, 0, 0); }
    else ph_joint_motor(w, wheel_j, E->drive, torque * grav);
    // the rider's back muscles: a gentle motor on the hip holds the torso upright, but a hard knock overpowers it
    if (E->njoint > 2 && joint_alive(g, E->joint[2]) && E->body[PART_TORSO] >= 0) {
        double th = w->body[E->body[PART_TORSO]].a - w->body[chair].a;
        double speed = -9.0 * th;
        speed = speed > 3 ? 3 : speed < -3 ? -3 : speed;
        ph_joint_motor(w, E->joint[2], speed, 30);
    }
    // the rider instinctively leans forward when the chair stands up too far (holding LEFT overrides that on purpose)
    {
        double tilt = w->body[chair].a;
        if (!left && tilt < -0.45 && tilt > -2.6) { w->body[chair].w += (-tilt - 0.45) * 30 * dt; }
    }
    // leaning: spin the whole rig about its middle
    double lean = (right ? 1 : 0) - (left ? 1 : 0);
    if (lean != 0) {
        w->body[chair].w += lean * 11 * dt;
        int torso = E->body[PART_TORSO];
        if (torso >= 0 && w->body[torso].used) w->body[torso].w += lean * 6 * dt;
    }
    if (g->held[GK_SPACE]) { g->held[GK_SPACE] = 0; g_player_eject(g, g->player); }     // one eject per press
    if (E->invuln > 0) E->invuln -= dt;
    // falling out of the world
    int tb = E->body[PART_TORSO];
    if (tb >= 0 && w->body[tb].p.y > 90) g_player_die(g, g->player, "FELL OFF THE WORLD");
}

void g_player_eject(struct game* g, int e) {
    struct g_ent* E = &g->ent[e];
    struct ph_world* w = &g->world;
    if (E->kind != K_PLAYER || E->dead) return;
    // the seat belt is the third joint
    if (E->njoint > 2 && joint_alive(g, E->joint[2])) {
        ph_remove_joint(w, E->joint[2]);
        int torso = E->body[PART_TORSO], chair = E->body[PART_CHAIR];
        if (torso >= 0 && chair >= 0) {
            v2 kick = { w->body[chair].v.x * 0.35 + 3.0, -6.5 };
            for (int i = 0; i < PART_CHAIR; i++)
                if (E->body[i] >= 0 && w->body[E->body[i]].used) { w->body[E->body[i]].v.x += kick.x; w->body[E->body[i]].v.y += kick.y; }
            g_say(g, "WHEEEEE", 1.5);
        }
    }
}

int g_player_part_of_body(const struct game* g, int e, int body) {
    for (int i = 0; i < PART_COUNT; i++) if (g->ent[e].body[i] == body) return i;
    return -1;
}

void g_player_die(struct game* g, int e, const char* why) {
    struct g_ent* E = &g->ent[e];
    if (E->dead) return;
    E->dead = 1;
    for (int j = 0; j < E->njoint; j++) ph_joint_motor(&g->world, E->joint[j], 0, 0);
    int tb = E->body[PART_TORSO];
    if (tb >= 0 && g->world.body[tb].used) {
        g_blood(g, g->world.body[tb].p.x, g->world.body[tb].p.y, 60);
        g->shake += 0.5;
    }
    if (g->state == GS_PLAYING) {
        g->state = GS_DEAD;
        g->end_timer = 0;
        char msg[80] = "";
        size_t n = 0;
        const char* tail = "  -  press R to try again";
        for (const char* p = why; *p && n < 40; p++) msg[n++] = *p;
        if (!n) { const char* s = "OUCH!"; while (*s) msg[n++] = *s++; }
        for (const char* p = tail; *p && n + 1 < sizeof msg; p++) msg[n++] = *p;
        msg[n] = 0;
        g_say(g, msg, 1e9);
    }
    if (g->vm) dm_fire_dies(g->vm, e, E->tag);
}

// A hit on part `part` at `speed` m/s (closing speed). Heads break first.
void g_player_impact(struct game* g, int e, int part, double speed) {
    struct g_ent* E = &g->ent[e];
    if (E->dead || E->invuln > 0 || part < 0) return;
    int head = part == PART_HEAD, torso = part == PART_TORSO;
    int body_part = part <= PART_SHIN_R;
    if (!body_part) return;                                   // bashing the chair itself is free
    double deadly = head ? 11.0 : torso ? 14.0 : 1e9;
    if (speed > deadly) {
        struct ph_body* b = &g->world.body[E->body[part]];
        g_blood(g, b->p.x, b->p.y, 30);
        g_player_die(g, e, head ? "HEAD FIRST!" : "SPLAT!");
        return;
    }
    if (speed > 6.0) {
        struct ph_body* b = &g->world.body[E->body[part]];
        g_blood(g, b->p.x, b->p.y, (int)(speed * 1.5));
        E->invuln = 0.15;
        g->shake += 0.04 * speed;
    }
}

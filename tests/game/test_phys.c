// Physics engine tests: things rest, bounce, hang, drive, break and never explode.
// build/run: sh tests/game/run.sh
#include "../../src/game/phys.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static void check(int ok, const char* name, const char* fmt, double a, double b) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s: ", name); printf(fmt, a, b); printf("\n"); }
}

static void run(struct ph_world* w, double seconds) {
    for (double t = 0; t < seconds; t += 1.0 / 60) ph_step(w, 1.0 / 60);
}

static int finite_world(const struct ph_world* w) {
    for (int i = 0; i < PH_MAX_BODIES; i++) {
        const struct ph_body* b = &w->body[i];
        if (b->used && (!isfinite(b->p.x) || !isfinite(b->p.y) || !isfinite(b->a) || !isfinite(b->v.x) || !isfinite(b->v.y) || !isfinite(b->w))) return 0;
    }
    return 1;
}

int main(void) {
    static struct ph_world w;

    // 1. a box dropped on the ground comes to rest on top of it
    ph_init(&w);
    int ground = ph_add_box(&w, 0, 10, 50, 1, 1, 0);          // top surface at y = 9
    int box = ph_add_box(&w, 0, 5, 0.5, 0.5, 1, 1);
    run(&w, 3);
    check(fabs(w.body[box].p.y - 8.5) < 0.03, "box rests on the ground", "y = %.4f (want 8.5)%.0s", w.body[box].p.y, 0);
    check(fabs(w.body[box].v.y) < 0.05 && fabs(w.body[box].w) < 0.05, "box holds still", "v = %.4f, w = %.4f", w.body[box].v.y, w.body[box].w);
    (void)ground;

    // 2. a ball bounces: 0.8 restitution brings it back up to roughly 0.64 of the height, then it settles
    ph_init(&w);
    ph_add_box(&w, 0, 10, 50, 1, 1, 0);
    int ball = ph_add_circle(&w, 0, 0, 0.5, 1, 1);            // drops from 8.5 above the ground
    w.body[ball].rest = 0.8;
    double min_y = 100, peak_after = 100;
    int bounced = 0;
    for (double t = 0; t < 4; t += 1.0 / 60) {
        ph_step(&w, 1.0 / 60);
        double y = w.body[ball].p.y;
        if (y > 8.4 && !bounced) bounced = 1;                 // reached the floor
        if (bounced && w.body[ball].v.y < 0 && y < peak_after) peak_after = y;
        if (y < min_y) min_y = y;
    }
    double up = 8.5 - peak_after;                             // height regained after the first hit (measured from the floor contact y = 8.5)
    check(bounced && up > 3.5 && up < 7.5, "ball bounces", "regained %.2f m of 8.5 (%.0s)", up, 0);
    run(&w, 8);
    check(fabs(w.body[ball].v.y) < 0.2 && fabs(w.body[ball].p.y - 8.5) < 0.05, "ball settles", "y = %.3f, vy = %.3f", w.body[ball].p.y, w.body[ball].v.y);

    // 3. a tower of five boxes stays a tower
    ph_init(&w);
    ph_add_box(&w, 0, 10, 50, 1, 1, 0);
    int top = 0;
    for (int i = 0; i < 5; i++) top = ph_add_box(&w, 0, 8.5 - i * 1.0, 0.5, 0.5, 1, 1);
    run(&w, 6);
    check(fabs(w.body[top].p.x) < 0.1 && fabs(w.body[top].p.y - 4.5) < 0.15, "five-box tower stands", "top at (%.3f, %.3f)", w.body[top].p.x, w.body[top].p.y);

    // 4. a pendulum keeps its length and does not gain energy
    ph_init(&w);
    int anchor = ph_add_box(&w, 0, 0, 0.1, 0.1, 1, 0);
    int bob = ph_add_circle(&w, 3, 0, 0.3, 1, 1);
    ph_add_pin(&w, anchor, bob, (v2){ 0, 0 });
    double worst = 0, e0 = w.body[bob].m * 14.0 * (0 - 0);        // reference level: the start (y = 0), so total energy starts at 0
    double emax = -1e9;
    for (double t = 0; t < 6; t += 1.0 / 60) {
        ph_step(&w, 1.0 / 60);
        double len = hypot(w.body[bob].p.x, w.body[bob].p.y);
        if (fabs(len - 3) > worst) worst = fabs(len - 3);
        double e = ph_kinetic_energy(&w) - w.body[bob].m * 14.0 * w.body[bob].p.y;     // KE + PE (y points down)
        if (e > emax) emax = e;
    }
    (void)e0;
    check(worst < 0.06, "pendulum keeps its length", "worst drift %.4f m%.0s", worst, 0);
    check(emax < 0.3, "pendulum does not gain energy", "energy rose to %.4f%.0s", emax, 0);

    // 5. a hinged door falls and stops at its limit
    ph_init(&w);
    int post = ph_add_box(&w, 0, 0, 0.1, 0.1, 1, 0);
    int door = ph_add_box(&w, 1.0, 0, 1.0, 0.1, 1, 1);
    w.body[post].group = w.body[door].group = 3;                   // the door overlaps its post: they must not fight
    int hinge = ph_add_pin(&w, post, door, (v2){ 0, 0 });
    ph_joint_limit(&w, hinge, -0.3, 0.6);
    run(&w, 4);
    check(w.body[door].a <= 0.6 + 0.06 && w.body[door].a > 0.45, "joint limit holds", "angle %.3f (limit 0.6)%.0s", w.body[door].a, 0);

    // 6. a motor drives a car: two wheels on a chassis cross the floor
    ph_init(&w);
    ph_add_box(&w, 0, 10, 100, 1, 1, 0);
    int chassis = ph_add_box(&w, 0, 8, 1.2, 0.3, 1, 1);
    int wl = ph_add_circle(&w, -1, 8.4, 0.5, 1, 1), wr = ph_add_circle(&w, 1, 8.4, 0.5, 1, 1);
    w.body[chassis].group = w.body[wl].group = w.body[wr].group = 1;
    w.body[wl].fric = w.body[wr].fric = 1.2;
    int j1 = ph_add_pin(&w, chassis, wl, (v2){ -1, 8.4 }), j2 = ph_add_pin(&w, chassis, wr, (v2){ 1, 8.4 });
    ph_joint_motor(&w, j1, 12, 60);
    ph_joint_motor(&w, j2, 12, 60);
    run(&w, 4);
    check(w.body[chassis].p.x > 5, "motorised car drives", "x = %.2f%.0s", w.body[chassis].p.x, 0);
    check(fabs(w.body[chassis].p.y - 8.0) < 0.5, "car stays on the road", "y = %.2f%.0s", w.body[chassis].p.y, 0);

    // 7. a joint that carries too much snaps and says so
    ph_init(&w);
    int hook = ph_add_box(&w, 0, 0, 0.2, 0.2, 1, 0);
    int weight = ph_add_box(&w, 0, 1, 0.5, 0.5, 20, 1);
    int rope = ph_add_pin(&w, hook, weight, (v2){ 0, 0.5 });
    w.joint[rope].break_force = 200;                               // the weight pulls with m*g = 20 * 1 * 14 = 280
    run(&w, 1);
    int broke = 0;
    ph_init(&w);
    hook = ph_add_box(&w, 0, 0, 0.2, 0.2, 1, 0);
    weight = ph_add_box(&w, 0, 1, 0.5, 0.5, 20, 1);
    rope = ph_add_pin(&w, hook, weight, (v2){ 0, 0.5 });
    w.joint[rope].break_force = 200;
    for (double t = 0; t < 1; t += 1.0 / 60) {
        ph_step(&w, 1.0 / 60);
        for (int e = 0; e < w.nev; e++) if (w.ev[e].kind == PE_JOINT_BREAK) broke++;
    }
    check(broke == 1 && !w.joint[rope].used, "overloaded joint breaks exactly once", "events %.0f%.0s", broke, 0);
    ph_init(&w);
    hook = ph_add_box(&w, 0, 0, 0.2, 0.2, 1, 0);
    weight = ph_add_box(&w, 0, 1, 0.5, 0.5, 2, 1);
    rope = ph_add_pin(&w, hook, weight, (v2){ 0, 0.5 });
    w.joint[rope].break_force = 200;
    run(&w, 3);
    check(w.joint[rope].used, "a light load does not break it", "force %.1f%.0s", w.joint[rope].last_force, 0);

    // 8. touch events: one per meeting, with the closing speed
    ph_init(&w);
    ph_add_box(&w, 0, 10, 50, 1, 1, 0);
    box = ph_add_box(&w, 0, 5, 0.5, 0.5, 1, 1);               // falls 3.5 m onto the surface at 9 -> v = sqrt(2*14*3.5) ~ 9.9
    int touches = 0;
    double speed = 0;
    for (double t = 0; t < 3; t += 1.0 / 60) {
        ph_step(&w, 1.0 / 60);
        for (int e = 0; e < w.nev; e++) if (w.ev[e].kind == PE_TOUCH && w.ev[e].speed > 3) { touches++; speed = w.ev[e].speed; }   // (the little hop after it is a second, gentle touch)
    }
    check(touches == 1, "one hard touch event for a landing", "%.0f events%.0s", touches, 0);
    check(speed > 8.5 && speed < 11, "touch speed", "%.2f m/s%.0s", speed, 0);

    // 9. same-group bodies do not collide; layers filter
    ph_init(&w);
    int p1 = ph_add_box(&w, 0, 0, 1, 1, 1, 1), p2 = ph_add_box(&w, 0.2, 0, 1, 1, 1, 1);
    w.body[p1].group = w.body[p2].group = 7;
    w.body[p1].gscale = w.body[p2].gscale = 0;
    run(&w, 1);
    check(fabs(w.body[p2].p.x - 0.2) < 1e-9, "a group does not push itself apart", "x = %.4f%.0s", w.body[p2].p.x, 0);
    w.body[p1].group = 0;
    w.body[p1].layer = 2; w.body[p2].mask = 1;
    run(&w, 1);
    check(fabs(w.body[p2].p.x - 0.2) < 1e-9, "layers and masks filter", "x = %.4f%.0s", w.body[p2].p.x, 0);

    // 10. sensors report but do not push
    ph_init(&w);
    ph_add_box(&w, 0, 10, 50, 1, 1, 0);
    int sens = ph_add_box(&w, 0, 8.5, 2, 0.5, 1, 0);
    w.body[sens].sensor = 1;
    box = ph_add_box(&w, 0, 5, 0.5, 0.5, 1, 1);
    int sensor_touches = 0;
    for (double t = 0; t < 2; t += 1.0 / 60) {
        ph_step(&w, 1.0 / 60);
        for (int e = 0; e < w.nev; e++) if (w.ev[e].kind == PE_TOUCH && (w.ev[e].a == sens || w.ev[e].b == sens)) sensor_touches++;
    }
    check(sensor_touches >= 1 && fabs(w.body[box].p.y - 8.5) < 0.1, "sensor sees the box and lets it land", "touches %.0f, y %.2f", sensor_touches, w.body[box].p.y);

    // 11. a hostile pile: 120 mixed bodies, some joints, 20 seconds: no NaN, no explosion
    srand(7);
    ph_init(&w);
    ph_add_box(&w, 0, 20, 60, 1, 1, 0);
    ph_add_box(&w, -30, 10, 1, 12, 1, 0);
    ph_add_box(&w, 30, 10, 1, 12, 1, 0);
    int prev = -1;
    for (int i = 0; i < 120; i++) {
        double x = (rand() % 400) / 10.0 - 20, y = -(rand() % 300) / 10.0;
        int b = rand() % 3 ? ph_add_box(&w, x, y, 0.3 + (rand() % 8) / 10.0, 0.3 + (rand() % 8) / 10.0, 1, 1) : ph_add_circle(&w, x, y, 0.3 + (rand() % 6) / 10.0, 1, 1);
        if (b < 0) break;
        w.body[b].a = (rand() % 628) / 100.0;
        if (prev >= 0 && i % 4 == 0) ph_add_pin(&w, prev, b, (v2){ (w.body[prev].p.x + w.body[b].p.x) / 2, (w.body[prev].p.y + w.body[b].p.y) / 2 });
        prev = b;
    }
    run(&w, 20);
    double top_speed = 0;
    for (int i = 0; i < PH_MAX_BODIES; i++) if (w.body[i].used) { double v = hypot(w.body[i].v.x, w.body[i].v.y); if (v > top_speed) top_speed = v; }
    check(finite_world(&w), "hostile pile stays finite", "%.0f%.0s", 0, 0);
    check(top_speed < 60, "hostile pile does not explode", "fastest body %.1f m/s%.0s", top_speed, 0);

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}

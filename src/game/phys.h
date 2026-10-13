// A small 2D rigid body physics engine for the dmccl game engine: circles and convex polygons, pin joints with limits,
// motors and breaking, friction, restitution, sensors. Written from scratch (impulse-based solver, SAT + edge clipping).
//
// Units: meters, seconds, radians. +y points DOWN (screen style), gravity is positive y.
// Everything is `double` and deterministic: the same inputs give the same world, so replays and ghosts work.
#ifndef GAME_PHYS_H
#define GAME_PHYS_H

#include <stdint.h>

typedef struct { double x, y; } v2;

#define PH_MAX_BODIES   256
#define PH_MAX_JOINTS   192
#define PH_MAX_CONTACTS 768
#define PH_MAX_VERTS    8
#define PH_MAX_EVENTS   96

enum { SH_CIRCLE, SH_POLY };

struct ph_body {
    uint8_t used, shape, dynamic, sensor;
    int ent;                          // the game entity this body belongs to (-1: none)
    int group;                        // bodies of the same non-zero group never collide (a ragdoll's own parts)
    uint32_t layer, mask;             // a collides with b when (a.layer & b.mask) && (b.layer & a.mask)
    v2 p;  double a;                  // position of the centre of mass, angle
    v2 v;  double w;                  // linear and angular velocity
    double m, im, I, iI;              // mass and inertia (and their inverses; 0 = immovable)
    double fric, rest, radius;
    double gscale;                    // gravity multiplier
    double damp_lin, damp_ang;        // velocity damping per second
    v2 vert[PH_MAX_VERTS];            // polygon, counter-clockwise, local to the centre of mass
    v2 nrm[PH_MAX_VERTS];             // outward edge normals
    int nv;
    double c, s;                      // cos/sin of a (refreshed every substep)
    double hit_mass;                  // game-side bookkeeping: how much this part weighs for impact damage
};

struct ph_joint {                     // a pin: point la of body a is held on point lb of body b
    uint8_t used, limit, motor;
    int a, b;
    v2 la, lb;                        // anchors, local to each body
    double ref;                       // relative angle (b - a) when the joint was made
    double lo, hi;                    // allowed relative angle range (when `limit`)
    double motor_speed, motor_torque; // target relative angular speed and the most torque the motor may use
    double break_force;               // impulse per second beyond which the pin snaps (0: never)
    double acc_x, acc_y, acc_lim, acc_motor;
    double last_force;                // the force it carried last step (for damage and tests)
};

struct ph_contact {
    int a, b;
    v2 n;                             // from a to b
    v2 pt;
    double depth;
    int fid;                          // which feature pair: lets impulses carry over to the next substep
    v2 ra, rb;
    double acc_n, acc_t, mass_n, mass_t, bias, rest_v;
    double fric;
};

enum { PE_TOUCH, PE_JOINT_BREAK };
struct ph_event {                     // things the game wants to hear about
    int kind;
    int a, b;                         // the two bodies (a joint's bodies for PE_JOINT_BREAK)
    double speed;                     // PE_TOUCH: how fast they were closing when they met (m/s)
    double impulse;                   // PE_TOUCH: hardest normal impulse this frame; PE_JOINT_BREAK: what it carried
    v2 pt;
};

struct ph_world {
    struct ph_body body[PH_MAX_BODIES];
    struct ph_joint joint[PH_MAX_JOINTS];
    struct ph_contact con[PH_MAX_CONTACTS];
    int ncon;
    struct ph_contact prev[PH_MAX_CONTACTS];    // last substep's contacts (warm starting, begin-touch detection)
    int nprev;
    struct ph_event ev[PH_MAX_EVENTS];
    int nev;
    v2 gravity;
    int substeps, iterations;
    double time;
    uint32_t pair_seen[PH_MAX_BODIES][PH_MAX_BODIES / 32];      // pairs that touched during the last frame
    uint32_t pair_now[PH_MAX_BODIES][PH_MAX_BODIES / 32];       // pairs that touched during this frame
};

void ph_init(struct ph_world* w);

// bodies (return the index, or -1 when the world is full)
int  ph_add_box(struct ph_world* w, double x, double y, double hw, double hh, double density, int dynamic);
int  ph_add_circle(struct ph_world* w, double x, double y, double r, double density, int dynamic);
int  ph_add_poly(struct ph_world* w, double x, double y, const v2* verts, int n, double density, int dynamic);   // convex, any winding
void ph_remove_body(struct ph_world* w, int body);              // also removes its joints

// joints
int  ph_add_pin(struct ph_world* w, int a, int b, v2 world_point);                         // pin at a world point
int  ph_add_pin_local(struct ph_world* w, int a, v2 la, int b, v2 lb);                     // pin between two local anchors
void ph_joint_limit(struct ph_world* w, int j, double lo, double hi);                      // relative angle limits (b minus a)
void ph_joint_motor(struct ph_world* w, int j, double speed, double max_torque);           // speed 0 + torque = a friction brake
void ph_remove_joint(struct ph_world* w, int j);

// stepping: dt in seconds; events from the step are in w->ev[0..nev)
void ph_step(struct ph_world* w, double dt);

// helpers
v2     ph_world_point(const struct ph_body* b, v2 local);
v2     ph_body_velocity_at(const struct ph_body* b, v2 world_point);
int    ph_body_at(const struct ph_world* w, v2 p);              // the topmost body containing the point, or -1
void   ph_apply_impulse(struct ph_body* b, v2 impulse, v2 world_point);
double ph_kinetic_energy(const struct ph_world* w);

#endif

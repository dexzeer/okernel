// Game engine internals shared by game.c (world, scripts), game_chars.c (characters) and game_gfx.c (renderer).
#ifndef GAME_INTERNAL_H
#define GAME_INTERNAL_H

#include "game.h"
#include "phys.h"
#include "../dmccl/dm.h"
#include "../dmccl/dm_sys.h"

#define G_MAX_ENTS    160
#define G_MAX_PARTS   18
#define G_MAX_PARTICLES 700
#define G_MAX_TEXTS   8
#define G_PPM         40.0      // pixels per meter at the reference zoom

// What an entity draws as (and, for the player, how it behaves).
enum { K_BLOCK, K_RAMP, K_CRATE, K_BALL, K_PLANK, K_SPIKES, K_GOAL, K_BRIDGE, K_ROPE, K_BARREL, K_PLAYER, K_DEBRIS, K_KINDS };

enum { PART_HEAD, PART_TORSO, PART_UARM_L, PART_FARM_L, PART_UARM_R, PART_FARM_R, PART_THIGH_L, PART_SHIN_L, PART_THIGH_R, PART_SHIN_R,
       PART_CHAIR, PART_WHEEL, PART_CASTER, PART_COUNT };

struct g_ent {
    uint8_t used, kind, dead, fixed;      // dead: a player whose ragdoll lies there limp; fixed: part of the level (static)
    char tag[20];                         // what `when X touches "tag"` matches (defaults to the kind's name)
    int body[G_MAX_PARTS];                // physics bodies, indexed by PART_* for a player, in creation order otherwise
    int nbody;
    int joint[G_MAX_PARTS + 4];           // joints a player's limbs hang on (so breaking them can be told apart)
    int njoint;
    double hp;
    uint32_t fill, line;
    uint32_t part_fill[G_MAX_PARTS];      // per-body colours for characters (0: use `fill`)
    double w, h;                          // size (for drawing and for scripts)
    double fx;                            // misc per-kind timer (a barrel's fuse flash, a player's blink)
    int player_kind;                      // which vehicle (0 wheelchair)
    int parent, child;                    // who made this entity / the last one it made (`parent`, `child` in scripts)
    int limbs_lost;                       // player: joints that snapped
    double drive;                         // player: the wheel speed the motor is currently asked for (eases up, so the chair does not flip at once)
    double invuln;                        // player: seconds of grace after a hit (so one crash is one hit)
};

struct g_particle {
    double x, y, vx, vy, life, max_life;
    uint32_t color;
    uint8_t size, shape;                  // shape 0 square, 1 star
};

struct g_text {
    double x, y, life;
    char s[28];
};

struct game {
    struct ph_world world;
    struct g_ent ent[G_MAX_ENTS];
    struct g_particle part[G_MAX_PARTICLES];
    int npart;
    struct g_text text[G_MAX_TEXTS];

    dm_program* prog;
    dm_vm* vm;
    char* src;                            // kept so restart can reload
    size_t src_len;
    void (*log_fn)(void*, const char*);
    void* log_user;

    uint8_t held[GK_COUNT];
    int state;
    double time;                          // level time (seconds)
    double accum;                         // time not yet consumed by fixed steps
    double ms_accum;                      // game milliseconds not yet handed to the script VM
    int player;                           // entity index of the player (-1 until spawned)
    int gore;
    double end_timer;                     // seconds since won / died
    char message[80];
    double message_time;
    double cam_x, cam_y, cam_zoom;        // camera centre (meters) and zoom factor
    double shake;
    double zoom_goal;
    uint32_t rng;
    int flash;                            // white flash frames
    double sky_t;
};

// game.c
int  g_ent_new(struct game* g, int kind, const char* tag);
void g_ent_kill(struct game* g, int e, int with_fx);         // fires `dies`, removes the bodies (a player only goes limp)
void g_ent_abort(struct game* g, int e);                     // gone, no fuss
void g_blood(struct game* g, double x, double y, int n);
void g_boom(struct game* g, double x, double y, double power);
void g_say(struct game* g, const char* msg, double secs);
double g_rand(struct game* g);                               // 0..1, deterministic
int  g_ent_of_body(const struct game* g, int body);
void g_particle_add(struct game* g, double x, double y, double vx, double vy, double life, uint32_t color, int size, int shape);

// game_chars.c
int  g_spawn_player(struct game* g, const char* kind, double x, double y);   // returns the entity or -1
void g_player_controls(struct game* g, double dt);
void g_player_die(struct game* g, int e, const char* why);
void g_player_eject(struct game* g, int e);
void g_player_impact(struct game* g, int e, int part, double speed);
int  g_player_part_of_body(const struct game* g, int e, int body);

// game_gfx.c
void g_draw(struct game* g, uint32_t* px, int w, int h, int stride);

#endif

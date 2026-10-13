// The dmccl game engine: a physics world + entities + a pixel renderer, driven by a dmccl level script.
//
//   struct game* g = game_new();
//   game_load(g, source, len, err, sizeof err);     // compiles + starts the level (restart = load again)
//   loop: game_key(g, KEY_UP, 1); game_update(g, dt); game_render(g, pixels, w, h);
//
// The same code runs in the kernel (a window) and on the host (tools/game/, headless screenshots and tests).
#ifndef GAME_H
#define GAME_H

#include <stddef.h>
#include <stdint.h>

struct game;

enum { GK_UP, GK_DOWN, GK_LEFT, GK_RIGHT, GK_SPACE, GK_SHIFT, GK_CTRL, GK_COUNT };

enum { GS_PLAYING, GS_DEAD, GS_WON };

struct game* game_new(void);
void         game_free(struct game* g);

// Compile and start a level. Returns 0, or -1 with a message ("line 12: ..." or a link error) in err.
int          game_load(struct game* g, const char* src, size_t len, char* err, size_t err_cap);
void         game_set_log(struct game* g, void (*fn)(void* user, const char* line), void* user);

// Input: held keys (arrows/WASD, space, shift, ctrl) and named key presses for `when key "r"` handlers.
void         game_key(struct game* g, int key, int down);
void         game_key_name(struct game* g, const char* name);       // "r", "space", "enter" ... fires `when key "name"`

void         game_update(struct game* g, double dt);                // advance the world by dt seconds (fixed 1/60 s steps inside)
void         game_render(struct game* g, uint32_t* px, int w, int h, int stride);   // 0xAARRGGBB, opaque

int          game_state(const struct game* g);                      // GS_*
double       game_time(const struct game* g);                       // seconds since the level started
const char*  game_message(const struct game* g);                    // the banner text ("" when none)
void         game_set_gore(struct game* g, int level);              // 0 cartoon, 1 red (default)

#endif

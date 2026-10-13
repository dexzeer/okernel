// Level solver: tries random key schedules until one wins, then prints it in --keys form. Proof that a level can be beaten.
//   gamebot level.dmccl [tries] [seed]
#include "../../src/game/game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void quiet(void* u, const char* l) { (void)u; (void)l; }
static unsigned rs;
static unsigned rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

#define MAXSEG 160
struct seg { double t; int up, down, left, right, space; };

static const char* NAMES[] = { "up", "down", "left", "right", "space" };

int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    static char src[40000];
    size_t n = fread(src, 1, sizeof src, f);
    fclose(f);
    int tries = argc > 2 ? atoi(argv[2]) : 300;
    rs = argc > 3 ? (unsigned)atoi(argv[3]) : 1;
    struct game* g = game_new();
    game_set_log(g, quiet, 0);
    for (int attempt = 0; attempt < tries; attempt++) {
        struct seg plan[MAXSEG];
        int np = 0;
        double t = 0.3 + (rnd() % 30) / 10.0;
        // a biased random walk over the key state
        struct seg cur = { 0, 0, 0, 0, 0, 0 };
        plan[np++] = cur;
        for (; t < 60 && np < MAXSEG - 1; t += 0.2 + (rnd() % 12) / 10.0) {
            cur.t = t;
            cur.up = rnd() % 10 < 8;
            cur.down = rnd() % 25 == 0;
            cur.left = rnd() % 8 == 0;
            cur.right = rnd() % 8 == 0;
            cur.space = rnd() % 40 == 0;
            plan[np++] = cur;
        }
        char err[200];
        if (game_load(g, src, n, err, sizeof err)) { printf("level error: %s\n", err); return 1; }
        int k = 0;
        for (double now = 0; now < 70; now += 1.0 / 60) {
            while (k < np && plan[k].t <= now) {
                game_key(g, GK_UP, plan[k].up); game_key(g, GK_DOWN, plan[k].down);
                game_key(g, GK_LEFT, plan[k].left); game_key(g, GK_RIGHT, plan[k].right); game_key(g, GK_SPACE, plan[k].space);
                k++;
            }
            game_update(g, 1.0 / 60);
            if (game_state(g) != GS_PLAYING) break;
        }
        if (game_state(g) == GS_WON) {
            printf("WON after %d tries (%.1f s). keys:\n", attempt + 1, game_time(g));
            for (int key = 0; key < 5; key++) {
                int on = 0;
                double start = 0;
                int first = 1;
                for (int i = 0; i < np; i++) {
                    int v = key == 0 ? plan[i].up : key == 1 ? plan[i].down : key == 2 ? plan[i].left : key == 3 ? plan[i].right : plan[i].space;
                    if (v && !on) { start = plan[i].t; on = 1; }
                    if (!v && on) { printf("%s%s@%.1f-%.1f", first ? "" : ",", NAMES[key], start, plan[i].t); first = 0; on = 0; }
                }
                if (on) printf("%s%s@%.1f-%.1f", first ? "" : ",", NAMES[key], start, 80.0);
                if (!first) printf(",");
            }
            printf("\n");
            return 0;
        }
    }
    printf("no win in %d tries\n", tries);
    return 1;
}

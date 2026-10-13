// Headless game runner for the host: plays a level with scripted keys and writes screenshots.
//   gamehost level.dmccl [--keys "up@0-6,right@2-3,space@4"] [--run SECONDS] [--shot out.ppm@T ...] [--size WxH]
// Keys are held between the two times (or pressed at one time). Prints the script's log and the final state.
#include "../../src/game/game_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void say(void* user, const char* line) { (void)user; printf("[log] %s\n", line); }

struct hold { int key; double t0, t1; };

static int key_of(const char* n) {
    static const char* const N[GK_COUNT] = { "up", "down", "left", "right", "space", "shift", "ctrl" };
    for (int i = 0; i < GK_COUNT; i++) if (!strcmp(n, N[i])) return i;
    return -1;
}

static void shot(struct game* g, const char* path, int w, int h) {
    uint32_t* px = (uint32_t*)calloc((size_t)w * h, 4);
    game_render(g, px, w, h, w);
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { unsigned char c[3] = { (px[i] >> 16) & 255, (px[i] >> 8) & 255, px[i] & 255 }; fwrite(c, 1, 3, f); }
    fclose(f);
    free(px);
}

int main(int argc, char** argv) {
    const char* path = 0;
    double run = 10;
    int W = 960, H = 540;
    struct hold holds[64];
    int nholds = 0;
    struct { char path[256]; double t; } shots[16];
    int nshots = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run") && i + 1 < argc) run = atof(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshots < 16) {
            char* at = strrchr(argv[++i], '@');
            if (!at) { fprintf(stderr, "--shot out.ppm@seconds\n"); return 2; }
            *at = 0;
            strncpy(shots[nshots].path, argv[i], 255);
            shots[nshots++].t = atof(at + 1);
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            char* s = strdup(argv[++i]);
            for (char* tok = strtok(s, ","); tok && nholds < 64; tok = strtok(0, ",")) {
                char* at = strchr(tok, '@');
                if (!at) continue;
                *at = 0;
                double t0, t1;
                int k = key_of(tok);
                if (k < 0) { fprintf(stderr, "unknown key %s\n", tok); return 2; }
                if (sscanf(at + 1, "%lf-%lf", &t0, &t1) == 2) { holds[nholds].key = k; holds[nholds].t0 = t0; holds[nholds++].t1 = t1; }
                else { t0 = atof(at + 1); holds[nholds].key = k; holds[nholds].t0 = t0; holds[nholds++].t1 = t0 + 0.05; }
            }
            free(s);
        } else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: gamehost level.dmccl [--keys ...] [--run S] [--shot f.ppm@T] [--size WxH]\n"); return 2; }
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* src = (char*)malloc((size_t)n + 1);
    if (fread(src, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    struct game* g = game_new();
    game_set_log(g, say, 0);
    char err[256];
    if (game_load(g, src, (size_t)n, err, sizeof err)) { fprintf(stderr, "level error: %s\n", err); return 1; }
    int shot_done[16] = { 0 };
    for (double t = 0; t < run; t += 1.0 / 60) {
        for (int i = 0; i < nholds; i++) game_key(g, holds[i].key, t >= holds[i].t0 && t < holds[i].t1);
        game_update(g, 1.0 / 60);
        if (getenv("DUMP") && ((int)(t * 60 + 0.5)) % atoi(getenv("DUMP")) == 0 && g->player >= 0) {
            struct g_ent* E = &g->ent[g->player];
            printf("t=%.0f", t);
            for (int p = PART_TORSO; p <= PART_CASTER; p += (p == PART_TORSO ? 7 : 1)) {
                if (p == PART_CHAIR || p == PART_WHEEL || p == PART_CASTER || p == PART_TORSO) {
                    struct ph_body* b = &g->world.body[E->body[p]];
                    printf("  [%d] (%.2f,%.2f) a=%.0f w=%.1f", p, b->p.x, b->p.y, b->a * 57.3, b->w);
                }
            }
            printf("\n");
        }
        for (int i = 0; i < nshots; i++) if (!shot_done[i] && t >= shots[i].t) { shot(g, shots[i].path, W, H); shot_done[i] = 1; }
    }
    static const char* const ST[] = { "playing", "dead", "won" };
    printf("state: %s  time: %.1f  message: %s\n", ST[game_state(g)], game_time(g), game_message(g));
    game_free(g);
    free(src);
    return 0;
}

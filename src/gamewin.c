// The game window; see gamewin.h.
#include "gamewin.h"
#include "cpufreq.h"
#include "filesystem.h"
#include "game/game.h"
#include "keyboard.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "window.h"

extern uint32_t tick_count;

extern const char lvl_1[], lvl_1_end[], lvl_2[], lvl_2_end[], lvl_3[], lvl_3_end[], lvl_4[], lvl_4_end[];

static const struct { const char* name; const char* start; const char* end; } LEVELS[] = {
    { "Rolling Start (hold UP and roll)", lvl_1, lvl_1_end },
    { "Wrecking Yard (wrecker, barrels, a pit)", lvl_2, lvl_2_end },
    { "Moon Walk (low gravity, trampolines)", lvl_3, lvl_3_end },
    { "Chain Reaction (barrels do not like each other)", lvl_4, lvl_4_end },
};
#define N_LEVELS ((int)(sizeof LEVELS / sizeof LEVELS[0]))

static struct {
    int win;
    struct game* g;
    uint32_t* px;
    int cap, sw, sh;
    int level;                           // built-in level index, -1 for a file
    char file[FS_MAX_NAME];
    uint32_t last_tick, last_render;
    int render_gap;                      // ticks between pictures: grows when drawing is slow so the physics keeps real time
    int esc_was_down;
    char msg[640];
} W = { .win = -1, .level = -1 };

int gamewin_win(void) { return W.win; }
int gamewin_level_count(void) { return N_LEVELS; }
const char* gamewin_level_name(int i) { return i >= 0 && i < N_LEVELS ? LEVELS[i].name : 0; }

static void say(void* user, const char* line) { (void)user; serial_printf("[game] %s\n", line); }

static void cat(char* d, size_t cap, const char* s) {
    size_t n = strlen(d);
    while (*s && n + 1 < cap) d[n++] = *s++;
    d[n] = 0;
}

static int start_level(const char* src, size_t len) {
    char err[200];
    if (game_load(W.g, src, len, err, sizeof err)) {
        W.msg[0] = 0;
        cat(W.msg, sizeof W.msg, "level error: ");
        cat(W.msg, sizeof W.msg, err);
        cat(W.msg, sizeof W.msg, "\n");
        serial_printf("[game] %s", W.msg);
        return -1;
    }
    return 0;
}

void gamewin_close(void) {
    if (W.win >= 0) {
        window_set_pixels(W.win, 0, 0, 0, 0, 0, 0);
        window_destroy(W.win);
        W.win = -1;
    }
    if (W.g) { game_free(W.g); W.g = 0; }
    if (W.px) { kfree(W.px); W.px = 0; W.cap = 0; }
    serial_printf("[game] closed\n");
}

const char* gamewin_open(const char* what) {
    W.msg[0] = 0;
    while (*what == ' ') what++;
    if (!*what) {                                         // no argument: the list
        cat(W.msg, sizeof W.msg, "KAnarchy Wheels - a ragdoll obstacle course, scripted in dmccl.\n");
        for (int i = 0; i < N_LEVELS; i++) {
            char line[96] = "  game ";
            line[7] = (char)('1' + i);
            line[8] = 0;
            cat(line, sizeof line, "    ");
            cat(line, sizeof line, LEVELS[i].name);
            cat(line, sizeof line, "\n");
            cat(W.msg, sizeof W.msg, line);
        }
        cat(W.msg, sizeof W.msg, "  game FILE   run a level script from the file system (write one with `edit FILE`)\n");
        cat(W.msg, sizeof W.msg, "keys: arrows/WASD drive and lean, space eject, R restart, N next level, Esc close\n");
        return W.msg;
    }
    static char file_src[FS_MAX_SIZE + 1];
    const char* src;
    size_t len;
    int level = -1;
    if (what[0] >= '1' && what[0] <= '9' && what[1] == 0) {
        level = what[0] - '1';
        if (level >= N_LEVELS) { cat(W.msg, sizeof W.msg, "no such level\n"); return W.msg; }
        src = LEVELS[level].start;
        len = (size_t)(LEVELS[level].end - LEVELS[level].start);
    } else {
        int n = fs_read(what, (uint8_t*)file_src, FS_MAX_SIZE);
        if (n < 0) { cat(W.msg, sizeof W.msg, "no such level or file: "); cat(W.msg, sizeof W.msg, what); cat(W.msg, sizeof W.msg, "\n"); return W.msg; }
        file_src[n] = 0;
        src = file_src;
        len = (size_t)n;
    }
    if (W.win >= 0) gamewin_close();
    W.g = game_new();
    if (!W.g) { cat(W.msg, sizeof W.msg, "out of memory\n"); return W.msg; }
    game_set_log(W.g, say, 0);
    if (start_level(src, len)) { game_free(W.g); W.g = 0; return W.msg; }
    W.level = level;
    W.file[0] = 0;
    if (level < 0) { strncpy(W.file, what, FS_MAX_NAME - 1); W.file[FS_MAX_NAME - 1] = 0; }

    int cw = 960, ch = 540;
    int ww = cw + 2 * WIN_BORDER, wh = ch + WIN_TITLE_H + 2 * WIN_BORDER;
    if (wh > SCREEN_H - TASKBAR_H - 8) {                  // small screens: a smaller picture, same shape
        ch = SCREEN_H - TASKBAR_H - 8 - WIN_TITLE_H - 2 * WIN_BORDER;
        cw = ch * 16 / 9;
        ww = cw + 2 * WIN_BORDER;
        wh = ch + WIN_TITLE_H + 2 * WIN_BORDER;
    }
    W.win = window_create("KAnarchy Wheels", (SCREEN_W - ww) / 2, (SCREEN_H - TASKBAR_H - wh) / 2 > 4 ? (SCREEN_H - TASKBAR_H - wh) / 2 : 4, ww, wh);
    if (W.win < 0) { game_free(W.g); W.g = 0; cat(W.msg, sizeof W.msg, "cannot create the window\n"); return W.msg; }
    window_set_close_button(W.win, 1);
    window_set_minimize_button(W.win, 1);
    window_set_hide_cursor(W.win, 1);
    window_set_focus(W.win);
    W.sw = W.sh = 0;
    W.last_tick = tick_count;
    W.last_render = 0;
    W.render_gap = 2;
    W.esc_was_down = 1;                                   // the Enter that ran the command is not an Esc
    serial_printf("[game] started %s\n", level >= 0 ? LEVELS[level].name : what);
    return 0;
}

void gamewin_key(char c) {
    if (W.win < 0 || !W.g) return;
    serial_printf("[game] key '%c'\n", c >= 32 ? c : '?');
    if (c == 'r' || c == 'R') { serial_printf("[game] restart\n"); game_key_name(W.g, "r"); return; }
    if (c == 'n' || c == 'N') {
        if (W.level >= 0 && W.level + 1 < N_LEVELS) {
            W.level++;
            start_level(LEVELS[W.level].start, (size_t)(LEVELS[W.level].end - LEVELS[W.level].start));
        }
        return;
    }
    if (c == ' ') { game_key_name(W.g, "space"); return; }
    if (c >= 'a' && c <= 'z') { char name[2] = { c, 0 }; game_key_name(W.g, name); }
}

static void sync_surface(const struct window* win) {
    int cw = win->w - 2 * WIN_BORDER, ch = win->h - WIN_TITLE_H - 2 * WIN_BORDER;
    if (cw < 160 || ch < 90) return;
    if (cw == W.sw && ch == W.sh) return;
    if (cw * ch > W.cap) {
        if (W.px) kfree(W.px);
        W.px = (uint32_t*)kmalloc((uint32_t)cw * (uint32_t)ch * 4);
        W.cap = W.px ? cw * ch : 0;
    }
    W.sw = cw;
    W.sh = ch;
}

static inline uint64_t rdtsc64(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

void gamewin_poll(void) {
    if (W.win < 0 || !W.g) return;
    struct window* win = window_get(W.win);
    if (!win) { gamewin_close(); return; }
    uint32_t now = tick_count;
    if (now == W.last_tick) return;
    uint32_t ticks = now - W.last_tick;
    W.last_tick = now;

    int focused = window_get_focused() == W.win;
    int esc = focused && keyboard_scancode_held(0x01, 0);
    if (esc && !W.esc_was_down) { gamewin_close(); return; }
    W.esc_was_down = esc;
    int held_up = focused && (keyboard_scancode_held(0x48, 1) || keyboard_scancode_held(0x11, 0));
    int held_down = focused && (keyboard_scancode_held(0x50, 1) || keyboard_scancode_held(0x1F, 0));
    int held_left = focused && (keyboard_scancode_held(0x4B, 1) || keyboard_scancode_held(0x1E, 0));
    int held_right = focused && (keyboard_scancode_held(0x4D, 1) || keyboard_scancode_held(0x20, 0));
    game_key(W.g, GK_UP, held_up);
    game_key(W.g, GK_DOWN, held_down);
    game_key(W.g, GK_LEFT, held_left);
    game_key(W.g, GK_RIGHT, held_right);
    game_key(W.g, GK_SPACE, focused && keyboard_scancode_held(0x39, 0));
    game_key(W.g, GK_SHIFT, focused && (keyboard_scancode_held(0x2A, 0) || keyboard_scancode_held(0x36, 0)));
    game_key(W.g, GK_CTRL, focused && keyboard_scancode_held(0x1D, 0));

    if (win->visible) game_update(W.g, ticks * 0.01);      // a minimised game waits

    if (!win->visible || now - W.last_render < (uint32_t)W.render_gap) return;
    sync_surface(win);
    if (!W.px) return;
    uint64_t t0 = rdtsc64();
    game_render(W.g, W.px, W.sw, W.sh, W.sw);
    window_set_pixels(W.win, W.px, 0, 0, W.sw, W.sh, W.sw);
    uint64_t cycles = rdtsc64() - t0;
    W.last_render = now;
    uint32_t mhz = cpufreq_tsc_mhz();
    if (mhz) {                                            // draw no more often than it can be afforded: ~1/3 of the time at most
        uint32_t ms = (uint32_t)(cycles / ((uint64_t)mhz * 1000));
        int gap = (int)(ms * 3 / 10) + 2;
        W.render_gap = gap < 2 ? 2 : gap > 10 ? 10 : gap;
    }
}

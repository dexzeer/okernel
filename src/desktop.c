#include "net/pci.h"
#include "net/e1000.h"
#include "net/network.h"
#include "net/tls_net.h"
#include "crypto/tls_client.h"  // TLS_FAIL_* reason codes for the no-downgrade gate
#include "rtc.h"
#include "graphics.h"
#include "wallpaper.h"
#include "window.h"
#include "paging.h"
#include "memlayout.h"
#include "gdt.h"
#include "syscall.h"
#include "idt.h"
#include "keyboard.h"
#include "memory.h"
#include "serial.h"
#include "io.h"
#include "filesystem.h"
#include "editor.h"
#include "okai.h"
#include "js/js_dom.h"
#include "theme.h"
#include "crypto/rand.h"
#include "crypto/ec.h" // ec_init() eager curve build (see ec.h)
#include "process.h"
#include "sched.h"
#include "sys_proc.h"
#include "spinlock.h"
#include "pfs.h"
#include "ata.h"
#include "userland_seed.h"

struct mboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} __attribute__((packed));

static int mouse_down = 0;
static int drag_win = -1;
static int drag_off_x = 0;
static int drag_off_y = 0;
static int resize_win = -1;
static int resize_off_w = 0;
static int resize_off_h = 0;

// Terminal management
#define MAX_TERMINALS 8
static int term_wins[MAX_TERMINALS];
static int term_count = 0;
static int active_term_idx = 0;

// Per-terminal shell state
static char term_bufs[MAX_TERMINALS][256];
static int term_lens[MAX_TERMINALS];

// System info window
static int info_win = -1;

uint32_t tick_count = 0;
int needs_redraw = 1; // Global flag: wallpaper + windows need full redraw

// FPS tracking
static uint32_t frame_count = 0;
static uint32_t fps = 0;
static uint32_t last_fps_tick = 0;

// Desktop icon system
#define ICON_SIZE 48
#define ICON_LABEL_H (CHAR_H + 4) // one text row + margin
#define ICON_SPACING 12
#define ICONS_PER_ROW 10
#define ICON_START_X 16
#define ICON_START_Y 16

// 16x16 .txt file icon bitmap (1=white, 0=transparent)
// A document with folded corner and horizontal lines
static const uint8_t txt_icon[16][2] = {
    {0x7F, 0xE0}, //  01111111 11100000  - top edge
    {0x40, 0x20}, //  01000000 00100000  - left edge + fold
    {0x40, 0x20}, //  01000000 00100000
    {0x41, 0xE0}, //  01000001 11100000  - fold corner
    {0x41, 0x00}, //  01000001 00000000  - fold bottom
    {0x41, 0x00}, //  01000001 00000000
    {0x41, 0x00}, //  01000001 00000000
    {0x41, 0xFC}, //  01000001 11111100  - line 1
    {0x41, 0x00}, //  01000001 00000000
    {0x41, 0xFC}, //  01000001 11111100  - line 2
    {0x41, 0x00}, //  01000001 00000000
    {0x41, 0xFC}, //  01000001 11111100  - line 3
    {0x41, 0x00}, //  01000001 00000000
    {0x41, 0x00}, //  01000001 00000000
    {0x7F, 0xFE}, //  01111111 11111110  - bottom edge
    {0x00, 0x00}, //  00000000 00000000
};

static void draw_txt_icon(int x, int y) {
    rect_fill(x, y, ICON_SIZE, ICON_SIZE, 0x00AAAAAA);
    rect_outline(x, y, ICON_SIZE, ICON_SIZE, 0x00555555, 2);

    int ox = x + 16;
    int oy = y + 8;
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            int byte_idx = col / 8;
            int bit_idx = 7 - (col % 8);
            if (txt_icon[row][byte_idx] & (1 << bit_idx)) {
                putpixel(ox + col, oy + row, 0x00000000);
            }
        }
    }
}

// Draw desktop icons, optionally limited to those intersecting a repair rect
// (pass 0,0,SCREEN_W,SCREEN_H to draw everything)
static void draw_desktop_icons_in(int rx, int ry, int rw, int rh) {
    int count = fs_get_count();
    for (int i = 0; i < count; i++) {
        const char* name = fs_get_name(i);
        if (!name) continue;

        int row = i / ICONS_PER_ROW;
        int col = i % ICONS_PER_ROW;
        int ix = ICON_START_X + col * (ICON_SIZE + ICON_SPACING);
        int iy = ICON_START_Y + row * (ICON_SIZE + ICON_LABEL_H + ICON_SPACING);

        // Icon + label footprint (label sits at iy+ICON_SIZE+4, 32px tall)
        if (ix >= rx + rw || ix + ICON_SIZE <= rx ||
            iy >= ry + rh || iy + ICON_SIZE + ICON_LABEL_H <= ry) {
            continue;
        }

        draw_txt_icon(ix, iy);

        // Draw filename label centered below icon
        int name_len = 0;
        while (name[name_len]) name_len++;

        int label_x = ix + (ICON_SIZE - name_len * CHAR_W) / 2;
        if (label_x < ix) label_x = ix;

        // Truncate name to fit (max ~5 chars for 32px icon)
        int max_chars = ICON_SIZE / CHAR_W;
        if (max_chars > 6) max_chars = 6;

        char label[8];
        int li = 0;
        while (li < max_chars && name[li]) {
            label[li] = name[li];
            li++;
        }
        label[li] = 0;
        if (name_len > max_chars) {
            label[li - 1] = '~';
        }

        draw_string(ix + 2, iy + ICON_SIZE + 4, label, 0x00FFFFFF, 0x00000000);
    }
}

static void draw_desktop_icons(void) {
    draw_desktop_icons_in(0, 0, SCREEN_W, SCREEN_H);
}

// Recomposite a screen region purely from the scene model:
// wallpaper -> icons -> window stack (back-to-front) -> taskbar.
// This is the single authoritative repair path — used to erase the cursor
// sprite and any other region whose pixels must return to model truth.
// skip_win: window id to leave out of the recomposite (-1 = none) — used
// by the drag path when the dragged window's pixels were already blitted.
static void desktop_paint_rect_skip(int skip_win, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;

    // All repair drawing is clip-limited so only the damaged rows/columns
    // get marked dirty (keeps flush volume proportional to repair size)
    graphics_set_clip(x, y, w, h);

    graphics_blit_wallpaper_rect(x, y, w, h);
    draw_desktop_icons_in(x, y, w, h);

    // Windows back-to-front by z (highest z = topmost, painted last so it
    // covers any lower-z okai chrome/heading that would otherwise bleed
    // through). Same order as the main draw loop in window_draw_all.
    int vis[MAX_WINDOWS], nv = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* win = window_get(i);
        if (!win || !win->visible || win->minimized || i == skip_win) continue;
        if (win->x < x + w && win->x + win->w > x &&
            win->y < y + h && win->y + win->h > y)
            vis[nv++] = i;
    }
    for (int a = 1; a < nv; a++) {
        int id = vis[a], zb = window_get(id)->z, b = a - 1;
        while (b >= 0 && window_get(vis[b])->z > zb) { vis[b + 1] = vis[b]; b--; }
        vis[b + 1] = id;
    }
    for (int a = 0; a < nv; a++) {
        int id = vis[a];
        window_paint_region(id, x, y, w, h);
        int ob = okai_find_by_win(id);
        if (ob >= 0) okai_paint_overlays(ob);
    }

    if (y + h > SCREEN_H - TASKBAR_H) {
        window_draw_taskbar();
    }

    graphics_clip_reset();
}

static void desktop_paint_rect(int x, int y, int w, int h) {
    desktop_paint_rect_skip(-1, x, y, w, h);
}

// Public wrapper so other modules (e.g. window.c on close/minimize) can repair
// a screen region now that the backbuffer is persistent and we no longer do a
// full repaint every frame.
void desktop_paint_rect_pub(int x, int y, int w, int h) {
    desktop_paint_rect_skip(-1, x, y, w, h);
}

// Cursor compositor state: where the sprite was painted last frame.
// The backbuffer is only ever "model + this one sprite".
static int cursor_shown = 0;
static int cursor_px = 0;
static int cursor_py = 0;
static int g_last_mouse_x = -1;
static int g_last_mouse_y = -1;

// Returns file index if icon clicked, -1 otherwise
static int check_icon_click(int mx, int my) {
    int count = fs_get_count();
    for (int i = 0; i < count; i++) {
        int row = i / ICONS_PER_ROW;
        int col = i % ICONS_PER_ROW;
        int ix = ICON_START_X + col * (ICON_SIZE + ICON_SPACING);
        int iy = ICON_START_Y + row * (ICON_SIZE + ICON_LABEL_H + ICON_SPACING);

        if (mx >= ix && mx < ix + ICON_SIZE &&
            my >= iy && my < iy + ICON_SIZE + ICON_LABEL_H) {
            return i;
        }
    }
    return -1;
}

static void shell_prompt(int win_id);
// Foreground-run waiter: armed by `run` (shell IRQ context, never blocks),
// consumed by the main-loop drain after the matching pid exits. The shell
// runs as pid 0 with no PCB-parent link to spawned children, so the waiter
// records pid+window explicitly; the drain matches zombie→waiter and prints
// the exit status into the owning window.
static int run_wait_pid = -1;
static int run_wait_win = -1;
void run_wait_arm(int pid, int win) {
    run_wait_pid = pid;
    run_wait_win = win;
}
// Offer-wake flag: set by on_keypress (keyboard IRQ context — must NEVER
// IRET there) after staging a kbd offer; consumed by the main loop (plain
// thread context) which wake-enters the oldest BLOCKED entered reader.
static volatile int kbd_wake_armed = 0;

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

// Resolve a shell word to a VFS binary path: exact name first, /bin/<name>.
// Writes into `out` (64B) and returns out, or 0 when neither exists.
static const char *binpath_of(const char *name) {
    static char bins[2][64];
    static int flip = 0;
    char *out = bins[flip]; flip ^= 1;
    uint32_t bi = 0;
    if (name[0] == '/') {
        while (name[bi] && bi < 63) { out[bi] = name[bi]; bi++; }
        out[bi] = 0;
        return fs_exists(out) ? out : 0;
    }
    const char *prefix = "/bin/";
    bi = 0;
    while (prefix[bi]) { out[bi] = prefix[bi]; bi++; }
    uint32_t ni = 0;
    while (name[ni] && bi < 63) { out[bi] = name[ni]; bi++; ni++; }
    out[bi] = 0;
    return fs_exists(out) ? out : 0;
}

// Shared `run` implementation (used by the `run` builtin AND the Unknown:
// filesystem fallback below). Parses `rest` (program + args + optional `&`),
// spawns via sched_spawn_elf, stages the entry drain, arms foreground wait.
// Runs in keyboard-IRQ context — never iret here, only stage.
static void shell_execute_run(int win_id, const char *rest) {
    char argbuf[256];
    uint32_t alen = 0;
    while (rest[alen] && alen < 255) { argbuf[alen] = rest[alen]; alen++; }
    argbuf[alen] = 0;
    // Tokenize (in place, max 16 argv entries incl. program name).
    char *rargv[17];
    uint32_t rargc = 0;
    uint32_t i = 0;
    int background = 0;
    while (i < alen && rargc < 16) {
        while (i < alen && argbuf[i] == ' ') i++;
        if (i >= alen) break;
        uint32_t s = i;
        while (i < alen && argbuf[i] != ' ') i++;
        int is_last = (i >= alen);
        // `&` as the final token means background (not an argument).
        uint32_t tlen = i - s;
        if (is_last && tlen == 1 && argbuf[s] == '&') {
            background = 1;
            break;
        }
        argbuf[i] = 0; // NUL-terminate (i<alen here, or trailing NUL)
        rargv[rargc++] = &argbuf[s];
        if (i < alen) i++;
    }
    rargv[rargc] = 0;
    if (rargc == 0) {
        window_puts(win_id, "Usage: run <path> [args...] [&]\n");
        return;
    }
    // Resolve the path: exact VFS name first, then /bin/<name>.
    const char *resolved = binpath_of(rargv[0]);
    if (!resolved) {
        window_puts(win_id, "run: cannot load ");
        window_puts(win_id, rargv[0]);
        window_put_char(win_id, '\n');
        return;
    }
    // PID 1 (lazy, first-run): the boot-time spawn raced init's fork/exec
    // children against the entry drain (see kernel_main note). Spawn init
    // here instead — the drain is live (we're past boot) and the run path
    // is proven. One-shot (init_armed); fail-soft when /sbin/init missing.
    // ORDER: the requested program's entry is queued below AFTER this block
    // runs — so enqueue init FIRST here would jump the queue. Instead this
    // block only ARMS (spawns the PCB); the init ENTRY is enqueued after the
    // program entry below. (Spawn-then-enqueue split keeps FIFO correct.)
    // RE-ENTRANCY GUARD: shell_execute_run runs in keyboard-IRQ context
    // while the main loop may ALSO be inside the entry drain (a tick stole
    // ring 3 mid-program and a keystroke landed). init_armed is set BEFORE
    // the spawn (not after) so a nested run can't double-spawn init; and
    // the program-entry enqueue below is skipped when the queue is full
    // (spawn is destroyed instead of wedging the drain — see below).
    int init_pid = -1;
    {
        static int init_armed = 0;
        if (!init_armed) {
            init_armed = 1;
            char *init_argv[] = { "/sbin/init", 0 };
            init_pid = sched_spawn_elf("/sbin/init", init_argv, 0, win_id);
            if (init_pid >= 0) {
                struct process *ip = process_get((uint32_t)init_pid);
                serial_printf("[boot] init pid=%d spawned from /sbin/init\n",
                              init_pid);
                if (ip) ip->term_win = win_id;
            } else {
                serial_puts("[boot] no /sbin/init — kernel shell only\n");
            }
        }
    }
    char path[64];
    {
        uint32_t pi = 0;
        while (resolved[pi] && pi < 63) { path[pi] = resolved[pi]; pi++; }
        path[pi] = 0;
    }
    if (entry_pending_any()) {
        window_puts(win_id, "run: another program is starting\n");
        return;
    }
    int pid = sched_spawn_elf(path, rargv, 0, win_id);
    if (pid < 0) {
        window_puts(win_id, "run: cannot load ");
        window_puts(win_id, path);
        window_put_char(win_id, '\n');
        return;
    }
    struct process *rp = process_get((uint32_t)pid);
    serial_printf("[run] pid=%d '%s' argc=%d bg=%d\n",
                  pid, path, rargc, background);
    window_puts(win_id, "Starting ");
    window_puts(win_id, path);
    window_put_char(win_id, '\n');
    syscall_can_exit = 1;
    // Enqueue the program entry; if the queue is FULL (a fork child or an
    // earlier spawn still staged — possible when a tick stole ring 3 and a
    // keystroke landed mid-drain), destroy the fresh spawn and fail LOUD
    // instead of silently dropping the entry (which wedges the waiter: the
    // drain announces Back/exit for a pid that never runs).
    if (rp) {
        if (entry_enqueue(pid, rp->user_eip, rp->user_esp, win_id)) {
            serial_printf("[run] pid=%d entry queue full — dropped\n", pid);
            window_puts(win_id, "run: system busy, try again\n");
            sched_reap((uint32_t)pid);
            if (init_pid >= 0) {
                struct process *ip = process_get((uint32_t)init_pid);
                if (ip) sched_reap((uint32_t)init_pid);
            }
            syscall_can_exit = 0;
            return;
        }
    } else {
        entry_enqueue(pid, 0x08048000, 0xBFFFF000 + 4096, win_id);
    }
    // Init entry goes second (FIFO after the program that triggered first-run).
    if (init_pid >= 0) {
        struct process *ip = process_get((uint32_t)init_pid);
        if (ip && entry_enqueue(init_pid, ip->user_eip, ip->user_esp, win_id)) {
            // Queue filled between the two enqueues (absurd — 4 slots):
            // reap init, keep the program (fail-soft, no wedge).
            serial_puts("[run] init entry dropped (queue full)\n");
            sched_reap((uint32_t)init_pid);
        }
    }
    if (!background) {
        // Foreground: the main loop waits (blocking) and prints the exit
        // status (run_wait_arm — armed here, consumed in the drain loop;
        // the shell IRQ thread itself never blocks or irets).
        extern void run_wait_arm(int pid, int win);
        run_wait_arm(pid, win_id);
    } else {
        serial_printf("[run] pid=%d background\n", pid);
        window_puts(win_id, "[bg] pid ");
        {
            char pb[12]; int pbi = 0, tv = pid;
            char rv[12]; int ri = 0;
            if (tv == 0) pb[pbi++] = '0';
            while (tv > 0 && pbi < 11) { rv[ri++] = '0' + (tv % 10); tv /= 10; }
            while (ri > 0) pb[pbi++] = rv[--ri];
            pb[pbi] = 0;
            window_puts(win_id, pb);
        }
        window_put_char(win_id, '\n');
    }
}

static void shell_execute(int win_id, const char* input);

// Int to string helper
static void put_uint(char* buf, uint32_t val) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char rev[16];
    int ri = 0;
    while (val > 0) { rev[ri++] = '0' + (val % 10); val /= 10; }
    int i = 0;
    while (ri > 0) { buf[i++] = rev[--ri]; }
    buf[i] = 0;
}

static void open_sysinfo(void) {
    if (info_win >= 0) return; // Already open
    info_win = window_create("System Info", 600, 80, 700, 500);
    window_set_close_button(info_win, 1);
    window_set_minimize_button(info_win, 1);
    window_puts(info_win, "okernel v0.7\n");
    window_puts(info_win, "Desktop Edition\n\n");
    window_puts(info_win, "Resolution: 1920x1080\n");
    window_puts(info_win, "Shell: okernel sh\n");
    window_puts(info_win, "Commands: help, clear,\n");
    window_puts(info_win, "echo, mem, uptime,\n");
    window_puts(info_win, "about, neofetch,\n");
    window_puts(info_win, "terminal, exit,\n");
    window_puts(info_win, "sysinfo, reboot,\n");
    window_puts(info_win, "shutdown\n");
}

static void close_sysinfo(void) {
    if (info_win >= 0) {
        window_destroy(info_win);
        info_win = -1;
    }
}

static int create_terminal(void) {
    if (term_count >= MAX_TERMINALS) return -1;
    int x = 80 + (term_count % 4) * 80;
    int y = 60 + (term_count % 4) * 60;
    int win_id = window_create("Terminal", x, y, 900, 650);
    window_set_close_button(win_id, 1);
    window_set_minimize_button(win_id, 1);
    term_wins[term_count] = win_id;
    term_lens[term_count] = 0;
    term_bufs[term_count][0] = 0;
    term_count++;
    return win_id;
}

static void shell_prompt(int win_id) {
    window_set_text_color(win_id, 10, 0); // Green on black
    window_puts(win_id, "okernel> ");
    window_set_text_color(win_id, 15, 0); // Back to white on black
}

static int find_term_idx(int win_id) {
    for (int i = 0; i < term_count; i++) {
        if (term_wins[i] == win_id) return i;
    }
    return -1;
}

static void destroy_terminal(int idx) {
    if (idx < 0 || idx >= term_count) return;
    // Can't close the main terminal (idx 0)
    if (idx == 0) return;

    window_destroy(term_wins[idx]);

    // Shift remaining terminals down
    for (int i = idx; i < term_count - 1; i++) {
        term_wins[i] = term_wins[i + 1];
        term_lens[i] = term_lens[i + 1];
        for (int j = 0; j < 256; j++) term_bufs[i][j] = term_bufs[i + 1][j];
    }
    term_count--;

    // Fix active index
    if (active_term_idx >= term_count) active_term_idx = term_count - 1;
}

static void shell_execute(int win_id, const char* input) {
    while (*input == ' ') input++;
    if (*input == 0) return;

    char cmd_buf[32];
    int cmd_len = 0;
    while (input[cmd_len] && input[cmd_len] != ' ') cmd_len++;
    if (cmd_len > 31) cmd_len = 31;
    for (int i = 0; i < cmd_len; i++) cmd_buf[i] = input[i];
    cmd_buf[cmd_len] = 0;

    serial_puts("[sh] exec: ");
    serial_puts(cmd_buf);
    serial_putchar('\n');

    const char* args = input + cmd_len;
    while (*args == ' ') args++;

    if (str_eq(cmd_buf, "help")) {
        window_puts(win_id, "Commands:\n");
        window_puts(win_id, "  help      - show this\n");
        window_puts(win_id, "  clear     - clear terminal\n");
        window_puts(win_id, "  echo      - print text\n");
        window_puts(win_id, "  mem       - memory info\n");
        window_puts(win_id, "  uptime    - system uptime\n");
        window_puts(win_id, "  about     - about okernel\n");
        window_puts(win_id, "  neofetch  - system info\n");
        window_puts(win_id, "  sysinfo   - open info window\n");
        window_puts(win_id, "  terminal  - open new terminal\n");
        window_puts(win_id, "  exit      - close this terminal\n");
        window_puts(win_id, "  ping      - ping gateway\n");
        window_puts(win_id, "  ip        - show IP address\n");
        window_puts(win_id, "  netdrop N - drop 1-in-N TCP pkts (test)\n");
        window_puts(win_id, "  resolve   - DNS lookup\n");
        window_puts(win_id, "  okai   - open web okai\n");
        window_puts(win_id, "  edit      - open text editor\n");
        window_puts(win_id, "  ls        - list files\n");
        window_puts(win_id, "  open      - open file in editor\n");
        window_puts(win_id, "  disk      - disk + persistent FS status\n");
        window_puts(win_id, "  save      - force one file to disk\n");
        window_puts(win_id, "  locktest  - spinlock/mutex selftest\n");
        window_puts(win_id, "  ps        - list processes\n");
        window_puts(win_id, "  run       - run ELF program (/bin/*) [&]\n");
        window_puts(win_id, "  reboot    - reboot system\n");
        window_puts(win_id, "  shutdown  - power off\n");
    }
    else if (str_eq(cmd_buf, "ping")) {
        uint8_t* gw = net_get_gateway();
        window_puts(win_id, "Pinging gateway ");
        char ip_buf[16];
        int idx = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t v = gw[i];
            if (v >= 100) { ip_buf[idx++] = '0' + v/100; v %= 100; }
            if (v >= 10) { ip_buf[idx++] = '0' + v/10; v %= 10; }
            ip_buf[idx++] = '0' + v;
            if (i < 3) ip_buf[idx++] = '.';
        }
        ip_buf[idx] = 0;
        window_puts(win_id, ip_buf);
        window_puts(win_id, "...\n");
        icmp_send_ping(gw, 0x1234, 1);
    }
    else if (str_eq(cmd_buf, "ip")) {
        uint8_t* ip = net_get_ip();
        window_puts(win_id, "IP: ");
        char ip_buf[16];
        int idx = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t v = ip[i];
            if (v >= 100) { ip_buf[idx++] = '0' + v/100; v %= 100; }
            if (v >= 10) { ip_buf[idx++] = '0' + v/10; v %= 10; }
            ip_buf[idx++] = '0' + v;
            if (i < 3) ip_buf[idx++] = '.';
        }
        ip_buf[idx] = 0;
        window_puts(win_id, ip_buf);
        window_puts(win_id, "\n");
    }
    else if (str_eq(cmd_buf, "netdrop")) {
        // Test hook: drop every Nth received TCP packet (netdrop 0 = off)
        int n = 0;
        for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++) {
            n = n * 10 + (args[i] - '0');
        }
        net_set_drop_rate(n);
        if (n > 0) {
            window_puts(win_id, "Dropping 1 in ");
            char buf[8];
            put_uint(buf, (uint32_t)n);
            window_puts(win_id, buf);
            window_puts(win_id, " packets.\n");
        } else {
            window_puts(win_id, "Packet drop disabled.\n");
        }
    }
    else if (str_eq(cmd_buf, "resolve")) {
        if (args[0] == 0) {
            window_puts(win_id, "Usage: resolve <hostname>\n");
        } else {
            window_puts(win_id, "Resolving ");
            window_puts(win_id, args);
            window_puts(win_id, "...\n");
            dns_resolve(args);
        }
    }
    else if (str_eq(cmd_buf, "okai")) {
        // One browser window: reuse it and open the URL as a new tab; with no
        // argument, open the internal homepage.
        const char* target = args[0] ? args : OKAI_HOME_URL;
        if (okai_window_count() > 0) {
            okai_new_tab(0, target);
            window_set_focus(okai_get(0)->win_id);
            window_puts(win_id, "New tab: ");
            window_puts(win_id, target);
            window_put_char(win_id, '\n');
        } else {
            int ok = okai_open(target);
            if (ok >= 0) {
                window_puts(win_id, "Opened: ");
                window_puts(win_id, target);
                window_put_char(win_id, '\n');
            } else {
                window_puts(win_id, "Cannot open okai.\n");
            }
        }
    }
    else if (str_eq(cmd_buf, "edit")) {
        if (args[0] == 0) {
            window_puts(win_id, "Usage: edit <filename>\n");
            window_puts(win_id, "Example: edit readme.txt\n");
        } else {
            int ed = editor_open(args);
            if (ed >= 0) {
                window_puts(win_id, "Opened editor: ");
                window_puts(win_id, args);
                window_put_char(win_id, '\n');
            } else {
                window_puts(win_id, "Cannot open editor.\n");
            }
        }
    }
    else if (str_eq(cmd_buf, "ls")) {
        int count = fs_get_count();
        if (count == 0) {
            window_puts(win_id, "(empty)\n");
        } else {
            for (int i = 0; i < count; i++) {
                const char* name = fs_get_name(i);
                int size = fs_get_size(name);
                window_puts(win_id, "  ");
                window_puts(win_id, name);
                window_puts(win_id, "  (");
                char buf[8];
                int bi = 0;
                if (size == 0) { buf[bi++] = '0'; }
                else {
                    char rev[8]; int ri = 0;
                    int tmp = size;
                    while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
                    while (ri > 0) buf[bi++] = rev[--ri];
                }
                buf[bi] = 0;
                window_puts(win_id, buf);
                window_puts(win_id, " bytes)\n");
            }
        }
    }
    else if (str_eq(cmd_buf, "open")) {
        if (args[0] == 0) {
            window_puts(win_id, "Usage: open <filename>\n");
        } else if (!fs_exists(args)) {
            window_puts(win_id, "File not found: ");
            window_puts(win_id, args);
            window_put_char(win_id, '\n');
        } else {
            int ed = editor_open(args);
            if (ed >= 0) {
                window_puts(win_id, "Opened: ");
                window_puts(win_id, args);
                window_put_char(win_id, '\n');
            } else {
                window_puts(win_id, "Cannot open editor.\n");
            }
        }
    }
    else if (str_eq(cmd_buf, "disk")) {
        // Disk + persistent-FS diagnostics: presence, mount, table.
        serial_puts("[sh] disk status\n");
        if (!ata_present()) {
            window_puts(win_id, "disk: no ATA disk (VFS only)\n");
        } else {
            window_puts(win_id, "disk: ATA present, ");
            window_puts(win_id, pfs_mounted() ? "PFS mounted\n" : "PFS NOT mounted\n");
        }
        pfs_status(); // serial table dump (names/sizes/sectors)
        window_puts(win_id, "(see serial for PFS table)\n");
    }
    else if (str_eq(cmd_buf, "save")) {
        // save <file>: force one VFS file through to disk now (normally
        // write-through is automatic — this is the manual override + proof).
        if (args[0] == 0) {
            window_puts(win_id, "Usage: save <filename>\n");
        } else if (!fs_exists(args)) {
            window_puts(win_id, "File not found: ");
            window_puts(win_id, args);
            window_put_char(win_id, '\n');
        } else if (!ata_present()) {
            window_puts(win_id, "save: no disk (VFS only)\n");
        } else {
            int rc = pfs_sync_file(args);
            window_puts(win_id, rc == 0 ? "saved to disk\n" : "save FAILED\n");
        }
    }
    else if (str_eq(cmd_buf, "locktest")) {
        // Exercise spinlock/mutex primitives (no SMP needed) — serial PASS/FAIL.
        sys_proc_lock_selftest();
        window_puts(win_id, "lock selftest ran (see serial)\n");
    }
    else if (str_eq(cmd_buf, "clear")) {
        window_clear(win_id);
    }
    else if (str_eq(cmd_buf, "echo")) {
        window_puts(win_id, args);
        window_put_char(win_id, '\n');
    }
    else if (str_eq(cmd_buf, "mem")) {
        uint32_t total = pmm_get_total_pages() * 4 / 1024;
        uint32_t used = pmm_get_used_pages() * 4 / 1024;
        uint32_t free = pmm_get_free_pages() * 4 / 1024;
        char buf[16];

        window_puts(win_id, "Memory:\n");
        put_uint(buf, total); window_puts(win_id, "  Total: "); window_puts(win_id, buf); window_puts(win_id, " MB\n");
        put_uint(buf, used);  window_puts(win_id, "  Used:  "); window_puts(win_id, buf); window_puts(win_id, " MB\n");
        put_uint(buf, free);  window_puts(win_id, "  Free:  "); window_puts(win_id, buf); window_puts(win_id, " MB\n");
    }
    else if (str_eq(cmd_buf, "uptime")) {
        uint32_t seconds = tick_count / 100;
        uint32_t h = seconds / 3600;
        uint32_t m = (seconds % 3600) / 60;
        uint32_t s = seconds % 60;
        char buf[16];

        put_uint(buf, h); int i = 0; while (buf[i]) window_put_char(win_id, buf[i++]);
        window_put_char(win_id, 'h');
        if (m < 10) window_put_char(win_id, '0');
        put_uint(buf, m); i = 0; while (buf[i]) window_put_char(win_id, buf[i++]);
        window_put_char(win_id, 'm');
        if (s < 10) window_put_char(win_id, '0');
        put_uint(buf, s); i = 0; while (buf[i]) window_put_char(win_id, buf[i++]);
        window_puts(win_id, "s\n");
    }
    else if (str_eq(cmd_buf, "about")) {
        window_set_text_color(win_id, 11, 0); // Cyan
        window_puts(win_id, "        _                        _ \n");
        window_puts(win_id, "       | |                      | |\n");
        window_puts(win_id, "   ___ | | _____ _ __ _ __   ___| |\n");
        window_puts(win_id, "  / _ \\| |/ / _ \\ '__| '_ \\ / _ \\ |\n");
        window_puts(win_id, " | (_) |   <  __/ |  | | | |  __/ |\n");
        window_puts(win_id, "  \\___/|_|\\_\\___|_|  |_| |_|\\___|_|\n\n");
        window_set_text_color(win_id, 15, 0); // White
        window_puts(win_id, "okernel v0.7 - Desktop Edition\n");
        window_puts(win_id, "Built from scratch in C and x86 assembly\n");
    }
    else if (str_eq(cmd_buf, "neofetch") || str_eq(cmd_buf, "sysinfo")) {
        window_puts(win_id, "okernel v0.7\n");
        window_puts(win_id, "Resolution: 1920x1080 (2x scale)\n");
        window_puts(win_id, "Shell: okernel sh\n");
        window_puts(win_id, "Memory: ");
        uint32_t total = pmm_get_total_pages() * 4 / 1024;
        uint32_t used = pmm_get_used_pages() * 4 / 1024;
        char buf[16];
        put_uint(buf, used); window_puts(win_id, buf);
        window_puts(win_id, "/");
        put_uint(buf, total); window_puts(win_id, buf);
        window_puts(win_id, "MB\n");
        open_sysinfo();
    }
    else if (str_eq(cmd_buf, "terminal")) {
        int new_win = create_terminal();
        if (new_win >= 0) {
            // Focus the new terminal
            active_term_idx = term_count - 1;
            window_set_focus(new_win);
            shell_prompt(new_win);
            window_puts(win_id, "Terminal opened.\n");
        } else {
            window_puts(win_id, "Max terminals reached.\n");
        }
    }
    else if (str_eq(cmd_buf, "exit")) {
        int idx = find_term_idx(win_id);
        if (idx == 0) {
            window_puts(win_id, "Cannot close main terminal.\n");
        } else {
            destroy_terminal(idx);
            // Focus the main terminal
            active_term_idx = 0;
            window_set_focus(term_wins[0]);
        }
    }
    else if (str_eq(cmd_buf, "reboot")) {
        window_puts(win_id, "Rebooting...\n");
        while (inb(0x64) & 0x02);
        outb(0x64, 0xFE);
        while (1) { __asm__ volatile("hlt"); }
    }
    else if (str_eq(cmd_buf, "ps")) {
        window_puts(win_id, "PID  STATE\n");
        // use process_get_by_slot
        for (int i = 0; i < 16; i++) {
            struct process *p = process_get_by_slot(i);
            if (p && p->state != 0) {
                char buf[32];
                int n = 0;
                uint32_t tmp = p->pid;
                if (tmp == 0) buf[n++] = '0';
                else {
                    char rev[8]; int ri = 0;
                    while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
                    while (ri > 0) buf[n++] = rev[--ri];
                }
                while (n < 4) buf[n++] = ' ';
                buf[n] = 0;
                window_puts(win_id, buf);
                const char *st = (p->state==2) ? "RUNNING" :
                                 (p->state==1) ? "READY" :
                                 (p->state==3) ? "BLOCKED" :
                                 (p->state==5) ? "ZOMBIE" : "EXITED";
                window_puts(win_id, st);
                window_put_char(win_id, '\n');
            }
        }
    }
    else if (str_eq(cmd_buf, "usermode")) {
        // Retired 2026-09-10: the legacy ring-3 smoke test (raw
        // user_test image via sched_spawn_user) is superseded by
        // `run /bin/hello` (ELF spawn through the entry drain,
        // covered by tests/headless/test_sh_hello.py).
        window_puts(win_id, "usermode retired; use run /bin/hello\n");
    }
    else if (str_eq(cmd_buf, "run")) {
        // run <path> [args...] [&]: spawn an ELF program from the VFS and
        // enter it through the main-loop drain (same path as usermode, but
        // ELF-aware via sched_spawn_elf with argc/argv on the user stack).
        // Foreground (default): the main loop waits and prints status.
        // Background (`&` last): enqueue and return to the prompt at once.
        // Runs in keyboard-IRQ context — never iret here, only stage.
        shell_execute_run(win_id, args);
    }
    else if (str_eq(cmd_buf, "shutdown")) {
        window_puts(win_id, "Shutting down...\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        while (1) { __asm__ volatile("hlt"); }
    }
    else {
        // Filesystem fallback (userland cutover): `run` resolution for
        // anything not a builtin — exact VFS name, then /bin/<cmd>. Pipes
        // and redirection are NOT interpreted here (print a hint instead of
        // silently misbehaving). Keeps `usermode` working until Phase 5
        // retires it in favor of `run /bin/hello`.
        const char *arrow = 0;
        for (const char *p = input; *p; p++) {
            if (*p == '|' || *p == '>') { arrow = p; break; }
        }
        if (arrow) {
            window_puts(win_id, "pipes/redirection are not supported yet\n");
        } else if (fs_exists(cmd_buf) || fs_exists(binpath_of(cmd_buf))) {
            shell_execute_run(win_id, input);
        } else {
            window_puts(win_id, "Unknown: ");
            window_puts(win_id, cmd_buf);
            window_put_char(win_id, '\n');
        }
    }
}

static void on_keypress(char c) {
    int win_id = window_get_focused();
    if (win_id < 0) return;

    // Check if this is an editor window
    int ed_id = editor_find_by_win(win_id);
    if (ed_id >= 0) {
        editor_handle_key(ed_id, c);
        editor_draw(ed_id);
        return;
    }

    // Check if this is a okai window
    int br_id = okai_find_by_win(win_id);
    if (br_id >= 0) {
        okai_handle_key(br_id, c);
        return;
    }

    int tidx = find_term_idx(win_id);
    if (tidx < 0) return; // Not a terminal window

    // Control codes (Ctrl+letter from the keyboard driver) are not terminal
    // input — ignore them here (the editor handles its own Ctrl+S/Ctrl+X;
    // the terminal has no control semantics yet).
    if ((unsigned char)c < 32 && c != '\b' && c != '\n' && c != '\t') return;

    if (c == '\b') {
        if (term_lens[tidx] > 0) {
            term_lens[tidx]--;
            window_put_char(win_id, '\b');
        }
    } else if (c == '\n') {
        window_put_char(win_id, '\n');
        term_bufs[tidx][term_lens[tidx]] = 0;
        // Offer the completed line to any ring-3 reader on fd 0 (keyboard
        // line queue — sys_read(0) drains; non-blocking, drops when full).
        sys_proc_kbd_offer(term_bufs[tidx], term_lens[tidx]);
        // OFFER-WAKE, DEFERRED (2026-09-09: waking the reader INLINE (IRET
        // from inside the keyboard IRQ) faulted — #PF err=4 at the resume EIP
        // with pid=0 live: the IRQ trap frame (keyboard IRQ entered via
        // irq_common_stub on the CURRENT trap stack) is buried under our IRET
        // — enter's caller-frame save + iret discards it, and the IRQ never
        // returns (EOI already sent? no — we never reach irq_handler's EOI...
        // actually EOI ran (irq_handler prologue); the fault is CR3/ESP0: we
        // prepared the reader's space but IRETed from IRQ context whose stack
        // is the interrupted thread's — trampoline resumes the IRQ stub frame
        // as if it were the drain caller → garbage). Correct shape: ARM a
        // flag; the MAIN LOOP (plain thread context, next iteration) performs
        // the wake-enter. One flag word (kbd_wake_armed): set here, consumed
        // + cleared by the loop before its poll/draw tail.
        kbd_wake_armed = 1;
        // FOREGROUND GATE (2026-09-08: sh read the STALE `run /bin/sh` line
        // because the kernel shell consumes every line even while a userland
        // foreground process owns the terminal — fresh keystrokes then race
        // the kernel shell vs the ring-3 reader, and the kernel usually wins
        // (offers+executes as a kernel command, e.g. `Unknown:`). While a
        // foreground `run` child (or init's shell — any non-zombie userland
        // process with term_win == this window) is live, the line belongs
        // to IT: offer only, never kernel-execute.
        {
            extern struct process *process_get_by_slot(int slot);
            int fg_live = 0;
            if (run_wait_pid >= 0) {
                extern struct process *process_get(uint32_t pid);
                struct process *w = process_get((uint32_t)run_wait_pid);
                if (w && w->term_win == win_id) fg_live = 1;
            }
            if (!fg_live) {
                for (int s = 0; s < 16; s++) {
                    struct process *p = process_get_by_slot(s);
                    if (!p) continue;
                    if (p->pid == 0) continue;
                    if (p->term_win != win_id) continue;
                    fg_live = 1;
                    break;
                }
            }
            if (!fg_live) shell_execute(win_id, term_bufs[tidx]);
        }
        term_lens[tidx] = 0;
        shell_prompt(win_id);
    } else {
        if (term_lens[tidx] < 255) {
            term_bufs[tidx][term_lens[tidx]++] = c;
            window_put_char(win_id, c);
        }
    }
}

static void on_timer(void) {
    tick_count++;
    // Scheduler tick FIRST (cheap, runs at IRQ): count down the running
    // user process's slice and round-robin on expiry. No-op when only the
    // kernel idle process exists — zero behavior change for existing flows.
    sched_tick();
    // Continuous entropy upkeep (review 2026-09-10 #4): every tick folds an
    // RDTSC sample into the fast pool (TIMER class); every 16th stir the
    // pool reseeds the key (see rand.c). Per-tick cost is a 32B memcpy —
    // the reseed's two SHA-256 passes amortize to ~2us/160ms. The MAC is
    // deliberately NOT mixed here (public, static — not entropy).
    {
        uint8_t sample[32];
        uint64_t t = 0;
        __asm__ volatile("rdtsc" : "=A"(t));
        for (int i = 0; i < 32; i++)
            sample[i] = (uint8_t)((t >> ((i & 7) * 8)) & 0xFF);
        rand_stir(sample); // TIMER class (wrapper)
    }
}

// Network event callback — prints to first terminal window
static void on_net_event(const char* msg) {
    if (term_wins[0] >= 0) {
        window_puts(term_wins[0], msg);
        shell_prompt(term_wins[0]);
    } else {
        serial_puts("[on_net_event] term_wins[0] invalid!\n");
    }
}

// Save HTTP response to the pending okse file
static char okse_save_file[140] = {0};
static int okse_saved_notified = 0;

void net_set_okse_save(const char* filename) {
    int i = 0;
    while (filename[i] && i < 139) {
        okse_save_file[i] = filename[i];
        i++;
    }
    okse_save_file[i] = 0;
    okse_saved_notified = 0;
}

static void check_save_http_response(void) {
    if (okse_save_file[0] == 0) return;

    int resp_len = http_get_response_len();

    // Keep overwriting file with latest accumulated data every frame
    if (resp_len > 0) {
        char* resp = http_get_response();
        if (resp) {
            fs_write(okse_save_file, (uint8_t*)resp, resp_len);
        }
    }

    // Once connection is closed and we have data, notify and stop
    if (!okse_saved_notified && !http_is_pending() && resp_len > 0) {
        okse_saved_notified = 1;
        if (term_wins[0] >= 0) {
            window_puts(term_wins[0], "Saved ");
            char buf[8]; int bi = 0;
            char rev[8]; int ri = 0;
            int tmp = resp_len;
            while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
            if (ri == 0) buf[bi++] = '0';
            while (ri > 0) buf[bi++] = rev[--ri];
            buf[bi] = 0;
            window_puts(term_wins[0], buf);
            window_puts(term_wins[0], " bytes to ");
            window_puts(term_wins[0], okse_save_file);
            window_puts(term_wins[0], "\n");
            shell_prompt(term_wins[0]);
        }
        okse_save_file[0] = 0;
    }
}

// Desktop-only syscall helpers (need paging.o + window.o + process.o):
// fd-1 terminal output for SYS_WRITE, page-granular user mapping for
// SYS_MMAP_USER. Installed via syscall_install at boot (see kernel_main).
// fd routing: SYS_WRITE fd 1/2 resolve through the CALLING process's fd
// table (PROC_FD_TERM → owning window, real ofd → sys_proc_write_fd), so
// later redirection/close/dup2 change where output goes instead of always
// hitting the focused window.
static int sys_write_to_terminal(const char *buf, uint32_t len) {
    // Legacy hook: terminal output with no fd context (sys_print mirror).
    // Routes to the focused window (best effort when no process owns it).
    if (!buf) return -1;
    int fw = window_get_focused();
    if (fw < 0) return -1;
    for (uint32_t i = 0; i < len; i++) window_put_char(fw, buf[i]);
    return 0;
}
// Owning-window write: used by the fd-aware SYS_WRITE path (fd 1/2 bound to
// the spawning window). Falls back to focused when the owner is gone.
static int sys_write_to_window(int win_id, const char *buf, uint32_t len) {
    if (!buf) return -1;
    if (win_id < 0 || !window_get(win_id)) win_id = window_get_focused();
    if (win_id < 0) return -1;
    for (uint32_t i = 0; i < len; i++) window_put_char(win_id, buf[i]);
    return (int)len;
}
void desktop_mmap_user(uint32_t virt, uint32_t phys) {
    paging_map_user(virt, phys);
}
static int desktop_current_pid(void) {
    struct process *cur = process_current();
    return cur ? (int)cur->pid : 0;
}
static void desktop_sched_yield(void) {
    sched_yield();
}

void kernel_main(uint32_t mboot_phys) {
    // mboot_phys is PHYS (GRUB via trampoline). GRUB structs live low, so
    // read through the boot PD's 0-4M LOW identity window (valid pre- and
    // post-paging_init: the full map keeps the low half). Use plain phys
    // derefs here (mboot_phys + off); P2V forms are for high-kernel math.
    serial_init();
    gdt_init();
    idt_init();
    memory_init(mboot_phys);

    // mboot_info.framebuffer_addr is the uint64 at offset 88 (NOT 44: count
    // the packed struct above — 4+4+4*9+2*4 = 88). A wrong offset reads
    // vbe_mode_info/garbage as fb (0x90 class) and paging maps nothing.
    uint32_t mboot_flags = *(volatile uint32_t*)(mboot_phys + 0);
    uint64_t mboot_fb = *(volatile uint64_t*)(mboot_phys + 88);
    uint32_t fb_addr = 0;
    if (mboot_flags & (1 << 12)) {
        fb_addr = (uint32_t)mboot_fb;
    }
    serial_printf("[boot] mboot phys=%x flags=%x fb=%x\n",
                  mboot_phys, mboot_flags, fb_addr);
    // paging_init BEFORE process_init: the boot PD maps only 0-4M, but .bss
    // statics (page_directory at phys ~0x2C00000, processes, bitmap tail)
    // live at phys 1M-14M. process_init's first .bss touch faults without
    // the full map (CR2=0xC0xxxxxx class, e=0002). FB addr 0 = still map.
    paging_init(fb_addr);
    process_init();

    // Syscall ABI hooks (syscall.c is COMMON so text links too; the
    // implementations need paging.o + window.o + process.o = desktop-only).
    // Installed once here: validation + terminal/keyboard + fd/process/VM.
    syscall_install(paging_user_range_valid, sys_write_to_terminal);
    syscall_install_winwrite(sys_write_to_window);
    syscall_install_proc(desktop_current_pid, desktop_sched_yield);
    syscall_install_mmap(desktop_mmap_user);
    syscall_install_full(sys_proc_munmap, sys_proc_kbd_read,
                          sys_proc_open, sys_proc_close,
                          sys_proc_write_fd, sys_proc_read_fd,
                          sys_proc_fork, sys_proc_exec,
                          sys_proc_sbrk, sys_proc_pipe,
                          sys_proc_dup, sys_proc_wait,
                           sys_proc_kill, sys_proc_mmap_anon);
    // NOTE (retired 2026-09-10): the legacy user_test image staging
    // (syscall_install_usertest*) lived here for sched_spawn_user; both are
    // gone with the `usermode` command. Ring-3 entry now serves ELF spawns
    // (run/init/fork) through the entry queue below.
    graphics_init(mboot_phys);
    window_init();
    fs_init();
    pfs_init(); // ATA disk + mount-or-format (diskless = VFS-only, safe)
    userland_seed(); // /bin/* + /sbin/init from embedded ELFs (skips present)
    // TOFU pin persistence (P2): reload last boot's verified leaf pins so
    // a reboot doesn't re-open the first-visit window to a network-only
    // attacker. Malformed files are ignored (fail-open to empty store =
    // plain TOFU, never a wedge).
    {
        extern int tls_pin_import(const uint8_t* in, uint32_t len);
        if (fs_exists("/.pins")) {
            static uint8_t pinbuf[1024];
            int n = fs_read("/.pins", pinbuf, sizeof(pinbuf));
            if (n > 0 && tls_pin_import(pinbuf, (uint32_t)n) == 0)
                serial_puts("[pins] restored pin store\n");
        }
    }
    // PID 1 is spawned LAZILY (first terminal creation) instead of here:
    // sched_spawn_elf needs the PFS/VFS settled AND the entry drain must be
    // reachable (main loop running). Spawning here (pre-loop) wedges the
    // drain ordering vs init's fork/exec children (bisected 2026-09-08:
    // boot-spawned init forked sh but the exec never dispatched — the boot
    // entry raced the first terminal's drain). kernel_shell_ready() below
    // fires once the first terminal exists; the shell path stays fully
    // working until then (fail-soft: no userland, no wedge).
    editor_init();
    okai_init();

    // Cache wallpaper for fast blitting
    graphics_cache_wallpaper(wp_pixels, wp_palette, WP_W, WP_H);

    // Draw initial wallpaper
    graphics_blit_wallpaper();

    irq_register_handler(0, on_timer);

    // Raise the PIT from the BIOS-default 18.2 Hz to 100 Hz. Vsync-less
    // software rendering needs a fine-grained tick to cap the redraw rate
    // without busy-looping the whole screen hundreds of times per second.
    outb(0x43, 0x36);            // channel 0, mode 3, 16-bit binary
    outb(0x40, 0x9C);            // divisor lo  (1193182 / 100 = 11932 = 0x2E9C)
    outb(0x40, 0x2E);            // divisor hi

    // Create main terminal — large, centered
    term_wins[0] = window_create("Terminal", 40, 30, 900, 650);
    window_set_close_button(term_wins[0], 1);
    window_set_minimize_button(term_wins[0], 1);
    term_count = 1;
    active_term_idx = 0;
    window_set_focus(term_wins[0]);

    window_set_text_color(term_wins[0], 11, 0);
    window_puts(term_wins[0], "        _                        _ \n");
    window_puts(term_wins[0], "       | |                      | |\n");
    window_puts(term_wins[0], "   ___ | | _____ _ __ _ __   ___| |\n");
    window_puts(term_wins[0], "  / _ \\| |/ / _ \\ '__| '_ \\ / _ \\ |\n");
    window_puts(term_wins[0], " | (_) |   <  __/ |  | | | |  __/ |\n");
    window_puts(term_wins[0], "  \\___/|_|\\_\\___|_|  |_| |_|\\___|_|\n\n");
    window_set_text_color(term_wins[0], 15, 0);
    window_puts(term_wins[0], "Welcome to okernel v0.7\n");
    window_puts(term_wins[0], "A minimalistic operating system.\n\n");
    window_set_text_color(term_wins[0], 8, 0);
    window_puts(term_wins[0], "Type 'help' for commands.\n");
    window_puts(term_wins[0], "Type 'terminal' for new window.\n");
    window_puts(term_wins[0], "Type 'exit' to close this terminal.\n\n");
    window_set_text_color(term_wins[0], 15, 0);
    shell_prompt(term_wins[0]);

    // PID 1 is spawned on FIRST `run` (lazy): the boot-time spawn raced
    // init's fork/exec children against the entry drain (bisected 2026-09-08:
    // boot-spawned init forked sh but exec never dispatched AND the run path
    // wedged while init's wait-storm owned ring 3). Deferring to first use
    // keeps boot deterministic; the shell path stays fully working until
    // then (fail-soft: no userland, no wedge). See shell_execute_run().
    // (No code here — the arm lives in shell_execute_run, first call.)

    // Init networking (full stack: PCI + RTL8139 + ARP + IP + ICMP)
    net_init();
    net_set_event_callback(on_net_event);

    // Seed the ChaCha20 CPRNG used by the TLS client for key material.
    // Honest model (review 2026-09-10 #4, tiers cryptoholes #1): the MAC
    // is PUBLIC — it enters only as domain separation (rand_personalize,
    // never counted). Real classes here: BOOT (RDTSC/RTC). RDRAND/RDSEED
    // opportunistically (rand_hw_init establishes the hardware tier when
    // the CPU has it). Readiness comes later: with hardware, a reseed
    // compressing >= 64B from a window with 2 classes; without, >= 128B
    // with BOOT+TIMER+INPUT lifetime classes (keystrokes typed for the
    // fetch URL and NIC RX both stir INPUT, so the first user-initiated
    // fetch always follows qualifying input). rand_bytes() fails closed
    // until then — no TLS without established entropy.
    {
        uint8_t* mac = e1000_get_mac();
        rand_personalize(mac, 6);
        uint8_t seed[32];
        uint64_t tsc = 0;
        __asm__ volatile("rdtsc" : "=A"(tsc));
        for (int i = 0; i < 32; i++)
            seed[i] = (uint8_t)(((tsc >> ((i & 7) * 8)) & 0xFF) ^ (i * 0x9E));
        rand_seed(seed);
        for (int s = 0; s < 16; s++) {
            uint64_t t2 = 0;
            __asm__ volatile("rdtsc" : "=A"(t2));
            uint8_t sample[32];
            for (int i = 0; i < 32; i++)
                sample[i] = (uint8_t)(((t2 >> ((i & 7) * 8)) & 0xFF) ^ (s * 37 + i));
            rand_stir_src(sample, RAND_SRC_BOOT);
        }
        serial_printf("[rand] CPRNG seeded (hw=%d), ready=%d (TIMER class pending)\n",
                      rand_hw_init(), rand_ready());
    }

    // Eager ECDSA curve contexts (Montgomery params for P-256/P-384) so no
    // first-verify lazy build can race preemption (review 2026-09-10 #11).
    ec_init();

    // Wall clock for certificate validity checking (CMOS RTC, port I/O).
    {
        x509_time now;
        if (rtc_read(&now) == 0) {
            x509_set_now(&now);
            serial_printf("[rtc] wall clock: %04d-%02d-%02d %02d:%02d:%02d\n",
                          now.year, now.month, now.day,
                          now.hour, now.minute, now.second);
        } else {
            // No clock: cert_verify now FAILS CLOSED (review 2026-09-10 #2)
            // — HTTPS refuses every chain rather than trusting build-date
            // validity forever. HTTP still works; fix the CMOS battery.
            serial_puts("[rtc] CMOS read failed, no clock: HTTPS disabled (fail-closed)\n");
        }
    }

    // Init input
    mouse_init_fb();
    keyboard_init();
    keyboard_set_callback(on_keypress);
    // Capture the main-loop thread's live ESP/EBP into pid 0 BEFORE sti:
    // the preemption stub saves/restores p->esp per thread, and process_init
    // ran one frame up (its ESP capture is stale by a frame). This is the
    // exact thread the timer will preempt — seed it precisely. Also arm
    // pid 0's slice (first tick decrements instead of expiring at boot).
    {
        extern void process_capture_idle_esp(void);
        process_capture_idle_esp();
    }
    process_idle_arm();
    sti();

    // Main loop
    while (1) {
        // Ring-3 entry drain: the shell (and fork) stage pid+eip+esp+window
        // into the entry run queue; here is plain ring-0 thread context, so
        // prepare (CR3+ESP0) + IRET below is safe. sys_exit resumes right
        // after the call (trampoline restores the caller frame:
        // EBP/ESI/EDI/ESP + segments): drain signals, unprepare, reap (or
        // leave zombies for wait()), announce, re-enable, loop on.
        // (The legacy single-slot usermode path was retired 2026-09-10;
        // only the queue remains.)
        for (;;) {
            uint32_t eip = 0, esp = 0;
            int pid = -1, win_id = -1, is_fork_child = 0;
            if (!entry_dequeue(&pid, &eip, &esp, &win_id)) {
                // queued entry (fork child or run-spawn)
            } else {
                break; // queue empty
            }
            is_fork_child = (pid >= 0) ? sched_fork_take_child((uint32_t)pid) : 0;
            // EXITED-SKIP (2026-09-09: post-hello #PF err=0 cr2=0 eip=0 — a
            // wait-parked parent was re-entered, reaped its zombie, and
            // EXITED... then a STALE queued entry for the same pid (staged
            // before the exit, e.g. fork-child entry + park-resume entry
            // both queued) dequeued into prepare+IRET with user_eip/esp from
            // a FREED address space (destroy poisoned esp/esp0/page_dir to 0
            // → CR3=kernel-PD-fallback... actually page_dir=0 → kernel PD,
            // eip/esp=stale user values → #PF at CR2==EIP==0 class). Never
            // enter a pid whose slot is UNUSED or whose state is
            // ZOMBIE/EXITED (reaped or awaiting reap — wait() owns it now).
            if (pid >= 0) {
                struct process *chk = process_get((uint32_t)pid);
                if (!chk || chk->state == PROC_ZOMBIE || chk->state == PROC_EXITED) {
                    serial_printf("[drain] skip dead pid=%d\n", pid);
                    continue;
                }
            }
            // WAKE ORDER: a BLOCKED parker with a staged resume whose wake
            // condition holds is re-entered through THIS drain (not its own
            // iret — its trap frame was discarded by the park trampoline).
            // Conditions: wait-park → a matching child is ZOMBIE now;
            // yield/read-park → any queued entry or READY sibling exists, or
            // (read) the kbd queue is non-empty. Otherwise skip (stay
            // BLOCKED until the tick WAKE pass or SIG_CHLD wakes us).
            // Pre-entry fork children (never entered) always run: their
            // queued entry IS their first frame.
            uint32_t park_ret = 0;
            int is_park_resume = 0;
            if (pid >= 0 && !is_fork_child) {
                struct process *dp = process_get((uint32_t)pid);
                if (dp && dp->state == PROC_BLOCKED && dp->entered_ring3) {
                    extern int sched_park_resume(uint32_t, uint32_t*, uint32_t*);
                    extern int sched_park_take(uint32_t, uint32_t*);
                    uint32_t peip = 0, pesp = 0;
                    if (sched_park_resume((uint32_t)pid, &peip, &pesp)) {
                        // Staged resume exists: check its wake condition.
                        // Wait-park (ret == -2): wake iff a matching child
                        // is a zombie NOW (reaped by the drain below after
                        // re-entry... actually reaped HERE would be simpler,
                        // but the retry path expects -2-then-reap; instead
                        // wake only on zombie and let the retry reap).
                        uint32_t pret = 0;
                        sched_park_take((uint32_t)pid, &pret);
                        // Peek only (take consumed the flag — re-stage).
                        extern void sched_park_stage(uint32_t, uint32_t, uint32_t, uint32_t);
                        sched_park_stage((uint32_t)pid, peip, pesp, pret);
                        int wake = 0;
                        if (pret == (uint32_t)-2) {
                            // wait-park: wake iff matching zombie exists
                            struct process *me = process_get((uint32_t)pid);
                            if (me) {
                                for (int s = 0; s < MAX_PROCESSES; s++) {
                                    struct process *c = process_get_by_slot(s);
                                    if (!c) continue;
                                    if (c->parent_pid != me->pid) continue;
                                    if (c->state == PROC_ZOMBIE || c->state == PROC_EXITED) { wake = 1; break; }
                                }
                            }
                        } else {
                            // yield/read-park: wake iff work exists.
                            // READ-PARK EXTRA RULE (2026-09-09: sh typed
                            // /bin/hello but never ran it — the offer sat in
                            // the queue while sh slept BLOCKED: yield-wake
                            // needs a queued entry or READY sibling, but a
                            // kbd offer is neither. A parked READER with a
                            // non-empty kbd queue is ALWAYS woken (the line
                            // it waits for is already there).
                            extern int sys_proc_kbd_pending(void);
                            wake = entry_pending_any() ? 1 : 0;
                            if (!wake && sys_proc_kbd_pending()) wake = 1;
                            if (!wake) {
                                for (int s = 0; s < MAX_PROCESSES; s++) {
                                    struct process *c = process_get_by_slot(s);
                                    if (!c || c->pid == 0) continue;
                                    if (c->state == PROC_READY && c->entered_ring3) { wake = 1; break; }
                                }
                            }
                        }
                        if (wake) {
                            // Consume the staged resume for real this time.
                            sched_park_take((uint32_t)pid, &park_ret);
                            // WAKE-TRACE (bisect 2026-09-09: post-hello #PF
                            // eip=esp=0 — find who enters zeros).
                            serial_printf("[wake] pid=%d eip=%x esp=%x ret=%d\n",
                                          pid, peip, pesp, (int)park_ret);
                            eip = peip; esp = pesp;
                            is_park_resume = 1;
                            dp->state = PROC_READY;
                            dp->ticks_left = SCHED_SLICE_TICKS;
                        } else {
                            continue; // stay parked
                        }
                    } else {
                        continue; // BLOCKED with no resume (legacy) — skip
                    }
                }
            }
            // Fork children resume via the entry drain (no live frame yet):
            // DEFERRAL GUARD (bisected 2026-09-08: exec garbage + child #PF —
            // a tick between drain-dequeue and IRET captures the DRAIN's
            // half-built register frame as the child's "thread", then the
            // drain IRETs anyway: two entries, one pid, garbage resume).
            // entered=0/unseed did NOT fix it (tick still raced the window).
            // Correct fix: CLOSE the window — hold the switch busy-guard
            // across the whole dequeue→IRET sequence so the tick CANNOT
            // switch mid-drain (it returns immediately, retries next tick —
            // by then the child has entered and owns a real frame). The IRET
            // itself runs with IF clear on this path (IRQ gates), and the
            // drain re-enables after resume; the guard is released on every
            // exit path below (park-continue, post-exit) — grep switch_busy.
            {
                extern volatile int switch_busy;
                switch_busy = 1;
                __asm__ volatile("cli" ::: "memory");
            }
            if (is_fork_child) {
                struct process *fc = process_get((uint32_t)pid);
                if (fc) fc->entered_ring3 = 1;
            }
            serial_puts("[usermode] entering ring 3 from main loop\n");
            // Stash the drain pid BEFORE the IRET (locals go stale across
            // enter/exit — re-captured after resume for the reap check).
            if (pid >= 0) drain_stash_pid(pid);
            // Activate the process's private address space + trap stack BEFORE
            // the IRET: ring-3 fetches and INT 0x80 traps must land in ITS
            // space, not the kernel PD. (Previously mapped into the shared
            // kernel PD — leaked user bits there permanently.)
            if (pid >= 0) sched_prepare((uint32_t)pid);
            // Mark entered BEFORE the IRET (preemption then treats this as
            // a live thread with a real saved frame — which it is after the
            // first tick saves it; the seed frame covers the gap before).
            if (pid >= 0) {
                struct process *ent = process_get((uint32_t)pid);
                if (ent) ent->entered_ring3 = 1;
            }
            // Fork child: EAX forced 0 by the enter path (see isr.asm), so
            // ring-3 observes fork() == 0. Park resume: staged EAX likewise.
            // Plain calls BEFORE the enter asm (flags live in .bss — safe
            // against the asm's EAX clobber). Never both (a pid is either a
            // fresh child or a resumed parker, not both — fork wins).
            // FORK-TRACE (input-bug bisect 2026-09-08): prove the flag path.
            if (is_fork_child) {
                serial_printf("[fork-enter] pid=%d eip=%x esp=%x EAX=0\n",
                              pid, eip, esp);
            }
            if (is_fork_child) enter_user_mode_fork_child();
            else if (is_park_resume) enter_user_mode_park_ret(park_ret);
            // ENTER-TRACE (bisect 2026-09-09: post-hello #PF eip=esp=0 —
            // prove every IRET's target; the crash enters pid 4 with zeros
            // and NO [wake]/[fork-enter]/[kbd-wake] line precedes it).
            serial_printf("[enter] pid=%d eip=%x esp=%x fork=%d park=%d\n",
                          pid, eip, esp, is_fork_child, is_park_resume);
            // Register calling convention: eip->EAX, esp->EDX (enter takes NO
            // stack args, so ESP points AT the return address and the save is
            // exact). Clobbers: eax,ebx,ecx,edx + memory. EBX is consumed as
            // the user_eip carrier and NOT restored — list it clobbered.
            __asm__ volatile(
                "push %%ebx; push %%esi;"
                "mov %0, %%eax; mov %1, %%edx;"
                "call enter_user_mode"
                : : "r"(eip), "r"(esp) : "eax", "ebx", "ecx", "edx", "memory");
            // RESUMED VIA TRAMPOLINE (not a normal return): sys_exit jumped
            // here with the caller frame restored (EBP/ESI/EDI/ESP/segments).
            // EBX is stale (entry clobbered it) — never trust it below.
            // NOTE: NO sti here. The trampoline ran cli, and the preemptive
            // scheduler needs IF managed by the switch paths, not blanket
            // set: an sti here opens a preemption window INSIDE the exit
            // sequence (unprepare/reap touch CR3 + free pages — a tick there
            // switches onto freed stacks/PDs → #PF at CR2==EIP in
            // context_switch, bisected 2026-09-08). IRQs stay off until the
            // exit sequence completes; the next loop iteration's hlt (idle)
            // or the next switch_to's sti re-enables.
            // Re-capture the drain pid (locals lived in registers/stack slots
            // across the IRET — the trampoline restored EBP/ESI/EDI/ESP but
            // EBX/EAX/ECX/EDX are STALE (entry clobbered EBX, idt hijack
            // clobbered EAX). The reap probe proved it: pid read 0 post-resume
            // (should be 1) → reap skipped → zombie never freed → next tick
            // switched to the dead slot (bisected 2026-09-08: [reapchk] pid=0
            // post-Back, then [sw] 0->1 into the zombie).
            // Parked return: the drain IRETed into ring 3, but the process
            // PARKED (wait-no-zombie / yield / read-empty) instead of
            // exiting — the park trampoline resumed the main loop right here
            // (same resume path as sys_exit: caller frame restored, pid
            // re-captured from the stash). Distinguish exit from park via
            // PCB state: ZOMBIE/EXITED = real exit (reap path below); anything
            // else = parked (reap NOTHING, announce NOTHING — the thread is
            // live and will be re-entered by the drain once woken).
            pid = -1;
            {
                extern int drain_last_pid(void);
                pid = drain_last_pid();
            }
            // Parked? (process alive, not a zombie) → stage its resume
            // (BLOCKED + trapped EIP/ESP/retval already saved by the arm),
            // re-arm the exit latch, sti, and continue the drain loop so
            // queued siblings run THIS iteration. The parker is re-entered
            // later via enter_user_mode(park_eip/esp) with the staged EAX
            // (park_ret_pending flag, fork-child-style).
            {
                struct process *pp = (pid >= 0) ? process_get((uint32_t)pid) : 0;
                if (pp && pp->state != PROC_ZOMBIE && pp->state != PROC_EXITED) {
                    // Drain-side pid getters (NOT self-slot: the drain runs
                    // as pid 0; self-slot reads slot 0's empty park state —
                    // bisected 2026-09-09: parked readers staged EIP=ESP=0
                    // and slept forever).
                    extern uint32_t syscall_park_eip_for(uint32_t pid);
                    extern uint32_t syscall_park_esp_for(uint32_t pid);
                    extern uint32_t syscall_take_park_ret_for(uint32_t pid);
                    extern void sched_park_stage(uint32_t pid, uint32_t eip, uint32_t esp, uint32_t ret);
                    uint32_t peip = syscall_park_eip_for((uint32_t)pid);
                    uint32_t pesp = syscall_park_esp_for((uint32_t)pid);
                    uint32_t pret = syscall_take_park_ret_for((uint32_t)pid);
                    // PARK-TRACE (bisect 2026-09-09): prove the drain stages
                    // the parker's real resume (not slot-0 zeros).
                    serial_printf("[park] pid=%d eip=%x esp=%x ret=%d\n",
                                  pid, peip, pesp, (int)pret);
                    pp->state = PROC_BLOCKED;
                    pp->ticks_left = SCHED_SLICE_TICKS;
                    sched_park_stage((uint32_t)pid, peip, pesp, pret);
                    syscall_can_exit = 1;
                    // PARK-HOME INVARIANT (2026-09-08: e1000_poll #PF err=0
                    // cr2=0x8001c esp=garbage after park cycles — the park
                    // trampoline resumes the drain with the PARKED process's
                    // PD still live + ITS ESP0 armed. Any IRQ before the next
                    // prepare (timer → sched_tick → e1000_poll → DMA decode)
                    // runs on the wrong address space/stack. Restore kernel
                    // PD + idle ESP0 here (same as the exit path below); the
                    // parker's resume re-prepares at re-entry.)
                    sched_unprepare();
                    {
                        extern volatile int switch_busy;
                        switch_busy = 0;
                    }
                    __asm__ volatile("sti" ::: "memory");
                    continue;
                }
            }
            syscall_can_exit = 0;
            // Safe-point signal drain (default actions: TERM→zombie+notify).
            sys_proc_drain_signals();
            // Back on the kernel PD + idle trap stack: the process's private
            // space was live during ring 3. Restore both BEFORE touching any
            // kernel state (window buffers, serial-adjacent heap) — the user
            // PD shares the high map, but its low half is private and its TLB
            // entries are stale for kernel work.
            sched_unprepare();
            // Reap policy: the legacy usermode path (parent_pid == 0, no
            // waiter) reaps immediately; `run` foreground leaves the zombie
            // for the waiter below (which prints status); fork children stay
            // for ring-3 wait(). Exited-without-zombie (legacy stub) reaps.
            int run_done_code = -1;
            {
                struct process *done = (pid >= 0) ? process_get((uint32_t)pid) : 0;
                if (done && done->state == PROC_ZOMBIE) {
                    if (pid == run_wait_pid) {
                        // Foreground run child: capture status, reap now,
                        // disarm waiter (announced below with the code).
                        run_done_code = done->exit_code;
                        sched_reap((uint32_t)pid);
                        // keep run_wait_win for the announcement below
                        run_wait_pid = -1;
                    } else if (done->parent_pid == 0) {
                        if (pid >= 0) sched_reap((uint32_t)pid);
                    }
                } else if (pid >= 0) {
                    // Exited without zombie (legacy stub path) — reap now.
                    struct process *d2 = process_get((uint32_t)pid);
                    if (d2) sched_reap((uint32_t)pid);
                }
            }
            {
                int fw = window_get_focused();
                if (fw >= 0) {
                    window_puts(fw, "Back from user mode.\n");
                    shell_prompt(fw);
                }
            }
            serial_puts("Back from user mode.\n");
            // Foreground-run status line (into the OWNING window, not
            // focused — the user may have clicked elsewhere mid-run).
            if (run_done_code >= 0) {
                int ow = run_wait_win;
                if (ow < 0 || !window_get(ow)) ow = window_get_focused();
                if (ow >= 0) {
                    window_puts(ow, "[run] exit code ");
                    {
                        char cb[12]; int cbi = 0, tv = run_done_code;
                        char rv[12]; int ri = 0;
                        if (tv == 0) cb[cbi++] = '0';
                        while (tv > 0 && cbi < 11) { rv[ri++] = '0' + (tv % 10); tv /= 10; }
                        while (ri > 0) cb[cbi++] = rv[--ri];
                        cb[cbi] = 0;
                        window_puts(ow, cb);
                    }
                    window_put_char(ow, '\n');
                    shell_prompt(ow);
                }
                serial_printf("[run] exit code %d\n", run_done_code);
                run_wait_win = -1;
            }
            // Exit sequence done (CR3 home, slot reaped-or-zombied): IRQs
            // back on for the poll/draw tail of this iteration. Also release
            // the drain deferral guard (held since dequeue — see above) so
            // the tick can switch again (the entered child now owns a real
            // frame; races are over). Release ONLY when the drain queue is
            // empty: with entries still staged (fork child behind a finished
            // parent, init behind run) the next for-iteration dequeues
            // straight into another IRET — keep the guard across it (it
            // re-arms at the top of the loop anyway; releasing here would
            // open the race window between iterations).
            {
                extern volatile int switch_busy;
                extern int entry_pending_any(void);
                if (!entry_pending_any()) switch_busy = 0;
            }
            __asm__ volatile("sti" ::: "memory");
        }
        // OFFER-WAKE CONSUME (see on_keypress: IRQ context must never IRET).
        // Plain thread context here: wake-enter the oldest BLOCKED entered
        // reader with a staged resume so it consumes the just-offered line.
        // Runs BEFORE the poll/draw tail (the reader may exit/park again —
        // its tail is the drain's abbreviated resume path, same as the IRQ
        // attempt but on a valid caller frame).
        if (kbd_wake_armed) {
            kbd_wake_armed = 0;
            extern int sched_park_resume(uint32_t, uint32_t*, uint32_t*);
            extern int sched_park_take(uint32_t, uint32_t*);
            extern void enter_user_mode_park_ret(uint32_t v);
            for (int s = 0; s < 16; s++) {
                struct process *rp = process_get_by_slot(s);
                if (!rp || rp->pid == 0) continue;
                if (rp->state != PROC_BLOCKED) continue;
                if (!rp->entered_ring3) continue;
                uint32_t reip = 0, resp = 0, rret = 0;
                if (!sched_park_resume((uint32_t)rp->pid, &reip, &resp)) continue;
                sched_park_take((uint32_t)rp->pid, &rret);
                serial_printf("[kbd-wake] pid=%d eip=%x esp=%x\n",
                              rp->pid, reip, resp);
                {
                    extern volatile int switch_busy;
                    switch_busy = 1;
                    __asm__ volatile("cli" ::: "memory");
                }
                drain_stash_pid(rp->pid);
                // READY BEFORE prepare (process_switch refuses BLOCKED —
                // the parked thread has no live frame; its resume is an
                // entry-drain IRET, never a CR3 handoff. Mark READY first so
                // the handoff is legal; the IRET below is the real resume).
                rp->state = PROC_READY;
                rp->ticks_left = SCHED_SLICE_TICKS;
                sched_prepare((uint32_t)rp->pid);
                rp->entered_ring3 = 1;
                rp->ticks_left = SCHED_SLICE_TICKS;
                enter_user_mode_park_ret(rret);
                serial_printf("[enter] pid=%d eip=%x esp=%x fork=0 park=1 (kbd-wake)\n",
                              rp->pid, reip, resp);
                __asm__ volatile(
                    "push %%ebx; push %%esi;"
                    "mov %0, %%eax; mov %1, %%edx;"
                    "call enter_user_mode"
                    : : "r"(reip), "r"(resp)
                    : "eax", "ebx", "ecx", "edx", "memory");
                // Resumed via trampoline (park or exit — same resume shape
                // as the drain: caller frame restored, pid from the stash).
                {
                    extern int drain_last_pid(void);
                    int wpid = drain_last_pid();
                    struct process *wpp = (wpid >= 0) ? process_get((uint32_t)wpid) : 0;
                    if (wpp && wpp->state != PROC_ZOMBIE && wpp->state != PROC_EXITED) {
                        uint32_t wpeip = syscall_park_eip_for((uint32_t)wpid);
                        uint32_t wpesp = syscall_park_esp_for((uint32_t)wpid);
                        uint32_t wpret = syscall_take_park_ret_for((uint32_t)wpid);
                        wpp->state = PROC_BLOCKED;
                        wpp->ticks_left = SCHED_SLICE_TICKS;
                        sched_park_stage((uint32_t)wpid, wpeip, wpesp, wpret);
                    }
                    sched_unprepare();
                    {
                        extern volatile int switch_busy;
                        extern int entry_pending_any(void);
                        if (!entry_pending_any()) switch_busy = 0;
                    }
                    __asm__ volatile("sti" ::: "memory");
                }
                break; // one wake per offer
            }
        }
        // Poll network for incoming packets
        e1000_poll();
        net_poll();
        http_poll();
        https_get_poll(); // advance any in-flight async HTTPS fetch
        // Persist TOFU pins on change (write-through VFS, same as editor).
        {
            extern int tls_pin_dirty(void);
            extern void tls_pin_clean(void);
            extern uint32_t tls_pin_export(uint8_t* out, uint32_t cap);
            if (tls_pin_dirty()) {
                static uint8_t pinbuf[1024];
                uint32_t n = tls_pin_export(pinbuf, sizeof(pinbuf));
                if (n > 0 && fs_write("/.pins", pinbuf, (int)n) > 0) {
                    tls_pin_clean();
                    serial_puts("[pins] pin store saved\n");
                }
            }
        }

        // Legacy completion poll: the trampoline path above already announces
        // inline AND latches user_exited (hijack calls note_exited) — so by
        // the time we get here the latch is ALWAYS set after a trampoline
        // exit. Drain it silently (no second announcement).
        user_mode_poll_finished();

        int mx = mouse_get_x();
        int my = mouse_get_y();
        int mb = mouse_get_left_button();

        // Mouse wheel → scroll the okai window under the cursor, or the
        // terminal's scrollback (same wheel model as the browser).
        int wheel = mouse_get_scroll();
        if (wheel != 0) {
            int wid = window_from_point(mx, my);
            if (wid >= 0) {
                int br_id = okai_find_by_win(wid);
                if (br_id >= 0) okai_handle_mouse_scroll(br_id, wheel);
                else if (find_term_idx(wid) >= 0) window_scroll_view(wid, wheel);
            }
        }

        if (mb && !mouse_down) {
            int clicked = 0;

            // Check taskbar clicks (bottom 20px)
            if (my >= SCREEN_H - TASKBAR_H) {
                // Count visible windows to match dynamic width
                int vis_count = 0;
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (window_get(i) && window_get(i)->visible) vis_count++;
                }
                if (vis_count > 0) {
                    int total_pad = (vis_count + 1) * 6;
                    int btn_w = (SCREEN_W - total_pad) / vis_count;
                    if (btn_w > 180) btn_w = 180;
                    if (btn_w < 60) btn_w = 60;

                    int tx = 6;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        struct window* w = window_get(i);
                        if (!w || !w->visible) continue;
                        if (mx >= tx && mx < tx + btn_w) {
                            window_restore(i);
                            window_set_focus(i);
                            int tidx = find_term_idx(i);
                            if (tidx >= 0) active_term_idx = tidx;
                            clicked = 1;
                            break;
                        }
                        tx += btn_w + 6;
                    }
                }
            }

            // Check desktop icon clicks
            if (!clicked) {
                int icon_idx = check_icon_click(mx, my);
                if (icon_idx >= 0) {
                    const char* name = fs_get_name(icon_idx);
                    if (name) {
                        int ed = editor_open(name);
                        if (ed >= 0) {
                            // Editor opened
                        }
                    }
                    clicked = 1;
                }
            }

            // Check window buttons and title bars
            if (!clicked) {
                for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                    struct window* w = window_get(i);
                    if (!w || !w->visible || w->minimized) continue;

                    if (window_check_close_click(i, mx, my)) {
                        int tidx = find_term_idx(i);
                        if (tidx >= 0) {
                            destroy_terminal(tidx);
                            if (term_count > 0) {
                                active_term_idx = 0;
                                window_set_focus(term_wins[0]);
                            }
                        } else if (i == info_win) {
                            close_sysinfo();
                        } else {
                            int ed_id = editor_find_by_win(i);
                            if (ed_id >= 0) {
                                editor_close(ed_id);
                            } else {
                                int br_id = okai_find_by_win(i);
                                if (br_id >= 0) {
                                    okai_close(br_id);
                                }
                            }
                        }
                        clicked = 1;
                        break;
                    }

                    if (window_check_minimize_click(i, mx, my)) {
                        window_minimize(i);
                        // window_minimize sets needs_redraw — the periodic
                        // full-redraw section rebuilds wallpaper and marks all
                        // remaining windows dirty later this same frame
                        clicked = 1;
                        break;
                    }

                    if (window_check_resize_grip(i, mx, my)) {
                        resize_win = i;
                        resize_off_w = w->w - (mx - w->x);
                        resize_off_h = w->h - (my - w->y);
                        window_restore(i);
                        window_set_focus(i);
                        int tidx = find_term_idx(i);
                        if (tidx >= 0) active_term_idx = tidx;
                        clicked = 1;
                        break;
                    }

                    // Click anywhere inside the window selects/focuses it (the
                    // whole window, not just the title bar) and routes keyboard
                    // input to it. A terminal also becomes the active terminal.
                    if (mx >= w->x && mx < w->x + w->w &&
                        my >= w->y && my < w->y + w->h) {
                        int tidx = find_term_idx(i);
                        if (tidx >= 0) active_term_idx = tidx;
                        window_restore(i);
                        window_set_focus(i);

                        // Tab strip: switch tabs / close via ×, then toolbar
                        // nav buttons — both before the drag/link branches that
                        // would otherwise claim the top chrome band.
                        {
                            int br_id = okai_find_by_win(i);
                            if (br_id >= 0) {
                                struct okai* ok = okai_get(br_id);
                                // Lock icon toggles a security popup; any other
                                // click inside the window dismisses it first.
                                if (okai_lock_hit(br_id, mx, my)) {
                                    ok->show_security = !ok->show_security;
                                    needs_redraw = 1;
                                    clicked = 1;
                                    break;
                                }
                                if (ok->show_security) { ok->show_security = 0; needs_redraw = 1; }
                                int on_close = 0;
                                int ti = okai_tab_hit(br_id, mx, my, &on_close);
                                if (ti >= 0) {
                                    if (on_close) okai_close_tab(br_id, ti);
                                    else okai_switch_tab(br_id, ti);
                                    clicked = 1;
                                    break;
                                }
                                int nav = okai_check_nav_click(br_id, mx, my);
                                if (nav != NAV_NONE) {
                                    serial_printf("[okai] nav action=%d (mx=%d my=%d)\n", nav, mx, my);
                                    if (nav == NAV_BACK) okai_nav_back(br_id);
                                    else if (nav == NAV_FWD) okai_nav_fwd(br_id);
                                    else if (nav == NAV_RELOAD) okai_nav_reload(br_id);
                                    else if (nav == NAV_HOME) okai_nav_home(br_id);
                                    else if (nav == NAV_NEWTAB) okai_new_tab(br_id, OKAI_HOME_URL);
                                    clicked = 1;
                                    break;
                                }
                            }
                        }

                        // Grid top mirrors window_paint_region: a no_titlebar
                        // window's content grid starts right below the border
                        // (the old +WIN_TITLE_H offset put every click ~1.25
                        // rows below the link it was aimed at — links never
                        // hit).
                        int grid_top = w->y + WIN_BORDER +
                                       (w->no_titlebar ? 0 : WIN_TITLE_H);
                        // Top chrome band: drag the window by it — except the
                        // address bar, which click-focuses for typing (like
                        // pressing 'g'). Anything else must NOT clear the URL
                        // display or fall through to content/link hit-testing;
                        // stray chrome clicks used to wipe it or trigger link
                        // navigation. For a no-titlebar browser the chrome is the
                        // whole tab strip + toolbar; for a titled window it is
                        // just the title bar (above grid_top).
                        if (w->no_titlebar
                                ? (my <= grid_top + CHROME_TAB_H + CHROME_TOOL_H)
                                : (my < grid_top)) {
                            int br_id2 = okai_find_by_win(i);
                            if (br_id2 >= 0 && okai_addr_bar_hit(br_id2, mx, my)) {
                                struct okai* ok = okai_get(br_id2);
                                if (ok && !ok->addr_bar_focused) {
                                    ok->addr_bar_focused = 1;
                                    ok->addr_input_len = 0;
                                    ok->addr_input[0] = 0;
                                    okai_render_content(br_id2);
                                }
                            } else {
                                drag_win = i;
                                drag_off_x = mx - w->x;
                                drag_off_y = my - w->y;
                            }
                        }
                        // Okai content → hit-test clickable links.
                        else if (mx >= w->x + WIN_BORDER && mx < w->x + w->w - WIN_BORDER &&
                                 my >= grid_top &&
                                 my < w->y + w->h - WIN_BORDER) {
                            int br_id = okai_find_by_win(i);
                            if (br_id >= 0) {
                                struct okai* ok = okai_get(br_id);
                                struct okai_tab* T = ok ? okai_tab_of(ok) : 0;
                                if (ok && T->link_count > 0) {
                                    int ox = w->x + WIN_BORDER;
                                    int cw = CONTENT_GW * w->font_scale;
                                    int chh = CONTENT_GH * w->font_scale;
                                    int col = (mx - ox) / cw;
                                    int row = (my - grid_top) / chh;
                                    serial_printf("[okai] click row=%d col=%d (mx=%d my=%d) links=%d\n",
                                                  row, col, mx, my, T->link_count);
                                    for (int li = 0; li < T->link_count; li++) {
                                        if (T->links[li].row == row &&
                                            col >= T->links[li].col0 &&
                                            col <= T->links[li].col1) {
                                            serial_printf("[okai] LINK HIT li=%d -> %s\n",
                                                          li, T->links[li].href);
                                            // Links navigate the CURRENT tab
                                            // in place — one browser window,
                                            // tabs instead of window sprawl.
                                            okai_navigate(br_id, T->links[li].href);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        clicked = 1;
                        break;
                    }
                }
            }
        }

        // (drag/resize repair runs after the cursor erase below — the drag
        // blit must copy pixels that contain no cursor sprite)

        if (!mb) {
            if (drag_win >= 0 || resize_win >= 0) needs_redraw = 1; // settle on release
            mouse_down = 0; drag_win = -1; resize_win = -1;
        }
        else { mouse_down = 1; }

        // Check for pending HTTP responses to save
        check_save_http_response();

        // Drive okai fetches. The network stack services a SINGLE global TCP
        // connection + response buffer, so only one fetch may be in flight at a
        // time. It is attributed to okai_fetch_owner; windows with token_count==0
        // are pending and are started in turn (lowest id first). Serializing this
        // way stops concurrent/mixed-protocol windows from corrupting the shared
        // TLS/TCP state (which froze the desktop) or mis-routing each other's
        // response bytes into the wrong buffer (which left windows blank).
        if (okai_fetch_owner >= 0) {
            int bi = okai_fetch_owner;
            struct okai* ok = okai_get(bi);
            struct okai_tab* T = ok ? okai_tab_of(ok) : 0;
            if (!ok) {
                okai_fetch_owner = -1;
        } else if (T->sub_res_phase > 0) {
            // Sub-resource fetch in progress
            if (T->is_https) {
                if (tls_is_done()) {
                    int resp_len = tls_get_response_len();
                    char* resp = tls_get_response();
                    if (resp && resp_len > 0) {
                        okai_sub_res_done(bi, resp, resp_len);
                    }
                    if (okai_start_sub_res_fetch(bi) != 0) {
                        okai_render_content(bi);
                        okai_fetch_owner = -1;
                    }
                } else if (!tls_is_active() && !tls_is_done()) {
                    T->sub_res_idx++;
                    if (okai_start_sub_res_fetch(bi) != 0) {
                        okai_render_content(bi);
                        okai_fetch_owner = -1;
                    }
                }
            } else {
                int resp_len = http_get_response_len();
                int done = http_is_done();
                if (done && resp_len > 0) {
                    char* resp = http_get_response();
                    if (resp) okai_sub_res_done(bi, resp, resp_len);
                    if (okai_start_sub_res_fetch(bi) != 0) {
                        okai_render_content(bi);
                        okai_fetch_owner = -1;
                    }
                } else if (!http_is_pending() && !http_is_retry_pending() && !http_is_done()) {
                    T->sub_res_idx++;
                    if (okai_start_sub_res_fetch(bi) != 0) {
                        okai_render_content(bi);
                        okai_fetch_owner = -1;
                    }
                }
            }
            } else if (T->is_https) {
                if (tls_is_done()) {
                    int resp_len = tls_get_response_len();
                    char* resp = tls_get_response();
                    if (resp && resp_len > 0) {
                        if (okai_check_redirect(bi, resp, resp_len)) {
                            // 3xx followed; owner stays bi, new fetch issued
                        } else {
                            resp_len = http_dechunk(resp, resp_len);
                            int css_len = html_extract_css(resp, resp_len,
                                                           T->css_text, OKAI_CSS_TEXT);
                            T->css_n = css_parse(T->css_text, css_len,
                                                  T->css_rules, CSS_MAX_RULES);
                            serial_printf("[br] css rules=%d\n", T->css_n);
                            serial_puts("[br] css text (first 200): ");
                            for (int ci = 0; ci < 200 && T->css_text[ci]; ci++)
                                serial_putchar(T->css_text[ci]);
                            serial_putchar('\n');
                            int count = html_parse(resp, resp_len,
                                                  T->tokens, OKAI_TAB_TOKENS);
                            serial_printf("[br] https parse: count=%d len=%d\n",
                                          count, resp_len);
                            T->token_count = count > 0 ? count : -1;
                            html_get_title(resp, resp_len, T->title, 64);
                            T->last_resp_len = resp_len;
                            okai_render_content(bi);
                            window_set_title(ok->win_id,
                                             T->title[0] ? T->title : "okai");
                            okai_fetch_owner = -1;
                        }
                    }
                } else if (!tls_is_active() && !tls_is_done()) {
                    // Fetch gave up. Classify: certificate/protocol failures
                    // MUST NOT fall back to plain HTTP — a MITM can force that
                    // downgrade by killing the TLS handshake. Only transport
                    // failures (timeout / unreachable / no A record) downgrade.
                    int fr = tls_get_fail_reason();
                    int cert_fail = (fr == TLS_FAIL_CERT ||
                                     fr == TLS_FAIL_HOSTNAME ||
                                     fr == TLS_FAIL_PROTO ||
                                     fr == TLS_FAIL_MAC ||
                                     fr == TLS_FAIL_ALERT ||
                                     fr == TLS_FAIL_RNG);
                    if (cert_fail) {
                        serial_printf("[okai] TLS cert failure (reason=%d), no HTTP fallback for %s\n",
                                      fr, T->url);
                        T->cert_failed = 1;
                        T->cert_detail = tls_get_fail_detail();
                        okai_fetch_owner = -1;
                        T->token_count = -1;
                        T->last_resp_len = 0;
                        okai_render_content(bi); // show SECURITY WARNING page
                    } else if (okai_fallback_http(bi) == 0) {
                        serial_printf("[okai] http fallback in flight for %s\n", T->url);
                    } else {
                        okai_fetch_owner = -1;
                        T->token_count = -1;
                        T->last_resp_len = 0;
                        okai_render_content(bi); // show "Unable to load" page
                    }
                }
            } else {
                int resp_len = http_get_response_len();
                int done = http_is_done();
                if (done && resp_len > 0) {
                    char* resp = http_get_response();
                    if (resp && okai_check_redirect(bi, resp, resp_len)) {
                        // 3xx followed; owner stays bi
                    } else if (resp) {
                        resp_len = http_dechunk(resp, resp_len);
                        int css_len = html_extract_css(resp, resp_len,
                                                       T->css_text, OKAI_CSS_TEXT);
                        T->css_n = css_parse(T->css_text, css_len,
                                              T->css_rules, CSS_MAX_RULES);
                        serial_printf("[br] css rules=%d\n", T->css_n);
                        serial_puts("[br] css text (first 200): ");
                        for (int ci = 0; ci < 200 && T->css_text[ci]; ci++)
                            serial_putchar(T->css_text[ci]);
                        serial_putchar('\n');
                        int count = html_parse(resp, resp_len,
                                               T->tokens, OKAI_TAB_TOKENS);
                        serial_printf("[br] parse: count=%d len=%d\n",
                                      count, resp_len);
                        // -1 sentinel: "parsed, nothing renderable" — keeps this
                        // block from re-parsing every frame on empty pages
                        T->token_count = count > 0 ? count : -1;
                        html_get_title(resp, resp_len, T->title, 64);
                        T->last_resp_len = resp_len;
                        okai_render_content(bi);
                        window_set_title(ok->win_id,
                                         T->title[0] ? T->title : "okai");
                        okai_fetch_owner = -1;
                    }
                } else if (!http_is_pending() && !http_is_retry_pending() && !http_is_done()) {
                    // Fetch gave up (connection closed with no data / unreachable)
                    okai_fetch_owner = -1;
                    T->token_count = -1;
                    T->last_resp_len = 0;
                    okai_render_content(bi); // show "Unable to load" page
                }
            }
        } else {
            // No fetch in flight: start the lowest-index window still needing one.
            for (int bi = 0; bi < MAX_OKAIS; bi++) {
                struct okai* ok = okai_get(bi);
                struct okai_tab* T = ok ? okai_tab_of(ok) : 0;
                if (ok && T->token_count == 0) {
                    if (okai_start_fetch(bi) == 0)
                        okai_fetch_owner = bi; // only claim ownership if a fetch fired
                    break;
                }
            }
        }

        // Check if JS DOM mutations require a re-render
        if (js_dom_is_rerender_needed()) {
            for (int bi = 0; bi < MAX_OKAIS; bi++) {
                struct okai* ok = okai_get(bi);
                if (ok) okai_render_content(bi);
            }
        }

        // Erase last frame's cursor sprite — only if the sprite overlaps
        // the dragged window's old footprint (avoids a full recomposite
        // when the cursor is far away from the drag region)
        if (cursor_shown) {
            // During an active drag we know the old window rect; skip the
            // erase when cursor is clearly outside it.  When not dragging
            // (drag_win < 0) always erase unconditionally.
            if (drag_win >= 0 && mb) {
                struct window* dw = window_get(drag_win);
                if (dw &&
                    (cursor_px + CURSOR_W <= dw->x ||
                     cursor_py + CURSOR_H <= dw->y ||
                     cursor_px >= dw->x + dw->w ||
                     cursor_py >= dw->y + dw->h)) {
                    // cursor fully outside old window — skip expensive erase
                } else {
                    desktop_paint_rect(cursor_px, cursor_py, CURSOR_W, CURSOR_H);
                }
            } else {
                desktop_paint_rect(cursor_px, cursor_py, CURSOR_W, CURSOR_H);
            }
            cursor_shown = 0;
        }

        // ---- Window drag: blit, don't re-render ----
        // A move doesn't change the window's own pixels, and the dragged
        // window is focused (topmost), so copying its old rectangle to the
        // new position is pixel-identical to re-rendering — then only the
        // exposed L-strips need recompositing. Re-rendering every cell via
        // the union repair pushed FPS from ~200 to ~15 during drags.
        if (mb && drag_win >= 0) {
            struct window* w = window_get(drag_win);
            if (w) {
                int nx = mx - drag_off_x;
                int ny = my - drag_off_y;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                // keep windows above the taskbar band (the taskbar is drawn
                // over them anyway); also keeps the blit clear of taskbar rows
                if (nx + w->w > SCREEN_W) nx = SCREEN_W - w->w;
                if (ny + w->h > SCREEN_H - TASKBAR_H) ny = SCREEN_H - TASKBAR_H - w->h;
                if (ny < 0) ny = 0;

                if (nx != w->x || ny != w->y) {
                    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
                    // If any OTHER window is dirty (text arriving below us),
                    // its repaint would overwrite the blit — take the full
                    // recomposite path for this frame instead
                    int other_dirty = 0;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        struct window* o = window_get(i);
                        if (o && o->visible && !o->minimized && i != drag_win && o->dirty) {
                            other_dirty = 1;
                            break;
                        }
                    }
                    if (other_dirty) {
                        w->x = nx; w->y = ny;
                        needs_redraw = 1;
                    } else {
                        // wipe the FPS-box HUD only if it overlaps the old
                        // window footprint — most drags are far from the
                        // top-right corner, saving a full recomposite call
                        {
                            int fps_rx = SCREEN_W - (9 * CHAR_W + 8) - 12;
                            int fps_ry = 4;
                            int fps_rw = (9 * CHAR_W + 8) + 16;
                            int fps_rh = CHAR_H + 8;
                            if (ox < fps_rx + fps_rw && ox + ow > fps_rx &&
                                oy < fps_ry + fps_rh && oy + oh > fps_ry) {
                                desktop_paint_rect_skip(drag_win,
                                    fps_rx, fps_ry, fps_rw, fps_rh);
                            }
                        }

                        w->x = nx; w->y = ny;
                        graphics_blit_rect(ox, oy, ow, oh, nx - ox, ny - oy);

                        // exposed L-strips: old footprint minus new
                        if (nx > ox)
                            desktop_paint_rect_skip(drag_win, ox, oy, nx - ox, oh);
                        if (nx + ow < ox + ow)
                            desktop_paint_rect_skip(drag_win, nx + ow, oy, (ox + ow) - (nx + ow), oh);
                        if (ny > oy)
                            desktop_paint_rect_skip(drag_win, ox, oy, ow, ny - oy);
                        if (ny + oh < oy + oh)
                            desktop_paint_rect_skip(drag_win, ox, ny + oh, ow, (oy + oh) - (ny + oh));
                    }
                }
            }
        }

        // ---- Live resize: band repair ----
        // Interior cells are anchored top-left and unchanged; only the
        // changed bands (old border/grip residue + newly exposed area)
        // need repaint. Content buffers are slack-allocated, no realloc.
        if (mb && resize_win >= 0) {
            struct window* w = window_get(resize_win);
            if (w) {
                int ow = w->w, oh = w->h;
                int was_redraw = needs_redraw;
                window_resize(resize_win,
                              (mx - w->x) + resize_off_w,
                              (my - w->y) + resize_off_h);
                if (!was_redraw && (w->w != ow || w->h != oh)) {
                    int dw = w->w > ow ? w->w - ow : ow - w->w;
                    int dh = w->h > oh ? w->h - oh : oh - w->h;
                    // covers stale border + grip pixels left inside
                    int band = WIN_BORDER + WIN_GRIP_SIZE + 2;
                    if (w->w != ow) {
                        int bx0 = w->x + (ow < w->w ? ow : w->w) - band;
                        int hmax = oh > w->h ? oh : w->h;
                        desktop_paint_rect(bx0, w->y, dw + band, hmax);
                    }
                    if (w->h != oh) {
                        int by0 = w->y + (oh < w->h ? oh : w->h) - band;
                        int wmax = ow > w->w ? ow : w->w;
                        desktop_paint_rect(w->x, by0, wmax, dh + band);
                    }
                    needs_redraw = 0; // bands above already repaired
                }
            }
        }

        // Keep the desktop live while the mouse moves: the cursor sprite is
        // repainted every iteration, but window content only refreshes on a
        // real redraw. Without this, moving the mouse leaves the desktop
        // frozen at the 1-second idle throttle.
        int cmx, cmy;
        int cursor_moved = 0;
        mouse_get_position(&cmx, &cmy);
        cursor_moved = (cmx != g_last_mouse_x || cmy != g_last_mouse_y);

        // Full redraw on demand (events / mouse movement), capped to at most
        // one per timer tick (100 Hz after the PIT bump), plus a 1-second idle
        // throttle so animations (cursor blink, clock) still advance.
        static uint32_t last_redraw_tick = 0;
        int composed = (needs_redraw || (tick_count - last_redraw_tick >= 100));
        if (composed) {
            // The wallpaper and desktop icons already live in the persistent
            // backbuffer (blitted once at init, repaired per-region on window
            // close/move). We no longer repaint the whole screen every frame,
            // and windows only repaint when they flagged themselves dirty
            // (keystroke, scroll, network data, cursor blink) — see window_draw.
            draw_desktop_icons();
            needs_redraw = 0;
            last_redraw_tick = tick_count;
        }

        // Draw windows back-to-front by z-stack. Each okai window's chrome and
        // heading overlay are painted right after the window itself, clipped to
        // the part of the window NOT covered by a higher-z window — so the
        // overlay never bleeds over a covering window, and that covering window
        // does NOT need a forced full repaint every frame (which tanked FPS when
        // a window sat over okai).
        {
            int vis[MAX_WINDOWS], nv = 0;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                struct window* w = window_get(i);
                if (w && w->visible && !w->minimized) vis[nv++] = i;
            }
            // insertion sort by z ascending (back to front)
            for (int a = 1; a < nv; a++) {
                int id = vis[a], z = window_get(id)->z, b = a - 1;
                while (b >= 0 && window_get(vis[b])->z > z) { vis[b + 1] = vis[b]; b--; }
                vis[b + 1] = id;
            }
            for (int a = 0; a < nv; a++) {
                int id = vis[a];
                struct window* w = window_get(id);
                window_draw(id);
                int ob = okai_find_by_win(id);
                if (ob >= 0) {
                    // Clip okai's overlay to the part of its window NOT covered
                    // by a higher-z window, so the overlay never paints over a
                    // covering window (which would otherwise need a forced full
                    // repaint every frame and tank FPS when a window sits over
                    // okai). The covering window is drawn later (higher z) and
                    // already occludes okai's content; okai's overlay only shows
                    // in the uncovered region.
                    int rects[64][4], nr = 1;
                    rects[0][0] = w->x; rects[0][1] = w->y;
                    rects[0][2] = w->w; rects[0][3] = w->h;
                    for (int c = a + 1; c < nv; c++) {
                        struct window* hw = window_get(vis[c]);
                        int hx0 = hw->x, hy0 = hw->y,
                            hx1 = hw->x + hw->w, hy1 = hw->y + hw->h;
                        int nr2 = 0, r2[64][4];
                        for (int i = 0; i < nr; i++) {
                            int rx = rects[i][0], ry = rects[i][1],
                                rw = rects[i][2], rh = rects[i][3];
                            int rx1 = rx + rw, ry1 = ry + rh;
                            if (hx0 >= rx1 || hx1 <= rx || hy0 >= ry1 || hy1 <= ry) {
                                r2[nr2][0]=rx; r2[nr2][1]=ry; r2[nr2][2]=rw; r2[nr2][3]=rh; nr2++;
                                continue;
                            }
                            if (rx < hx0) { r2[nr2][0]=rx; r2[nr2][1]=ry; r2[nr2][2]=hx0-rx; r2[nr2][3]=rh; nr2++; }
                            if (rx1 > hx1) { r2[nr2][0]=hx1; r2[nr2][1]=ry; r2[nr2][2]=rx1-hx1; r2[nr2][3]=rh; nr2++; }
                            int lx = rx > hx0 ? rx : hx0, rxr = rx1 < hx1 ? rx1 : hx1;
                            if (ry < hy0) { r2[nr2][0]=lx; r2[nr2][1]=ry; r2[nr2][2]=rxr-lx; r2[nr2][3]=hy0-ry; nr2++; }
                            if (ry1 > hy1) { r2[nr2][0]=lx; r2[nr2][1]=hy1; r2[nr2][2]=rxr-lx; r2[nr2][3]=ry1-hy1; nr2++; }
                        }
                        if (nr2 > 64) nr2 = 64;
                        nr = nr2;
                        for (int i = 0; i < nr; i++) {
                            rects[i][0]=r2[i][0]; rects[i][1]=r2[i][1];
                            rects[i][2]=r2[i][2]; rects[i][3]=r2[i][3];
                        }
                    }
                    okai_paint_overlays_rects(ob, rects, nr);
                }
            }
        }
        window_draw_taskbar();

        // FPS counter (drawn before the cursor so the sprite sits on top)
        static int g_show_fps = 1; // debug HUD; set 0 to leave the desktop clean
        if (tick_count - last_fps_tick >= 100) {
            fps = frame_count;
            frame_count = 0;
            last_fps_tick = tick_count;
        }
        if (g_show_fps) {
            // FPS counter — box sized from glyph metrics ("FPS: 9999" = 9 cells)
            char fps_buf[16] = "FPS: ";
            char num[8];
            put_uint(num, fps);
            int fi = 5;
            int ni = 0;
            while (num[ni]) fps_buf[fi++] = num[ni++];
            fps_buf[fi] = 0;
            int fps_w = 9 * CHAR_W + 8;
            rect_fill(SCREEN_W - fps_w - 4, 4, fps_w, CHAR_H + 8, 0x00000000);
            draw_string(SCREEN_W - fps_w, 8, fps_buf, 0x00FFFFFF, 0x00000000);
        }

        // Cursor composited last. With the backbuffer now persistent, we erase
        // the old sprite (restoring the underlying scene via desktop_paint_rect)
        // and draw the new one only when it actually moved — or after a composite
        // that may have overwritten its pixels. A static cursor costs zero work.
        mouse_get_position(&cursor_px, &cursor_py);
        if (cursor_shown && cursor_moved)
            desktop_paint_rect(g_last_mouse_x, g_last_mouse_y, CURSOR_W, CURSOR_H);
        mouse_paint_cursor(cursor_px, cursor_py);
        if (cursor_moved) {
            g_last_mouse_x = cmx;
            g_last_mouse_y = cmy;
        }
        cursor_shown = 1;
        // Honest FPS: count actually-presented frames (a composite or a cursor
        // movement), not main-loop spins.
        if (composed || cursor_moved) frame_count++;

        graphics_flush();
    }
}

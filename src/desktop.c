#include "net/pci.h"
#include "net/e1000.h"
#include "net/network.h"
#include "net/tls_net.h"
#include "graphics.h"
#include "wallpaper.h"
#include "window.h"
#include "paging.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "memory.h"
#include "serial.h"
#include "io.h"
#include "filesystem.h"
#include "editor.h"
#include "okai.h"
#include "theme.h"
#include "crypto/rand.h"

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

static void shell_prompt(int win_id) {
    window_set_text_color(win_id, 10, 0); // Green on black
    window_puts(win_id, "okernel> ");
    window_set_text_color(win_id, 15, 0); // Back to white on black
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

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
    else if (str_eq(cmd_buf, "shutdown")) {
        window_puts(win_id, "Shutting down...\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        while (1) { __asm__ volatile("hlt"); }
    }
    else {
        window_puts(win_id, "Unknown: ");
        window_puts(win_id, cmd_buf);
        window_put_char(win_id, '\n');
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

    if (c == '\b') {
        if (term_lens[tidx] > 0) {
            term_lens[tidx]--;
            window_put_char(win_id, '\b');
        }
    } else if (c == '\n') {
        window_put_char(win_id, '\n');
        term_bufs[tidx][term_lens[tidx]] = 0;
        shell_execute(win_id, term_bufs[tidx]);
        term_lens[tidx] = 0;
        shell_prompt(win_id);
    } else {
        if (term_lens[tidx] < 255) {
            term_bufs[tidx][term_lens[tidx]++] = c;
            window_put_char(win_id, c);
        }
    }
}

static int rand_stir_ticks = 0;
static void on_timer(void) {
    tick_count++;
    // Keep mixing entropy into the CPRNG (forward secrecy) for the first ~32
    // ticks. rand_seed() at boot already primed it to "ready"; this just
    // continues folding fresh RDTSC + MAC jitter into the keystream.
    if (rand_stir_ticks < 32) {
        uint8_t sample[32];
        uint64_t t = 0;
        __asm__ volatile("rdtsc" : "=A"(t));
        uint8_t* mac = e1000_get_mac();
        for (int i = 0; i < 32; i++)
            sample[i] = (uint8_t)((t >> ((i & 7) * 8)) ^ ((mac[i % 6] << 1) & 0xFF) ^ (rand_stir_ticks * 37 + i));
        rand_stir(sample);
        rand_stir_ticks++;
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

void kernel_main(uint32_t mboot_addr) {
    serial_init();
    gdt_init();
    idt_init();
    memory_init(mboot_addr);

    struct mboot_info* mboot = (struct mboot_info*)mboot_addr;
    uint32_t fb_addr = 0;
    if (mboot->flags & (1 << 12)) {
        fb_addr = (uint32_t)mboot->framebuffer_addr;
    }
    if (fb_addr) paging_init(fb_addr);

    graphics_init(mboot_addr);
    window_init();
    fs_init();
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

    // Init networking (full stack: PCI + RTL8139 + ARP + IP + ICMP)
    net_init();
    net_set_event_callback(on_net_event);

    // Seed the ChaCha20 CPRNG used by the TLS client for key material.
    // Mix RDTSC + the e1000 MAC, stir 32x so the CPRNG reaches "ready"
    // immediately (rand_ready() requires 1024 bytes of mixed entropy).
    {
        uint8_t* mac = e1000_get_mac();
        uint8_t seed[32];
        uint64_t tsc = 0;
        __asm__ volatile("rdtsc" : "=A"(tsc));
        for (int i = 0; i < 32; i++)
            seed[i] = (uint8_t)(mac[i % 6] ^ ((tsc >> ((i & 7) * 8)) & 0xFF) ^ (i * 0x9E));
        rand_seed(seed);
        for (int s = 0; s < 32; s++) {
            uint64_t t2 = 0;
            __asm__ volatile("rdtsc" : "=A"(t2));
            uint8_t sample[32];
            for (int i = 0; i < 32; i++)
                sample[i] = (uint8_t)((t2 >> ((i & 7) * 8)) ^ ((mac[i % 6] << 1) & 0xFF) ^ (s * 37 + i));
            rand_stir(sample);
        }
        serial_printf("[rand] CPRNG seeded, ready=%d\n", rand_ready());
    }

    // Init input
    mouse_init_fb();
    keyboard_init();
    keyboard_set_callback(on_keypress);
    sti();

    // Main loop
    while (1) {
        // Poll network for incoming packets
        e1000_poll();
        net_poll();
        http_poll();
        https_get_poll(); // advance any in-flight async HTTPS fetch

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
                    // Fetch gave up (TLS timeout / unreachable / no A record).
                    // If we haven't already, retry once over plain HTTP — some
                    // hosts don't serve HTTPS. Ownership stays with this window;
                    // the next loop iteration takes the HTTP branch (is_https is
                    // now 0) so this branch won't re-fire.
                    if (okai_fallback_http(bi) == 0) {
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

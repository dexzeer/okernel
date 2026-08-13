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
    info_win = window_create("System Info", 240, 60, 280, 220);
    window_set_close_button(info_win, 1);
    window_set_minimize_button(info_win, 1);
    window_puts(info_win, "okernel v0.2\n");
    window_puts(info_win, "Desktop Edition\n\n");
    window_puts(info_win, "Resolution: 640x480\n");
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
    int x = 80 + (term_count % 4) * 30;
    int y = 60 + (term_count % 4) * 25;
    int win_id = window_create("Terminal", x, y, 440, 360);
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
        window_puts(win_id, "  reboot    - reboot system\n");
        window_puts(win_id, "  shutdown  - power off\n");
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
        uint32_t seconds = tick_count / 18;
        uint32_t h = seconds / 3600;
        uint32_t m = (seconds % 3600) / 60;
        uint32_t s = seconds % 60;
        char buf[16];
        int idx = 0;

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
        window_puts(win_id, "okernel v0.2 - Desktop Edition\n");
        window_puts(win_id, "Built from scratch in C and x86 assembly\n");
    }
    else if (str_eq(cmd_buf, "neofetch") || str_eq(cmd_buf, "sysinfo")) {
        window_puts(win_id, "okernel v0.2\n");
        window_puts(win_id, "Resolution: 640x480\n");
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

static void on_timer(void) {
    tick_count++;
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

    // Cache wallpaper for fast blitting
    graphics_cache_wallpaper(wp_pixels, WP_W, WP_H);

    // Draw initial wallpaper
    graphics_blit_wallpaper();

    irq_register_handler(0, on_timer);

    // Create main terminal — large, centered
    term_wins[0] = window_create("Terminal", 60, 40, 440, 360);
    window_set_close_button(term_wins[0], 1);
    window_set_minimize_button(term_wins[0], 1);
    term_count = 1;
    active_term_idx = 0;
    window_set_focus(term_wins[0]);

    // Show welcome message with ASCII art (like text mode)
    window_set_text_color(term_wins[0], 11, 0); // Cyan
    window_puts(term_wins[0], "        _                        _ \n");
    window_puts(term_wins[0], "       | |                      | |\n");
    window_puts(term_wins[0], "   ___ | | _____ _ __ _ __   ___| |\n");
    window_puts(term_wins[0], "  / _ \\| |/ / _ \\ '__| '_ \\ / _ \\ |\n");
    window_puts(term_wins[0], " | (_) |   <  __/ |  | | | |  __/ |\n");
    window_puts(term_wins[0], "  \\___/|_|\\_\\___|_|  |_| |_|\\___|_|\n\n");
    window_set_text_color(term_wins[0], 15, 0); // White
    window_puts(term_wins[0], "Welcome to okernel v0.2\n");
    window_puts(term_wins[0], "A minimalistic operating system.\n\n");
    window_set_text_color(term_wins[0], 8, 0); // Grey
    window_puts(term_wins[0], "Type 'help' for commands.\n");
    window_puts(term_wins[0], "Type 'terminal' for new window.\n");
    window_puts(term_wins[0], "Type 'exit' to close this terminal.\n\n");
    window_set_text_color(term_wins[0], 15, 0); // Back to white
    shell_prompt(term_wins[0]);

    // Init input
    mouse_init_fb();
    keyboard_init();
    keyboard_set_callback(on_keypress);
    sti();

    // Main loop
    while (1) {
        int mx = mouse_get_x();
        int my = mouse_get_y();
        int mb = mouse_get_left_button();

        if (mb && !mouse_down) {
            int clicked = 0;

            // Check taskbar clicks (bottom 20px)
            if (my >= SCREEN_H - 20) {
                // Count visible windows to match dynamic width
                int vis_count = 0;
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (window_get(i) && window_get(i)->visible) vis_count++;
                }
                if (vis_count > 0) {
                    int total_pad = (vis_count + 1) * 4;
                    int btn_w = (SCREEN_W - total_pad) / vis_count;
                    if (btn_w > 120) btn_w = 120;
                    if (btn_w < 30) btn_w = 30;

                    int tx = 4;
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
                        tx += btn_w + 4;
                    }
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
                        }
                        clicked = 1;
                        break;
                    }

                    if (window_check_minimize_click(i, mx, my)) {
                        window_minimize(i);
                        // Restore wallpaper where the window was
                        graphics_blit_wallpaper();
                        // Mark remaining windows dirty
                        for (int j = 0; j < MAX_WINDOWS; j++) {
                            struct window* w2 = window_get(j);
                            if (w2 && w2->visible && !w2->minimized && j != i) w2->dirty = 1;
                        }
                        clicked = 1;
                        break;
                    }

                    if (mx >= w->x && mx < w->x + w->w &&
                        my >= w->y && my < w->y + WIN_TITLE_H + WIN_BORDER) {
                        drag_win = i;
                        drag_off_x = mx - w->x;
                        drag_off_y = my - w->y;
                        window_restore(i);
                        int tidx = find_term_idx(i);
                        if (tidx >= 0) {
                            active_term_idx = tidx;
                            window_set_focus(i);
                        }
                        clicked = 1;
                        break;
                    }
                }
            }
        }

        if (mb && drag_win >= 0) {
            struct window* w = window_get(drag_win);
            if (w) {
                w->x = mx - drag_off_x;
                w->y = my - drag_off_y;
                if (w->x < 0) w->x = 0;
                if (w->y < 0) w->y = 0;
                if (w->x + w->w > SCREEN_W) w->x = SCREEN_W - w->w;
                if (w->y + w->h > SCREEN_H) w->y = SCREEN_H - w->h;
                needs_redraw = 1;
            }
        }

        if (!mb) { mouse_down = 0; drag_win = -1; }
        else { mouse_down = 1; }

        // Only blit wallpaper when windows change, then mark dirty
        if (needs_redraw) {
            graphics_blit_wallpaper();
            for (int i = 0; i < MAX_WINDOWS; i++) {
                struct window* w = window_get(i);
                if (w && w->visible && !w->minimized) w->dirty = 1;
            }
            needs_redraw = 0;
        }

        // Draw windows (only dirty ones)
        mouse_hide_cursor();
        window_draw_all();
        window_draw_taskbar();
        mouse_draw_cursor();

        // FPS counter
        frame_count++;
        if (tick_count - last_fps_tick >= 18) {
            fps = frame_count;
            frame_count = 0;
            last_fps_tick = tick_count;
        }
        // FPS counter — wider box for 3-digit numbers
        char fps_buf[16] = "FPS: ";
        char num[8];
        put_uint(num, fps);
        int fi = 5;
        int ni = 0;
        while (num[ni]) fps_buf[fi++] = num[ni++];
        fps_buf[fi] = 0;
        rect_fill(SCREEN_W - 70, 2, 68, 10, 0);
        draw_string(SCREEN_W - 68, 3, fps_buf, 15, 0);

        graphics_flush();
    }
}

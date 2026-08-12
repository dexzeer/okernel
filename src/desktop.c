#include "graphics.h"
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

// Shell state
static int shell_win = -1;
static char shell_buf[256];
static int shell_len = 0;
static uint32_t tick_count = 0;

static void shell_prompt(int win_id) {
    window_puts(win_id, "okernel> ");
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
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
        window_puts(win_id, "  help     - show this message\n");
        window_puts(win_id, "  clear    - clear terminal\n");
        window_puts(win_id, "  echo     - print text\n");
        window_puts(win_id, "  mem      - memory info\n");
        window_puts(win_id, "  uptime   - system uptime\n");
        window_puts(win_id, "  about    - about okernel\n");
        window_puts(win_id, "  neofetch - system info\n");
        window_puts(win_id, "  reboot   - reboot system\n");
        window_puts(win_id, "  shutdown - power off\n");
    }
    else if (str_eq(cmd_buf, "clear")) {
        window_clear(win_id);
    }
    else if (str_eq(cmd_buf, "echo")) {
        window_puts(win_id, args);
        window_put_char(win_id, '\n');
    }
    else if (str_eq(cmd_buf, "mem")) {
        uint32_t total = pmm_get_total_pages() * 4;
        uint32_t used = pmm_get_used_pages() * 4;
        uint32_t free = pmm_get_free_pages() * 4;

        char buf[16];
        uint32_t tmp;

        window_puts(win_id, "Memory:\n");

        // Total
        tmp = total / 1024; int idx = 0;
        if (tmp == 0) { buf[idx++] = '0'; }
        else { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx] = 0;
        window_puts(win_id, "  Total: ");
        window_puts(win_id, buf);
        window_puts(win_id, " MB\n");

        // Used
        tmp = used / 1024; idx = 0;
        if (tmp == 0) { buf[idx++] = '0'; }
        else { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx] = 0;
        window_puts(win_id, "  Used:  ");
        window_puts(win_id, buf);
        window_puts(win_id, " MB\n");

        // Free
        tmp = free / 1024; idx = 0;
        if (tmp == 0) { buf[idx++] = '0'; }
        else { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx] = 0;
        window_puts(win_id, "  Free:  ");
        window_puts(win_id, buf);
        window_puts(win_id, " MB\n");
    }
    else if (str_eq(cmd_buf, "uptime")) {
        // Approximate uptime from tick count (IRQ0 fires ~18.2 Hz)
        uint32_t seconds = tick_count / 18;
        uint32_t minutes = seconds / 60;
        uint32_t hours = minutes / 60;
        char buf[16];
        int idx = 0;

        // Hours
        uint32_t h = hours;
        if (h == 0) { buf[idx++] = '0'; }
        else { char rev[16]; int ri = 0; while (h > 0) { rev[ri++] = '0' + (h % 10); h /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx++] = 'h';

        // Minutes
        uint32_t m = minutes % 60;
        if (m < 10) buf[idx++] = '0';
        h = m;
        { char rev[16]; int ri = 0; while (h > 0) { rev[ri++] = '0' + (h % 10); h /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx++] = 'm';

        // Seconds
        uint32_t s = seconds % 60;
        if (s < 10) buf[idx++] = '0';
        h = s;
        { char rev[16]; int ri = 0; while (h > 0) { rev[ri++] = '0' + (h % 10); h /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx++] = 's';
        buf[idx] = 0;

        window_puts(win_id, "Uptime: ");
        window_puts(win_id, buf);
        window_put_char(win_id, '\n');
    }
    else if (str_eq(cmd_buf, "about")) {
        window_puts(win_id, "okernel v0.2\n");
        window_puts(win_id, "Desktop Edition\n");
        window_puts(win_id, "Built from scratch\n");
        window_puts(win_id, "in C and x86 assembly\n");
    }
    else if (str_eq(cmd_buf, "neofetch")) {
        window_puts(win_id, "        ___         \n");
        window_puts(win_id, "       /   \\  okernel\n");
        window_puts(win_id, "      /     \\ v0.2  \n");
        window_puts(win_id, "     /  ____ \\      \n");
        window_puts(win_id, "    /  /    \\ \\     \n");
        window_puts(win_id, "   /__/      \\_\\    \n");
        window_puts(win_id, "OS: okernel 0.2\n");
        window_puts(win_id, "Resolution: 640x480\n");
        window_puts(win_id, "Shell: okernel sh\n");
        window_puts(win_id, "Memory: ");
        // Show used/total
        uint32_t total = pmm_get_total_pages() * 4 / 1024;
        uint32_t used = pmm_get_used_pages() * 4 / 1024;
        char buf[16];
        uint32_t tmp = used; int idx = 0;
        if (tmp == 0) { buf[idx++] = '0'; }
        else { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx++] = '/'; tmp = total;
        { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } }
        buf[idx++] = 'M'; buf[idx] = 0;
        window_puts(win_id, buf);
        window_put_char(win_id, '\n');
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
        window_puts(win_id, "Unknown command: ");
        window_puts(win_id, cmd_buf);
        window_put_char(win_id, '\n');
    }
}

static void on_keypress(char c) {
    int win_id = window_get_focused();
    if (win_id < 0) return;

    if (c == '\b') {
        if (shell_len > 0) {
            shell_len--;
            window_put_char(win_id, '\b');
        }
    } else if (c == '\n') {
        window_put_char(win_id, '\n');
        shell_buf[shell_len] = 0;
        shell_execute(win_id, shell_buf);
        shell_len = 0;
        shell_prompt(win_id);
    } else {
        if (shell_len < 255) {
            shell_buf[shell_len++] = c;
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

    // Get framebuffer address from multiboot and set up paging
    struct mboot_info* mboot = (struct mboot_info*)mboot_addr;
    uint32_t fb_addr = 0;
    if (mboot->flags & (1 << 12)) {
        fb_addr = (uint32_t)mboot->framebuffer_addr;
    }
    if (fb_addr) {
        paging_init(fb_addr);
    }

    graphics_init(mboot_addr);
    window_init();

    // Register timer callback for uptime
    irq_register_handler(0, on_timer);

    // Create terminal window
    shell_win = window_create("Terminal", 40, 40, 380, 300);
    window_set_focus(shell_win);
    shell_prompt(shell_win);

    // Create system info window (neofetch-style)
    int info_win = window_create("System Info", 260, 60, 300, 240);
    window_puts(info_win, "        ___         \n");
    window_puts(info_win, "       /   \\  okernel\n");
    window_puts(info_win, "      /     \\ v0.2  \n");
    window_puts(info_win, "     /  ____ \\      \n");
    window_puts(info_win, "    /  /    \\ \\     \n");
    window_puts(info_win, "   /__/      \\_\\    \n");
    window_puts(info_win, "OS: okernel 0.2\n");
    window_puts(info_win, "Res: 640x480\n");
    window_puts(info_win, "Shell: okernel sh\n");
    window_puts(info_win, "Type 'help' in\n");
    window_puts(info_win, "terminal for cmds\n");

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
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                struct window* w = window_get(i);
                if (!w || !w->visible) continue;
                if (mx >= w->x && mx < w->x + w->w &&
                    my >= w->y && my < w->y + WIN_TITLE_H + WIN_BORDER) {
                    drag_win = i;
                    drag_off_x = mx - w->x;
                    drag_off_y = my - w->y;
                    window_set_focus(i);
                    break;
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
            }
        }

        if (!mb) { mouse_down = 0; drag_win = -1; }
        else { mouse_down = 1; }

        // Draw wallpaper
        for (int y = 0; y < SCREEN_H; y++) {
            uint8_t color = 1 + (y / 60);
            if (color > 9) color = 9;
            hline(0, y, SCREEN_W, color);
        }

        // Draw dots pattern
        for (int y = 0; y < SCREEN_H; y += 30)
            for (int x = 0; x < SCREEN_W; x += 30)
                putpixel(x, y, 3);

        window_draw_all();
        mouse_draw_cursor();
        graphics_flush();
    }
}

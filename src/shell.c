#include "shell.h"
#include "vga.h"
#include "terminal.h"
#include "memory.h"
#include "io.h"
#include <stdint.h>

static void cmd_list(void) {
    terminal_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_puts("Active terminals:\n");
    terminal_set_color(VGA_WHITE, VGA_BLACK);
    int active = terminal_get_active();
    char buf[4];
    for (int i = 0; i < 8; i++) {
        if (!terminal_is_active(i)) continue;
        buf[0] = '0' + i;
        buf[1] = 0;
        terminal_puts("  Terminal ");
        terminal_puts(buf);
        if (i == active) {
            terminal_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            terminal_puts(" [ACTIVE]");
            terminal_set_color(VGA_WHITE, VGA_BLACK);
        }
        terminal_putchar('\n');
    }
}

static void cmd_switch(const char* args) {
    if (!args) {
        terminal_set_color(VGA_RED, VGA_BLACK);
        terminal_puts("Usage: switch <id>\n");
        terminal_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    // Simple single digit parse
    if (args[0] < '0' || args[0] > '7' || args[1] != 0) {
        terminal_set_color(VGA_RED, VGA_BLACK);
        terminal_puts("Invalid terminal id (0-7)\n");
        terminal_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    int id = args[0] - '0';
    terminal_switch(id);
    terminal_flush();
    shell_prompt();
}

static void cmd_help(void) {
    terminal_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_puts("Available commands:\n");
    terminal_set_color(VGA_WHITE, VGA_BLACK);
    terminal_puts("  help      - show this message\n");
    terminal_puts("  clear     - clear the screen\n");
    terminal_puts("  echo      - print text\n");
    terminal_puts("  mem       - show memory usage\n");
    terminal_puts("  terminal  - open a new terminal\n");
    terminal_puts("  exit      - close current terminal\n");
    terminal_puts("  list      - show active terminals\n");
    terminal_puts("  switch N  - switch to terminal N\n");
    terminal_puts("  reboot    - reboot the system\n");
    terminal_puts("  shutdown  - power off the system\n");
    terminal_puts("  about     - about okernel\n");
}

static void cmd_clear(void) {
    terminal_clear();
}

static void cmd_echo(const char* args) {
    if (args) {
        terminal_puts(args);
        terminal_putchar('\n');
    }
}

static void cmd_mem(void) {
    uint32_t total = pmm_get_total_pages();
    uint32_t used = pmm_get_used_pages();
    uint32_t free = pmm_get_free_pages();
    uint32_t total_mb = (total * 4) / 1024;
    uint32_t used_mb = (used * 4) / 1024;
    uint32_t free_mb = (free * 4) / 1024;

    terminal_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_puts("Memory Status\n");
    terminal_set_color(VGA_WHITE, VGA_BLACK);

    char buf[16];
    uint32_t tmp;
    int idx;

    // Helper macro for int-to-string
    #define INT_STR(val) do { \
        tmp = (val); idx = 0; \
        if (tmp == 0) { buf[idx++] = '0'; } \
        else { char rev[16]; int ri = 0; while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; } while (ri > 0) { buf[idx++] = rev[--ri]; } } \
        buf[idx] = 0; \
    } while(0)

    terminal_puts("  Total: ");
    INT_STR(total_mb); terminal_puts(buf); terminal_puts(" MB (");
    INT_STR(total); terminal_puts(buf); terminal_puts(" pages)\n");

    terminal_puts("  Used:  ");
    INT_STR(used_mb); terminal_puts(buf); terminal_puts(" MB (");
    INT_STR(used); terminal_puts(buf); terminal_puts(" pages)\n");

    terminal_puts("  Free:  ");
    INT_STR(free_mb); terminal_puts(buf); terminal_puts(" MB (");
    INT_STR(free); terminal_puts(buf); terminal_puts(" pages)\n");

    #undef INT_STR
}

static void cmd_terminal(void) {
    int id = terminal_create();
    if (id >= 0) {
        terminal_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        terminal_puts("Terminal ");
        char id_buf[4] = {'0' + id, 0};
        terminal_puts(id_buf);
        terminal_puts(" created.\n\n");
        terminal_set_color(VGA_WHITE, VGA_BLACK);
        terminal_switch(id);
        shell_prompt();
    } else {
        terminal_set_color(VGA_RED, VGA_BLACK);
        terminal_puts("Error: maximum terminals reached\n");
        terminal_set_color(VGA_WHITE, VGA_BLACK);
    }
}

static void cmd_exit(void) {
    int id = terminal_get_active();
    if (id == 0) {
        terminal_set_color(VGA_RED, VGA_BLACK);
        terminal_puts("Cannot close the main terminal\n");
        terminal_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    terminal_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_puts("Closing terminal ");
    char id_buf[4] = {'0' + id, 0};
    terminal_puts(id_buf);
    terminal_puts("...\n");
    terminal_set_color(VGA_WHITE, VGA_BLACK);
    terminal_destroy(id);
}

static void cmd_about(void) {
    terminal_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_puts("        _                        _ \n");
    terminal_puts("       | |                      | |\n");
    terminal_puts("   ___ | | _____ _ __ _ __   ___| |\n");
    terminal_puts("  / _ \\| |/ / _ \\ '__| '_ \\ / _ \\ |\n");
    terminal_puts(" | (_) |   <  __/ |  | | | |  __/ |\n");
    terminal_puts("  \\___/|_|\\_\\___|_|  |_| |_|\\___|_|\n\n");
    terminal_set_color(VGA_WHITE, VGA_BLACK);
    terminal_puts("okernel v0.1\n");
    terminal_puts("A minimalistic operating system\n");
    terminal_puts("Built from scratch in C and x86 assembly\n");
}

static void cmd_reboot(void) {
    terminal_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_puts("Rebooting...\n");
    // Wait for keyboard controller to be ready, then reset CPU
    while (inb(0x64) & 0x02);
    outb(0x64, 0xFE);
    while (1) { __asm__ volatile("hlt"); }
}

static void cmd_shutdown(void) {
    terminal_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_puts("Shutting down...\n");
    // QEMU/Bochs ACPI power-off
    outw(0x604, 0x2000);
    // Fallback: try APM
    outw(0xB004, 0x2000);
    while (1) { __asm__ volatile("hlt"); }
}

void shell_init(void) {
}

void shell_execute(const char* input) {
    while (*input == ' ') input++;
    if (*input == 0) return;

    const char* cmd = input;
    const char* args = 0;
    int cmd_len = 0;
    while (cmd[cmd_len] && cmd[cmd_len] != ' ') cmd_len++;
    if (cmd[cmd_len] == ' ') {
        args = &cmd[cmd_len + 1];
    }

    char cmd_buf[32];
    int j;
    for (j = 0; j < cmd_len && j < 31; j++) cmd_buf[j] = cmd[j];
    cmd_buf[j] = 0;

    if (cmd_buf[0] == 'h' && cmd_buf[1] == 'e' && cmd_buf[2] == 'l' && cmd_buf[3] == 'p' && cmd_buf[4] == 0) {
        cmd_help();
    }
    else if (cmd_buf[0] == 'c' && cmd_buf[1] == 'l' && cmd_buf[2] == 'e' && cmd_buf[3] == 'a' && cmd_buf[4] == 'r' && cmd_buf[5] == 0) {
        cmd_clear();
    }
    else if (cmd_buf[0] == 'e' && cmd_buf[1] == 'c' && cmd_buf[2] == 'h' && cmd_buf[3] == 'o' && cmd_buf[4] == 0) {
        cmd_echo(args);
    }
    else if (cmd_buf[0] == 'm' && cmd_buf[1] == 'e' && cmd_buf[2] == 'm' && cmd_buf[3] == 0) {
        cmd_mem();
    }
    else if (cmd_buf[0] == 't' && cmd_buf[1] == 'e' && cmd_buf[2] == 'r' && cmd_buf[3] == 'm' && cmd_buf[4] == 'i' && cmd_buf[5] == 'n' && cmd_buf[6] == 'a' && cmd_buf[7] == 'l' && cmd_buf[8] == 0) {
        cmd_terminal();
    }
    else if (cmd_buf[0] == 'e' && cmd_buf[1] == 'x' && cmd_buf[2] == 'i' && cmd_buf[3] == 't' && cmd_buf[4] == 0) {
        cmd_exit();
    }
    else if (cmd_buf[0] == 'l' && cmd_buf[1] == 'i' && cmd_buf[2] == 's' && cmd_buf[3] == 't' && cmd_buf[4] == 0) {
        cmd_list();
    }
    else if (cmd_buf[0] == 's' && cmd_buf[1] == 'w' && cmd_buf[2] == 'i' && cmd_buf[3] == 't' && cmd_buf[4] == 'c' && cmd_buf[5] == 'h' && cmd_buf[6] == 0) {
        cmd_switch(args);
    }
    else if (cmd_buf[0] == 'a' && cmd_buf[1] == 'b' && cmd_buf[2] == 'o' && cmd_buf[3] == 'u' && cmd_buf[4] == 't' && cmd_buf[5] == 0) {
        cmd_about();
    }
    else if (cmd_buf[0] == 'r' && cmd_buf[1] == 'e' && cmd_buf[2] == 'b' && cmd_buf[3] == 'o' && cmd_buf[4] == 'o' && cmd_buf[5] == 't' && cmd_buf[6] == 0) {
        cmd_reboot();
    }
    else if (cmd_buf[0] == 's' && cmd_buf[1] == 'h' && cmd_buf[2] == 'u' && cmd_buf[3] == 't' && cmd_buf[4] == 'd' && cmd_buf[5] == 'o' && cmd_buf[6] == 'w' && cmd_buf[7] == 'n' && cmd_buf[8] == 0) {
        cmd_shutdown();
    }
    else {
        terminal_set_color(VGA_RED, VGA_BLACK);
        terminal_puts("unknown command: ");
        terminal_puts(cmd_buf);
        terminal_putchar('\n');
        terminal_set_color(VGA_WHITE, VGA_BLACK);
    }
}

void shell_prompt(void) {
    terminal_set_color(VGA_GREEN, VGA_BLACK);
    terminal_puts("> ");
    terminal_set_color(VGA_WHITE, VGA_BLACK);
}

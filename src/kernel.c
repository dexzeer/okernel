#include "vga.h"
#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "memory.h"
#include "shell.h"
#include "serial.h"
#include "io.h"

static char input_buf[256];
static int input_len = 0;

static void on_keypress(char c) {
    // Scroll keys
    if (c == '\x12') { terminal_scroll(5); return; }    // Page Up
    if (c == '\x04') { terminal_scroll(-5); return; }   // Page Down
    if (c == '\x11') { terminal_scroll(1); return; }    // Up/Left Arrow
    if (c == '\x10') { terminal_scroll(-1); return; }   // Down/Right Arrow

    if (c == '\b') {
        if (input_len > 0) {
            input_len--;
            terminal_puts("\b \b");
        }
    } else if (c == '\n') {
        terminal_putchar('\n');
        if (input_len > 0) {
            input_buf[input_len] = 0;
            shell_execute(input_buf);
            input_len = 0;
        }
        shell_prompt();
    } else if (c == '\t') {
    } else {
        if (input_len < 255) {
            input_buf[input_len++] = c;
            terminal_putchar(c);
        }
    }
}

void kernel_main(uint32_t mboot_addr) {
    serial_init();
    gdt_init();
    idt_init();

    uint16_t* vga = (uint16_t*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x0F00;

    memory_init(mboot_addr);
    terminal_init();

    terminal_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_puts("        _                        _ \n");
    terminal_puts("       | |                      | |\n");
    terminal_puts("   ___ | | _____ _ __ _ __   ___| |\n");
    terminal_puts("  / _ \\| |/ / _ \\ '__| '_ \\ / _ \\ |\n");
    terminal_puts(" | (_) |   <  __/ |  | | | |  __/ |\n");
    terminal_puts("  \\___/|_|\\_\\___|_|  |_| |_|\\___|_|\n");
    terminal_puts("                                    \n");
    terminal_puts("                                    \n\n");

    terminal_set_color(VGA_WHITE, VGA_BLACK);
    terminal_puts("Welcome to okernel v0.1\n");
    terminal_puts("A minimalistic operating system.\n\n");
    terminal_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_puts("Type 'help' for available commands.\n");
    terminal_puts("Type 'terminal' to open a new terminal.\n");
    terminal_puts("Page Up/Down or Arrow Keys to scroll.\n");
    terminal_puts("Home = jump to bottom.\n\n");

    shell_init();
    keyboard_init();
    keyboard_set_callback(on_keypress);
    sti();

    shell_prompt();

    while (1) {
        __asm__ volatile("hlt");
    }
}

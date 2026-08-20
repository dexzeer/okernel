#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "serial.h"

// Scancode set 1 → ASCII lookup (unshifted), 128 entries
static const char scancode_ascii[128] = {
    0,    0,   '1', '2', '3', '4', '5', '6',  // 0x00-0x07
    '7',  '8', '9', '0', '-', '=',  0,   0,    // 0x08-0x0F
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',  // 0x10-0x17
    'o',  'p', '[', ']', '\n', 0,  'a', 's',   // 0x18-0x1F
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',  // 0x20-0x27
    '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',   // 0x28-0x2F
    'b',  'n', 'm', ',', '.', '/', 0,   '*',   // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,      // 0x38-0x3F
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x40-0x47 (F1-F8)
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x48-0x4F (numpad)
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x50-0x57
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x58-0x5F
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x60-0x67
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x68-0x6F
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x70-0x77
    0,    0,   0,   0,   0,   0,   0,   0,      // 0x78-0x7F
};

// Shifted scancode lookup, 128 entries
static const char scancode_shift[128] = {
    0,    0,   '!', '@', '#', '$', '%', '^',  // 0x00-0x07
    '&',  '*', '(', ')', '_', '+',  0,   0,   // 0x08-0x0F
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', // 0x10-0x17
    'O',  'P', '{', '}', '\n', 0,  'A', 'S',  // 0x18-0x1F
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',  // 0x20-0x27
    '"',  '~', 0,  '|', 'Z', 'X', 'C', 'V',   // 0x28-0x2F
    'B',  'N', 'M', '<', '>', '?', 0,   '*',  // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,      // 0x38-0x3F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x40-0x47
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x48-0x4F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x50-0x57
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x58-0x5F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x60-0x67
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x68-0x6F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x70-0x77
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x78-0x7F
};

static keyboard_callback_t callback = 0;
static int shift_pressed = 0;
static int extended = 0; // 1 = received 0xE0 prefix

static void keyboard_irq(void) {
    uint8_t scancode = inb(0x60);

    // Extended key prefix
    if (scancode == 0xE0) {
        extended = 1;
        return;
    }

    // Extended key (arrow keys, etc.)
    if (extended) {
        extended = 0;
        if (scancode & 0x80) return; // key release, ignore

        switch (scancode) {
            case 0x48: if (callback) callback('\x11'); break; // Up Arrow
            case 0x50: if (callback) callback('\x10'); break; // Down Arrow
            case 0x4B: if (callback) callback('\x11'); break; // Left Arrow (scroll up too)
            case 0x4D: if (callback) callback('\x10'); break; // Right Arrow (scroll down too)
    // Home = jump to bottom (terminal mode only, handled by caller)
    // case 0x47: terminal_scroll_bottom(); break;
        }
        return;
    }

    // Key release
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            shift_pressed = 0;
        }
        return;
    }

    // Special keys
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }

    // Backspace
    if (scancode == 0x0E) {
        if (callback) callback('\b');
        return;
    }

    // Page Up / Page Down
    if (scancode == 0x49) { if (callback) callback('\x12'); return; } // Page Up
    if (scancode == 0x51) { if (callback) callback('\x04'); return; } // Page Down

    // Convert scancode to ASCII
    char c = 0;
    if (shift_pressed) {
        c = scancode_shift[scancode];
    } else {
        c = scancode_ascii[scancode];
    }

    if (c && callback) {
        callback(c);
    }
}

void keyboard_init(void) {
    callback = 0;
    shift_pressed = 0;
    extended = 0;
    irq_register_handler(1, keyboard_irq);
}

void keyboard_set_callback(keyboard_callback_t cb) {
    callback = cb;
}

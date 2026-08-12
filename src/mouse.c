#include "mouse.h"
#include "idt.h"
#include "io.h"

static mouse_callback_t callback = 0;

static int mouse_cycle = 0;
static int8_t mouse_byte[3];

static void mouse_irq(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;

    uint8_t data = inb(0x60);

    // 3-byte PS/2 mouse packet
    // Byte 0: buttons + sign bits + always-1 bit
    // Byte 1: X movement
    // Byte 2: Y movement

    switch (mouse_cycle) {
        case 0:
            mouse_byte[0] = data;
            if (data & 0x08) mouse_cycle++; // Bit 3 must be set
            break;
        case 1:
            mouse_byte[1] = data;
            mouse_cycle++;
            break;
        case 2:
            mouse_byte[2] = data;
            mouse_cycle = 0;

            if (callback) {
                int8_t dx = mouse_byte[1];
                int8_t dy = -mouse_byte[2]; // Y inverted for screen
                uint8_t buttons = mouse_byte[0] & 0x07;
                callback(dx, dy, buttons, 0); // No scroll in 3-byte mode
            }
            break;
    }
}

void mouse_init(void) {
    mouse_cycle = 0;
    callback = 0;

    // Enable auxiliary device (mouse)
    while (inb(0x64) & 0x02);
    outb(0x64, 0xA8);

    while (inb(0x64) & 0x02);
    outb(0x64, 0x20);

    while (inb(0x64) & 0x02);
    uint8_t status = inb(0x60);
    status |= 0x02;   // Enable IRQ12
    status &= ~0x20;  // Disable mouse clock

    while (inb(0x64) & 0x02);
    outb(0x64, 0x60);
    while (inb(0x64) & 0x02);
    outb(0x60, status);

    // Reset mouse
    while (inb(0x64) & 0x02);
    outb(0x64, 0xD4);
    while (inb(0x64) & 0x02);
    outb(0x60, 0xFF);
    while (inb(0x64) & 0x02);
    inb(0x60); // ack

    // Enable data reporting
    while (inb(0x64) & 0x02);
    outb(0x64, 0xD4);
    while (inb(0x64) & 0x02);
    outb(0x60, 0xF4);

    // Drain any pending bytes
    while (inb(0x64) & 0x01) inb(0x60);

    irq_register_handler(12, mouse_irq);
}

void mouse_set_callback(mouse_callback_t cb) {
    callback = cb;
}

#include "serial.h"
#include "io.h"

void serial_init(void) {
    outb(0x3F8 + 1, 0x00); // Disable interrupts
    outb(0x3F8 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(0x3F8 + 0, 0x03); // Set divisor to 3 (lo byte) -> 38400 baud
    outb(0x3F8 + 1, 0x00); //                  (hi byte)
    outb(0x3F8 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(0x3F8 + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(0x3F8 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

static int serial_transmit_empty(void) {
    return inb(0x3F8 + 5) & 0x20;
}

void serial_putchar(char c) {
    while (!serial_transmit_empty());
    outb(0x3F8, c);
}

void serial_puts(const char* str) {
    while (*str) {
        if (*str == '\n') serial_putchar('\r');
        serial_putchar(*str++);
    }
}

#include "serial.h"
#include "io.h"
#include <stdarg.h>

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

// Minimal printf-style formatter over the serial port. Supports a useful
// subset: %s %c %d %i %u %x %X and %%. Any flags/width/precision between
// '%' and the conversion are skipped (so "%04x" prints the value, no padding)
// — enough for kernel debug logging.
static void serial_putuint(uint32_t v, int base) {
    char buf[12];
    int i = 0;
    if (v == 0) { serial_putchar('0'); return; }
    while (v > 0) {
        int d = v % base;
        buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        v /= base;
    }
    while (i > 0) serial_putchar(buf[--i]);
}

static void serial_putint(int32_t v) {
    if (v < 0) { serial_putchar('-'); serial_putuint((uint32_t)(-v), 10); }
    else serial_putuint((uint32_t)v, 10);
}

void serial_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { serial_putchar(*p); continue; }
        p++;
        if (*p == 0) break;
        if (*p == '%') { serial_putchar('%'); continue; }
        // Skip flags, width, precision (e.g. "-", "0", "4", "." ...)
        while (*p && (*p == '-' || *p == '+' || *p == ' ' ||
               *p == '#' || *p == '.' || (*p >= '0' && *p <= '9'))) {
            p++;
        }
        if (*p == 0) break;
        char c = *p;
        if (c == 's') {
            const char* s = va_arg(ap, const char*);
            if (s) serial_puts(s);
        } else if (c == 'c') {
            serial_putchar((char)va_arg(ap, int));
        } else if (c == 'd' || c == 'i') {
            serial_putint(va_arg(ap, int));
        } else if (c == 'u') {
            serial_putuint(va_arg(ap, unsigned int), 10);
        } else if (c == 'x' || c == 'X') {
            serial_putuint(va_arg(ap, unsigned int), 16);
        } else {
            serial_putchar(c);
        }
    }
    va_end(ap);
}

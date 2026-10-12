#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

// Initialize serial port (COM1)
void serial_init(void);

// Copy of everything written (the last SERIAL_RING bytes; serial_total counts all of it), for the stick log (klog.c).
#define SERIAL_RING 131072
extern char serial_ring[SERIAL_RING];
extern volatile uint32_t serial_total;

// Write a character to serial port
void serial_putchar(char c);

// Write a string to serial port
void serial_puts(const char* str);

// Minimal printf over the serial port. Supports %s %c %d %i %u %x %X %%.
void serial_printf(const char* fmt, ...);

#endif

#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

// Initialize serial port (COM1)
void serial_init(void);

// Write a character to serial port
void serial_putchar(char c);

// Write a string to serial port
void serial_puts(const char* str);

#endif

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Keyboard callback type — called with the ASCII character
typedef void (*keyboard_callback_t)(char c);

// Initialize the keyboard driver
void keyboard_init(void);

// Register a callback for keypress events
void keyboard_set_callback(keyboard_callback_t callback);

// 1 while either Alt key is physically held (for Ctrl+Alt+T et al).
int keyboard_alt_held(void);

// 1 while a key is physically down: set-1 scancode (0x39 = space), extended_key = 1 for the 0xE0 keys (0x48 up arrow ...).
int keyboard_scancode_held(int scancode, int extended_key);

#endif

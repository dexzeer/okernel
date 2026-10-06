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

#endif

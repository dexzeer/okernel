#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Keyboard callback type — called with the ASCII character
typedef void (*keyboard_callback_t)(char c);

// Initialize the keyboard driver
void keyboard_init(void);

// Register a callback for keypress events
void keyboard_set_callback(keyboard_callback_t callback);
// Keys without a character. With this callback set they arrive here (from the IRQ) instead of as the old scroll codes.
#define KEY_EXT_UP 1
#define KEY_EXT_DOWN 2
#define KEY_EXT_RIGHT 3
#define KEY_EXT_LEFT 4
#define KEY_EXT_HOME 5
#define KEY_EXT_END 6
#define KEY_EXT_INSERT 7
#define KEY_EXT_DELETE 8
#define KEY_EXT_PGUP 9
#define KEY_EXT_PGDN 10
#define KEY_EXT_ESC 11                       // not an extended scancode, but no window but a terminal wants it
void keyboard_set_ext_callback(void (*callback)(int code));

// 1 while either Alt key is physically held (for Ctrl+Alt+T et al).
int keyboard_alt_held(void);

// 1 while a key is physically down: set-1 scancode (0x39 = space), extended_key = 1 for the 0xE0 keys (0x48 up arrow ...).
int keyboard_scancode_held(int scancode, int extended_key);
// Every key press and release since the last call, in order (the IRQ queues them; polling held state loses a tap shorter than a
// main-loop pass). 1 = an event was returned. Only one consumer: the /dev/okwin input (src/lx_okwin.c).
int keyboard_next_transition(int* scancode, int* extended_key, int* down);

#endif

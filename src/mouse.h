#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

// Mouse callback: dx, dy, buttons, scroll wheel delta
typedef void (*mouse_callback_t)(int dx, int dy, uint8_t buttons, int scroll);

void mouse_init(void);
void mouse_set_callback(mouse_callback_t cb);

#endif

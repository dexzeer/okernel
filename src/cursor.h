#ifndef CURSOR_H
#define CURSOR_H

// Mouse driver + framebuffer-composited cursor (cursor.c). The sprite is
// never in the backbuffer; see the header comment in cursor.c.

void mouse_init_fb(void);            // PS/2 init + sprite build + IRQ12
void cursor_enable(void);            // show the sprite (after the first full flush)
// graphics_flush() hook: redraw the sprite over freshly copied FB rows
// [y0, y1). Caller has interrupts disabled.
void cursor_fb_repair(int y0, int y1);

void mouse_get_position(int* x, int* y);
int  mouse_get_x(void);
int  mouse_get_y(void);
int  mouse_get_left_button(void);
int  mouse_get_scroll(void);         // wheel delta since the last call

#endif

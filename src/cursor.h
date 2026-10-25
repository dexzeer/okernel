#ifndef CURSOR_H
#define CURSOR_H

// Mouse driver + framebuffer-composited cursor (cursor.c). The sprite is
// never in the backbuffer; see the header comment in cursor.c.

void mouse_init_fb(void);            // PS/2 init + sprite build + IRQ12
void cursor_enable(void);            // show the sprite (after the first full flush)
struct display_ops;
void cursor_set_display(const struct display_ops* d);   // use its hardware cursor if it has one
// graphics_flush() hook: redraw the sprite over freshly copied FB rows
// [y0, y1). Caller has interrupts disabled.
void cursor_fb_repair(int y0, int y1);

// Bounding box of the sprite currently on the framebuffer ([x0,x1) x [y0,y1));
// 0 when it is not shown. Used by the display self check.
int  cursor_rect(int* x0, int* y0, int* x1, int* y1);

// Pointer motion from a non-PS/2 driver (USB mouse): dx/dy in screen pixels (positive = right/down), wheel positive =
// down/toward the tail like the PS/2 path, buttons bit0 left, bit1 right, bit2 middle. Safe from the main loop.
void mouse_inject(int dx, int dy, int wheel, int buttons);

void mouse_get_position(int* x, int* y);
int  mouse_get_x(void);
int  mouse_get_y(void);
int  mouse_get_left_button(void);
int  mouse_get_buttons(void);        // bit 0 left, 1 right, 2 middle
int  mouse_get_scroll(void);         // wheel delta since the last call

#endif

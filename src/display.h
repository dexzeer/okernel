#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

// A display backend turns the backbuffer (0x00RRGGBB, SCREEN_W x SCREEN_H) into
// pixels on a screen. Backends: the plain framebuffer GRUB gave us (graphics.c,
// the fallback that works everywhere), virtio-gpu (vgpu.c) and the Intel
// integrated GPU (i915.c). graphics_flush() computes the damaged rectangles
// and hands them to the active backend.

struct display_rect { int x, y, w, h; };

struct display_ops {
    const char* name;
    // Show the damaged rectangles of the backbuffer. Thread context.
    void (*present)(const struct display_rect* r, int n);
    // Hardware cursor (optional; NULL = cursor.c composites a software cursor on
    // the framebuffer). The image is straight-alpha 0xAARRGGBB, w x h <= 64x64.
    int  (*cursor_define)(const uint32_t* argb, int w, int h, int hot_x, int hot_y);
    // IRQ context (IF=0): must not block or allocate. x,y = pointer position.
    void (*cursor_move)(int x, int y);
    void (*cursor_show)(int on);
};

// Probe backends in order (virtio-gpu, i915); returns the first that works or
// NULL (keep using the GRUB framebuffer). `backbuffer` is the 32bpp scene.
const struct display_ops* vgpu_probe(uint32_t* backbuffer, int w, int h);
const struct display_ops* i915_probe(uint32_t* backbuffer, int w, int h);

#endif

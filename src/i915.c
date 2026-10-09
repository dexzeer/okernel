// Intel integrated graphics display backend (filled in by the i915 driver step).
#include "display.h"

const struct display_ops* i915_probe(uint32_t* backbuffer, int w, int h) {
    (void)backbuffer; (void)w; (void)h;
    return 0;
}

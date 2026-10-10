// The text-mode kernel runs without paging (physical = linear) and has no graphics layer: the
// pieces of the desktop kernel that i915.c (used for the read-only `gpu` report) refers to.
#include <stdint.h>

void* paging_map_mmio(uint32_t phys_addr, uint32_t len) { (void)len; return (void*)phys_addr; }
uint8_t* graphics_fb_base(void) { return 0; }

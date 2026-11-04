// The text-mode kernel stays on the boot page tables (the first 4 GB identity-mapped) and has no graphics layer: the
// pieces of the desktop kernel that i915.c (used for the read-only `gpu` report) refers to.
#include <stdint.h>

void* paging_map_mmio(uint64_t phys_addr, uint64_t len) { (void)len; return phys_addr >> 32 ? 0 : (void*)(uintptr_t)phys_addr; }
uint8_t* graphics_fb_base(void) { return 0; }
uint64_t graphics_fb_phys(void) { return 0; }

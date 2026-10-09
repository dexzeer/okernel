#ifndef I915_H
#define I915_H

#include <stdint.h>

// Intel integrated graphics (display controller 8086:xxxx, PCI class 3).
//
// Status: groundwork. Boot-time probing only reads PCI configuration space
// (identity, BARs, GMCH/stolen-memory registers) and maps BAR0 - no register is
// touched, so the GRUB framebuffer keeps working exactly as before. The shell
// command `gpu` prints a read-only report of the display engine (pipes, planes,
// cursor, where the scanout buffer lives in the GTT) so the next steps can be
// written against what the real machine does. Nothing here changes the display.

struct i915_gen {
    uint8_t gen;            // 2..12, 0 = unknown
    const char* name;       // platform / codename ("Skylake", ...)
};

// Classify a device id (pure function; the only part tested on the host).
struct i915_gen i915_classify(uint16_t device_id);

// Decode helpers for the report (pure functions, host-tested).
const char* i915_plane_format(uint8_t gen, uint32_t plane_ctl);
int i915_plane_enabled(uint8_t gen, uint32_t plane_ctl);
int i915_plane_tiling(uint8_t gen, uint32_t plane_ctl);        // 0 linear, 1 X, 2 Y, 3 other
// GTT page-table entry -> physical page address (bits 31:12; gen8+ also bits 38:32 in the high dword).
uint64_t i915_pte_phys(uint8_t gen, uint32_t lo, uint32_t hi);
int i915_pte_valid(uint8_t gen, uint32_t lo);

#ifdef KERNEL
// Print the report through `out`. Safe to call at any time; returns 0 when no
// Intel display device was found.
int i915_report(void (*out)(const char* line, void* ud), void* ud);
#endif

#endif

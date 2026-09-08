#ifndef MEMLAYOUT_H
#define MEMLAYOUT_H

#include <stdint.h>

// High-half kernel layout: desktop kernel links at 0xC0100000 (phys 1MB),
// so kernel virtual = phys + KERNEL_VBASE. User space is 0x00000000 to
// 0xBFFFFFFF, private per process. Low identity 0-128M stays mapped
// (supervisor, transitional) plus FB/MMIO supervisor entries; per-process
// user isolation (clearing low PD 1-767) is Priority 3.
#define KERNEL_VBASE 0xC0000000u
#define KERNEL_PHYS_BASE 0x00100000u
#define V2P(v) ((uint32_t)(v) - KERNEL_VBASE)
// P2V forms: plain P2V(p) for pointers, P2V_U32(p) + off when the result
// feeds address math (keeps objdump readable; never wrap arithmetic inside
// P2V — write P2V_U32(base) + off, not P2V(base + off)).
#define P2V_U32(p) ((uint32_t)(p) + KERNEL_VBASE)
#define P2V(p) ((void*)(P2V_U32(p)))
#define IS_HIGH(v) ((uint32_t)(v) >= KERNEL_VBASE)

// PD indices
#define PD_KERNEL_BASE 768   // 0xC0000000 >> 22
#define PD_LOW_IDENT 0       // 0x00000000: 0-1M DMA/VGA window

#endif

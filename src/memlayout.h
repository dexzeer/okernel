#ifndef MEMLAYOUT_H
#define MEMLAYOUT_H

#include <stdint.h>

// The kernel's address space:
//   0x0000000000000000 .. 0x00007FFFFFFFEFFF  user space, private per process. In the kernel's own tree the first 128 MB are an identity map.
//   0xFFFF800000000000 ..                     the direct map: all RAM, phys + DIRECT_BASE (up to 512 GB). Page tables live here, and the heap.
//   0xFFFFC00000000000 ..                     device registers and the framebuffer (paging_map_mmio), shared by every address space, supervisor only.
//   0xFFFFFFFF80000000 ..                     the kernel image, phys + KERNEL_VBASE (-mcmodel=kernel reaches only the top 2 GB).
// Physical addresses are 64-bit. The heap and the kernel image sit below 4 GB, so a kernel buffer is always reachable by a device with 32-bit
// DMA; pages from pmm_alloc_page (user memory, page tables) can be anywhere and must not be handed to a device.
typedef uint64_t phys_t;

#define USER_SPACE_TOP   0x00007FFFFFFFF000ull
#define PROC_STACK_TOP   USER_SPACE_TOP            // the kernel's own ring-3 programs: their stack page ends here
#define KERNEL_PHYS_BASE 0x00100000u
#define KERNEL_VBASE     0xFFFFFFFF80000000ull
#define DIRECT_BASE      0xFFFF800000000000ull
#define MMIO_BASE        0xFFFFC00000000000ull

static inline phys_t v2p_addr(uintptr_t v) {
    if (v >= KERNEL_VBASE) return v - KERNEL_VBASE;
    if (v >= MMIO_BASE) return 0;                   // device mappings have no meaning as a physical address here
    if (v >= DIRECT_BASE) return v - DIRECT_BASE;
    return v;                                       // identity-mapped low memory
}
#define V2P(v) v2p_addr((uintptr_t)(v))
#define P2V_ADDR(p) ((uintptr_t)(phys_t)(p) + DIRECT_BASE)
#define P2V(p) ((void*)P2V_ADDR(p))
#define IS_HIGH(v) ((uintptr_t)(v) >= DIRECT_BASE)

#endif

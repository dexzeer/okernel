#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Set up the high-half kernel map (kernel high at 0xC0000000, user low
// private) plus framebuffer/MMIO supervisor mappings
void paging_init(uint32_t framebuffer_addr);

// Map a physical address into the kernel high MMIO window (returns HIGH
// virtual address, never phys)
void* paging_map(uint32_t phys_addr);

// Low 0-1M DMA/VGA window: identity supervisor, virt == phys
void* paging_map_low(uint32_t phys_addr);

// Kernel page-directory PHYS address (CR3 value), for sharing PD 768-1023
uint32_t paging_kernel_pd_phys(void);

// Map a PHYS page at a user-low virtual address (ring 3 read/write).
// virt must be < 0xC0000000; phys is the physical page address.
void paging_map_user(uint32_t virt_addr, uint32_t phys_addr);

// Same, but into an explicit page directory (PHYS address of PD).
// Used to build a process address space before switching to it.
void paging_map_user_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t phys_addr);

// Check a user buffer range is fully mapped user-accessible in the CURRENT
// address space (CR3). Used to validate syscall pointers from ring 3.
int paging_user_range_valid(uint32_t virt_addr, uint32_t len);

#endif

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Set up the high-half kernel map (kernel high at 0xC0000000, user low
// private) plus framebuffer/MMIO supervisor mappings
void paging_init(uint32_t framebuffer_addr, uint32_t fb_bytes);

// Program the page attribute table (entry 1 = write-combining) on the calling
// CPU. Returns 0 on CPUs without a PAT. Every CPU must call it (smp.c does).
int pat_init(void);
int pat_enabled(void);

// Map a physical address into the kernel high MMIO window (returns HIGH
// virtual address, never phys)
void* paging_map(uint32_t phys_addr);

// Uncached mapping of device registers (len bytes at phys), contiguous in
// virtual memory. Boot-time only: process page directories copy the kernel half
// when they are created.
void* paging_map_mmio(uint32_t phys_addr, uint32_t len);
void* paging_probe_window(uint32_t phys4mb);   // scratch 4MB uncached window for boot-time scans; 0 unmaps

// Low 0-1M DMA/VGA window: identity supervisor, virt == phys
void* paging_map_low(uint32_t phys_addr);

// The kernel's PML4 PHYS address (CR3 value): pid 0, boot, application processors
uint32_t paging_kernel_pd_phys(void);

// Address spaces (4-level tables; user half 0..3 GB private, kernel half shared). `cr3` is the PML4's PHYS address.
uint32_t aspace_create(void);
uint64_t* aspace_pte(uint32_t cr3, uint32_t va, int alloc);   // the 4 KB entry for va (0: absent / not allocatable)
int aspace_map(uint32_t cr3, uint32_t va, uint32_t phys, int writable);   // 0 ok, -1 out of memory
int aspace_set_writable(uint32_t cr3, uint32_t va, int writable);
int aspace_present(uint32_t cr3, uint32_t va);
uint32_t aspace_unmap(uint32_t cr3, uint32_t va);               // phys of the page that was mapped (0: none); caller frees it
int aspace_walk_user(uint32_t cr3, int (*fn)(uint32_t va, uint32_t phys, void* ctx), void* ctx);
void aspace_free_user(uint32_t cr3);                            // frees user pages + user tables; reload CR3 if running
void aspace_destroy(uint32_t cr3);                              // everything, including the PML4
int paging_va_mapped(uint32_t va);                              // readable now? (panic screen: no allocation)

// Map a PHYS page at a user-low virtual address (ring 3 read/write).
// virt must be < 0xC0000000; phys is the physical page address.
void paging_map_user(uint32_t virt_addr, uint32_t phys_addr);

// Same, but into an explicit address space (PHYS address of its PML4).
// Used to build a process address space before switching to it.
void paging_map_user_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t phys_addr);

// Check a user buffer range is fully mapped user-accessible in the CURRENT
// address space (CR3). Used to validate syscall pointers from ring 3.
int paging_user_range_valid(uint32_t virt_addr, uint32_t len);

#endif

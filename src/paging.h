#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "memlayout.h"

// Set up the kernel's page tables (memlayout.h) and map the framebuffer.
void paging_init(phys_t framebuffer_addr, uint64_t fb_bytes);

// Program the page attribute table (entry 1 = write-combining) on the calling
// CPU. Returns 0 on CPUs without a PAT. Every CPU must call it (smp.c does).
int pat_init(void);
int pat_enabled(void);

// Where paging_init mapped the framebuffer (0: none), and its physical address.
uint8_t* paging_fb_ptr(void);
phys_t paging_fb_phys(void);

// Cached mapping of the 4 MB block around a physical address (RAM-like: ACPI tables); returns the address of phys_addr itself.
void* paging_map(phys_t phys_addr);

// Uncached mapping of `len` bytes of device registers, contiguous in virtual memory. 0 when the pool is exhausted.
void* paging_map_mmio(phys_t phys_addr, uint64_t len);
void* paging_probe_window(phys_t phys4mb);   // scratch 4MB uncached window for scans; 0 unmaps

// Low 0-1M DMA/VGA window: identity supervisor, virt == phys
void* paging_map_low(uint32_t phys_addr);

// The kernel's PML4 PHYS address (CR3 value): pid 0, boot, application processors. Always below 4 GB (the image is).
phys_t paging_kernel_pd_phys(void);

// Address spaces (4-level tables; user half private, kernel half shared). `cr3` is the PML4's PHYS address.
phys_t aspace_create(void);
uint64_t* aspace_pte(phys_t cr3, uint64_t va, int alloc);   // the 4 KB entry for va (0: absent / not allocatable)
int aspace_map(phys_t cr3, uint64_t va, phys_t phys, int writable);   // 0 ok, -1 out of memory
int aspace_set_writable(phys_t cr3, uint64_t va, int writable);
int aspace_present(phys_t cr3, uint64_t va);
phys_t aspace_unmap(phys_t cr3, uint64_t va);               // phys of the page that was mapped (0: none); caller frees it
int aspace_walk_user(phys_t cr3, int (*fn)(uint64_t va, phys_t phys, void* ctx), void* ctx);
void aspace_free_user(phys_t cr3);                          // frees user pages + user tables; reload CR3 if running
void aspace_destroy(phys_t cr3);                            // everything, including the PML4
int paging_va_mapped(uintptr_t va);                         // readable now? (panic screen: no allocation)
#define PTE_ADDR(e) ((phys_t)(e) & 0x000FFFFFFFFFF000ull)

// Map a PHYS page at a user virtual address (ring 3 read/write) in the kernel's own tree.
void paging_map_user(uint64_t virt_addr, phys_t phys_addr);

// Same, but into an explicit address space. Used to build a process address space before switching to it.
void paging_map_user_pd(phys_t pd_phys, uint64_t virt_addr, phys_t phys_addr);

// Check a user buffer range is fully mapped user-accessible in the CURRENT
// address space (CR3). Used to validate syscall pointers from ring 3.
int paging_user_range_valid(uint64_t virt_addr, uint64_t len);

#endif

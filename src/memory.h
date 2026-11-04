#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include "memlayout.h"

// Initialize memory manager with multiboot info
void memory_init(uint32_t mboot_addr);
// Boot module `idx` (GRUB `module` line) through the high alias; len and cmdline are optional. 0 = none.
const uint8_t* memory_boot_module(int idx, uint32_t* len, const char** cmdline);

// Physical memory manager
// A page anywhere in RAM (above 4 GB first, the low pages stay for the heap and devices). Not for DMA: kernel buffers from the heap are what
// devices get. pmm_alloc_page_low: a page below 4 GB. Both return 0 when out of memory.
phys_t pmm_alloc_page(void);
phys_t pmm_alloc_page_low(void);
void pmm_free_page(phys_t addr);
// Usable RAM region i of the bootloader's map; 0 past the last one.
int pmm_region(int i, uint64_t* base, uint64_t* end);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

// Kernel heap (segregated fit, coalescing, 16-byte aligned, IRQ-safe).
// kfree/krealloc validate the block; bad or double frees are logged and
// ignored.
// Sizes are size_t so that an absurd request fails: blocks are limited to less than 4 GB (a size truncated to 32 bits on the way here would
// succeed with a small block).
void* kmalloc(size_t size);
void* kcalloc(size_t n, size_t size);
void* krealloc(void* ptr, size_t size);
void kfree(void* ptr);
uint32_t ksize(void* ptr);   // usable bytes of a live block (0 if invalid)
void heap_stats(uint32_t* total, uint32_t* used, uint32_t* peak, uint32_t* largest_free);
int  heap_pretouch(uint32_t budget_cycles);   // idle loop: back the heap pages with RAM ahead of the allocator (see memory.c); 1 = more to do

#endif

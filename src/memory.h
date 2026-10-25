#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

// Initialize memory manager with multiboot info
void memory_init(uint32_t mboot_addr);
// Boot module `idx` (GRUB `module` line) through the high alias; len and cmdline are optional. 0 = none.
const uint8_t* memory_boot_module(int idx, uint32_t* len, const char** cmdline);

// Physical memory manager
uint32_t pmm_alloc_page(void);
void pmm_free_page(uint32_t addr);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

// Kernel heap (segregated fit, coalescing, 16-byte aligned, IRQ-safe).
// kfree/krealloc validate the block; bad or double frees are logged and
// ignored.
void* kmalloc(uint32_t size);
void* kcalloc(uint32_t n, uint32_t size);
void* krealloc(void* ptr, uint32_t size);
void kfree(void* ptr);
uint32_t ksize(void* ptr);   // usable bytes of a live block (0 if invalid)
void heap_stats(uint32_t* total, uint32_t* used, uint32_t* peak, uint32_t* largest_free);
int  heap_pretouch(uint32_t budget_cycles);   // idle loop: back the heap pages with RAM ahead of the allocator (see memory.c); 1 = more to do

#endif

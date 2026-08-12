#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

// Initialize memory manager with multiboot info
void memory_init(uint32_t mboot_addr);

// Physical memory manager
uint32_t pmm_alloc_page(void);
void pmm_free_page(uint32_t addr);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

// Simple kernel heap allocator
void* kmalloc(uint32_t size);
void kfree(void* ptr);

#endif

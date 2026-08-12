#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

// Initialize memory manager with multiboot info
void memory_init(uint32_t mboot_addr);

// Allocate a physical page (returns physical address, 0 on failure)
uint32_t pmm_alloc_page(void);

// Free a physical page
void pmm_free_page(uint32_t addr);

// Get memory stats
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

#endif

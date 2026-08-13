#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Set up identity-mapped paging for the first 4MB
// and map the framebuffer at the given address
void paging_init(uint32_t framebuffer_addr);

// Map a physical address to a virtual address (returns virtual address)
// Uses a new page table for the given 4MB region
void* paging_map(uint32_t phys_addr);

#endif

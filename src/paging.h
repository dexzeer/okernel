#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Set up identity-mapped paging for the first 4MB
// and map the framebuffer at the given address
void paging_init(uint32_t framebuffer_addr);

#endif

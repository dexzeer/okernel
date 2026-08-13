#include "paging.h"
#include "serial.h"
#include <stdint.h>

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_table[1024] __attribute__((aligned(4096)));
static uint32_t extra_page_tables[4][1024] __attribute__((aligned(4096)));
static int extra_pt_count = 0;

void paging_init(uint32_t framebuffer_addr) {
    // Zero everything
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0;
        page_table[i] = 0;
    }
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 1024; i++)
            extra_page_tables[t][i] = 0;

    // Identity map first 4MB (kernel, BSS, stack, VGA at 0xA0000, low memory buffers)
    for (int i = 0; i < 1024; i++) {
        page_table[i] = (i * 0x1000) | 0x03;
    }
    page_directory[0] = ((uint32_t)page_table) | 0x03;

    // Map framebuffer
    if (framebuffer_addr) {
        paging_map(framebuffer_addr);
    }

    // Load and enable
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

void* paging_map(uint32_t phys_addr) {
    uint32_t pd_index = phys_addr >> 22;
    uint32_t base = phys_addr & 0xFFC00000; // Align to 4MB

    // Use an extra page table
    if (extra_pt_count >= 4) return 0;
    uint32_t* pt = extra_page_tables[extra_pt_count];

    for (int i = 0; i < 1024; i++) {
        pt[i] = (base + i * 0x1000) | 0x03;
    }
    page_directory[pd_index] = ((uint32_t)pt) | 0x03;
    extra_pt_count++;

    // Flush TLB
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));

    return (void*)phys_addr;
}

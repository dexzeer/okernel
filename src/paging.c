#include "paging.h"
#include <stdint.h>

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_table[1024] __attribute__((aligned(4096)));
static uint32_t fb_page_table[1024] __attribute__((aligned(4096)));

void paging_init(uint32_t framebuffer_addr) {
    // Zero everything
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0;
        page_table[i] = 0;
        fb_page_table[i] = 0;
    }

    // Identity map first 4MB (kernel, BSS, stack, VGA at 0xA0000)
    for (int i = 0; i < 1024; i++) {
        page_table[i] = (i * 0x1000) | 0x03;
    }
    page_directory[0] = ((uint32_t)page_table) | 0x03;

    // Map framebuffer (typically at 0xFD000000)
    // Calculate which 4MB chunk it's in
    uint32_t pd_index = framebuffer_addr >> 22;
    uint32_t base = framebuffer_addr & 0xFFC00000; // Align to 4MB

    for (int i = 0; i < 1024; i++) {
        fb_page_table[i] = (base + i * 0x1000) | 0x03;
    }
    page_directory[pd_index] = ((uint32_t)fb_page_table) | 0x03;

    // Load and enable
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

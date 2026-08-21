#include "paging.h"
#include "serial.h"
#include <stdint.h>

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
// 32 tables for low memory (0–128MB), up to 3 for framebuffer (1080p LFB is
// ~8.3MB), 1 for e1000 MMIO
static uint32_t page_tables[40][1024] __attribute__((aligned(4096)));
static int pt_count = 0;

static void map_4mb(uint32_t base_addr) {
    uint32_t pd_index = base_addr >> 22;
    if (page_directory[pd_index] & 0x01) return;
    if (pt_count >= 40) return;

    uint32_t* pt = page_tables[pt_count];
    for (int i = 0; i < 1024; i++) {
        pt[i] = (base_addr + i * 0x1000) | 0x03;
    }
    page_directory[pd_index] = ((uint32_t)pt) | 0x03;
    pt_count++;
}

void paging_init(uint32_t framebuffer_addr) {
    for (int i = 0; i < 1024; i++) page_directory[i] = 0;

    // Identity map first 4MB
    for (int i = 0; i < 1024; i++) {
        page_tables[0][i] = (i * 0x1000) | 0x03;
    }
    page_directory[0] = ((uint32_t)page_tables[0]) | 0x03;
    pt_count = 1;

    // Identity map 4MB–128MB
    for (uint32_t addr = 0x00400000; addr < 0x08000000; addr += 0x00400000) {
        map_4mb(addr);
    }

    // Map framebuffer (typically 0xFD000000). A 1920x1080x32bpp LFB is
    // ~8.3MB, so map three consecutive 4MB windows to cover it regardless of
    // alignment (two windows would be 8MB, one short).
    if (framebuffer_addr) {
        uint32_t fb_base = framebuffer_addr & 0xFFC00000;
        for (int i = 0; i < 3; i++) map_4mb(fb_base + i * 0x00400000);
    }

    // Map e1000 MMIO (typically 0xFEB80000)
    // The e1000 driver calls paging_map() after init, but we also map
    // the high MMIO area here to be safe
    map_4mb(0xFEB80000 & 0xFFC00000); // 0xFE000000

    // Enable paging
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

void* paging_map(uint32_t phys_addr) {
    map_4mb(phys_addr & 0xFFC00000);
    return (void*)phys_addr;
}

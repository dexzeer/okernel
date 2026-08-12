#include "memory.h"
#include "serial.h"
#include <stdint.h>

// Multiboot header flags
#define MULTIBOOT_MAGIC 0x2BADB002
#define MULTIBOOT_FLAG_MEM (1 << 1)

// Multiboot info structure (partial)
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    // ... more fields we don't need yet
};

// Bitmap for physical memory manager
// Each bit represents one 4KB page
// 0 = free, 1 = used
#define PAGE_SIZE 4096
#define BITMAP_SIZE (128 * 1024 / 8) // Support up to 128MB of RAM
static uint8_t bitmap[BITMAP_SIZE];

static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

static void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

void memory_init(uint32_t mboot_addr) {
    struct multiboot_info* mboot = (struct multiboot_info*)mboot_addr;

    // Clear bitmap (all pages marked as used by default)
    for (int i = 0; i < BITMAP_SIZE; i++) {
        bitmap[i] = 0xFF;
    }

    if (!(mboot->flags & MULTIBOOT_FLAG_MEM)) {
        serial_puts("[mem] no memory info from multiboot\n");
        // Assume 1MB-16MB if no info
        mboot->mem_lower = 640;   // KB below 1MB
        mboot->mem_upper = 15 * 1024; // KB above 1MB
    }

    // Total memory = lower (< 1MB) + upper (> 1MB)
    // We only manage upper memory (> 1MB) for simplicity
    uint32_t mem_upper_kb = mboot->mem_upper;
    uint32_t mem_upper_bytes = mem_upper_kb * 1024;
    uint32_t mem_upper_start = 1024 * 1024; // 1MB

    total_pages = mem_upper_bytes / PAGE_SIZE;

    // Cap to bitmap size
    if (total_pages > BITMAP_SIZE * 8) {
        total_pages = BITMAP_SIZE * 8;
    }

    serial_puts("[mem] upper memory: ");
    // Print in decimal
    uint32_t mb = mem_upper_bytes / (1024 * 1024);
    serial_putchar('0' + (mb / 100));
    serial_putchar('0' + ((mb / 10) % 10));
    serial_putchar('0' + (mb % 10));
    serial_puts(" MB\n");

    // Mark pages below 1MB as used (BIOS, IVT, BDA, etc.)
    uint32_t reserved_pages = 1024 * 1024 / PAGE_SIZE; // 256 pages
    for (uint32_t i = 0; i < reserved_pages && i < total_pages; i++) {
        bitmap_set(i);
    }

    // Mark pages used by kernel (from 1MB to end of kernel)
    extern uint32_t _kernel_start;
    extern uint32_t _kernel_end;
    uint32_t kernel_start = (uint32_t)&_kernel_start;
    uint32_t kernel_end = (uint32_t)&_kernel_end;

    // Align to page boundaries
    kernel_start = kernel_start & ~(PAGE_SIZE - 1);
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint32_t kernel_start_page = kernel_start / PAGE_SIZE;
    uint32_t kernel_end_page = kernel_end / PAGE_SIZE;

    for (uint32_t i = kernel_start_page; i < kernel_end_page && i < total_pages; i++) {
        bitmap_set(i);
    }

    // Mark all pages ABOVE the kernel as FREE
    for (uint32_t i = kernel_end_page; i < total_pages; i++) {
        bitmap_clear(i);
    }

    used_pages = 0;
    for (uint32_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) used_pages++;
    }

    serial_puts("[mem] total pages: ");
    // Print total_pages in decimal
    char buf[12];
    int idx = 0;
    uint32_t tmp = total_pages;
    if (tmp == 0) { buf[idx++] = '0'; }
    else {
        char rev[12];
        int ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
        while (ri > 0) { buf[idx++] = rev[--ri]; }
    }
    buf[idx] = 0;
    serial_puts(buf);
    serial_puts(", used: ");
    tmp = used_pages; idx = 0;
    if (tmp == 0) { buf[idx++] = '0'; }
    else {
        char rev[12];
        int ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
        while (ri > 0) { buf[idx++] = rev[--ri]; }
    }
    buf[idx] = 0;
    serial_puts(buf);
    serial_puts("\n");
}

uint32_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            return i * PAGE_SIZE;
        }
    }
    return 0; // Out of memory
}

void pmm_free_page(uint32_t addr) {
    uint32_t page = addr / PAGE_SIZE;
    if (page < total_pages && bitmap_test(page)) {
        bitmap_clear(page);
        used_pages--;
    }
}

uint32_t pmm_get_total_pages(void) { return total_pages; }
uint32_t pmm_get_used_pages(void) { return used_pages; }
uint32_t pmm_get_free_pages(void) { return total_pages - used_pages; }

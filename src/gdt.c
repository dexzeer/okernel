#include "gdt.h"
#include "io.h"
#include "serial.h"

// GDT entry structure
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// GDT pointer
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

// 6 GDT entries: null/code/data/ucode/udata/TSS-lo (8-byte 32-bit TSS desc).
// NOTE (high-half): gdt[]/gp/kernel_tss are HIGH-linked .bss. gdt_flush(&gp)
// and ltr run AFTER the trampoline enables paging (gdt_init is called from
// high kernel_main), so lgdt/ltr read valid high mappings. Do NOT call
// gdt_init pre-paging.
// (2026-09-07: an earlier struct tss_entry was 16 bytes (64-bit form) and
// gp.limit overshot by 16 — ltr survived by luck but traps risked #DE/#GP
// at LOW EIP. Now exact: 6x8 = 48 bytes.)
static struct gdt_entry gdt[6];  // null/code/data/ucode/udata/TSS-lo
static struct gdt_ptr gp;

// The actual TSS
static struct tss kernel_tss;

// Assembly function to load the GDT and reload segments
extern void gdt_flush(uint32_t);

static void gdt_set_entry(int num, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t granularity) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity  = ((limit >> 16) & 0x0F);
    gdt[num].granularity |= (granularity & 0xF0);
    gdt[num].access = access;
}

static void tss_set_entry(int num, uint32_t base, uint32_t limit) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity  = 0x00;
    // Access: present=1, DPL=3, type=available 32-bit TSS (0x89)
    gdt[num].access = 0x89;  // 1_11_0_1001 = present, DPL=3, 0, available TSS
}

void gdt_init(void) {
    // GDT = 6 entries exactly (null/code/data/ucode/udata/TSS-lo): the TSS
    // descriptor is 8 bytes on 32-bit (NOT 16 — that is the 64-bit form).
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)&gdt;

    // Null segment
    gdt_set_entry(0, 0, 0, 0, 0);

    // Kernel code segment (selector 0x08, DPL=0, ring 0)
    // Access: present=1, DPL=0, S=1, type=execute/read (0x9A)
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // Kernel data segment (selector 0x10, DPL=0, ring 0)
    // Access: present=1, DPL=0, S=1, type=read/write (0x92)
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // User code segment (selector 0x18, DPL=3, ring 3)
    // Access: present=1, DPL=3, S=1, type=execute/read (0x9A)
    // DPL bits are in bits 5-6 of access byte: 0x9A | (3 << 5) = 0xFA
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    // User data segment (selector 0x20, DPL=3, ring 3)
    // Access: present=1, DPL=3, S=1, type=read/write (0x92)
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    // TSS descriptor (selector 0x28)
    tss_set_entry(5, (uint32_t)&kernel_tss, sizeof(struct tss) - 1);

    // Initialize TSS
    kernel_tss.prev_tss = 0;
    kernel_tss.ss0 = GDT_KERNEL_DATA;
    kernel_tss.esp0 = 0; // Will be set before entering user mode
    kernel_tss.cs = GDT_KERNEL_CODE;
    kernel_tss.ds = GDT_KERNEL_DATA;
    kernel_tss.es = GDT_KERNEL_DATA;
    kernel_tss.fs = GDT_KERNEL_DATA;
    kernel_tss.gs = GDT_KERNEL_DATA;
    kernel_tss.ss = GDT_KERNEL_DATA;
    kernel_tss.iomap_base = 0xFFFF; // No I/O permission bitmap

    // Load GDT
    gdt_flush((uint32_t)&gp);

    // Load TSS
    uint16_t tss_sel = GDT_TSS; __asm__ volatile("ltr %0" : : "r"(tss_sel));

    serial_puts("[gdt] GDT + TSS initialized (user segments at 0x18/0x20)\n");
}

void tss_set_kernel_stack(uint32_t esp0) {
    kernel_tss.esp0 = esp0;
}

void tss_get_esp0(uint32_t *esp0, uint32_t *ss0) {
    if (esp0) *esp0 = kernel_tss.esp0;
    if (ss0) *ss0 = kernel_tss.ss0;
}

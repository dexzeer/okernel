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

// GDT pointer. The CPU runs in long mode (the kernel itself in compatibility mode): the pseudo-descriptor is 10 bytes,
// a 16-bit limit and a 64-bit base. Read as the 6-byte form it is the same limit and the low 32 bits of the base.
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
    uint32_t base_hi;
} __attribute__((packed));

// 0x00 null, 0x08 kernel code (32-bit), 0x10 kernel data, 0x18 user code (32-bit), 0x20 user data, 0x28 user code
// (64-bit), 0x30 kernel code (64-bit), 0x38 kernel data (for SYSCALL's SS), 0x40 TSS (16 bytes). The order is
// dictated by SYSRET: with STAR[63:48] = 0x18 it loads 32-bit CS = 0x18|3, 64-bit CS = 0x28|3, SS = 0x20|3.
// gdt[]/gp/kernel_tss are HIGH-linked .bss; gdt_flush(&gp) and ltr run AFTER the trampoline enables paging
// (gdt_init is called from high kernel_main). Do NOT call gdt_init pre-paging.
static struct gdt_entry gdt[10];
static struct gdt_ptr gp;

// The actual TSS (64-bit format: the CPU reads rsp0 on every ring-3 -> ring-0 interrupt)
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

// 16-byte system descriptor in slots num and num+1: available 64-bit TSS, DPL 0, base above 4 GB zero.
static void tss_set_entry(int num, uint32_t base, uint32_t limit) {
    gdt_set_entry(num, base, limit, 0x89, 0x00);
    gdt[num + 1].limit_low = 0;              // base[63:32]
    gdt[num + 1].base_low = 0;
    gdt[num + 1].base_middle = 0;
    gdt[num + 1].access = 0;
    gdt[num + 1].granularity = 0;
    gdt[num + 1].base_high = 0;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)&gdt;
    gp.base_hi = 0;

    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);     // 0x08 kernel code, 32-bit (compatibility mode), DPL 0
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);     // 0x10 kernel data
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);     // 0x18 user code, 32-bit, DPL 3
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);     // 0x20 user data, DPL 3
    gdt_set_entry(5, 0, 0xFFFFFFFF, 0xFA, 0xAF);     // 0x28 user code, 64-bit (L=1), DPL 3
    gdt_set_entry(6, 0, 0xFFFFFFFF, 0x9A, 0xAF);     // 0x30 kernel code, 64-bit (L=1), DPL 0
    gdt_set_entry(7, 0, 0xFFFFFFFF, 0x92, 0xCF);     // 0x38 kernel data
    tss_set_entry(8, (uint32_t)&kernel_tss, sizeof(struct tss) - 1);

    kernel_tss.iomap_base = sizeof(struct tss);      // no I/O permission bitmap (offset past the limit)

    gdt_flush((uint32_t)&gp);
    uint16_t tss_sel = GDT_TSS; __asm__ volatile("ltr %0" : : "r"(tss_sel));

    serial_puts("[gdt] GDT + TSS initialized (long mode; user segments at 0x18/0x20, 64-bit user code 0x28)\n");
}

void gdt_load_ap(void) {
    gdt_flush((uint32_t)&gp);
}

void tss_set_kernel_stack(uint32_t esp0) {
    kernel_tss.rsp0 = esp0;                          // zero-extended: kernel stacks are below 4 GB
}

void tss_get_esp0(uint32_t *esp0, uint32_t *ss0) {
    if (esp0) *esp0 = (uint32_t)kernel_tss.rsp0;
    if (ss0) *ss0 = GDT_KERNEL_DATA;
}

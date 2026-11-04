#include "gdt.h"
#include "io.h"
#include "serial.h"
#include "lapic.h"

// GDT entry structure
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// 0x00 null, 0x08 kernel code, 0x10 kernel data, 0x18 user code (32-bit, unused), 0x20 user data, 0x28 user code, 0x30 TSS (16 bytes).
static struct gdt_entry gdt[8];
static struct gdt_ptr gp;

// The actual TSS (the CPU reads rsp0 on every ring-3 -> ring-0 interrupt)
static struct tss kernel_tss;
uint64_t kernel_rsp0;           // the same value, for the SYSCALL entry (boot/lm.asm)
extern void lx_syscall_entry(void);

// Assembly function to load the GDT and reload segments
extern void gdt_flush(void*);

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

// 16-byte system descriptor in slots num and num+1: available 64-bit TSS, DPL 0.
static void tss_set_entry(int num, uint64_t base, uint32_t limit) {
    gdt_set_entry(num, (uint32_t)base, limit, 0x89, 0x00);
    uint32_t hi = (uint32_t)(base >> 32);
    gdt[num + 1].limit_low = (uint16_t)hi;
    gdt[num + 1].base_low = (uint16_t)(hi >> 16);
    gdt[num + 1].base_middle = 0;
    gdt[num + 1].access = 0;
    gdt[num + 1].granularity = 0;
    gdt[num + 1].base_high = 0;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)(uintptr_t)&gdt;

    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);     // 0x08 kernel code, L=1, DPL 0
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);     // 0x10 kernel data
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);     // 0x18 user code, 32-bit, DPL 3 (SYSRET's layout; nothing runs in it)
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);     // 0x20 user data, DPL 3
    gdt_set_entry(5, 0, 0xFFFFFFFF, 0xFA, 0xAF);     // 0x28 user code, L=1, DPL 3
    tss_set_entry(6, (uint64_t)(uintptr_t)&kernel_tss, sizeof(struct tss) - 1);

    kernel_tss.iomap_base = sizeof(struct tss);      // no I/O permission bitmap (offset past the limit)

    // The SYSCALL instruction: entry in boot/lm.asm. SFMASK: IF TF DF NT AC cleared on entry.
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);
    wrmsr(0xC0000081, ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(0xC0000082, (uint64_t)(uintptr_t)lx_syscall_entry);
    wrmsr(0xC0000084, 0x44700);

    gdt_flush(&gp);
    uint16_t tss_sel = GDT_TSS; __asm__ volatile("ltr %0" : : "r"(tss_sel));

    serial_puts("[gdt] GDT + TSS initialized (64-bit)\n");
}

void gdt_load_ap(void) {
    gdt_flush(&gp);
}

void tss_set_kernel_stack(uintptr_t rsp0) {
    kernel_tss.rsp0 = rsp0;
    kernel_rsp0 = rsp0;
}

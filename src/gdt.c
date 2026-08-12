#include "gdt.h"
#include "io.h"

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

// Our GDT has 3 entries: null, code, data
static struct gdt_entry gdt[3];
static struct gdt_ptr gp;

// Assembly function to load the GDT and reload segments
extern void gdt_flush(uint32_t);

static void gdt_set_entry(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity  = ((limit >> 16) & 0x0F);
    gdt[num].granularity |= (granularity & 0xF0);

    gdt[num].access = access;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)&gdt;

    gdt_set_entry(0, 0, 0, 0, 0);                // Null segment
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment

    gdt_flush((uint32_t)&gp);
}

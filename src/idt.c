#include "idt.h"
#include "io.h"
#include "serial.h"

// IDT entry structure
struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

// External assembly ISR handlers
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);

// External IRQ handlers (32-47)
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

// IRQ handler function pointer type
typedef void (*irq_handler_t)(void);

// IRQ handler table
static irq_handler_t irq_handlers[16] = {0};

// Register an IRQ handler
void irq_register_handler(int irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector  = sel;
    idt[num].zero      = 0;
    idt[num].flags     = flags;
}

// Remap the PIC so IRQs 0-15 don't conflict with CPU exceptions 0-15
static void pic_remap(void) {
    // Start initialization sequence (ICW1)
    outb(0x20, 0x11); // Master PIC
    outb(0xA0, 0x11); // Slave PIC

    // Set vector offsets (ICW2)
    outb(0x21, 0x20); // Master PIC: IRQ 0-7 → INT 32-39
    outb(0xA1, 0x28); // Slave PIC: IRQ 8-15 → INT 40-47

    // Tell Master about Slave at IRQ2 (ICW3)
    outb(0x21, 0x04);
    // Tell Slave its cascade identity (ICW3)
    outb(0xA1, 0x02);

    // 8086 mode (ICW4)
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // Unmask all IRQs
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    // Zero out the IDT
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // CPU exception handlers (ISRs 0-14)
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);

    // IRQ handlers (IRQ 0-15 → INT 32-47)
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    pic_remap();

    // Load IDT
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

// Called from ISR stubs when a CPU exception fires
void isr_handler(int int_num) {
    serial_puts("[ISR] Exception ");
    serial_putchar('0' + (int_num / 10));
    serial_putchar('0' + (int_num % 10));
    serial_putchar('\n');

    // Halt on exceptions
    cli();
    while (1) { __asm__ volatile("hlt"); }
}

// Called from IRQ stubs — irq is the interrupt number (32-47)
void irq_handler(int irq) {
    // Convert interrupt number to IRQ number (0-15)
    int irq_num = irq - 32;

    // Send EOI (End of Interrupt) to PIC
    if (irq_num >= 8) {
        outb(0xA0, 0x20); // Send EOI to slave PIC
    }
    outb(0x20, 0x20); // Send EOI to master PIC

    // Call registered handler
    if (irq_num >= 0 && irq_num < 16 && irq_handlers[irq_num]) {
        irq_handlers[irq_num]();
    }
}

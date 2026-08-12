#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IRQ handler function pointer type
typedef void (*irq_handler_t)(void);

// Initialize the IDT
void idt_init(void);

// Register an IRQ handler (IRQ 0-15)
void irq_register_handler(int irq, irq_handler_t handler);

// Enable/disable interrupts
static inline void sti(void) {
    __asm__ volatile("sti");
}

static inline void cli(void) {
    __asm__ volatile("cli");
}

#endif

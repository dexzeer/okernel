#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IRQ handler function pointer type
typedef void (*irq_handler_t)(void);

// Initialize the IDT
void idt_init(void);

// Install the INT 0x80 syscall gate (call after idt_init)
void idt_init_syscall(void);

// Install an interrupt gate for any vector (the SMP wake IPI), and load the
// shared IDT on an application processor.
void idt_set_vector(uint8_t vector, void (*handler)(void));
void idt_load_ap(void);

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

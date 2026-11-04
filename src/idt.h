#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// What boot/lm.asm leaves on the stack for an interrupt, lowest address first. The same layout as struct lx_regs (lx.h).
struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err;
    uint64_t rip, cs, rflags, rsp, ss;
};

// IRQ handler function pointer type
typedef void (*irq_handler_t)(void);

// Initialize the IDT
void idt_init(void);

// Install the handler of any vector (the SMP wake IPI), and load the shared IDT on an application processor.
void idt_set_vector(uint8_t vector, void (*handler)(struct trapframe*));
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

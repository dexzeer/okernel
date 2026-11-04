#include "idt.h"
#include "io.h"
#include "serial.h"
#include "syscall.h"
#include "process.h" // struct types only: no calls into process.o (desktop-only); every use here goes through weak aliases, so the text build still links.

// 16-byte gates. Every gate points at a 64-bit thunk (boot/lm.asm) that builds a struct trapframe and calls the handler registered here.
struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint64_t lm_vec_table[256];                       // the thunk of each vector
extern void (*lm_handler[256])(struct trapframe*);       // the handler of each vector

static struct idt_entry idt[256];
static struct idt_ptr idtp;

// IRQ handler table
static irq_handler_t irq_handlers[16] = {0};

// Register an IRQ handler
void irq_register_handler(int irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
}

static void idt_set_gate(uint8_t num, void (*handler)(struct trapframe*), uint8_t flags) {
    uint64_t t = handler ? lm_vec_table[num] : 0;
    lm_handler[num] = handler;
    idt[num].base_low  = (uint16_t)t;
    idt[num].selector  = handler ? 0x08 : 0;
    idt[num].ist       = 0;
    idt[num].flags     = handler ? flags : 0;
    idt[num].base_mid  = (uint16_t)(t >> 16);
    idt[num].base_high = (uint32_t)(t >> 32);
    idt[num].zero      = 0;
}

// Remap the PIC so IRQs 0-15 don't conflict with CPU exceptions 0-15
static void pic_remap(void) {
    outb(0x20, 0x11); // Master PIC
    outb(0xA0, 0x11); // Slave PIC
    outb(0x21, 0x20); // Master PIC: IRQ 0-7 -> INT 32-39
    outb(0xA1, 0x28); // Slave PIC: IRQ 8-15 -> INT 40-47
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

void idt_set_vector(uint8_t vector, void (*handler)(struct trapframe*)) {
    idt_set_gate(vector, handler, 0x8E);
}

void idt_load_ap(void) {
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

static void isr_handler(struct trapframe* f);
static void irq_handler(struct trapframe* f);

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    for (int i = 0; i < 256; i++) idt_set_gate((uint8_t)i, 0, 0);

    // CPU exceptions 0-14, 16-19
    for (int i = 0; i < 20; i++)
        if (i != 15) idt_set_gate((uint8_t)i, isr_handler, 0x8E);

    // IRQ 0-15 -> INT 32-47
    for (int i = 32; i < 48; i++) idt_set_gate((uint8_t)i, irq_handler, 0x8E);

    pic_remap();

    // INT 0x80 syscall gate: DPL=3 (0xEE) so ring 3 can call it
    idt_set_gate(0x80, isr_handler, 0xEE);

    __asm__ volatile("lidt %0" : : "m"(idtp));
}

// CPU exceptions and INT 0x80.
static void isr_handler(struct trapframe* f) {
    int int_num = (int)f->vec;

    if (int_num == 0x80) {
        // Syscall number in RAX, arguments in RDI/RSI/RDX (boot/lm.asm builds this frame for the SYSCALL instruction); the handler's return value goes
        // back in the saved RAX, which the stub reloads before iretq. sys_exit (rax=1) and blocking syscalls (wait-no-zombie, yield, read-empty;
        // syscall_arm_park) never iret: they jump to a trampoline that returns to the caller of enter_user_mode in the main loop, and this frame and
        // the whole trap stack are discarded. The main loop then runs queued siblings; the tick WAKE pass flips a parker READY and the drain re-enters
        // it with the staged return value.
        uint64_t nr_val = f->rax, a0_val = f->rdi, a1_val = f->rsi, a2_val = f->rdx;
        {
            extern int syscall_take_park(void);
            extern uint32_t syscall_take_park_ret(void);
            extern void syscall_stash_park_ret(uint32_t v);
            extern void user_park_trampoline(void) __attribute__((noreturn));
            if (syscall_take_park()) {
                syscall_stash_park_ret(syscall_take_park_ret());
                user_park_trampoline();
            }
        }
        if (nr_val == 1) {
            extern volatile int syscall_can_exit;
            extern void user_exit_trampoline(void) __attribute__((noreturn));
            if (syscall_can_exit) {
                syscall_can_exit = 0;
                // Record status before the jump: the trampoline discards this frame, so the handler never runs for SYS_EXIT. The text build
                // has no sys_proc.o: weak, NULL, nothing to zombie there.
                extern void sys_proc_exit_current(int code) __attribute__((weak));
                if (sys_proc_exit_current) sys_proc_exit_current((int)a0_val);
                {
                    extern void syscall_note_exited(void);
                    syscall_note_exited();
                }
                user_exit_trampoline();
            }
            return;
        }
        // A tick may have stolen ring 3 for pid 0 (main loop), so current/CR3/rsp0 are the kernel's, but the trapped RIP/RSP are still the user's:
        // reconcile first, so the stash, the validation and the fd table below see the owner, and fork finds the true parent.
        {
            extern void syscall_reconcile_ring3(void) __attribute__((weak));
            if (syscall_reconcile_ring3) syscall_reconcile_ring3();
        }
        syscall_stash_trap(f);
        syscall_handler(nr_val, a0_val, a1_val, a2_val);
        // Exec redirect: SYS_EXEC replaced user-low in place. The trapped RIP/RSP point into the OLD image, so the iret goes to the new one.
        {
            extern int syscall_take_exec_redirect(void);
            if (syscall_take_exec_redirect()) {
                extern struct process *process_current(void) __attribute__((weak));
                if (process_current) {
                    struct process *pcb = process_current();
                    if (pcb) {
                        f->rip = pcb->user_rip;
                        f->rsp = pcb->user_rsp;
                    }
                }
            }
            // The sign is the program's business: it reads RAX as an int.
            f->rax = syscall_take_ret();
        }
        return;
    }

    if (int_num == 13) {
        // A fault-safe RDMSR/WRMSR (cpufreq.c) names its instruction and the place to resume at; weak: the text build has no cpufreq.c
        extern volatile uintptr_t msr_fix_from __attribute__((weak));
        extern volatile uintptr_t msr_fix_to __attribute__((weak));
        if (&msr_fix_from && msr_fix_from && f->rip == msr_fix_from) {
            f->rip = msr_fix_to;
            return;
        }
    }

    {
        // An exception on an application processor only parks that CPU (smp.c); weak so the text build, which has no SMP, links.
        extern int smp_fault_hook(int vec, struct trapframe* frame) __attribute__((weak));
        if (smp_fault_hook) smp_fault_hook(int_num, f);
    }

    if (int_num == 13) {
        uint32_t err_code = (uint32_t)f->err;
        serial_puts("[ISR] GP fault! error_code=0x");
        serial_printf("%x", err_code);
        // Full decode (EXT/IDT/TI/index): an absurd index means the code is NOT a selector.
        serial_printf(" ext=%d idt=%d ti=%d idx=%x\n",
                      err_code & 1, (err_code >> 1) & 1,
                      (err_code >> 2) & 1, (err_code >> 3) & 0x1FFF);
    } else {
        serial_puts("[ISR] Exception ");
        serial_putchar('0' + (int_num / 10));
        serial_putchar('0' + (int_num % 10));
        if (int_num == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            uint32_t fpid = 0xFFFFFFFF;
            {
                extern struct process *process_current(void) __attribute__((weak));
                if (process_current) {
                    struct process *pcb = process_current();
                    if (pcb) fpid = pcb->pid;
                }
            }
            serial_printf(" err=%x cr2=%lx rip=%lx rsp=%lx pid=%d cs=%x rbx=%lx\n",
                          (uint32_t)f->err, cr2, f->rip, f->rsp, fpid, (uint32_t)f->cs & 0xFFFF, f->rbx);
        } else {
            serial_printf(" rip=%lx\n", f->rip);
        }
    }

    {
        // The desktop kernel shows the panic screen, saves a crash record and reboots into the recovery system (panic.c); weak: the text
        // builds have none and just stop here.
        extern void panic_exception(int vec, struct trapframe* frame) __attribute__((weak, noreturn));
        if (panic_exception) panic_exception(int_num, f);
    }
    cli();
    while (1) { __asm__ volatile("hlt"); }
}

// Stall sampler (prof.c, desktop build only; weak so the text build links without it)
extern volatile int prof_on __attribute__((weak));
void stall_sample(uint64_t rip, uint32_t cs, uint64_t rsp) __attribute__((weak));

static void irq_handler(struct trapframe* f) {
    int irq_num = (int)f->vec - 32;
    if (irq_num >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    if (irq_num == 0 && &prof_on && prof_on && stall_sample) stall_sample(f->rip, (uint32_t)f->cs & 0xFFFF, f->rsp);
    if (irq_num >= 0 && irq_num < 16 && irq_handlers[irq_num]) {
        irq_handlers[irq_num]();
    }
}

#include "idt.h"
#include "io.h"
#include "serial.h"
#include "syscall.h"

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

// INT 0x80 syscall stub (defined in isr.asm)
extern void isr80(void);

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

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

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

    // IRQ handlers (IRQ 0-15 -> INT 32-47)
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

    // INT 0x80 syscall gate — DPL=3 (0xEE) so ring 3 can call it
    idt_set_gate(0x80, (uint32_t)isr80, 0x08, 0xEE);

    // Load IDT
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

// Called from ISR stubs when a CPU exception fires
// Push sequence (boot/isr.asm isr_common_stub, entered via isr80 which
// pushed err=0 + int_num=0x80 first): int_num, pusha(EDI ESI EBP ESP EBX EDX
// ECX EAX), saved-ds, int_num-copy, call. At isr_handler entry the C
// prologue pushed EBP (saved-ebp slot = __builtin_frame_address(0)).
// Pushed-EDI base is frame+16: saved-ebp(4) + call-ret(4) + int_num-copy(4)
// + saved-ds(4) = 16. (frame+20 is WRONG — it shifts every register one
// slot and eax decodes the int_num slot: "unknown syscall 128".)
// Full trap-stack order above the handler frame (low→high), verified against
// the stub 2026-09-08:
//   pushed[0..7] = pusha (EDI ESI EBP ESP EBX EDX ECX EAX),
//   pushed[8] = stub int_num (0x80), pushed[9] = stub err (0),
//   pushed[10] = CPU EIP, pushed[11] = CPU CS, pushed[12] = CPU EFLAGS,
//   pushed[13] = CPU ESP, pushed[14] = CPU SS.
// (The two stub pushes err+int_num sit BETWEEN pusha and the CPU frame — an
// earlier draft omitted them and used pushed[11/14] for EIP/ESP, which read
// CS:SS (0x1b/0x23) and fork children #PF'd e=0004 at CPL=3.)
void isr_handler(int int_num) {
    // INT 0x80 = syscall
    if (int_num == 0x80) {
        // Syscall registers from the pusha frame the stub pushed (base =
        // frame+16 per the layout above). PUSHA order on the stack is EDI
        // ESI EBP ESP EBX EDX ECX EAX, so EAX=[7] EBX=[4] ECX=[6] EDX=[5].
        uint32_t *pushed = (uint32_t*)(__builtin_frame_address(0) + 16);
        uint32_t eax_val = pushed[7]; // eax
        uint32_t ebx_val = pushed[4]; // ebx
        uint32_t ecx_val = pushed[6]; // ecx
        uint32_t edx_val = pushed[5]; // edx
        // Ring-3 return values ride back in the saved EAX slot ([7]): the
        // stub's popa reloads EAX from it before iret, so writing it here is
        // how syscalls (getpid/sbrk/mmap/pipe/dup/open/read/...) return ints
        // to user code. syscall_handler sets it via syscall_set_ret().
        // sys_exit (eax=1) returns to the main loop via user_exit_trampoline:
        // forward the saved shell ESP (user_ret_esp, written by
        // enter_user_mode) in EAX and JMP — never CALL, never return here.
        // The trampoline restores segments + FULL caller frame
        // (EBP/EBX/ESI/EDI/ESP) and jumps to the shell's return address; this
        // frame (and the whole trap stack) is discarded.
        // PARK (new, same mechanism): blocking syscalls (wait-no-zombie,
        // yield, read-empty) park the caller as BLOCKED and jmp to
        // user_park_trampoline instead of ireting -2/0 back to ring 3. The
        // main loop then runs queued siblings on THIS iteration; the tick
        // WAKE pass flips the parker READY and the drain re-enters it via
        // enter_user_mode with the staged return value (park_ret, EAX slot
        // forced like the fork-child path). syscall_handler arms this via
        // syscall_arm_park(retval); idt.c consumes after dispatch (same
        // consume-once pattern as the exec redirect below).
        {
            extern int syscall_take_park(void);
            extern uint32_t syscall_take_park_ret(void);
            extern uint32_t user_ret_esp;
            extern void user_park_trampoline(void) __attribute__((noreturn));
            if (syscall_take_park()) {
                uint32_t pret = syscall_take_park_ret();
                (void)pret; // staged for the re-entry path (drain consumes)
                extern void syscall_stash_park_ret(uint32_t v);
                syscall_stash_park_ret(pret);
                uint32_t saved = user_ret_esp;
                __asm__ volatile(
                    "mov %0, %%eax; jmp user_park_trampoline"
                    : : "r"(saved) : "eax", "memory");
                __builtin_unreachable();
            }
        }
        if (eax_val == 1) {
            extern volatile int syscall_can_exit;
            extern uint32_t user_ret_esp;
            extern void user_exit_trampoline(void) __attribute__((noreturn));
            if (syscall_can_exit) {
                syscall_can_exit = 0;
                // Record status BEFORE the jump: the trampoline discards this
                // frame, so the handler never runs for SYS_EXIT. Trap-safe
                // (PCB + parent bitmask only, no switches/waits). Text build
                // has no sys_proc.o — weak → NULL, nothing to zombie there.
                extern void sys_proc_exit_current(int code) __attribute__((weak));
                if (sys_proc_exit_current) sys_proc_exit_current((int)ebx_val);
                {
                    extern void syscall_note_exited(void);
                    syscall_note_exited();
                }
                uint32_t saved = user_ret_esp;
                __asm__ volatile(
                    "mov %0, %%eax; jmp user_exit_trampoline"
                    : : "r"(saved) : "eax", "memory");
                __builtin_unreachable();
            }
            return;
        }
        // Stash the trapped user resume state for fork (child resumes after
        // the fork call, not at image entry). CPU EIP/ESP are pushed[10/13]
        // per the layout above; callee-saved EBX/EDI/ESI/EBP ride pushed
        // [4]/[0]/[1]/[2] (pusha order EDI ESI EBP ESP EBX EDX ECX EAX).
        // The fork stub (sys_proc.c) needs them: fork children enter via a
        // FRESH IRET (only EIP/CS/EFLAGS/ESP/SS+EAX restored), so mid-function
        // resumes would otherwise inherit garbage frames (bisected 2026-09-08:
        // sh children died touching EBP-relative line[]). Direct call (same
        // linkage as the handler).
        // ALSO reconcile BEFORE stash: a tick may have stolen ring 3 for
        // pid 0 (main loop), so current/CR3/ESP0 are the kernel's — but the
        // CPU-pushed EIP/ESP are still the USER's (the trap itself switched
        // to ESP0, it doesn't care which PD was live). Reconcile first so
        // the stash + validate + fd-table paths below see the OWNER, and so
        // fork's parent lookup finds the true parent (else the child is
        // parented to pid 0 and the real parent's wait never fires).
        {
            extern void syscall_reconcile_ring3(void) __attribute__((weak));
            if (syscall_reconcile_ring3) syscall_reconcile_ring3();
        }
        {
            extern void syscall_stash_trap_full(uint32_t eip, uint32_t esp,
                                               uint32_t ebx, uint32_t edi,
                                               uint32_t esi, uint32_t ebp,
                                               int have_regs);
            syscall_stash_trap_full(pushed[10], pushed[13], pushed[4],
                                    pushed[0], pushed[1], pushed[2], 1);
        }
        syscall_handler(eax_val, ebx_val, ecx_val, edx_val);
        // Exec redirect: SYS_EXEC success replaced user-low in place. The
        // trapped EIP/ESP point into the OLD image (now freed pages) — iret
        // there would #PF. Instead patch the iret frame (user CS:EIP +
        // SS:ESP slots, located off the pushed-EDI base) to the new image's
        // user_eip/esp. Layout above: EIP is pushed[10], ESP is pushed[13]
        // (CS pushed[11] stays 0x1B, SS pushed[14] stays 0x23).
        // Trap-safe: same address space (CR3 unchanged), same trap stack,
        // popa+iret proceed normally — only the resume address changes.
        // NOTE: process.h field offsets (pid/state/page_dir/esp/ebp/eip/
        // entered_ring3/user_esp/user_eip) are read via a local mirror to
        // keep idt.c (COMMON, text build) from including process.h
        // (desktop-only). The offsets are ABI — see process.h; a
        // _Static_assert-free comment guards drift (both structs are u32s).
        {
            extern int syscall_take_exec_redirect(void);
            extern uint32_t syscall_take_ret(void);
            if (syscall_take_exec_redirect()) {
                // process_current is desktop-only (COMMON/text has no
                // process.o): weak alias → NULL when unlinked; no redirect
                // in text mode (exec hook is NULL there anyway).
                extern void *process_current(void) __attribute__((weak));
                if (process_current) {
                    uint32_t *pcb = (uint32_t*)process_current();
                    if (pcb) {
                        uint32_t new_eip = pcb[8]; // user_eip
                        uint32_t new_esp = pcb[7]; // user_esp
                        pushed[10] = new_eip;
                        pushed[13] = new_esp;
                    }
                }
            }
            // Stage the handler's return value into the saved-EAX slot so popa
            // reloads it into EAX before iret (ring-3 sees it as the syscall's
            // return). sys_exit never reaches here (trampolined above).
            // (On the exec-redirect path no set_ret ran — take_ret returns 0
            // and the store is harmless: the new image starts fresh.)
            uint32_t ret = syscall_take_ret();
            *(volatile uint32_t *)&pushed[7] = ret;
            __asm__ volatile("" ::: "memory");
        }
        return; // Don't halt on syscall
    }

    if (int_num == 13) {
        // GP fault — the CPU pushes an error code for vector 13. Reconstruct
        // it from the stub's exact push sequence (isr_common_stub):
        //   CPU [err] [eip cs eflags] | stub: push int_num, pusha (32B),
        //   push saved-ds (4B), push int_num-copy (4B), call (ret addr).
        // So at handler entry the CPU error code is pushed[9] above the
        // pushed-edi base (frame+16, see above). Verified by arithmetic on
        // the stub: err lands at handler-EBP+52, base at handler-EBP+16,
        // (52-16)/4 = 9. (An earlier draft read pushed[11] = CPU CS and
        // printed it as the error code.)
        uint32_t *pushed_edi =
            (uint32_t*)(__builtin_frame_address(0) + 16);
        uint32_t err_code = pushed_edi[9];
        serial_puts("[ISR] GP fault! error_code=0x");
        serial_printf("%x", err_code);
        // Full decode (EXT/IDT/TI/index): an absurd index (0x1080/0xf00 vs a
        // 48-byte GDT) means the code is NOT a selector — descriptor-table
        // faults carry segment numbers instead. Don't chase the index.
        serial_printf(" ext=%d idt=%d ti=%d idx=%x\n",
                      err_code & 1, (err_code >> 1) & 1,
                      (err_code >> 2) & 1, (err_code >> 3) & 0x1FFF);
    } else {
        uint32_t *pushed_edi =
            (uint32_t*)(__builtin_frame_address(0) + 16);
        serial_puts("[ISR] Exception ");
        serial_putchar('0' + (int_num / 10));
        serial_putchar('0' + (int_num % 10));
        if (int_num == 14) {
            // #PF: ERRCODE vector — CPU pushed err BEFORE the stub's
            // int_num, so from the pushed-EDI base: pusha[0..7],
            // int_num[8], err[9], EIP[10], CS[11], EFLAGS[12], ESP[13],
            // SS[14] (same as the GP handler above: err = pushed[9]).
            // CR2 holds the faulting linear address.
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            // Faulting pid (input-bug bisect): weak process_current (COMMON
            // text build has no process.o); pid = pcb[0] per process.h ABI.
            uint32_t fpid = 0xFFFFFFFF;
            {
                extern void *process_current(void) __attribute__((weak));
                if (process_current) {
                    uint32_t *pcb = (uint32_t*)process_current();
                    if (pcb) fpid = pcb[0];
                }
            }
            serial_printf(" err=%x cr2=%x eip=%x esp=%x pid=%d\n",
                          pushed_edi[9], cr2, pushed_edi[10],
                          pushed_edi[13], fpid);
        } else {
            serial_putchar('\n');
        }
    }

    cli();
    while (1) { __asm__ volatile("hlt"); }
}

// Called from IRQ stubs
void irq_handler(int irq) {
    int irq_num = irq - 32;
    if (irq_num >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    if (irq_num >= 0 && irq_num < 16 && irq_handlers[irq_num]) {
        irq_handlers[irq_num]();
    }
}

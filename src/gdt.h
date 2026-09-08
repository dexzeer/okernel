#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// GDT segment selectors
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

// Task State Segment (104 bytes + I/O map base, x86 32-bit).
// LAYOUT WARNING: every field order/size below is ABI — the CPU reads
// esp0/ss0 at offsets 4/8 on every ring-3 -> ring-0 trap. A reordered or
// resized struct silently breaks all syscalls (trap frame lands on a garbage
// stack; faults as #DE/#PF at LOW EIP like 0x2d). Verified 2026-09-07.
struct tss {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);
void tss_set_kernel_stack(uint32_t esp0);
void tss_get_esp0(uint32_t *esp0, uint32_t *ss0);
// enter_user_mode takes NO stack args: caller passes eip in EAX, esp in EDX
// (see main-loop inline asm in desktop.c). DOES NOT RETURN via ret — sys_exit
// resumes the caller through user_exit_trampoline (boot/isr.asm).
void enter_user_mode(void);
// Flag the NEXT enter_user_mode IRET as a fork-child entry (EAX forced 0).
// Plain ring-0 call, must run BEFORE the enter inline asm in the main loop.
void enter_user_mode_fork_child(void);
// Stage the EAX value for the NEXT enter_user_mode IRET (park-resume path).
// Plain cdecl ring-0 call, must run BEFORE the enter inline asm.
void enter_user_mode_park_ret(uint32_t v);

#endif

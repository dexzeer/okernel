#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// GDT segment selectors
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#ifdef LONGMODE
// The desktop kernel runs the CPU in long mode (kernel code in compatibility mode); the text kernel stays in
// 32-bit protected mode (src/gdt32.c, src/idt.c without LONGMODE).
#define GDT_USER_CODE64 0x28
#define GDT_KERNEL_CODE64 0x30
#define GDT_KERNEL_DATA2 0x38
#define GDT_TSS         0x40

// Task State Segment, 64-bit format (104 bytes). LAYOUT WARNING: the CPU reads rsp0 (offset 4) on every ring-3 ->
// ring-0 interrupt; a reordered or resized struct silently breaks every trap.
struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));
#else
#define GDT_TSS         0x28

// Task State Segment (104 bytes + I/O map base, x86 32-bit).
// LAYOUT WARNING: every field order/size below is ABI - the CPU reads
// esp0/ss0 at offsets 4/8 on every ring-3 -> ring-0 trap.
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
#endif

void gdt_init(void);
// Load the already-built GDT on an application processor. No TSS: an AP never
// leaves ring 0.
void gdt_load_ap(void);
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

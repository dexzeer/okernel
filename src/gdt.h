#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// GDT segment selectors. The order is dictated by SYSRET: with STAR[63:48] = 0x18 it loads 64-bit CS = 0x28|3, SS = 0x20|3.
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE32 0x18      // never used; only here for SYSRET's fixed layout
#define GDT_USER_DATA   0x20
#define GDT_USER_CODE   0x28
#define GDT_TSS         0x30

// Task State Segment, 64-bit format (104 bytes). LAYOUT WARNING: the CPU reads rsp0 (offset 4) on every ring-3 -> ring-0 interrupt; a
// reordered or resized struct silently breaks every trap.
struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);
// Load the already-built GDT on an application processor. No TSS: an AP never leaves ring 0.
void gdt_load_ap(void);
void tss_set_kernel_stack(uintptr_t rsp0);
// DOES NOT RETURN through a ret of its own: sys_exit and parking resume the caller through user_exit_trampoline (boot/isr.asm), which looks
// to the caller exactly like a normal return.
void enter_user_mode(uint64_t user_rip, uint64_t user_rsp);
// Flag the NEXT enter_user_mode IRET as a fork-child entry (RAX forced 0).
void enter_user_mode_fork_child(void);
// Stage the RAX value for the NEXT enter_user_mode IRET (park-resume path).
void enter_user_mode_park_ret(uint32_t v);

#endif

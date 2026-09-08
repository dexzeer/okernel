/* Linker script for okernel DESKTOP (high-half kernel) */

ENTRY(_start)

/* LOAD MODEL: single low-phys load at 1MB (GRUB ELF path reads p_paddr).
   - .multiboot: VMA == LMA == phys 0x100000 (GRUB finds header in 1st 8KB).
   - _start trampoline: runs at LOW phys (e_entry = See Below).
   - Everything else: VMA high (0xC01xxxxx), LMA phys (0x001xxxxx).
   ENTRY RESOLUTION (2026-09-07): ENTRY(_start) with _start's VMA high sets
   e_entry = 0xC0100030; GRUB jumps there with paging OFF -> triple fault
   (SeaBIOS text only). The .multiboot LOW VMA does NOT fix e_entry. Two
   candidate fixes: (a) separate low .trampoline section (VMA==LMA==phys)
   holding _start, ENTRY stays _start; (b) ENTRY(_start_low) alias. Next:
   implement (a) in start.asm + this script, rebuild, verify e_entry low via
   `readelf -h | grep Entry`, then ISO boot -> 1920x1080 + serial. */

SECTIONS {
    .multiboot 0x100000 : AT(0x100000) {
        *(.multiboot)
    }

    /* LOW trampoline: VMA == LMA == phys (GRUB entry, paging OFF).
       e_entry = _start here. Must stay within the first 8KB+slack of the
       image so GRUB's load covers it (it does: right after .multiboot). */
    .trampoline 0x100030 : AT(0x100030) {
        *(.trampoline)
    }

    . = 0xC0100000 + SIZEOF(.multiboot) + SIZEOF(.trampoline);

    _kernel_start = .;

    /* Kernel code: LMA = phys 0x100030 (VMA 0xC0100030). Linear AT form. */
    .text : AT(0x100000 + (ADDR(.text) - 0xC0100000)) {
        *(.text)
    }

    /* Read-only data */
    .rodata : AT(0x100000 + (ADDR(.rodata) - 0xC0100000)) {
        *(.rodata)
    }

    /* Initialized data */
    .data : AT(0x100000 + (ADDR(.data) - 0xC0100000)) {
        *(.data)
    }

    /* Uninitialized data (BSS) */
    . = ALIGN(0x1000);
    .bss : AT(0x100000 + (ADDR(.bss) - 0xC0100000)) {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}

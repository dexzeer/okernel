/* Linker script for the okernel kernels (64-bit, code model kernel).

   LOAD MODEL: one low-phys load at 1 MB (GRUB's ELF path reads p_paddr and enters e_entry with paging off).
   - .multiboot, .trampoline, .boottab: VMA == LMA == phys. e_entry = _start in .trampoline; GRUB requires it below 4 GB.
   - Everything else: VMA = phys + 0xFFFFFFFF80000000, LMA = phys. */

ENTRY(_start)

KV = 0xFFFFFFFF80000000;

SECTIONS {
    .multiboot 0x100000 : AT(0x100000) {
        *(.multiboot)
    }

    /* Right after the header, so it is inside the first part of the image whatever GRUB reads first. */
    .trampoline : {
        *(.trampoline)
    }

    /* Boot page tables: no bytes in the file, _start clears them. */
    .boottab ALIGN(0x1000) (NOLOAD) : {
        *(.boottab)
    }

    . = KV + ALIGN(0x1000);
    _kernel_start = .;

    .text : AT(ADDR(.text) - KV) {
        *(.text .text.*)
    }

    . = ALIGN(16);
    .rodata : AT(ADDR(.rodata) - KV) {
        _rodata_start = .;              /* the end of the code (panic.c: what a return address may point into) */
        *(.rodata .rodata.*)
    }

    . = ALIGN(16);
    .data : AT(ADDR(.data) - KV) {
        *(.data .data.*)
    }

    /* Symbol table of the panic screen (tools/ksyms.py). Last of the loaded sections before .bss: the first link
       has it empty, the second fills it in, and nothing above moves between the two. */
    . = ALIGN(8);
    .ksyms : AT(ADDR(.ksyms) - KV) {
        _ksyms_start = .;
        KEEP(*(.ksyms))
        _ksyms_end = .;
    }

    . = ALIGN(0x1000);
    .bss (NOLOAD) : AT(ADDR(.bss) - KV) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note .note.*)
    }
}

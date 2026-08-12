/* Linker script for okernel */

ENTRY(_start)

SECTIONS {
    /* Load at 1MB — standard for multiboot kernels */
    . = 1M;

    _kernel_start = .;

    /* Multiboot header — must be in the first 8KB */
    .multiboot : {
        *(.multiboot)
    }

    /* Kernel code */
    .text : {
        *(.text)
    }

    /* Read-only data */
    .rodata : {
        *(.rodata)
    }

    /* Initialized data */
    .data : {
        *(.data)
    }

    /* Uninitialized data (BSS) */
    .bss : {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}

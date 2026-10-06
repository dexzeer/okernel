/* Linker script for okernel */

/* TEXT BUILD ENTERS AT _start_high, NOT _start: the LOW trampoline builds
   its boot page tables with `label - 0xC0000000' expressions that only
   resolve against HIGH-VMA links (linker-high.ld). Low-linked they wrap to
   garbage phys -> CR3 garbage -> triple fault before any C runs (bisected
   2026-09-30: #DF at _start+0x5D, IDT=0). The text kernel never enables
   paging, so the plain `mov esp,push,call kernel_main' high entry is the
   correct one here (valid under GRUB's flat GDT). The orphaned .trampoline
   section links as dead code, never executed. */

ENTRY(_start_high)

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

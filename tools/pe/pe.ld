/* Linker script for programs and DLLs of the Windows side (wrapped into PE files by tools/pe/mkpe.py).
   Use: ld -m elf_x86_64 -q -T pe.ld --defsym=__base=0x10000000 -e <entry> ... */
SECTIONS
{
    . = __base + 0x1000;
    .text   : ALIGN(0x1000) { *(.text .text.*) *(.stubs) }
    .rdata  : ALIGN(0x1000) { *(.rdata .rdata.* .rodata .rodata.*) }
    /* unwind tables: .eh_frame (DWARF CFI of the compiled code) is converted by mkpe into .pdata/.xdata; hand written
       unwind data (a program's own __try emulation) goes into .sehdata (UNWIND_INFO) and .sehfunc (RUNTIME_FUNCTION entries),
       with 32-bit relocations that mkpe turns into RVAs */
    .eh_frame : ALIGN(8) { KEEP(*(.eh_frame)) }
    .sehfunc  : ALIGN(4) { KEEP(*(.sehfunc)) }
    .sehdata  : ALIGN(0x1000) { KEEP(*(.sehdata)) }
    .data   : ALIGN(0x1000) { *(.data .data.*) *(.iat) *(.tls) }
    .bss    : ALIGN(0x1000) { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.gnu*) *(.dynsym) *(.dynstr) *(.dynamic) *(.interp) *(.hash) *(.gnu.hash) *(.eh_frame_hdr) }
}

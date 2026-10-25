/* Linker script for programs and DLLs of the Windows side (wrapped into PE files by tools/pe/mkpe.py).
   Use: ld -m elf_x86_64 -T pe.ld --defsym=__base=0x10000000 -e <entry> ... */
SECTIONS
{
    . = __base + 0x1000;
    .text   : ALIGN(0x1000) { *(.text .text.*) *(.stubs) }
    .rdata  : ALIGN(0x1000) { *(.rdata .rdata.* .rodata .rodata.* .eh_frame) }
    .data   : ALIGN(0x1000) { *(.data .data.*) *(.iat) *(.tls) }
    .bss    : ALIGN(0x1000) { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.gnu*) *(.dynsym) *(.dynstr) *(.dynamic) *(.interp) *(.hash) *(.gnu.hash) *(.eh_frame_hdr) }
}

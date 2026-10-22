; The kernel symbol table for the panic screen (tools/ksyms.py makes it; see the desktop link in the Makefile).
section .ksyms progbits alloc noexec nowrite align=4
    incbin "build-ksyms/ksyms.bin"

section .note.GNU-stack noalloc noexec nowrite progbits

; Multiboot header constants
MBALIGN  equ 1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ 1 << 1            ; provide memory map
VIDEO    equ 1 << 2            ; provide video mode info
FLAGS    equ MBALIGN | MEMINFO | VIDEO
MAGIC    equ 0x1BADB002        ; multiboot magic number
CHECKSUM equ -(MAGIC + FLAGS)

; Multiboot header (must be in first 8KB of kernel)
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    ; Video: linear graphics, 640x480, 8bpp
    dd 0    ; mode_type = 0 (linear)
    dd 640  ; width
    dd 480  ; height
    dd 8    ; bpp

; Stack — 32KB for our kernel
section .bss
align 16
stack_bottom:
    resb 32768
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    push eax                ; multiboot magic
    push ebx                ; multiboot info pointer
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

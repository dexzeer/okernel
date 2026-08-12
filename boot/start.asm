; Multiboot header constants
MBALIGN  equ 1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ 1 << 1            ; provide memory map
FLAGS    equ MBALIGN | MEMINFO
MAGIC    equ 0x1BADB002        ; multiboot magic number
CHECKSUM equ -(MAGIC + FLAGS)

; Multiboot header (must be in first 8KB of kernel)
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

; Stack — 16KB for our kernel
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; Prevent executable stack warning
section .note.GNU-stack noalloc noexec nowrite progbits

; Entry point
section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top      ; set up the stack
    push eax                ; push multiboot magic (check if loaded by GRUB)
    push ebx                ; push multiboot info pointer
    call kernel_main        ; jump to our C kernel

    cli
.hang:
    hlt
    jmp .hang

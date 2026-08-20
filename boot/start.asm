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
    ; Video: linear graphics, 1024x768, 32bpp
    dd 0    ; mode_type = 0 (linear)
    dd 1024 ; width
    dd 768  ; height
    dd 32   ; bpp

; Stack — 256KB. tls_client_run keeps several ~18KB on-stack record buffers
; live at once (rec_buf, pt, plus nested recv_aead/send_aead buffers), so a
; small stack would overflow during a TLS handshake. 256KB is well clear.
section .bss
align 16
stack_bottom:
    resb 262144
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

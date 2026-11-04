; Kernel entry: multiboot header, the 32-bit stub GRUB jumps to, and the switch to 64-bit long mode.
;
; GRUB loads the ELF64 image by physical address and enters _start in 32-bit protected mode with paging off, so everything up to
; lm64 runs at its low (physical) address. The stub builds the boot page tables, enables long mode and jumps to _start_high, the
; first code at the kernel's linked address.

MBALIGN  equ 1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ 1 << 1            ; provide memory map
VIDEO    equ 1 << 2            ; provide video mode info
%ifdef TEXTMODE
%ifdef FBTEXT
FLAGS    equ MBALIGN | MEMINFO | VIDEO ; text kernel drawing on a framebuffer (UEFI has no VGA text)
%define HASVIDEO
%else
FLAGS    equ MBALIGN | MEMINFO ; text build: stay in VGA text mode
%endif
%else
FLAGS    equ MBALIGN | MEMINFO | VIDEO
%define HASVIDEO
%endif
%ifndef VID_W
%define VID_W 1920            ; mode requested from the bootloader (GRUB falls back to its own
%define VID_H 1080            ; choice when the display lacks it; the kernel adapts, see graphics_init)
%endif
MAGIC    equ 0x1BADB002        ; multiboot magic number
CHECKSUM equ -(MAGIC + FLAGS)

; Multiboot header (must be in the first 8 KB of the image). No address fields: GRUB loads by ELF segments.
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
%ifdef HASVIDEO
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    ; Video: linear graphics, 1920x1080, 32bpp
    dd 0    ; mode_type = 0 (linear)
    dd VID_W ; width
    dd VID_H ; height
    dd 32   ; bpp
%endif

; Boot page tables, linked and loaded low (the stub uses them with paging off). They map the first 4 GB three times with 2 MB pages:
;   0x0000000000000000  identity (the stub itself, multiboot structures, devices of the text kernel)
;   0xFFFF800000000000  the direct map (memlayout.h), which is what P2V gives before paging_init
;   0xFFFFFFFF80000000  the kernel image: only the first GB, the code model only reaches the top 2 GB of the address space
; The last GB is the PCI hole on any machine with less RAM than that, so it is uncached. paging_init replaces all of this on the desktop;
; the text kernel never leaves these tables.
section .boottab nobits alloc write align=4096
boot_pml4:    resb 4096
boot_pdpt_lo: resb 4096
boot_pdpt_hi: resb 4096
boot_pd:      resb 4096 * 4

section .trampoline
bits 32
global _start
extern kernel_main

_start:
    mov esp, 0x7FF00            ; free low memory below 1 MB
    push ebx                    ; multiboot info (phys): cpuid clobbers ebx
    push eax                    ; multiboot magic

    ; x86-64 is required. Without it, say so on the debug ports and halt.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_lm
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29           ; LM
    jnz .have_lm
.no_lm:
    mov esi, .nolm_msg
.say:
    lodsb
    test al, al
    jz .halt
    mov dx, 0x3F8
    out dx, al
    mov dx, 0xE9
    out dx, al
    jmp .say
.halt:
    cli
    hlt
    jmp .halt
.nolm_msg: db "KAnarchy needs an x86-64 CPU (long mode)", 10, 0
.have_lm:

    ; .boottab is nobits: GRUB does not promise to clear it.
    mov edi, boot_pml4
    mov ecx, (4096 * 7) / 4
    xor eax, eax
    cld
    rep stosd

    mov eax, boot_pdpt_lo + 3
    mov [boot_pml4], eax                    ; identity
    mov [boot_pml4 + 256 * 8], eax          ; direct map
    mov eax, boot_pdpt_hi + 3
    mov [boot_pml4 + 511 * 8], eax          ; kernel image
    mov eax, boot_pd + 3
    mov [boot_pdpt_hi + 510 * 8], eax       ; 0xFFFFFFFF80000000 = the first GB
    xor ecx, ecx
.fill_pdpt:
    mov eax, ecx
    shl eax, 12
    add eax, boot_pd + 3
    mov [boot_pdpt_lo + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jb .fill_pdpt
    xor ecx, ecx
.fill_pd:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                            ; present, writable, 2 MB
    cmp ecx, 512 * 3
    jb .cached
    or eax, 0x18                            ; PWT + PCD: the last GB holds device registers
.cached:
    mov [boot_pd + ecx * 8], eax
    inc ecx
    cmp ecx, 512 * 4
    jb .fill_pd

    mov eax, cr4
    or eax, 0x20                ; CR4.PAE
    mov cr4, eax
    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, 0x100               ; LME
    wrmsr
    mov eax, boot_pml4
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax                ; paging on: the CPU is in long mode, this code runs in compatibility mode under GRUB's CS

    pop esi                     ; multiboot magic
    pop edi                     ; multiboot info
    lgdt [boot_gdtr]
    jmp 0x08:lm64

bits 64
lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rax, _start_high
    jmp rax

align 8
boot_gdt:
    dq 0x0000000000000000        ; null
    dq 0x00AF9A000000FFFF        ; 0x08: code, 64-bit
    dq 0x00CF92000000FFFF        ; 0x10: data
boot_gdtr:
    dw boot_gdtr - boot_gdt - 1
    dq boot_gdt

section .text
bits 64
; First code at the linked address: edi = multiboot info (phys), esi = multiboot magic; the upper halves are zero.
global _start_high
_start_high:
    mov rsp, stack_top
    xor ebp, ebp
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang

; The boot stack, on which the main loop runs for good.
section .bss
align 16
stack_bottom:
    resb 524288
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits

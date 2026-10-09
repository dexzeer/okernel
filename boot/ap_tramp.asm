; Application-processor startup trampoline.
;
; An AP wakes in 16-bit real mode at CS:IP = (vector<<8):0 after an INIT +
; STARTUP IPI, with paging off and the first megabyte as its only memory.
; smp.c copies this blob to phys 0x8000 (SIPI vector 0x08), patches the three
; data slots below, then wakes the AP. The code takes the CPU real mode ->
; flat 32-bit protected mode -> paging on with the kernel's page directory
; (which identity-maps low memory, so the instruction stream keeps running
; across the switch) -> the AP's own stack -> the C entry point, a high
; address reached through the same page directory.
;
; Everything here is position dependent on TRAMP_PHYS: label arithmetic is
; assembled as constants (both labels live in this section).

%define TRAMP_PHYS 0x8000
%define OFF(x) ((x) - ap_tramp_start)
%define ABS(x) (OFF(x) + TRAMP_PHYS)

section .rodata
align 16
global ap_tramp_start
global ap_tramp_end
global ap_tramp_cr3
global ap_tramp_stack
global ap_tramp_entry

bits 16
ap_tramp_start:
    cli
    cld
    mov ax, cs                  ; CS = 0x0800, so DS-relative offsets work
    mov ds, ax
    lgdt [OFF(ap_gdtr)]
    mov eax, cr0
    or eax, 1                   ; CR0.PE
    mov cr0, eax
    jmp dword 0x08:ABS(ap_pm32) ; flush the prefetch queue, load a 32-bit CS

bits 32
ap_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr4
    or eax, 0x10                ; CR4.PSE: the kernel map has 4MB pages
    mov cr4, eax
    mov eax, [ABS(ap_tramp_cr3)]
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000001          ; PG | PE
    mov cr0, eax
    mov esp, [ABS(ap_tramp_stack)]
    mov eax, [ABS(ap_tramp_entry)]
    jmp eax                     ; high-half C entry; never returns

align 8
ap_gdt:
    dq 0x0000000000000000       ; null
    dq 0x00CF9A000000FFFF       ; 0x08 code, flat 4G
    dq 0x00CF92000000FFFF       ; 0x10 data, flat 4G
ap_gdtr:
    dw 3 * 8 - 1
    dd ABS(ap_gdt)

align 4
ap_tramp_cr3:   dd 0
ap_tramp_stack: dd 0
ap_tramp_entry: dd 0
ap_tramp_end:

bits 32
section .note.GNU-stack noalloc noexec nowrite progbits

; ISR and IRQ stubs for the IDT
; These are assembly trampolines that save CPU state and call C handlers

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push dword 0        ; dummy error code
    push dword %1       ; interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push dword %1       ; interrupt number (error code already on stack)
    jmp isr_common_stub
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    push dword 0        ; dummy error code
    push dword %2       ; interrupt number (32 + IRQ)
    jmp irq_common_stub
%endmacro

section .text

; CPU exception stubs
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14

; IRQ stubs (mapped to INT 32-47)
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; Common ISR stub — saves registers, calls C handler, restores
extern isr_handler
isr_common_stub:
    pusha               ; push edi, esi, ebp, esp, ebx, edx, ecx, eax

    mov ax, ds
    push eax            ; save data segment

    mov ax, 0x10        ; load kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push dword [esp + 36]  ; push interrupt number (4 ds + 32 pusha = 36)
    call isr_handler
    add esp, 4          ; clean up argument

    pop eax             ; restore data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa                ; restore registers
    add esp, 8          ; remove error code and interrupt number
    iret

; Common IRQ stub
extern irq_handler
irq_common_stub:
    pusha

    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push dword [esp + 36]  ; push interrupt number (4 ds + 32 pusha = 36)
    call irq_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8
    iret

; GDT flush — reload segment registers after GDT update
global gdt_flush
gdt_flush:
    mov eax, [esp + 4]  ; get pointer to GDT pointer
    lgdt [eax]          ; load GDT

    mov ax, 0x10        ; kernel data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush     ; far jump to reload CS with kernel code segment
.flush:
    ret

; Prevent executable stack warning (must be AFTER all code)
section .note.GNU-stack noalloc noexec nowrite progbits

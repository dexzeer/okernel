; Interrupt stubs for the local-APIC vectors (desktop kernel only).
; These vectors only ever reach application processors; they run on the kernel
; segments an AP always has, so no segment reload is needed.

section .text

extern smp_ipi_wake

; Wake-up IPI: the AP leaves hlt, the handler acknowledges it, and the worker
; loop looks at the job queue.
global isr_ipi_wake
isr_ipi_wake:
    pusha
    cld
    call smp_ipi_wake
    popa
    iret

; Spurious vector: the CPU needs a handler, and a spurious interrupt must not
; be acknowledged.
global isr_spurious
isr_spurious:
    iret

section .note.GNU-stack noalloc noexec nowrite progbits

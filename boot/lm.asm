; Interrupt entry and the way in and out of 64-bit user mode.
;
; Every IDT gate points at a tiny thunk (lm_vec_N) that pushes [error code] and the vector number and lands in lm_enter. lm_enter saves
; the registers as a struct trapframe (idt.h: the same layout as struct lx_regs) and calls the C handler registered with idt_set_vector.
; The CPU always pushes SS RSP RFLAGS CS RIP and aligns the stack to 16 bytes first, and the frame below that is 176 - 40 bytes long, so
; the handler is called with a correctly aligned stack.
;
; Linux subsystem tasks (src/lx.c) are run like this: the main loop calls lx_enter(), which saves its own context and irets into ring 3
; with the task's registers. Every way back into the kernel (the SYSCALL instruction, an interrupt, an exception) stores the task's
; registers in its struct lx_regs, because TSS.rsp0 points at the end of that struct, and "returns" from lx_enter into the main loop with a
; reason code. An interrupt is first handed to its handler as if the main loop itself had been interrupted at lx_irq_resume, so timer
; ticks, the keyboard and the NIC are served exactly as always.

bits 64
section .text

extern kernel_rsp0

%define LX_SYSCALL   1
%define LX_IRQ       2
%define LX_FAULT     3
%define LX_INT80     4

%define F_VEC   (15 * 8)
%define F_CS    (18 * 8)

; Vectors that push an error code: 8 10 11 12 13 14 17 21 29 30. Every thunk leaves [vec][err] on the stack (err = 0 when the CPU pushed none).
%assign v 0
%rep 256
lm_vec_ %+ v:
%if (v == 8) || ((v >= 10) && (v <= 14)) || (v == 17) || (v == 21) || (v == 29) || (v == 30)
    push qword v
%else
    push qword 0
    push qword v
%endif
    jmp lm_enter
%assign v v+1
%endrep

%macro PUSH_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

lm_enter:
    PUSH_REGS
    cld                                 ; C assumes DF=0; the interrupted code may be inside a backward rep movs. iretq restores its flags.
    test byte [rsp + F_CS], 3
    jz lm_kernel
    cmp dword [lx_running], 0
    jne lm_task
lm_kernel:
    mov rax, [rsp + F_VEC]
    mov rdi, rsp
    call [lm_handler + rax * 8]
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                         ; vec, err
    iretq

; ---- an interrupt or exception from a Linux subsystem task: the frame just built is the task's struct lx_regs --------------------------
lm_task:
    mov rax, [rsp + F_VEC]
    cmp eax, 32
    jb .fault
    cmp eax, 0x80
    je .int80
    ; A hardware interrupt: serve it as if the main loop had been interrupted at lx_irq_resume. Its iretq then lands there, with the stack
    ; pointer and the flags lx_enter left (interrupts on).
    mov dword [lx_running], 0
    mov rcx, [lx_main_rsp]
    mov rsp, rcx
    and rsp, -16
    push qword 0x10                     ; ss
    push rcx                            ; rsp
    mov rcx, [lx_main_rflags]
    or rcx, 0x200
    push rcx                            ; rflags
    push qword 0x08                     ; cs
    push qword lx_irq_resume            ; rip
    push qword 0                        ; err
    push rax                            ; vec
    PUSH_REGS
    jmp lm_kernel
.fault:
    mov rax, cr2
    mov [lx_fault_cr2], rax
    mov dword [lx_reason], LX_FAULT
    jmp lx_leave
.int80:
    mov dword [lx_reason], LX_INT80
    jmp lx_leave

; ---- the SYSCALL instruction ------------------------------------------------------------------------------
; rcx = user RIP, r11 = user RFLAGS, rsp = user stack, IF cleared by SFMASK, CS/SS = kernel segments.
; Build the same frame an interrupt would have (at the end of the task's struct lx_regs), then leave to the main loop.
; A program of the kernel's own (not a Linux subsystem task) traps like an interrupt: frame on its kernel stack, vector 0x80, back through iretq.
global lx_syscall_entry
lx_syscall_entry:
    cmp dword [lx_running], 0
    jne .task
    mov [lx_usp], rsp
    mov rsp, [kernel_rsp0]
    push qword 0x23                     ; ss
    push qword [lx_usp]                 ; rsp
    push r11                            ; rflags
    push qword 0x2B                     ; cs
    push rcx                            ; rip
    push qword 0                        ; err
    push qword 0x80                     ; vec
    PUSH_REGS
    cld
    jmp lm_kernel
.task:
    mov [lx_usp], rsp
    mov rsp, [lx_ktop]
    push qword 0x23                     ; ss
    push qword [lx_usp]                 ; rsp
    push r11                            ; rflags
    push qword 0x2B                     ; cs
    push rcx                            ; rip
    push qword 0                        ; err
    push qword 0x100                    ; vec: 0x100 marks a SYSCALL entry
    PUSH_REGS
    mov dword [lx_reason], LX_SYSCALL
    ; fall through

; Back to the main loop, as the return of lx_enter.
lx_leave:
    mov rsp, [lx_main_rsp]
    mov dword [lx_running], 0
lx_resume:
    push qword [lx_main_rflags]
    popfq
    mov eax, [lx_reason]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; The fake-interrupt landing point (see lm_task): the stack pointer is the main loop's again.
global lx_irq_resume
lx_irq_resume:
    mov dword [lx_reason], LX_IRQ
    jmp lx_resume

; uint32_t lx_enter(void) -- the registers to load are the struct lx_regs that ends at lx_ktop.
; Returns LX_* once the task traps; the task's registers are then in its struct lx_regs.
global lx_enter
lx_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq
    pop qword [lx_main_rflags]
    mov [lx_main_rsp], rsp
    cli
    mov dword [lx_running], 1
    mov rsp, [lx_ktop]
    sub rsp, 22 * 8                     ; &struct lx_regs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                         ; vec, err
    iretq                               ; rip cs rflags rsp ss

; ---- tables and state --------------------------------------------------------------------------------------
section .data
align 8
global lm_vec_table
lm_vec_table:
%assign v 0
%rep 256
    dq lm_vec_ %+ v
%assign v v+1
%endrep

section .bss
align 8
global lm_handler
lm_handler:     resq 256                ; the C handler of each vector: void fn(struct trapframe*)
global lx_ktop
lx_ktop:        resq 1                  ; end of the running task's struct lx_regs (TSS.rsp0)
lx_usp:         resq 1
global lx_fault_cr2
lx_fault_cr2:   resq 1
lx_main_rsp:    resq 1
lx_main_rflags: resq 1
global lx_reason
lx_reason:      resd 1
lx_running:     resd 1                  ; 1 while a Linux subsystem task owns ring 3 (the other ring-3 programs trap like the kernel does)

section .note.GNU-stack noalloc noexec nowrite progbits

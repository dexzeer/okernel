; Long-mode interrupt entry and 64-bit user mode for the desktop kernel.
;
; The CPU runs in long mode, but the kernel is 32-bit code that runs in COMPATIBILITY mode (CS = 0x08, flat 32-bit):
; every kernel address is below 4 GB, so nothing in it needed to change. Interrupts are the one thing long mode
; does differently: the IDT holds 16-byte gates that must point at 64-bit code, and the CPU pushes an 8-byte frame
; (SS RSP RFLAGS CS RIP, always all five) on a stack it picks from the 64-bit TSS (rsp0) when it comes from ring 3.
;
; Each of the 256 vectors gets a tiny 64-bit thunk (lm_vec_N) that pushes [error code] and the vector number and
; lands in lm_enter. From 32-bit code (kernel or 32-bit user programs), lm_enter turns the 64-bit frame back into
; the 32-bit frame the existing handlers (isr.asm, smp_stubs.asm) expect, switches to compatibility mode and jumps to
; the handler registered with idt_set_gate (lm_legacy[N]). The handler ends in a 32-bit `iret`, which returns
; straight to the interrupted 32-bit code: the 64-bit frame is dead after the conversion. Only r8-r15 are used before
; the switch (a 32-bit context cannot see them), so the interrupted eax..ebp reach the handler untouched.
;
; 64-bit user programs (the Linux subsystem, src/lx.c) are run like this: the main loop calls lx_enter(regs), which
; saves its own 32-bit context and irets into ring 3 with the task's registers. Every way back into the kernel (the
; SYSCALL instruction, an interrupt, an exception) stores the task's registers in `regs` and "returns" from
; lx_enter into the main loop with a reason code. An interrupt is first handed to the real handler as if the main
; loop itself had been interrupted at lx_irq_resume, so timer ticks, the keyboard and the NIC are served exactly as
; always. The registers are stored by pointing TSS.rsp0 at the end of the task's `regs` (struct lx_regs in lx.h).

bits 64
section .text

%define LX_SYSCALL   1
%define LX_IRQ       2
%define LX_FAULT     3
%define LX_INT80     4

; Vectors that push an error code: 8 10 11 12 13 14 17 21 29 30. Every thunk leaves [vec][err] on the stack
; (err = 0 when the CPU pushed none), so the frame is the same for all of them.
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

%define ERRMASK 0x60227D00

lm_enter:
    cmp qword [rsp + 24], 0x2B          ; saved CS = 64-bit user code?
    je lm_user64
    pop r9                              ; vector
    pop r15                             ; error code (0 if none)
    xor r14d, r14d                      ; r14 = 1 when the CPU pushed an error code (shapes the 32-bit frame)
    cmp r9d, 31
    ja .noerr
    mov r10d, ERRMASK
    bt r10d, r9d
    jnc .noerr
    inc r14d
.noerr:
    mov r10d, lm_legacy                 ; r10 = legacy handler of this vector (32-bit address)
    mov r10d, [r10 + r9*4]
    pop r11                             ; RIP
    pop r12                             ; CS
    pop r13                             ; RFLAGS
    pop r8                              ; RSP of the interrupted code
    pop r9                              ; SS     (rsp is now the end of the 64-bit frame)
    test r12b, 3
    jnz .ring3
.ring0:
    ; ring 0 (kernel in compatibility mode): the legacy frame goes just below the interrupted ESP, exactly where a
    ; 32-bit CPU would have pushed it, so the final `iret` leaves ESP as it was.
    mov r8d, r8d
    mov rsp, r8
    mov [rsp - 4], r13d
    mov [rsp - 8], r12d
    mov [rsp - 12], r11d
    sub rsp, 12
    jmp .frame_done
.ring3:
    ; ring 3 (32-bit user code): the legacy frame sits at the top of the kernel stack, five dwords including ESP:SS.
    mov [rsp - 4], r9d
    mov [rsp - 8], r8d
    mov [rsp - 12], r13d
    mov [rsp - 16], r12d
    mov [rsp - 20], r11d
    sub rsp, 20
.frame_done:
    test r14d, r14d
    jz .go
    mov [rsp - 4], r15d                 ; the error code is the lowest dword of the frame
    sub rsp, 4
.go:
    mov r9d, 0x10                       ; a ring-3 interrupt leaves SS null; compatibility mode needs a real stack segment
    mov ss, r9w
    push qword 0x08                     ; far return into the 32-bit kernel code segment
    push r10
    retfq

; ---- an interrupt or exception from 64-bit user code ------------------------------------------------------------
; Stack: [vec][err][rip][cs][rflags][rsp][ss], the top of the task's struct lx_regs. Save everything below it.
lm_user64:
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
    mov rax, [rsp + 15*8]               ; vector
    cmp eax, 32
    jb .fault
    cmp eax, 0x80
    je .int80
    ; A hardware interrupt: serve it with the real handler, as if the main loop had been interrupted at
    ; lx_irq_resume (ESP = what lx_enter left, flags = the main loop's with IF set); its `iret` then lands there.
    mov r10d, lm_legacy
    mov r10d, [r10 + rax*4]
    mov r11d, lx_irq_resume
    mov r12d, 0x08
    mov r13d, [lx_main_eflags]
    or r13d, 0x200
    mov r8d, [lx_main_esp]
    xor r14d, r14d                      ; IRQs push no error code
    xor r15d, r15d
    jmp lm_enter.ring0
.fault:
    mov rax, cr2
    mov [lx_fault_cr2], rax
    mov dword [lx_reason], LX_FAULT
    jmp lx_leave64
.int80:
    mov dword [lx_reason], LX_INT80
    jmp lx_leave64

; ---- the SYSCALL instruction ------------------------------------------------------------------------------
; rcx = user RIP, r11 = user RFLAGS, rsp = user stack, IF cleared by SFMASK, CS/SS = kernel segments 0x30/0x38.
; Build the same frame an interrupt would have (rsp0 area at lx_ktop), then leave to the main loop.
global lx_syscall_entry
lx_syscall_entry:
    mov [lx_usp], rsp
    mov rsp, [lx_ktop]
    push qword 0x23                     ; ss
    push qword [lx_usp]                 ; rsp
    push r11                            ; rflags
    push qword 0x2B                     ; cs
    push rcx                            ; rip
    push qword 0                        ; err
    push qword 0x100                    ; vec: 0x100 marks a SYSCALL entry
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
    mov dword [lx_reason], LX_SYSCALL
    ; fall through

; Back to the 32-bit main loop: its stack, a flat kernel stack segment, and a far return into compatibility mode.
lx_leave64:
    mov esp, [lx_main_esp]
    mov r9d, 0x10
    mov ss, r9w
    push qword 0x08
    mov r10d, lx_resume32
    push r10
    retfq

; ---- 32-bit side ----------------------------------------------------------------------------------------
bits 32

; uint32_t lx_enter(void) -- the registers to load are the struct lx_regs at lx_ktop - sizeof(struct lx_regs).
; Returns LX_* once the task traps; the task's registers are in its struct lx_regs.
global lx_enter
lx_enter:
    push ebx
    push esi
    push edi
    push ebp
    pushfd
    pop eax
    mov [lx_main_eflags], eax
    mov [lx_main_esp], esp
    cli
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    jmp 0x30:lx_enter64

; The fake-interrupt landing point (see lm_user64): ESP and EFLAGS are the main loop's again.
global lx_irq_resume
lx_irq_resume:
    mov dword [lx_reason], LX_IRQ
lx_resume32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword [lx_main_eflags]
    popfd
    mov eax, [lx_reason]
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

bits 64
lx_enter64:
    mov esp, [lx_ktop]
    sub rsp, 22*8                       ; &struct lx_regs
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
align 4
global lm_vec_table
lm_vec_table:
%assign v 0
%rep 256
    dd lm_vec_ %+ v
%assign v v+1
%endrep

section .bss
align 8
global lm_legacy
lm_legacy:
    resd 256
global lx_ktop
lx_ktop:        resq 1                  ; end of the running task's struct lx_regs (TSS.rsp0)
lx_usp:         resq 1
global lx_fault_cr2
lx_fault_cr2:   resq 1
global lx_reason
lx_reason:      resd 1
global lx_main_esp
lx_main_esp:    resd 1
global lx_main_eflags
lx_main_eflags: resd 1

section .note.GNU-stack noalloc noexec nowrite progbits

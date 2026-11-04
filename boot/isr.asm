; Segment reload, ring-3 entry for the kernel's own programs, and the kernel-thread switch. Interrupt entry is in lm.asm.

bits 64
section .text

; void gdt_flush(void* gdt_pointer): load the GDT and reload the segment registers.
global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    pop rax                   ; the return address
    push qword 0x08
    push rax
    retfq                     ; reloads CS and returns

; ---- enter_user_mode ----
; void enter_user_mode(uint64_t user_rip, uint64_t user_rsp)
; Saves the caller's callee-saved registers and stack pointer, then irets into ring 3. It "returns" when the program exits or parks: the
; trap handler jumps to user_exit_trampoline / user_park_trampoline, which discard the trap stack and return to the caller of this function.
; Fork-child RAX: enter reads user_fork_child (set by enter_user_mode_fork_child) and zeroes RAX when set, so the child observes fork() == 0.
; Park-resume: RAX comes from user_park_retval (user_park_ret_pending) and every other register from user_resume_regs, as they were at the trap
; the program parked on (syscall_load_park_regs fills them).
global enter_user_mode
global enter_user_mode_fork_child
global enter_user_mode_park_ret
global user_fork_child
global user_park_ret_pending
global user_park_retval
enter_user_mode:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [user_ret_rsp], rsp
    push qword 0x23           ; SS
    push rsi                  ; RSP
    push qword 0x3202         ; RFLAGS (IF=1, IOPL=3, reserved bit 1)
    push qword 0x2B           ; CS (64-bit user code)
    push rdi                  ; RIP
    xor eax, eax
    cmp dword [user_fork_child], 0
    je .no_fork
    mov dword [user_fork_child], 0
    jmp .rax_done
.no_fork:
    cmp dword [user_park_ret_pending], 0
    je .rax_done
    mov dword [user_park_ret_pending], 0
    mov eax, [user_park_retval]
    mov rbx, [user_resume_regs + 0 * 8]
    mov rbp, [user_resume_regs + 1 * 8]
    mov r12, [user_resume_regs + 2 * 8]
    mov r13, [user_resume_regs + 3 * 8]
    mov r14, [user_resume_regs + 4 * 8]
    mov r15, [user_resume_regs + 5 * 8]
    mov rcx, [user_resume_regs + 6 * 8]
    mov rdx, [user_resume_regs + 7 * 8]
    mov rsi, [user_resume_regs + 8 * 8]
    mov rdi, [user_resume_regs + 9 * 8]
    mov r8,  [user_resume_regs + 10 * 8]
    mov r9,  [user_resume_regs + 11 * 8]
    mov r10, [user_resume_regs + 12 * 8]
    mov r11, [user_resume_regs + 13 * 8]
    iretq
.rax_done:
    ; nothing of the kernel's stays in the registers
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; Flag the NEXT enter_user_mode IRET as a fork-child entry (RAX forced 0).
enter_user_mode_fork_child:
    mov dword [user_fork_child], 1
    ret

; Stage the RAX value for the NEXT enter_user_mode IRET (park-resume path).
enter_user_mode_park_ret:
    mov [user_park_retval], edi
    mov dword [user_park_ret_pending], 1
    ret

; ---- context_switch: preemptive kernel-thread switch ----
; void context_switch(uintptr_t *old_rsp_p, uintptr_t new_rsp, uintptr_t new_cr3);
; IRQs OFF across the call. Saves the outgoing callee-saved registers on its stack and the stack pointer in *old_rsp_p, loads CR3 and the
; incoming stack, and returns into the incoming thread as if its own switch call returned.
global context_switch
context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov cr3, rdx
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; Resume points of the ring-3 programs: the caller of enter_user_mode continues as if the call had returned. The trap stack is abandoned
; (its frame never irets). cli: a timer IRQ here would bury the frame. The exit and park paths differ only in what the C side recorded.
global user_exit_trampoline
global user_park_trampoline
user_exit_trampoline:
user_park_trampoline:
    cli
    mov rsp, [user_ret_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; DATA ONLY here: never put code after a .bss/.note section.
section .bss
align 8
global user_ret_rsp
user_ret_rsp: resq 1
global user_resume_regs
user_resume_regs: resq 14
user_fork_child: resd 1
user_park_ret_pending: resd 1
user_park_retval: resd 1

; Prevent the executable-stack warning (must be LAST: code after it lands non-executable; see AGENTS.md)
section .note.GNU-stack noalloc noexec nowrite progbits

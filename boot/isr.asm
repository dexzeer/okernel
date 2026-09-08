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

; ---- enter_user_mode ----
; void enter_user_mode(uint32_t user_eip, uint32_t user_esp)
; NAKED LEAF, register convention: eip arrives in EAX, esp in EDX (the caller
; does add-esp AFTER the call — push esi/push ebx + call — so ESP on entry
; points AT the return address; save ESP+4 as the caller's frame top).
; IRET frame needs SS:RPL=3 (0x23) and CS:RPL=3 (0x1B). User data segments
; load BEFORE iret (ring-0 selector in DS at ring 3 = GP on first access).
; Fork-child EAX: enter reads user_fork_child (set by enter_user_mode_fork_
; child) and zeroes EAX when set (consumes the flag), so the child observes
; fork() == 0. Park-resume EAX: same mechanism via user_park_ret_pending /
; user_park_retval (set by the drain from sched_park_take before enter).
; Plain path leaves EAX from the lea (kernel ESP — harmless: spawns ignore
; EAX; only fork/park children depend on it and they take flagged paths).
global enter_user_mode
global enter_user_mode_fork_child
global user_fork_child
global user_park_ret_pending
global user_park_retval
enter_user_mode:
    mov ebx, eax              ; ebx = user_eip (from caller: "a")
    mov ecx, edx              ; ecx = user_esp (from caller: "d")
    ; Save the full caller frame for the trampolines (EBP/ESI/EDI + ESP top;
    ; EBX NOT saved — it carries user_eip; trampolines must not restore it).
    lea eax, [esp + 4]
    mov [user_ret_esp], eax
    mov [user_ret_ebp], ebp
    mov [user_ret_esi], esi
    mov [user_ret_edi], edi
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword 0x23           ; SS (index 4 | RPL 3)
    push ecx                  ; ESP
    push dword 0x3202         ; EFLAGS (IF=1, IOPL=3, reserved bit1)
    push dword 0x1B           ; CS (index 3 | RPL 3)
    push ebx                  ; EIP
    cmp dword [user_fork_child], 0
    je .no_fork_eax
    mov dword [user_fork_child], 0
    xor eax, eax
    jmp .eax_done
.no_fork_eax:
    cmp dword [user_park_ret_pending], 0
    je .eax_done
    mov dword [user_park_ret_pending], 0
    mov eax, [user_park_retval]
.eax_done:
    iret

; Flag the NEXT enter_user_mode IRET as a fork-child entry (EAX forced 0).
; Plain ring-0 call, runs BEFORE the enter inline asm in the main loop.
enter_user_mode_fork_child:
    mov dword [user_fork_child], 1
    ret

; Stage the EAX value for the NEXT enter_user_mode IRET (park-resume path).
; Plain cdecl ring-0 call, runs BEFORE the enter inline asm.
global enter_user_mode_park_ret
enter_user_mode_park_ret:
    mov eax, [esp + 4]
    mov [user_park_retval], eax
    mov dword [user_park_ret_pending], 1
    ret

; ---- context_switch: preemptive kernel-thread switch (Phase 2) ----
; void context_switch(uint32_t *old_esp_p, uint32_t new_esp, uint32_t new_cr3);
; Naked asm, IRQs OFF across the call. Saves outgoing (EBX/ESI/EDI/EBP/ESP +
; return EIP) into *old_esp_p's stack frame, loads CR3 (full TLB flush), then
; ESP + regs, RETs into the incoming thread as if its switch call returned.
; Stack layout on entry: [ret][old_esp_p][new_esp][new_cr3]; after 4 pushes
; args sit at esp+20/24/28. Clobbers EAX/ECX/EDX (caller-saved).
global context_switch
context_switch:
    push ebx
    push esi
    push edi
    push ebp
    mov eax, [esp + 20]       ; old_esp_p
    mov [eax], esp            ; *old_esp_p = ESP (points at saved EBP)
    mov eax, [esp + 28]       ; new_cr3
    mov cr3, eax
    mov ecx, [esp + 24]       ; new_esp
    mov esp, ecx
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; Resume point: idt.c JMPS here (never calls) with EAX = saved caller ESP.
; Restores segments + EBP/ESI/EDI/ESP, jumps to [saved-4] (the CALL's return
; address: saved = retaddr+4). Trap stack discarded (stub never resumes).
; EBX NOT restored (see above). cli: a timer IRQ here would bury the frame.
global user_exit_trampoline
user_exit_trampoline:
    cli
    mov ecx, eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ebp, [user_ret_ebp]
    mov esi, [user_ret_esi]
    mov edi, [user_ret_edi]
    mov esp, ecx
    jmp [esp - 4]

; Park trampoline: same resume as exit (blocking syscalls — wait/yield/read —
; park the caller as BLOCKED and return straight to the main loop, which runs
; queued siblings; the drain re-enters the parker with its staged EAX).
global user_park_trampoline
user_park_trampoline:
    cli
    mov ecx, eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ebp, [user_ret_ebp]
    mov esi, [user_ret_esi]
    mov edi, [user_ret_edi]
    mov esp, ecx
    jmp [esp - 4]

; Saved caller frame for the resume points. .bss: zero-init. DATA ONLY here —
; never put CODE after a .bss/.note section (triple-fault; see AGENTS.md).
section .bss
align 4
global user_ret_esp
user_ret_esp: resd 1
global user_ret_ebp
user_ret_ebp: resd 1
global user_ret_esi
user_ret_esi: resd 1
global user_ret_edi
user_ret_edi: resd 1
global user_fork_child
user_fork_child: resd 1
global user_park_ret_pending
user_park_ret_pending: resd 1
global user_park_retval
user_park_retval: resd 1

; Prevent executable stack warning (must be LAST — code after it lands non-executable; see AGENTS.md)
section .note.GNU-stack noalloc noexec nowrite progbits

; call_on_stack(fn, arg, stack_top): run fn(arg) on a private stack.
;
; The boot stack is 256KB; the web engine (HTML tree builder, nested layout,
; selector matching) can recurse deeper than that on real pages, so okai runs
; it on a dedicated heap stack. Interrupts taken while on it nest on it too
; (ring 0 -> ring 0 has no stack switch) — it lives in the shared kernel half,
; so it is mapped in every address space.
;
; engine_main_sp is the stack pointer of the stack this call came from, valid while fn runs (restored on
; return, so nested calls work): everything below it is free, which is where the engine hands control
; back to the main loop's pollers (okai.c engine_pump) without growing the engine stack.
;
;   void call_on_stack(void (*fn)(void*), void* arg, void* stack_top);

section .text
global call_on_stack
global engine_main_sp
call_on_stack:
    push ebp
    mov ebp, esp
    push dword [engine_main_sp]     ; the enclosing value, put back below
    mov [engine_main_sp], esp
    mov eax, [ebp + 8]      ; fn
    mov ecx, [ebp + 12]     ; arg
    mov edx, [ebp + 16]     ; stack top
    and edx, 0xFFFFFFF0     ; 16-byte align
    mov esp, edx
    sub esp, 12             ; keep the call site 16-byte aligned after the push
    push ecx
    call eax
    lea esp, [ebp - 4]      ; back to the caller's stack, at the saved value
    pop dword [engine_main_sp]
    pop ebp
    ret

section .bss
engine_main_sp: resd 1

; Must be LAST (see AGENTS.md: code after it lands non-executable)
section .note.GNU-stack noalloc noexec nowrite progbits

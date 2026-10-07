; call_on_stack(fn, arg, stack_top): run fn(arg) on a private stack.
;
; The boot stack is 256KB; the web engine (HTML tree builder, nested layout,
; selector matching) can recurse deeper than that on real pages, so okai runs
; it on a dedicated heap stack. Interrupts taken while on it nest on it too
; (ring 0 -> ring 0 has no stack switch) — it lives in the shared kernel half,
; so it is mapped in every address space.
;
;   void call_on_stack(void (*fn)(void*), void* arg, void* stack_top);

section .text
global call_on_stack
call_on_stack:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]      ; fn
    mov ecx, [ebp + 12]     ; arg
    mov edx, [ebp + 16]     ; stack top
    and edx, 0xFFFFFFF0     ; 16-byte align
    mov esp, edx
    sub esp, 12             ; keep the call site 16-byte aligned after the push
    push ecx
    call eax
    mov esp, ebp            ; back to the caller's stack
    pop ebp
    ret

; Must be LAST (see AGENTS.md: code after it lands non-executable)
section .note.GNU-stack noalloc noexec nowrite progbits

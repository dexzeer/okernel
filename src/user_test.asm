; user_test.asm — User-mode test program (runs in ring 3)
; POSITION-INDEPENDENT: the kernel copies these bytes to a low user page
; (0x08048000) and enters there, so nothing here may use absolute addresses.
; msg is addressed RIP-relative-style via call/pop (works at any base).
; Ring-3 entry loads DS/ES/FS/GS with 0x23 (enter_user_mode already does,
; belt and suspenders). Uses RPL=3 selectors everywhere: bare 0x20 faults
; as GP(selector=0x20) the moment a segment register is loaded.

section .text
global user_mode_test
global user_mode_test_end

user_mode_test:
    ; We're now in ring 3! Set up user data segment
    mov ax, 0x23        ; GDT_USER_DATA | RPL 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; ebx = address of msg (call/pop trick: no absolute addr)
    call .get_pc
.get_pc:
    pop ebx
    add ebx, msg - .get_pc

    ; Test: call int 0x80 with syscall number 0 (print)
    ; eax = syscall number, ebx = argument
    mov eax, 0          ; syscall 0 = print
    int 0x80

    ; Exercise the extended ABI (each prints its own serial proof line):
    ; getpid -> eax, write(fd=1) of a second line, yield, sbrk, mmap.
    mov eax, 3          ; syscall 3 = getpid
    int 0x80
    ; write(1, msg2, len): reuse the call/pop base in ebx (still = msg).
    push ebx
    call .get_pc2
.get_pc2:
    pop ecx
    add ecx, msg2 - .get_pc2
    mov ebx, 1          ; fd 1 = terminal
    mov edx, msg2len
    mov eax, 2          ; syscall 2 = write
    int 0x80
    pop ebx
    mov eax, 4          ; syscall 4 = yield
    int 0x80
    mov ebx, 4096
    mov eax, 11         ; syscall 11 = sbrk(4096)
    int 0x80
    mov ebx, 0          ; auto-pick
    mov eax, 16         ; syscall 16 = mmap
    int 0x80

    ; Test: syscall number 1 (exit)
    mov eax, 1          ; syscall 1 = exit
    int 0x80

    ; Should never reach here (exit should return to kernel)
    ; If we do, loop forever
.hang:
    jmp .hang

msg: db "Hello from user mode!", 0
msg2: db "ring-3 write() works", 10, 0
msg2len equ $ - msg2 - 1
user_mode_test_end:

; Prevent executable stack warning
section .note.GNU-stack noalloc noexec nowrite progbits

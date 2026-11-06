; A DOS program for tests/headless/doom_win.ps1 and manual runs: 440 Hz on the PC speaker for three seconds of the emulated clock.
; nasm -f bin -o beep.com beep.asm
    bits 16
    org 0x100

start:
    mov al, 0xB6
    out 0x43, al
    mov ax, 2712                    ; 1193182 / 440
    out 0x42, al
    mov al, ah
    out 0x42, al
    in al, 0x61
    or al, 3
    out 0x61, al
    xor ah, ah
    int 0x1A
    mov bx, dx
.wait:
    xor ah, ah
    int 0x1A
    sub dx, bx
    cmp dx, 55
    jb .wait
    in al, 0x61
    and al, 0xFC
    out 0x61, al
    mov ax, 0x4C00
    int 0x21

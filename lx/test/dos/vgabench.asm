; A DOS program for tests/headless/test_lx_dosbox.py: draws a moving pattern in VGA mode 13h for 10 seconds of the emulated
; clock, one pixel at a time so the CPU core of the emulator has real work, one frame per vertical retrace, then writes
; "frames N ticks T" to BENCH.TXT. Esc stops it. nasm -f bin -o vgabench.com vgabench.asm
    bits 16
    org 0x100

RUNTICKS equ 182                    ; 18.2 ticks a second

start:
    mov ax, 0x0013
    int 0x10
    mov ax, 0xA000
    mov es, ax
    xor ah, ah
    int 0x1A
    mov [t0], dx

frame:
    xor di, di
    mov bx, [frames]
    mov dx, 200
.row:
    mov cx, 320
    mov al, dl
    add al, bl
.px:
    mov ah, al
    xor ah, cl
    and ah, 0x7F
    add ah, 32
    mov [es:di], ah
    inc di
    add al, 1
    dec cx
    jnz .px
    dec dx
    jnz .row
    mov dx, 0x3DA
.notret:
    in al, dx
    test al, 8
    jnz .notret
.ret:
    in al, dx
    test al, 8
    jz .ret
    inc word [frames]
    mov ah, 1
    int 0x16
    jz .nokey
    xor ah, ah
    int 0x16
    cmp al, 27
    je done
.nokey:
    xor ah, ah
    int 0x1A
    sub dx, [t0]
    mov [ticks], dx
    cmp dx, RUNTICKS
    jb frame

done:
    mov ax, 0x0003
    int 0x10
    mov ax, cs
    mov es, ax
    mov di, line
    mov si, s_frames
    call puts
    mov ax, [frames]
    call putn
    mov si, s_ticks
    call puts
    mov ax, [ticks]
    call putn
    mov ax, 0x0A0D
    stosw
    mov ah, 0x3C
    xor cx, cx
    mov dx, name
    int 0x21
    jc .end
    mov bx, ax
    mov ah, 0x40
    mov cx, di
    sub cx, line
    mov dx, line
    int 0x21
    mov ah, 0x3E
    int 0x21
.end:
    mov ax, 0x4C00
    int 0x21

puts:
    lodsb
    test al, al
    jz .e
    stosb
    jmp puts
.e:
    ret

putn:                               ; ax in decimal at es:di
    mov bx, 10
    xor cx, cx
.d:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .d
.o:
    pop ax
    add al, '0'
    stosb
    loop .o
    ret

name:     db "BENCH.TXT", 0
s_frames: db "frames ", 0
s_ticks:  db " ticks ", 0
frames:   dw 0
ticks:    dw 0
t0:       dw 0
line:     times 40 db 0

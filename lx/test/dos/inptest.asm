; A DOS program for tests/headless/test_lx_dosbox.py: reads the mouse driver (INT 33h) and the keyboard (INT 16h) until Esc,
; then writes what it saw to INPUT.TXT: "mouse P mx N my N btn B scan S text T". nasm -f bin -o inptest.com inptest.asm
    bits 16
    org 0x100

start:
    xor ax, ax
    int 0x33
    mov [present], ax
    mov ax, cs
    mov es, ax
    mov word [tp], text

poll:
    mov ax, 0x000B
    int 0x33
    add [mx], cx
    add [my], dx
    mov ax, 0x0003
    int 0x33
    or [btn], bx
    mov ah, 1
    int 0x16
    jz poll
    xor ah, ah
    int 0x16
    cmp al, 27
    je done
    mov [scan], ah
    test al, al
    jz poll
    cmp al, 0xE0
    je poll
    mov di, [tp]
    cmp di, text + 30
    jae poll
    stosb
    mov [tp], di
    jmp poll

done:
    mov di, line
    mov si, s_mouse
    call puts
    mov ax, [present]
    call putu
    mov si, s_mx
    call puts
    mov ax, [mx]
    call puts16
    mov si, s_my
    call puts
    mov ax, [my]
    call puts16
    mov si, s_btn
    call puts
    mov ax, [btn]
    call putu
    mov si, s_scan
    call puts
    xor ah, ah
    mov al, [scan]
    call putu
    mov si, s_text
    call puts
    mov si, text
    call puts
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

puts16:                             ; ax signed
    test ax, ax
    jns putu
    push ax
    mov al, '-'
    stosb
    pop ax
    neg ax
putu:                               ; ax unsigned
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

name:    db "INPUT.TXT", 0
s_mouse: db "mouse ", 0
s_mx:    db " mx ", 0
s_my:    db " my ", 0
s_btn:   db " btn ", 0
s_scan:  db " scan ", 0
s_text:  db " text ", 0
present: dw 0
mx:      dw 0
my:      dw 0
btn:     dw 0
scan:    db 0
tp:      dw 0
text:    times 32 db 0
line:    times 100 db 0

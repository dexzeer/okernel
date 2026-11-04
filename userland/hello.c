// hello.c — first real userland program (/bin/hello).
// Freestanding i386 ET_EXEC: raw int 0x80, no libc. Entry ABI from the
// kernel's ELF spawn: ESP -> [argc][argv][envp], EIP = _start.
// Syscalls used: 0 print(msg), 2 write(1,buf,len), 3 getpid, 1 exit(status).
// Build: gcc -m32 -nostdlib -static -e _start -o hello hello.c (host gcc
// with multilib emits a valid ET_EXEC i386; the kernel validates).

#include "usys.h"

// The entry stack is [argc][argv][envp] (the kernel's ELF spawn): _start hands that RSP to hmain, so no guess about what the compiler's
// prologue pushed.
__asm__(".globl _start\n_start:\n    mov %rsp, %rdi\n    call hmain\n");

void hmain(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)sp[1];
    usys_print("hello from userland");
    {
        // write(1, "pid=", ...) — tiny itoa inline (no libc).
        int pid = usys_getpid();
        char buf[32];
        int n = 0;
        buf[n++] = 'p'; buf[n++] = 'i'; buf[n++] = 'd'; buf[n++] = '=';
        char rev[12]; int ri = 0;
        if (pid == 0) rev[ri++] = '0';
        while (pid > 0 && ri < 11) { rev[ri++] = '0' + (pid % 10); pid /= 10; }
        while (ri > 0) buf[n++] = rev[--ri];
        buf[n++] = '\n';
        usys_write(1, buf, n);
    }
    {
        // echo argv[1..] back (proves argc/argv stack build).
        for (int i = 1; i < argc; i++) {
            const char *s = argv[i];
            unsigned len = 0;
            while (s[len]) len++;
            usys_write(1, s, len);
            usys_write(1, "\n", 1);
        }
    }
    usys_exit(0);
}

// hello.c — first real userland program (/bin/hello).
// Freestanding i386 ET_EXEC: raw int 0x80, no libc. Entry ABI from the
// kernel's ELF spawn: ESP -> [argc][argv][envp], EIP = _start.
// Syscalls used: 0 print(msg), 2 write(1,buf,len), 3 getpid, 1 exit(status).
// Build: gcc -m32 -nostdlib -static -e _start -o hello hello.c (host gcc
// with multilib emits a valid ET_EXEC i386; the kernel validates).

static int sys_print(const char *msg) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(msg) : "memory", "ecx", "edx");
    return r;
}

static int sys_write(int fd, const char *buf, unsigned len) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(2), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return r;
}

static int sys_getpid(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(3) : "memory", "ecx", "edx");
    return r;
}

static void sys_exit(int code) {
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code) : "memory");
    while (1) { }
}

void _start(void) {
    // Read argc/argv from the kernel-built stack. NOTE: this prologue
    // (push ebp/mov esp,ebp + pushes) shifts ESP by 16, so read the entry
    // ESP back from EBP (entry ESP == EBP after `mov esp,ebp`): ESP_at_entry
    // -> [argc][argv][envp]. Do NOT use a live ESP read (it points 16+ bytes
    // into our own frame — the classic argc=0 bug).
    int argc;
    char **argv;
    __asm__ volatile("mov %%ebp, %%eax; mov (%%eax), %%ecx; mov 4(%%eax), %%edx"
                     : "=c"(argc), "=d"(argv) : : "eax", "memory");
    sys_print("hello from userland");
    {
        // write(1, "pid=", ...) — tiny itoa inline (no libc).
        int pid = sys_getpid();
        char buf[32];
        int n = 0;
        buf[n++] = 'p'; buf[n++] = 'i'; buf[n++] = 'd'; buf[n++] = '=';
        char rev[12]; int ri = 0;
        if (pid == 0) rev[ri++] = '0';
        while (pid > 0 && ri < 11) { rev[ri++] = '0' + (pid % 10); pid /= 10; }
        while (ri > 0) buf[n++] = rev[--ri];
        buf[n++] = '\n';
        sys_write(1, buf, n);
    }
    {
        // echo argv[1..] back (proves argc/argv stack build).
        for (int i = 1; i < argc; i++) {
            const char *s = argv[i];
            unsigned len = 0;
            while (s[len]) len++;
            sys_write(1, s, len);
            sys_write(1, "\n", 1);
        }
    }
    sys_exit(0);
}

// Third probe: fork child does sys_read(0) on an EMPTY queue (parks), then
// prints what it got. Isolates read-park resume vs fork resume.
static int sys_fork(void) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(9) : "memory", "ecx", "edx"); return r; }
static int sys_read(int fd, char *b, unsigned l) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(6), "b"(fd), "c"(b), "d"(l) : "memory"); return r; }
static int sys_print(const char *m) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(m) : "memory", "ecx", "edx"); return r; }
static void sys_exit(int c) { __asm__ volatile("int $0x80" : : "a"(1), "b"(c) : "memory"); while (1) {} }
void _start(void) {
    sys_print("dbg3: starting");
    static char buf[64];
    int c = sys_fork();
    if (c < 0) { sys_print("dbg3: fork failed"); sys_exit(1); }
    if (c == 0) {
        sys_print("dbg3: child reading fd0 (queue drained or stale)...");
        int n = sys_read(0, buf, 63);
        if (n < 0) { sys_print("dbg3: child read err"); sys_exit(2); }
        buf[63] = 0;
        if (n >= 0 && n < 64) buf[n] = 0;
        sys_print("dbg3: child got:");
        sys_print(buf);
        sys_exit(7);
    }
    sys_print("dbg3: parent done");
    sys_exit(0);
}

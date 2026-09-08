// Fork-then-exec probe: child execs hello immediately (no read, no parse).
// If THIS faults, the bug is fork-x-exec, not sh's line buffer.
static int sys_fork(void) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(9) : "memory", "ecx", "edx"); return r; }
static int sys_exec(const char *p) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(10), "b"(p) : "memory", "ecx", "edx"); return r; }
static int sys_wait(int p) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(14), "b"(p) : "memory", "ecx", "edx"); return r; }
static int sys_print(const char *m) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(m) : "memory", "ecx", "edx"); return r; }
static void sys_exit(int c) { __asm__ volatile("int $0x80" : : "a"(1), "b"(c) : "memory"); while (1) {} }
void _start(void) {
    sys_print("forkexec: starting");
    int c = sys_fork();
    if (c < 0) { sys_print("forkexec: fork failed"); sys_exit(1); }
    if (c == 0) { sys_exec("/bin/hello"); sys_print("forkexec: exec failed"); sys_exit(1); }
    for (int i = 0; i < 10000000; i++) { int g = sys_wait(c); if (g == c) break; if (g == -1) break; }
    sys_print("forkexec: child reaped");
    sys_exit(0);
}

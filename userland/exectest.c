// exectest.c — prove exec() (/bin/exectest).
// Prints BEFORE, execs /bin/hello (which prints its own lines), and — if
// exec returns — prints AFTER with the return code (exec only returns on
// failure).

static int sys_print(const char *msg) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(msg) : "memory", "ecx", "edx");
    return r;
}

static int sys_exec(const char *path) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(10), "b"(path) : "memory", "ecx", "edx");
    return r;
}

static void sys_exit(int code) {
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code) : "memory");
    while (1) { }
}

void _start(void) {
    sys_print("exectest: BEFORE exec");
    int rc = sys_exec("/bin/hello");
    // Only reached on failure.
    sys_print("exectest: exec returned (failure)");
    sys_exit(rc < 0 ? 1 : rc);
}

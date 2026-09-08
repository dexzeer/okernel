// forktest.c — prove fork()/wait() (/bin/forktest).
// Parent forks; child prints CHILD and exits(7); parent waits, prints the
// reaped pid, exits(0). Serial + window both show the sequence.

static int sys_print(const char *msg) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(msg) : "memory", "ecx", "edx");
    return r;
}

static int sys_fork(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(9) : "memory", "ecx", "edx");
    return r;
}

static int sys_wait(int pid) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(14), "b"(pid) : "memory", "ecx", "edx");
    return r;
}

static void sys_exit(int code) {
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code) : "memory");
    while (1) { }
}

void _start(void) {
    // Prove the child path FIRST (serial order proof even if wait breaks).
    sys_print("forktest: starting");
    int child = sys_fork();
    if (child < 0) {
        sys_print("forktest: fork failed");
        sys_exit(1);
    }
    if (child == 0) {
        sys_print("forktest: CHILD running");
        sys_exit(7);
    }
    sys_print("forktest: parent waiting");
    int got = 0;
    for (int i = 0; i < 10000000; i++) {
        got = sys_wait(-1);
        if (got > 0) break;
        if (got == -1) break; // no child at all (real error)
        // got == -2 (WAIT_PARK): parked, child alive but not exited — retry.
    }
    if (got > 0) sys_print("forktest: reaped child");
    else sys_print("forktest: wait found nothing");
    sys_exit(0);
}

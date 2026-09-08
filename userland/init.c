// init.c — PID 1 (/sbin/init). Spawned by the kernel at boot once the VFS
// is up. Job: spawn the user shell on the console, then reap orphans
// forever (wait(-1) loop). Never exits (halts if wait fails forever).

static int sys_fork(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(9) : "memory", "ecx", "edx");
    return r;
}

static int sys_exec(const char *path) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(10), "b"(path) : "memory", "ecx", "edx");
    return r;
}

static int sys_wait(int pid) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(14), "b"(pid) : "memory", "ecx", "edx");
    return r;
}

static int sys_print(const char *msg) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(msg) : "memory", "ecx", "edx");
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

static int sys_yield(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(4) : "memory", "ecx", "edx");
    return r;
}

void _start(void) {
    sys_print("init: starting user shell");
    {
        // Diagnostic: prove getpid + exec-trap path work in THIS process
        // before forking (bisect: child exec never dispatched).
        int me = sys_getpid();
        if (me == 1) sys_print("init: I am pid 1");
        else sys_print("init: BAD PID (not 1)");
    }
    int runs = 0;
    for (;;) {
        int child = sys_fork();
        if (child < 0) {
            sys_print("init: fork failed, halting");
            sys_exit(1);
        }
        if (child == 0) {
            // Child: become the shell (exec never returns on success).
            // NOTE: no syscalls here before exec except exec itself — the
            // fork-child entry resumes at user_eip (image entry, see
            // sys_proc_fork limits), NOT at the trapped fork site, so any
            // child-side print would run twice/confuse ordering. Exec first.
            int rc = sys_exec("/bin/sh");
            sys_print("init: exec returned!");
            {
                // Report the return code (proves EAX writeback on exec-fail).
                if (rc == -1) sys_print("init: rc=-1 (not found?)");
                else sys_print("init: rc other (redirect broken?)");
            }
            sys_print("init: cannot exec /bin/sh");
            sys_exit(1);
        }
        // Parent (init): wait for the shell, then restart it (respawn).
        // Also reaps any reparented orphans along the way (wait(-1)).
        runs++;
        int spins = 0;
        for (;;) {
            int got = sys_wait(-1);
            if (got == child) break; // shell exited → respawn
            if (got == -1) break;    // no children (shouldn't happen)
            // got == -2 (parked) or an orphan reap: yield EVERY iteration
            // (not 1-in-64). A spinning waiter starves the child it waits
            // for: with SLICE=10 ticks the child may not run for ~100ms per
            // steal, and during that window every wait parks again — the
            // parent's spin is pure serial-log flood + stolen slices. Yield
            // hands the slice straight to the child (tick picks next READY).
            if (got == -2) sys_yield();
            else spins++;
            // else: reaped an orphan, keep waiting for shell
        }
        if (runs > 1000000) sys_exit(0); // sanity bound (never hit)
    }
}

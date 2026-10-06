// init.c — PID 1 (/sbin/init). Spawned by the kernel at boot once the VFS
// is up. Job: spawn the user shell on the console, then reap orphans
// forever (wait(-1) loop). Never exits (halts if wait fails forever).

#include "usys.h"

void _start(void) {
    usys_print("init: starting user shell");
    {
        // Diagnostic: prove getpid + exec-trap path work in THIS process
        // before forking (bisect: child exec never dispatched).
        int me = usys_getpid();
        if (me == 1) usys_print("init: I am pid 1");
        else usys_print("init: BAD PID (not 1)");
    }
    int runs = 0;
    for (;;) {
        int child = usys_fork();
        if (child < 0) {
            usys_print("init: fork failed, halting");
            usys_exit(1);
        }
        if (child == 0) {
            // Child: become the shell (exec never returns on success).
            // NOTE: no syscalls here before exec except exec itself — the
            // fork-child entry resumes at user_eip (image entry, see
            // usys_proc_fork limits), NOT at the trapped fork site, so any
            // child-side print would run twice/confuse ordering. Exec first.
            int rc = usys_exec("/bin/sh");
            usys_print("init: exec returned!");
            {
                // Report the return code (proves EAX writeback on exec-fail).
                if (rc == -1) usys_print("init: rc=-1 (not found?)");
                else usys_print("init: rc other (redirect broken?)");
            }
            usys_print("init: cannot exec /bin/sh");
            usys_exit(1);
        }
        // Parent (init): wait for the shell, then restart it (respawn).
        // Also reaps any reparented orphans along the way (wait(-1)).
        runs++;
        int spins = 0;
        for (;;) {
            int got = usys_wait(-1);
            if (got == child) break; // shell exited → respawn
            if (got == -1) break;    // no children (shouldn't happen)
            // got == -2 (parked) or an orphan reap: yield EVERY iteration
            // (not 1-in-64). A spinning waiter starves the child it waits
            // for: with SLICE=10 ticks the child may not run for ~100ms per
            // steal, and during that window every wait parks again — the
            // parent's spin is pure serial-log flood + stolen slices. Yield
            // hands the slice straight to the child (tick picks next READY).
            if (got == -2) usys_yield();
            else spins++;
            // else: reaped an orphan, keep waiting for shell
        }
        if (runs > 1000000) usys_exit(0); // sanity bound (never hit)
    }
}

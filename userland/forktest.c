// forktest.c — prove fork()/wait() (/bin/forktest).
// Parent forks; child prints CHILD and exits(7); parent waits, prints the
// reaped pid, exits(0). Serial + window both show the sequence.

#include "usys.h"

void _start(void) {
    // Prove the child path FIRST (serial order proof even if wait breaks).
    usys_print("forktest: starting");
    int child = usys_fork();
    if (child < 0) {
        usys_print("forktest: fork failed");
        usys_exit(1);
    }
    if (child == 0) {
        usys_print("forktest: CHILD running");
        usys_exit(7);
    }
    usys_print("forktest: parent waiting");
    int got = 0;
    for (int i = 0; i < 10000000; i++) {
        got = usys_wait(-1);
        if (got > 0) break;
        if (got == -1) break; // no child at all (real error)
        // got == -2 (WAIT_PARK): parked, child alive but not exited — retry.
    }
    if (got > 0) usys_print("forktest: reaped child");
    else usys_print("forktest: wait found nothing");
    usys_exit(0);
}

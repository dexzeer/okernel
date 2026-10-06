// Fork-then-exec probe: child execs hello immediately (no read, no parse).
// If THIS faults, the bug is fork-x-exec, not sh's line buffer.
#include "usys.h"
void _start(void) {
    usys_print("forkexec: starting");
    int c = usys_fork();
    if (c < 0) { usys_print("forkexec: fork failed"); usys_exit(1); }
    if (c == 0) { usys_exec("/bin/hello"); usys_print("forkexec: exec failed"); usys_exit(1); }
    for (int i = 0; i < 10000000; i++) { int g = usys_wait(c); if (g == c) break; if (g == -1) break; }
    usys_print("forkexec: child reaped");
    usys_exit(0);
}

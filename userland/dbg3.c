// Third probe: fork child does usys_read(0) on an EMPTY queue (parks), then
// prints what it got. Isolates read-park resume vs fork resume.
#include "usys.h"
void _start(void) {
    usys_print("dbg3: starting");
    static char buf[64];
    int c = usys_fork();
    if (c < 0) { usys_print("dbg3: fork failed"); usys_exit(1); }
    if (c == 0) {
        usys_print("dbg3: child reading fd0 (queue drained or stale)...");
        int n = usys_read(0, buf, 63);
        if (n < 0) { usys_print("dbg3: child read err"); usys_exit(2); }
        buf[63] = 0;
        if (n >= 0 && n < 64) buf[n] = 0;
        usys_print("dbg3: child got:");
        usys_print(buf);
        usys_exit(7);
    }
    usys_print("dbg3: parent done");
    usys_exit(0);
}

// Child-resume probe: fork, then BOTH sides print who they are immediately.
// Proves whether the fork child ever runs (EAX=0 path) vs only the parent.
#include "usys.h"
void _start(void) {
    int c = usys_fork();
    if (c < 0) { usys_print("dbgchild: fork failed"); usys_exit(1); }
    if (c == 0) { usys_print("dbgchild: I AM THE CHILD"); usys_exit(7); }
    usys_print("dbgchild: I am parent");
    usys_exit(0);
}

// exectest.c — prove exec() (/bin/exectest).
// Prints BEFORE, execs /bin/hello (which prints its own lines), and — if
// exec returns — prints AFTER with the return code (exec only returns on
// failure).

#include "usys.h"

void _start(void) {
    usys_print("exectest: BEFORE exec");
    int rc = usys_exec("/bin/hello");
    // Only reached on failure.
    usys_print("exectest: exec returned (failure)");
    usys_exit(rc < 0 ? 1 : rc);
}

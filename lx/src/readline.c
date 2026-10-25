#include "lxsys.h"

__attribute__((force_align_arg_pointer)) void _start(void) {
    char buf[128];
    puts_("name? ");
    i64 n = sys_read(0, buf, sizeof buf - 1);
    if (n <= 0) { puts_("eof\n"); sys_exit(1); }
    buf[n] = 0;
    puts_("hello, "); puts_(buf);
    puts_("bye\n");
    sys_exit(0);
}

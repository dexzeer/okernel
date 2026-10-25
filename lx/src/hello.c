#include "lxsys.h"

__attribute__((force_align_arg_pointer)) void _start(void) {
    puts_("hello from x86-64 Linux\n");
    sys_exit(0);
}

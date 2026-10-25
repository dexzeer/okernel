#include "lxsys.h"

// _start receives the initial stack: argc, argv[], 0, envp[], 0, auxv
void _start_c(u64 *sp) {
    long argc = (long)sp[0];
    char **argv = (char**)(sp + 1);
    puts_("argc="); put_dec(argc); puts_("\n");
    for (long i = 0; i < argc; i++) { puts_("["); puts_(argv[i]); puts_("]\n"); }
    char **envp = argv + argc + 1;
    for (; *envp; envp++) { puts_("env "); puts_(*envp); puts_("\n"); }
    sys_exit(0);
}

__asm__(".globl _start\n_start:\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call _start_c\n");

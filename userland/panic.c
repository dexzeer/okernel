// panic.c — /bin/panic: crashes the kernel on purpose, to test the panic screen and the recovery system.
//   run /bin/panic         a kernel panic (kpanic) with a message
//   run /bin/panic fault   an invalid instruction executed in the kernel (the CPU-exception path)
// On an installed disk the panic screen saves a crash record and the next boot starts the recovery system.

#include "usys.h"

static int is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// The entry stack is [argc][argv][envp] (the kernel's ELF spawn): _start hands that ESP to main, so no guess about
// what the compiler's prologue pushed.
__asm__(".globl _start\n_start:\n    push %esp\n    call pmain\n");

void pmain(int *sp) {
    int argc = sp[0];
    char **argv = (char **)sp[1];
    int mode = argc > 1 && is(argv[1], "fault") ? 1 : 0;
    if (argc > 1 && !mode) {
        unsigned n = 0;
        while (argv[1][n]) n++;
        usys_write(1, "panic: unknown option '", 23);
        usys_write(1, argv[1], n);
        usys_write(1, "'\n", 2);
        usys_print("usage: panic [fault]  (crashes the kernel: panic screen, then the recovery system)");
        usys_exit(1);
    }
    usys_print(mode ? "panic: executing an invalid instruction in the kernel..." : "panic: asking the kernel to panic...");
    usys_panic(mode);
    usys_print("panic: this kernel has no panic screen (text mode)");
    usys_exit(1);
}

// Second probe: fork child prints the FORK-RETURNED ESP (its live stack
// pointer right after fork returns) and its reed ESP via a read trap.
// Proves whether the child's stack mapping itself is corrupt.
#include "usys.h"
static unsigned get_esp(void) { unsigned v; __asm__ volatile("mov %%esp, %0" : "=r"(v)); return v; }
static void print_hex(unsigned v) {
    char b[12]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 8; i++) { unsigned n = (v >> (28 - i*4)) & 15; b[2+i] = n < 10 ? '0'+n : 'a'+n-10; }
    b[10]=0; usys_print(b);
}
void _start(void) {
    unsigned before = get_esp();
    usys_print("dbg2: parent esp before fork:");
    print_hex(before);
    int c = usys_fork();
    if (c < 0) { usys_print("dbg2: fork failed"); usys_exit(1); }
    if (c == 0) {
        unsigned ce = get_esp();
        usys_print("dbg2: CHILD esp after fork:");
        print_hex(ce);
        // touch the stack hard: 512B memset + readback
        volatile char *s = (volatile char*)(ce - 512);
        for (int i = 0; i < 512; i++) s[i] = (char)(i & 127);
        int bad = 0;
        for (int i = 0; i < 512; i++) if (s[i] != (char)(i & 127)) bad++;
        if (bad) usys_print("dbg2: CHILD STACK CORRUPT");
        else usys_print("dbg2: child stack OK");
        usys_exit(7);
    }
    unsigned pe = get_esp();
    usys_print("dbg2: parent esp after fork:");
    print_hex(pe);
    usys_exit(0);
}

// Second probe: fork child prints the FORK-RETURNED ESP (its live stack
// pointer right after fork returns) and its reed ESP via a read trap.
// Proves whether the child's stack mapping itself is corrupt.
static int sys_fork(void) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(9) : "memory", "ecx", "edx"); return r; }
static int sys_print(const char *m) { int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(m) : "memory", "ecx", "edx"); return r; }
static void sys_exit(int c) { __asm__ volatile("int $0x80" : : "a"(1), "b"(c) : "memory"); while (1) {} }
static unsigned get_esp(void) { unsigned v; __asm__ volatile("mov %%esp, %0" : "=r"(v)); return v; }
static void print_hex(unsigned v) {
    char b[12]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 8; i++) { unsigned n = (v >> (28 - i*4)) & 15; b[2+i] = n < 10 ? '0'+n : 'a'+n-10; }
    b[10]=0; sys_print(b);
}
void _start(void) {
    unsigned before = get_esp();
    sys_print("dbg2: parent esp before fork:");
    print_hex(before);
    int c = sys_fork();
    if (c < 0) { sys_print("dbg2: fork failed"); sys_exit(1); }
    if (c == 0) {
        unsigned ce = get_esp();
        sys_print("dbg2: CHILD esp after fork:");
        print_hex(ce);
        // touch the stack hard: 512B memset + readback
        volatile char *s = (volatile char*)(ce - 512);
        for (int i = 0; i < 512; i++) s[i] = (char)(i & 127);
        int bad = 0;
        for (int i = 0; i < 512; i++) if (s[i] != (char)(i & 127)) bad++;
        if (bad) sys_print("dbg2: CHILD STACK CORRUPT");
        else sys_print("dbg2: child stack OK");
        sys_exit(7);
    }
    unsigned pe = get_esp();
    sys_print("dbg2: parent esp after fork:");
    print_hex(pe);
    sys_exit(0);
}

// Child-resume probe: fork, then BOTH sides print who they are immediately.
// Proves whether the fork child ever runs (EAX=0 path) vs only the parent.
static int sys_print(const char *msg) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(msg) : "memory", "ecx", "edx");
    return r;
}
static int sys_fork(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(9) : "memory", "ecx", "edx");
    return r;
}
static void sys_exit(int code) {
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code) : "memory");
    while (1) { }
}
void _start(void) {
    int c = sys_fork();
    if (c < 0) { sys_print("dbgchild: fork failed"); sys_exit(1); }
    if (c == 0) { sys_print("dbgchild: I AM THE CHILD"); sys_exit(7); }
    sys_print("dbgchild: I am parent");
    sys_exit(0);
}

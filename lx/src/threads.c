#include "lxsys.h"

#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_THREAD 0x10000
#define CLONE_SYSVSEM 0x40000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define SYS_clone 56
#define SYS_futex 202
#define SYS_gettid 186
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define PROT_RW 3
#define MAP_PRIV_ANON 0x22

static volatile int shared;
static volatile int child_tid;
static volatile int counter;

static long thread_fn(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 1000; i++) __sync_fetch_and_add(&counter, 1);
    shared = (int)(100 + id);
    puts_("thread running tid="); put_dec(sc0(SYS_gettid)); puts_("\n");
    return 0;
}

static i64 spawn(void *stack_top, long (*fn)(void*), void *arg, volatile int *ctid) {
    i64 flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
    i64 ret;
    register i64 r10 __asm__("r10") = (i64)ctid;
    register i64 r8 __asm__("r8") = 0;
    register long (*rfn)(void*) __asm__("r12") = fn;
    register void *rarg __asm__("r13") = arg;
    __asm__ volatile(
        "syscall\n"
        "test %%rax, %%rax\n"
        "jnz 1f\n"
        "mov %%r13, %%rdi\n"
        "call *%%r12\n"
        "mov %%eax, %%edi\n"
        "mov $60, %%eax\n"
        "syscall\n"
        "1:\n"
        : "=a"(ret)
        : "a"(SYS_clone), "D"(flags), "S"(stack_top), "d"(ctid), "r"(r10), "r"(r8), "r"(rfn), "r"(rarg)
        : "rcx", "r11", "memory");
    return ret;
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    u8 *stack = (u8*)sc6(SYS_mmap, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    child_tid = -1;
    i64 tid = spawn(stack + 65536 - 16, thread_fn, (void*)1, &child_tid);
    puts_("parent: created thread "); put_dec(tid > 0); puts_("\n");
    // join: the kernel zeroes child_tid and wakes the futex when the thread exits
    while (child_tid != 0) sc6(SYS_futex, (i64)&child_tid, FUTEX_WAIT, child_tid, 0, 0, 0);
    puts_("joined: shared="); put_dec(shared); puts_(" counter="); put_dec(counter); puts_("\n");
    sys_exit(shared == 101 && counter == 1000 ? 0 : 1);
}

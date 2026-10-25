#include "lxsys.h"

struct ksigaction { u64 handler, flags, restorer, mask; };
#define SA_RESTORER 0x04000000ul
#define SA_SIGINFO 4
#define SYS_rt_sigaction 13
#define SYS_kill 62
#define SYS_alarm 37
#define SYS_pause 34
#define SYS_nanosleep 35

__asm__(".globl restore_rt\nrestore_rt:\n mov $15,%rax\n syscall\n");
extern void restore_rt(void);

static volatile int got_usr1, got_alrm, got_segv;

static void on_usr1(int sig) { got_usr1 = sig; puts_("handler: SIGUSR1\n"); }
static void on_alrm(int sig) { got_alrm = sig; puts_("handler: SIGALRM\n"); }
static void on_segv(int sig, void *info, void *uctx) {
    got_segv = sig;
    puts_("handler: SIGSEGV addr=");
    put_dec(((u64*)info)[2] == 0 ? 0 : 1);
    puts_("\n");
    // skip the faulting instruction: the saved rip is gregs[16] of the ucontext (uc_mcontext starts at offset 40)
    u64 *rip = (u64*)((char*)uctx + 40 + 16 * 8);
    *rip += 2;                                        // "mov (%rax), %eax" is two bytes
}

static void act(int sig, void *h, u64 flags) {
    struct ksigaction a = { (u64)h, flags | SA_RESTORER, (u64)restore_rt, 0 };
    sc4(SYS_rt_sigaction, sig, (i64)&a, 0, 8);
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    act(10, on_usr1, 0);
    act(14, on_alrm, 0);
    act(11, on_segv, SA_SIGINFO);
    i64 pid = sc0(SYS_getpid);
    sc2(SYS_kill, pid, 10);
    puts_("after kill: got_usr1="); put_dec(got_usr1); puts_("\n");
    sc1(SYS_alarm, 1);
    puts_("waiting for alarm\n");
    sc0(SYS_pause);
    puts_("after pause: got_alrm="); put_dec(got_alrm); puts_("\n");
    __asm__ volatile(".byte 0x8b, 0x00" :: "a"(0) : "memory", "cc");   // mov (%rax),%eax with rax = 0: faults; the handler steps over its 2 bytes
    puts_("after segv: got_segv="); put_dec(got_segv); puts_("\n");
    // a blocking call interrupted by a signal fails with EINTR
    sc1(SYS_alarm, 1);
    struct { i64 s, n; } ts = { 5, 0 };
    i64 r = sc2(SYS_nanosleep, (i64)&ts, 0);
    puts_("nanosleep: "); put_dec(r); puts_("\n");
    puts_("signals done\n");
    sys_exit(0);
}

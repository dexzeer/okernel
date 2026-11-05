// Signals of the Linux subsystem: queueing, default actions, handler frames (rt_sigframe), rt_sigreturn, timers.
#include "lx.h"
#include "fpu.h"
#include "serial.h"
#include "string.h"

extern struct lx_task *lx_task_at(int i);
extern void lx_task_exit(struct lx_task *t, int status);

#define SIGHUP 1
#define SIGINT 2
#define SIGKILL 9
#define SIGSEGV 11
#define SIGALRM 14
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGWINCH 28

#define SIG_DFL 0
#define SIG_IGN 1

// no libgcc in the kernel: count trailing zeros by hand
static int ctz64(uint64_t v) { int n = 0; while (!(v & 1)) { v >>= 1; n++; } return n; }

static inline uint64_t bit(int sig) { return 1ull << (sig - 1); }

// What the default action of the signal does: 1 terminate, 0 ignore
static int default_kills(int sig) {
    switch (sig) {
    case SIGCHLD: case SIGCONT: case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU: case SIGURG: case SIGWINCH: return 0;
    default: return 1;
    }
}

// ---- queueing -----------------------------------------------------------------------------------------------------

void lx_signal(struct lx_task *t, int sig) {
    if (sig <= 0 || sig >= LX_NSIG || !t || !t->sh) return;
    if (sig == SIGKILL) {
        serial_printf("[lx] pid %d killed\n", t->tgid);
        lx_task_exit(t, SIGKILL);
        return;
    }
    uint64_t h = t->sh->act[sig].handler;
    if (h == SIG_IGN) return;
    if (h == SIG_DFL && !default_kills(sig)) return;
    t->sig_pending |= bit(sig);
}

// A signal for a process: it goes to a thread that does not block it
void lx_signal_group(int tgid, int sig) {
    struct lx_task *pick = 0;
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        struct lx_task *c = lx_task_at(i);
        if (!c || !c->pid || c->tgid != tgid || c->state == LXT_ZOMBIE || c->state == LXT_FREE) continue;
        if (!pick) pick = c;
        if (!(c->sig_blocked & bit(sig))) { pick = c; break; }
    }
    if (pick) lx_signal(pick, sig);
}

// ---- delivery -----------------------------------------------------------------------------------------------------

struct sigctx {            // struct sigcontext (x86_64)
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
};

struct ucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp; int32_t ss_flags; int32_t pad; uint64_t ss_size;
    struct sigctx mc;
    uint64_t sigmask;
};

struct siginfo {
    int32_t si_signo, si_errno, si_code, pad;
    uint64_t a, b;                                   // si_pid/si_uid or si_addr ...
    uint8_t rest[128 - 32];
};

struct rtframe {
    uint64_t pretcode;
    struct ucontext uc;
    struct siginfo info;
    uint8_t tramp[16];                               // mov rax, 15; syscall
};

static const uint8_t tramp_code[16] = { 0x48, 0xC7, 0xC0, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x05, 0, 0, 0, 0, 0, 0, 0 };

// Build the handler frame on the task's stack and point it at the handler. 0 on success, -1 if the stack is unusable.
static int deliver_one(struct lx_task *t, int sig) {
    struct lx_sigaction act = t->sh->act[sig];
    struct lx_regs *R = &t->regs;
    uint64_t sp = R->rsp;
    if ((act.flags & LX_SA_ONSTACK) && t->sigaltstack_size && !(sp >= t->sigaltstack_sp && sp < t->sigaltstack_sp + t->sigaltstack_size))
        sp = t->sigaltstack_sp + t->sigaltstack_size;
    else sp -= 128;                                   // the red zone of the interrupted code
    sp -= 512;
    sp &= ~63ull;
    uint64_t fpstate = sp;
    sp -= sizeof(struct rtframe);
    sp &= ~15ull;
    sp -= 8;                                          // (sp + 8) % 16 == 0 as at a function entry after the call
    uint64_t frame = sp;
    struct rtframe f;
    memset(&f, 0, sizeof f);
    f.uc.ss_sp = t->sigaltstack_sp; f.uc.ss_size = t->sigaltstack_size;
    f.uc.mc = (struct sigctx){ R->r8, R->r9, R->r10, R->r11, R->r12, R->r13, R->r14, R->r15, R->rdi, R->rsi, R->rbp, R->rbx,
                               R->rdx, R->rax, R->rcx, R->rsp, R->rip, R->rflags, 0x33, 0, 0, 0, 0, 0, 0, 0, fpstate, { 0 } };
    f.uc.mc.err = R->err; f.uc.mc.trapno = R->vec == 0x100 ? 0 : R->vec;
    f.uc.sigmask = t->sig_blocked;
    f.info.si_signo = sig;
    f.info.si_code = 0;
    f.info.a = (uint64_t)t->tgid;
    if (R->vec == 14 && sig == SIGSEGV) { extern uint64_t lx_fault_cr2; f.info.si_code = 1; f.info.a = lx_fault_cr2; f.uc.mc.cr2 = lx_fault_cr2; }
    memcpy(f.tramp, tramp_code, 16);
    uint64_t tramp_va = frame + __builtin_offsetof(struct rtframe, tramp);
    f.pretcode = (act.flags & 0x04000000u) ? act.restorer : tramp_va;      // SA_RESTORER
    if (lx_copy_out(t, frame, &f, sizeof f)) return -1;
    if (lx_copy_out(t, fpstate, t->fx, 512)) return -1;
    t->saved_mask = t->sig_blocked;
    t->sig_blocked |= act.mask;
    if (!(act.flags & LX_SA_NODEFER)) t->sig_blocked |= bit(sig);
    R->rsp = frame;
    R->rip = act.handler;
    R->rdi = (uint64_t)sig;
    R->rsi = frame + __builtin_offsetof(struct rtframe, info);
    R->rdx = frame + __builtin_offsetof(struct rtframe, uc);
    R->rax = 0;
    R->rflags &= ~0x400ull;                           // DF clear for the handler
    if (act.flags & LX_SA_RESETHAND) t->sh->act[sig].handler = SIG_DFL;
    return 0;
}

// Called before the task goes back to user mode. Returns 1 if the task died.
int lx_sig_check(struct lx_task *t) {
    for (;;) {
        uint64_t deliverable = t->sig_pending & ~t->sig_blocked;
        if (!deliverable) return 0;
        int sig = ctz64(deliverable) + 1;
        t->sig_pending &= ~bit(sig);
        uint64_t h = t->sh->act[sig].handler;
        if (h == SIG_IGN) continue;
        if (h == SIG_DFL) {
            if (!default_kills(sig)) continue;
            serial_printf("[lx] pid %d (%s) killed by signal %d\n", t->tgid, t->comm, sig);
            lx_task_exit(t, sig);
            return 1;
        }
        if (deliver_one(t, sig)) {                    // no usable stack: the process dies of SIGSEGV
            serial_printf("[lx] pid %d (%s): cannot build the signal frame\n", t->tgid, t->comm);
            lx_task_exit(t, SIGSEGV);
            return 1;
        }
        return 0;
    }
}

int64_t lx_sys_rt_sigreturn(struct lx_task *t) {
    struct lx_regs *R = &t->regs;
    struct ucontext uc;
    if (lx_copy_in(t, &uc, R->rsp, sizeof uc)) { lx_task_exit(t, SIGSEGV); return 0; }
    struct sigctx *m = &uc.mc;
    R->r8 = m->r8; R->r9 = m->r9; R->r10 = m->r10; R->r11 = m->r11; R->r12 = m->r12; R->r13 = m->r13; R->r14 = m->r14; R->r15 = m->r15;
    R->rdi = m->rdi; R->rsi = m->rsi; R->rbp = m->rbp; R->rbx = m->rbx; R->rdx = m->rdx; R->rax = m->rax; R->rcx = m->rcx;
    R->rsp = m->rsp; R->rip = m->rip;
    R->rflags = (m->eflags & 0xDD5) | 0x202;          // user-changeable flags only; IF stays on
    t->sig_blocked = uc.sigmask & ~(bit(SIGKILL) | bit(SIGSTOP));
    if (m->fpstate) lx_copy_in(t, t->fx, m->fpstate, 512);
    t->skip_ret = 1;                                  // rax is the interrupted code's, not a system call result
    return 0;
}

// A task blocked in a system call: if a signal wants to run, wake it. A handler without SA_RESTART makes the call fail with
// EINTR; with SA_RESTART the call is simply repeated afterwards (it was rewound already).
int lx_sig_wake_blocked(struct lx_task *t) {
    uint64_t deliverable = t->sig_pending & ~t->sig_blocked;
    if (!deliverable) return 0;
    int sig = ctz64(deliverable) + 1;
    uint64_t h = t->sh->act[sig].handler;
    int restart = h > SIG_IGN && (t->sh->act[sig].flags & LX_SA_RESTART);
    if (h > SIG_IGN && !restart && t->blocked_rewound) { t->regs.rip += 2; t->regs.rax = (uint64_t)-LX_EINTR; }
    t->blocked_rewound = 0;                           // (a default-fatal signal dies in lx_sig_check)
    t->wake_ms = 0;
    if (t->futex_state == 1) t->futex_state = 0;      // no longer waiting on a futex
    t->state = LXT_RUN;
    return 1;
}

// ---- timers (alarm, setitimer ITIMER_REAL) --------------------------------------------------------------------------

// POSIX timers. glibc's SIGEV_THREAD timers come here as SIGEV_THREAD_ID: its helper thread sits in sigwaitinfo and
// reads the timer back out of si_value, so the siginfo has to be right (lx_ptimer_siginfo).
#define LX_PTIMERS 64
#define SIGEV_NONE 1
#define SIGEV_THREAD_ID 4
struct ptimer {
    int used, tgid, notify, signo, tid;
    uint64_t value, next_ms, interval_ms;
    uint32_t overrun;
};
static struct ptimer ptimers[LX_PTIMERS];
extern int lx_task_by_pid(int pid, struct lx_task **out);

static void ptimers_tick(int64_t now) {
    for (int i = 0; i < LX_PTIMERS; i++) {
        struct ptimer *p = &ptimers[i];
        if (!p->used) continue;
        struct lx_task *o = 0;
        if (!lx_task_by_pid(p->tgid, &o) || o->state == LXT_ZOMBIE || o->state == LXT_FREE) { p->used = 0; continue; }
        if (!p->next_ms || now < (int64_t)p->next_ms) continue;
        if (p->interval_ms) {
            p->next_ms += p->interval_ms;
            if ((int64_t)p->next_ms <= now) {        // a late tick: count what was missed, and do not replay it
                uint64_t missed = ((uint64_t)now - p->next_ms) / p->interval_ms + 1;
                p->overrun += (uint32_t)missed;
                p->next_ms += missed * p->interval_ms;
            }
        } else p->next_ms = 0;
        if (p->notify == SIGEV_NONE) continue;
        if (p->notify == SIGEV_THREAD_ID) {
            struct lx_task *c = 0;
            if (lx_task_by_pid(p->tid, &c) && c->tgid == p->tgid) lx_signal(c, p->signo);
        } else lx_signal_group(p->tgid, p->signo);
    }
}

void lx_ptimer_siginfo(struct lx_task *t, int sig, uint8_t *si) {
    for (int i = 0; i < LX_PTIMERS; i++) {
        struct ptimer *p = &ptimers[i];
        if (!p->used || p->tgid != t->tgid || p->signo != sig) continue;
        if (p->notify == SIGEV_THREAD_ID && p->tid != t->pid) continue;
        int32_t code = -2, id = i;                   // SI_TIMER
        memcpy(si + 8, &code, 4);
        memcpy(si + 16, &id, 4);
        memcpy(si + 20, &p->overrun, 4);
        memcpy(si + 24, &p->value, 8);
        p->overrun = 0;
        return;
    }
}

struct lx_its { int64_t isec, insec, vsec, vnsec; };

int64_t lx_sys_ptimer(struct lx_task *t, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t now = lx_now_ms();
    if (nr == 222) {                                 // timer_create(clock, sigevent, id)
        int i = 0;
        while (i < LX_PTIMERS && ptimers[i].used) i++;
        if (i == LX_PTIMERS) return -LX_EAGAIN;
        struct ptimer *p = &ptimers[i];
        memset(p, 0, sizeof *p);
        p->tgid = t->tgid;
        p->signo = SIGALRM;
        if (a1) {
            struct { uint64_t value; int32_t signo, notify, tid; } e;
            if (lx_copy_in(t, &e, a1, sizeof e)) return -LX_EFAULT;
            p->value = e.value; p->signo = e.signo; p->notify = e.notify; p->tid = e.tid;
        }
        int32_t id = i;
        if (lx_copy_out(t, a2, &id, 4)) return -LX_EFAULT;
        p->used = 1;
        return 0;
    }
    if (a0 >= LX_PTIMERS || !ptimers[a0].used || ptimers[a0].tgid != t->tgid) return -LX_EINVAL;
    struct ptimer *p = &ptimers[a0];
    struct lx_its o = { (int64_t)(p->interval_ms / 1000), (int64_t)(p->interval_ms % 1000) * 1000000, 0, 0 };
    if (p->next_ms > (uint64_t)now) { uint64_t rem = p->next_ms - (uint64_t)now; o.vsec = (int64_t)(rem / 1000); o.vnsec = (int64_t)(rem % 1000) * 1000000; }
    switch (nr) {
    case 223: {                                      // timer_settime(id, flags, new, old)
        struct lx_its n;
        if (lx_copy_in(t, &n, a2, sizeof n)) return -LX_EFAULT;
        if (n.vnsec < 0 || n.vnsec >= 1000000000 || n.insec < 0 || n.insec >= 1000000000) return -LX_EINVAL;
        if (a3 && lx_copy_out(t, a3, &o, sizeof o)) return -LX_EFAULT;
        uint64_t val = (uint64_t)(n.vsec * 1000 + (n.vnsec + 999999) / 1000000);
        p->interval_ms = (uint64_t)(n.isec * 1000 + (n.insec + 999999) / 1000000);
        if (!val) p->next_ms = 0;
        else if (a1 & 1) p->next_ms = val;           // TIMER_ABSTIME: in the monotonic clock's terms
        else p->next_ms = (uint64_t)now + val;
        return 0;
    }
    case 224: return lx_copy_out(t, a1, &o, sizeof o) ? -LX_EFAULT : 0;
    case 225: { int64_t v = p->overrun; p->overrun = 0; return v; }
    case 226: p->used = 0; return 0;
    }
    return -LX_ENOSYS;
}

void lx_sig_timers(void) {
    int64_t now = lx_now_ms();
    ptimers_tick(now);
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        struct lx_task *t = lx_task_at(i);
        if (!t || !t->pid || t->tgid != t->pid || !t->itimer_next_ms || t->state == LXT_ZOMBIE || t->state == LXT_FREE) continue;
        if (now < (int64_t)t->itimer_next_ms) continue;
        if (t->itimer_interval_ms) t->itimer_next_ms += t->itimer_interval_ms; else t->itimer_next_ms = 0;
        lx_signal(t, SIGALRM);
    }
}

struct lx_itimerval { int64_t isec, iusec, vsec, vusec; };

int64_t lx_sys_setitimer(struct lx_task *t, uint64_t which, uint64_t unew, uint64_t uold) {
    if (which != 0) return unew || uold ? 0 : 0;                    // only ITIMER_REAL runs
    int64_t now = lx_now_ms();
    if (uold) {
        struct lx_itimerval o = { (int64_t)(t->itimer_interval_ms / 1000), (int64_t)(t->itimer_interval_ms % 1000) * 1000, 0, 0 };
        if (t->itimer_next_ms > (uint64_t)now) { uint64_t rem = t->itimer_next_ms - (uint64_t)now; o.vsec = (int64_t)(rem / 1000); o.vusec = (int64_t)(rem % 1000) * 1000; }
        if (lx_copy_out(t, uold, &o, sizeof o)) return -LX_EFAULT;
    }
    if (unew) {
        struct lx_itimerval n;
        if (lx_copy_in(t, &n, unew, sizeof n)) return -LX_EFAULT;
        uint64_t val = (uint64_t)(n.vsec * 1000 + n.vusec / 1000), iv = (uint64_t)(n.isec * 1000 + n.iusec / 1000);
        if (n.vusec && !val) val = 1;
        t->itimer_next_ms = val ? (uint64_t)now + val : 0;
        t->itimer_interval_ms = iv;
    }
    return 0;
}

int64_t lx_sys_alarm(struct lx_task *t, uint64_t secs) {
    int64_t now = lx_now_ms();
    int64_t old = 0;
    if (t->itimer_next_ms > (uint64_t)now) old = (int64_t)((t->itimer_next_ms - (uint64_t)now + 999) / 1000);
    t->itimer_next_ms = secs ? (uint64_t)now + secs * 1000 : 0;
    t->itimer_interval_ms = 0;
    return old;
}

int64_t lx_sys_getitimer(struct lx_task *t, uint64_t which, uint64_t uval) {
    struct lx_itimerval o = { 0, 0, 0, 0 };
    int64_t now = lx_now_ms();
    if (which == 0) {
        o.isec = (int64_t)(t->itimer_interval_ms / 1000); o.iusec = (int64_t)(t->itimer_interval_ms % 1000) * 1000;
        if (t->itimer_next_ms > (uint64_t)now) { uint64_t rem = t->itimer_next_ms - (uint64_t)now; o.vsec = (int64_t)(rem / 1000); o.vusec = (int64_t)(rem % 1000) * 1000; }
    }
    return lx_copy_out(t, uval, &o, sizeof o);
}

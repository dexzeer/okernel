#include "serial.h"
#include "gdt.h"
#include "memlayout.h"
#include "syscall.h"
#include <stdint.h>

// 1 while a user program runs (shell stays interactive behind it).
// sys_exit clears it AND latches user_exited so the main loop announces
// completion once (poll pattern, same as https_get_poll/http_poll).
volatile int syscall_can_exit = 0;
static volatile int user_exited = 0;

// Main-loop entry run queue: the shell stages pid+eip+esp (+owning window)
// per spawn; the main loop consumes entries in order. Replaces the old single
// user_entry_pending/pid slot (one process at a time — fork children and
// back-to-back spawns collided on it).
#define ENTRY_QUEUE_LEN 4
struct entry_req {
    int used;
    int pid;
    uint32_t eip;
    uint32_t esp;
    int win_id;
};
static struct entry_req entry_queue[ENTRY_QUEUE_LEN];

// Legacy single-slot staging (kept for the usermode path + text build).
// New code should use entry_enqueue/entry_dequeue below.
volatile uint32_t user_entry_eip = 0;
volatile uint32_t user_entry_esp = 0;
volatile int user_entry_pending = 0;
volatile int user_entry_pid = -1;

// Enqueue a ring-3 entry request. Returns 0 on success, -1 when full.
int entry_enqueue(int pid, uint32_t eip, uint32_t esp, int win_id) {
    for (int i = 0; i < ENTRY_QUEUE_LEN; i++) {
        if (!entry_queue[i].used) {
            entry_queue[i].used = 1;
            entry_queue[i].pid = pid;
            entry_queue[i].eip = eip;
            entry_queue[i].esp = esp;
            entry_queue[i].win_id = win_id;
            return 0;
        }
    }
    return -1;
}

// Dequeue the oldest pending request. Returns 0 on success, -1 when empty.
int entry_dequeue(int *pid, uint32_t *eip, uint32_t *esp, int *win_id) {
    for (int i = 0; i < ENTRY_QUEUE_LEN; i++) {
        if (entry_queue[i].used) {
            entry_queue[i].used = 0;
            if (pid) *pid = entry_queue[i].pid;
            if (eip) *eip = entry_queue[i].eip;
            if (esp) *esp = entry_queue[i].esp;
            if (win_id) *win_id = entry_queue[i].win_id;
            return 0;
        }
    }
    return -1;
}

// 1 when any entry request is pending (queue or legacy slot).
int entry_pending_any(void) {
    if (user_entry_pending) return 1;
    for (int i = 0; i < ENTRY_QUEUE_LEN; i++) {
        if (entry_queue[i].used) return 1;
    }
    return 0;
}

// Kernel copy of the ring-3 test image's source page + entry offset + len.
// desktop.c installs these at boot (needs the linked user_mode_test symbol;
// syscall.c is COMMON so it cannot reference it directly).
static const uint8_t *user_test_src_page = 0;
static uint32_t user_test_src_off = 0;
static uint32_t user_test_src_len = 0;
void syscall_install_usertest(const uint8_t *page, uint32_t off) {
    user_test_src_page = page;
    user_test_src_off = off;
}
void syscall_install_usertest_len(uint32_t len) { user_test_src_len = len; }
uint8_t* user_test_page_base(void) { return (uint8_t*)user_test_src_page; }
uint32_t user_test_page_off(void) { return user_test_src_off; }
uint32_t user_test_len(void) { return user_test_src_len; }

// Drain-side pid stash: the main-loop drain writes pid here BEFORE the IRET
// (ring-0 thread context, live locals) and reads it back AFTER the trampoline
// resumes (locals in EBX/EAX/ECX/EDX went stale across enter/exit). Static
// word, main-loop-only (only the drain loop ever enters ring 3).
static int drain_pid_stash = -1;
void drain_stash_pid(int pid) { drain_pid_stash = pid; }
int drain_last_pid(void) {
    int pid = drain_pid_stash;
    drain_pid_stash = -1;
    return pid;
}

// Slot-of-current (trap context): syscall.c is COMMON so it cannot include
// process.h (desktop-only); resolve via weak aliases + pointer scan (slots
// never move; UNUSED slots return NULL so dead entries never match). Used
// by every per-slot array below (retval, trap stash, park state).
static unsigned trap_slot_self(void) {
    extern void *process_current(void) __attribute__((weak));
    extern void *process_get_by_slot(int slot) __attribute__((weak));
    if (process_current && process_get_by_slot) {
        void *cur = process_current();
        for (unsigned s = 0; s < 16; s++) {
            if (process_get_by_slot((int)s) == cur) return s;
        }
    }
    return 0;
}

// Ring-3 return-value slot: idt.c owns the pusha frame; the handler stages
// the value here and idt.c writes it into pushed[7] after dispatch returns.
// (syscall_handler is COMMON and must not touch the trap frame layout.)
// PER-SLOT (same staleness class as the trap stash + park state: two threads
// trapping back-to-back shared one global — the second thread's take_ret
// consumed the first thread's value. Bisected 2026-09-08: sh's fork child
// observed a stale EAX (0x8049109 = code bytes, not a pid) at its exec trap
// — the child's set_ret/take_ret raced the parent's park/yield retvals
// through this single global. Slot-indexed like everything else here.)
static uint32_t syscall_retval[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
void syscall_set_ret(uint32_t v) { syscall_retval[trap_slot_self()] = v; }
uint32_t syscall_take_ret(void) {
    unsigned s = trap_slot_self();
    uint32_t v = syscall_retval[s];
    syscall_retval[s] = 0;
    return v;
}

// Trapped ring-3 resume state (set by idt.c before dispatch, read by fork):
// the CPU-pushed user EIP/ESP for THIS trap. Lets fork children resume after
// the fork call instead of restarting at the image entry. PER-PID (indexed
// by slot): a single global goes stale when two processes trap back-to-back
// (bisected 2026-09-08: sh's fork child inherited the PARENT's older trapped
// ESP — the stash held whoever trapped last, not the forker — so the child
// resumed on a dead stack: garbage exec string, #PF in the prefix loop).
// Slot-indexed (slots never move; pid lookup skips UNUSED): stale entries
// for dead slots are simply never read.
static uint32_t trap_user_eip[16];
static uint32_t trap_user_esp[16];
void syscall_stash_trap(uint32_t eip, uint32_t esp) {
    extern void *process_current(void) __attribute__((weak));
    unsigned slot = 0;
    if (process_current) {
        // current_slot is slot-indexed; derive via pointer subtraction is
        // desktop-only — instead stash by pid→slot scan (16 iterations,
        // trap context, cheap). Fallback slot 0 (never read for pid 0).
        extern void *process_get_by_slot(int slot) __attribute__((weak));
        void *cur = process_current();
        if (process_get_by_slot) {
            for (unsigned s = 0; s < 16; s++) {
                if (process_get_by_slot((int)s) == cur) { slot = s; break; }
            }
        }
    }
    trap_user_eip[slot] = eip;
    trap_user_esp[slot] = esp;
}
uint32_t syscall_trap_eip(void) { return trap_user_eip[trap_slot_self()]; }
uint32_t syscall_trap_esp(void) { return trap_user_esp[trap_slot_self()]; }

// Serial chatter policy: per-syscall SUCCESS lines are OFF by default —
// init/sh/forktest wait/yield/read-poll rings emit millions of lines and
// drown the log (8.9M-line serials observed), hiding real evidence.
// Keep ALWAYS: error/fail lines, lifecycle lines ([fork]/[exec]/[exit]/
// spawn), and explicit PROGRAM-OUTPUT syscalls (print/write payload —
// hello/pipetest/sh proof rides on these; gating them blinds verification).
// Gate the REST (getpid/yield/read/fd/vm control-plane chatter).
#define USER_SYSTRACE 0
#if USER_SYSTRACE
#define UTRACE(...) serial_printf(__VA_ARGS__)
#else
#define UTRACE(...) do {} while (0)
#endif

int user_mode_poll_finished(void) {
    if (user_exited) {
        user_exited = 0;
        return 1;
    }
    return 0;
}

// Latch completion for the idt.c SYS_EXIT hijack path (which bypasses the
// handler — the trampoline discards the frame, so SYS_EXIT in the switch
// below only runs for direct handler calls, never real ring-3 exits).
void syscall_note_exited(void) {
    user_exited = 1;
}

// Exec-redirect flag: armed by SYS_EXEC success, consumed by idt.c after
// dispatch returns (its iret then targets user_eip/esp, not trapped EIP).
// Staged value (not the PCB directly): idt.c owns the trap frame, and the
// flag must survive only until the immediate iret — consume-once.
static volatile int exec_redirect_armed = 0;
void syscall_arm_exec_redirect(void) { exec_redirect_armed = 1; }
int syscall_take_exec_redirect(void) {
    int v = exec_redirect_armed;
    exec_redirect_armed = 0;
    return v;
}

// Park flag + staged return + stashed resume EIP/ESP: armed by blocking
// syscalls (wait-no-zombie, yield, read-empty) INSTEAD of set_ret — idt.c
// consumes after dispatch and jmps to user_park_trampoline (trap frame
// discarded, main loop resumes). PER-SLOT (arrays indexed like the trap
// stash): a single global went stale when two threads parked without an
// intervening re-entry (bisected 2026-09-08 class: sh's fork child inherited
// a dead resume — drain keys by pid, arm wrote a global last-parker).
// The drain re-enters the parker via enter_user_mode at park_eip/esp with
// park_ret forced into EAX (same user_fork_child mechanism, separate flag:
// user_park_ret_pending).
// Resume ESP: the trapped user ESP (unchanged by the trap — ring-3 int
// pushes EFLAGS/CS/EIP on the KERNEL trap stack, not the user stack), so
// re-entry continues after the int $0x80 exactly like a normal iret would.
static volatile int park_armed[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static uint32_t park_ret[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static uint32_t park_eip[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static uint32_t park_esp[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
void syscall_arm_park(uint32_t retval) {
    unsigned s = trap_slot_self();
    park_armed[s] = 1;
    park_ret[s] = retval;
    // Same-slot reads (the stash is per-slot — arm reads the CURRENT
    // thread's slot, not a global last-trapper; see stash above).
    park_eip[s] = trap_user_eip[s];
    park_esp[s] = trap_user_esp[s];
}
int syscall_take_park(void) {
    // Consume the CURRENT thread's slot (idt.c runs on the trapper's trap
    // stack — current is reconciled by now, so self-slot is the armer).
    unsigned s = trap_slot_self();
    int v = park_armed[s];
    park_armed[s] = 0;
    return v;
}
uint32_t syscall_take_park_ret(void) {
    unsigned s = trap_slot_self();
    uint32_t v = park_ret[s];
    park_ret[s] = 0;
    return v;
}
void syscall_stash_park_ret(uint32_t v) {
    // idt.c re-stages for the drain (same trap, same thread — self slot).
    park_ret[trap_slot_self()] = v;
}
uint32_t syscall_park_eip(void) { return park_eip[trap_slot_self()]; }
uint32_t syscall_park_esp(void) { return park_esp[trap_slot_self()]; }

// Syscall numbers (int 0x80 ABI: eax = number, ebx/ecx/edx = args).
// 0-1 are the legacy pair; 2+ are the extended ABI (validated pointers,
// per-process address spaces). Ring 3 must never pass a kernel address.
#define SYS_PRINT       0   // ebx = user string (NUL-terminated, <=4K incl NUL)
#define SYS_EXIT        1   // no args; terminates the calling user program
#define SYS_WRITE       2   // ebx = fd (1 = terminal), ecx = user buf, edx = len
#define SYS_GETPID      3   // no args; returns pid
#define SYS_YIELD       4   // no args; voluntarily give up the time slice
#define SYS_MMAP_USER   5   // ebx = user virt page, ecx = phys page (PMM-owned)
#define SYS_READ        6   // ebx = fd (0 = keyboard), ecx = user buf, edx = len
#define SYS_OPEN        7   // ebx = user path (NUL-terminated), ecx = flags (0=ro,1=wo,2=rw)
#define SYS_CLOSE       8   // ebx = fd
#define SYS_FORK        9   // no args; copy address space, child returns 0
#define SYS_EXEC        10  // ebx = user path (ELF in VFS); load + enter
#define SYS_SBRK        11  // ebx = increment bytes; grow user heap, return old break
#define SYS_PIPE        12  // ebx = user int[2]; create pipe, return fds
#define SYS_DUP         13  // ebx = oldfd; duplicate onto lowest free fd
#define SYS_WAIT        14  // ebx = pid (-1 = any child); reap zombie, return pid
#define SYS_KILL        15  // ebx = pid, ecx = signo (0=TERM default); deliver signal
#define SYS_MMAP        16  // ebx = user virt page (0 = auto); map fresh zero page
#define SYS_MUNMAP      17  // ebx = user virt page; unmap one page

// Simple syscall handler for int 0x80
// Called from isr_handler when int_num == 0x80
// Registers: eax = syscall number, ebx = arg1, ecx = arg2, edx = arg3.
// ABI note: syscall.c is COMMON (both text + desktop link it). Desktop-only
// services (paging validation, window output) live behind the hooks below —
// desktop.c installs them at boot; text mode leaves them NULL, in which case
// pointer-taking syscalls fail safe with an error line. This keeps the text
// build linking (no paging.o/window.o there) without #ifdefs. Hook typedefs
// live in syscall.h (shared with desktop.c install sites).
static sys_validate_fn sys_validate = 0;
static sys_write_hook_fn sys_write_hook = 0;
static sys_write_win_hook_fn sys_write_win_hook = 0;
static sys_pid_hook_fn sys_pid_hook = 0;
static sys_yield_hook_fn sys_yield_hook = 0;
static sys_mmap_hook_fn sys_mmap_hook = 0;
static sys_munmap_hook_fn sys_munmap_hook = 0;
static sys_read_hook_fn sys_read_hook = 0;
static sys_open_hook_fn sys_open_hook = 0;
static sys_close_hook_fn sys_close_hook = 0;
static sys_write_fd_hook_fn sys_write_fd_hook = 0;
static sys_read_fd_hook_fn sys_read_fd_hook = 0;
static sys_fork_hook_fn sys_fork_hook = 0;
static sys_exec_hook_fn sys_exec_hook = 0;
static sys_sbrk_hook_fn sys_sbrk_hook = 0;
static sys_pipe_hook_fn sys_pipe_hook = 0;
static sys_dup_hook_fn sys_dup_hook = 0;
static sys_wait_hook_fn sys_wait_hook = 0;
static sys_kill_hook_fn sys_kill_hook = 0;
static sys_mmap_anon_hook_fn sys_mmap_anon_hook = 0;
void syscall_install(sys_validate_fn v, sys_write_hook_fn w) {
    sys_validate = v;
    sys_write_hook = w;
}
// Desktop installs the owning-window write hook (needs window.o +
// process.o; syscall.c stays COMMON). Falls back to sys_write_hook.
void syscall_install_winwrite(sys_write_win_hook_fn w) {
    sys_write_win_hook = w;
}
// Desktop installs process hooks (needs process.o/sched.o = desktop-only).
void syscall_install_proc(sys_pid_hook_fn p, sys_yield_hook_fn y) {
    sys_pid_hook = p;
    sys_yield_hook = y;
}
// Desktop installs the page-mapper hook (needs paging.o = desktop-only).
void syscall_install_mmap(sys_mmap_hook_fn m) {
    sys_mmap_hook = m;
}
// Desktop installs the full file/process/VM hook block (desktop-only).
void syscall_install_full(sys_munmap_hook_fn mu, sys_read_hook_fn r,
                           sys_open_hook_fn o, sys_close_hook_fn c,
                           sys_write_fd_hook_fn wf, sys_read_fd_hook_fn rf,
                           sys_fork_hook_fn fk, sys_exec_hook_fn ex,
                           sys_sbrk_hook_fn sb, sys_pipe_hook_fn pp,
                           sys_dup_hook_fn dp, sys_wait_hook_fn wt,
                           sys_kill_hook_fn kl, sys_mmap_anon_hook_fn ma) {
    sys_munmap_hook = mu;
    sys_read_hook = r;
    sys_open_hook = o;
    sys_close_hook = c;
    sys_write_fd_hook = wf;
    sys_read_fd_hook = rf;
    sys_fork_hook = fk;
    sys_exec_hook = ex;
    sys_sbrk_hook = sb;
    sys_pipe_hook = pp;
    sys_dup_hook = dp;
    sys_wait_hook = wt;
    sys_kill_hook = kl;
    sys_mmap_anon_hook = ma;
}

void syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    (void)ecx; (void)edx;
    // NOTE: ring-3 preemption reconcile (stolen-context switch-home) runs in
    // idt.c BEFORE dispatch (it must precede the trap stash + validate, so
    // fork parents + fd tables resolve to the true owner). See isr_handler.
    switch (eax) {
        case SYS_PRINT: { // sys_print(const char* msg)
            // Validate BEFORE dereferencing: a ring-3 pointer into unmapped
            // or kernel memory must print an error, never fault the kernel.
            if (!sys_validate || !sys_validate(ebx, 1)) {
                serial_puts("[user] sys_print: bad pointer\n");
                break;
            }
            // NUL must appear within the validated window; extend page by page.
            uint32_t scan_len = 1;
            const char *msg = (const char*)ebx;
            while (scan_len < 4096) {
                if (!sys_validate(ebx, scan_len + 1)) break;
                if (msg[scan_len - 1] == 0) break;
                scan_len++;
            }
            if (msg[scan_len - 1] != 0) {
                serial_puts("[user] sys_print: unterminated string\n");
                break;
            }
            serial_printf("[user] %s\n", msg);
            // Mirror to the OWNING terminal (not focused): the winwrite hook
            // + the calling process's term_win routes to the spawner, so
            // output lands where `run` was typed even if focus moved mid-run
            // (or init's children inherited a window that is no longer
            // focused). Falls back to the legacy focused-window hook.
            {
                uint32_t mlen = scan_len - 1;
                extern int sys_proc_term_win(void) __attribute__((weak));
                int routed = 0;
                if (sys_write_win_hook && sys_proc_term_win) {
                    int w = sys_proc_term_win();
                    if (w >= 0) {
                        sys_write_win_hook(w, msg, mlen);
                        sys_write_win_hook(w, "\n", 1);
                        routed = 1;
                    }
                }
                if (!routed && sys_write_hook) {
                    sys_write_hook(msg, mlen);
                    sys_write_hook("\n", 1);
                }
            }
            break;
        }
        case SYS_EXIT: // sys_exit(ebx = status)
            // NOTE: real ring-3 exits NEVER reach here — idt.c hijacks
            // eax==1 to the trampoline before dispatch (the frame is
            // discarded). This arm covers direct handler calls only; the
            // live path records via sys_proc_exit_current + note_exited in
            // the hijack. Kept for symmetry with the ABI table.
            serial_puts("[user] exit syscall — user program finished\n");
            {
                extern void sys_proc_exit_current(int code) __attribute__((weak));
                if (sys_proc_exit_current) sys_proc_exit_current((int)ebx);
            }
            syscall_can_exit = 0;
            user_exited = 1;
            break;
        case SYS_WRITE: { // sys_write(fd, buf, len)
            // fd routing through the CALLING process's fd table:
            // PROC_FD_TERM (fds 1/2) → owning-window hook; real ofd index →
            // sys_write_fd_hook (files/pipes); PROC_FD_KBD/free → -1.
            // (The old code special-cased fd==1 to the focused window, so
            // close/dup2/redirect could never change where output went.)
            if (edx > 4096) {
                serial_puts("[user] sys_write: len too large (max 4096)\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            if (edx > 0 && (!sys_validate || !sys_validate(ecx, edx))) {
                serial_puts("[user] sys_write: bad buffer\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            {
                const char *buf = (const char*)ecx;
                // Text build has no process.o: sys_fd_resolve lives there
                // (desktop-only). Weak alias → NULL when unlinked, and the
                // fd path fails safe (terminal fast path still works by fd).
                // Owning-window binding: route PROC_FD_TERM through the
                // winwrite hook with the CALLING process's term_win, so
                // output lands in the spawning terminal even when focus
                // moved elsewhere mid-run.
                extern int sys_fd_resolve(int fd) __attribute__((weak));
                extern int sys_proc_term_win(void) __attribute__((weak));
                int route = sys_fd_resolve ? sys_fd_resolve((int)ebx)
                                           : ((int)ebx == 1 ? -2 : -1);
                int n = (int)edx;
                if (route == -2) { // PROC_FD_TERM → owning window
                    if (sys_write_win_hook && sys_proc_term_win) {
                        int w = sys_proc_term_win();
                        sys_write_win_hook(w, buf, edx);
                    } else if (sys_write_hook) {
                        sys_write_hook(buf, edx);
                    }
                } else if (route >= 0) {
                    if (!sys_write_fd_hook) {
                        serial_puts("[user] sys_write: no fd backend\n");
                        syscall_set_ret((uint32_t)-1);
                        break;
                    }
                    int rc = sys_write_fd_hook((int)ebx, buf, edx);
                    if (rc < 0) {
                        serial_puts("[user] sys_write: bad fd\n");
                        syscall_set_ret((uint32_t)-1);
                        break;
                    }
                    n = rc;
                } else {
                    serial_puts("[user] sys_write: bad fd\n");
                    syscall_set_ret((uint32_t)-1);
                    break;
                }
                serial_puts("[user] write: ");
                for (uint32_t i = 0; i < edx; i++)
                    serial_putchar(buf[i]);
                serial_putchar('\n');
                syscall_set_ret((uint32_t)n);
            }
            break;
        }
        case SYS_GETPID: { // sys_getpid()
            int pid = sys_pid_hook ? sys_pid_hook() : 0;
            syscall_set_ret((uint32_t)pid);
            UTRACE("[user] pid=%d\n", pid);
            break;
        }
        case SYS_YIELD: { // sys_yield()
            UTRACE("[user] yield\n");
            if (sys_yield_hook) sys_yield_hook();
            // Yield PARKS (no iret back to ring 3): mark BLOCKED + return
            // straight to the main loop via the park trampoline (idt.c
            // consumes the arm after dispatch). The drain then runs queued
            // siblings THIS iteration; the tick WAKE pass flips us READY
            // and the drain re-enters us with EAX=0. Without this the
            // waiter spins in ring 3 while run N+1 sits queued (bisected
            // 2026-09-08: run N wedged under init's wait/yield storm).
            syscall_arm_park(0);
            break;
        }
        case SYS_MMAP_USER: { // sys_mmap_user(user_virt_page, phys_page)
            // Page-granular user mapping helper (desktop-only: needs
            // paging.o). Both addresses must be page-aligned, virt user-low.
            // Routed through the mmap hook (NULL in text mode → fail safe).
            if ((ebx & 0xFFF) || (ecx & 0xFFF)) {
                serial_puts("[user] sys_mmap_user: addresses must be page-aligned\n");
                break;
            }
            if (ebx >= KERNEL_VBASE || ebx == 0) {
                serial_puts("[user] sys_mmap_user: virt out of user range\n");
                break;
            }
            if (!sys_mmap_hook) {
                serial_puts("[user] sys_mmap_user: unavailable in text mode\n");
                break;
            }
            sys_mmap_hook(ebx, ecx);
            UTRACE("[user] mmap user %x -> phys %x\n", ebx, ecx);
            break;
        }
        case SYS_READ: { // sys_read(fd, buf, len)
            // fd 0 = keyboard line (hook drains one queued line); other fds
            // route to the fd table via the fd hook when installed.
            if (edx > 4096) {
                serial_puts("[user] sys_read: len too large (max 4096)\n");
                break;
            }
            if (edx > 0 && (!sys_validate || !sys_validate(ecx, edx))) {
                serial_puts("[user] sys_read: bad buffer\n");
                break;
            }
            {
                char *buf = (char*)ecx;
                int n;
                if ((int32_t)ebx == 0) {
                    if (!sys_read_hook) {
                        serial_puts("[user] sys_read: no keyboard in text mode\n");
                        syscall_set_ret((uint32_t)-1);
                        break;
                    }
                    n = sys_read_hook(buf, edx);
                    if (n == 0) {
                        // Empty kbd queue: PARK like yield (no iret) — the
                        // sh read-poll ring is the third spin-storm after
                        // wait/yield (same wedge shape: spins while siblings
                        // sit queued). Re-entry returns EAX=0 → ring 3
                        // retries the read.
                        syscall_arm_park(0);
                        break;
                    }
                } else if (sys_read_fd_hook) {
                    n = sys_read_fd_hook((int)ebx, buf, edx);
                    if (n < 0) {
                        serial_puts("[user] sys_read: bad fd\n");
                        syscall_set_ret((uint32_t)-1);
                        break;
                    }
                } else {
                    serial_puts("[user] sys_read: bad fd (only 0 supported)\n");
                    syscall_set_ret((uint32_t)-1);
                    break;
                }
                syscall_set_ret((uint32_t)n);
                UTRACE("[user] read fd=%d n=%d\n", ebx, n);
                syscall_set_ret((uint32_t)n);
            }
            break;
        }
        case SYS_OPEN: { // sys_open(path, flags)
            // Copy the user path onto the kernel stack (bounded): validate
            // page-by-page and NUL-terminate inside 256 bytes.
            if (!sys_validate || !sys_validate(ebx, 1)) {
                serial_puts("[user] sys_open: bad path\n");
                break;
            }
            {
                char kpath[256];
                uint32_t i = 0;
                const char *up = (const char*)ebx;
                while (i < 255) {
                    if (!sys_validate(ebx + i, 1)) break;
                    kpath[i] = up[i];
                    if (up[i] == 0) break;
                    i++;
                }
                kpath[255] = 0;
                if (up[i] != 0 && i == 255) {
                    serial_puts("[user] sys_open: path too long\n");
                    syscall_set_ret((uint32_t)-1);
                    break;
                }
                if (!sys_open_hook) {
                    serial_puts("[user] sys_open: unavailable in text mode\n");
                    syscall_set_ret((uint32_t)-1);
                    break;
                }
                int fd = sys_open_hook(kpath, ecx);
                syscall_set_ret((uint32_t)fd);
                UTRACE("[user] open '%s' -> fd=%d\n", kpath, fd);
                syscall_set_ret((uint32_t)fd);
            }
            break;
        }
        case SYS_CLOSE: { // sys_close(fd)
            if (!sys_close_hook) {
                serial_puts("[user] sys_close: unavailable in text mode\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            int rc = sys_close_hook((int)ebx);
            syscall_set_ret((uint32_t)rc);
            UTRACE("[user] close fd=%d rc=%d\n", ebx, rc);
            syscall_set_ret((uint32_t)rc);
            break;
        }
        case SYS_FORK: { // sys_fork()
            if (!sys_fork_hook) {
                serial_puts("[user] sys_fork: unavailable in text mode\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            int child = sys_fork_hook();
            syscall_set_ret((uint32_t)child);
            UTRACE("[user] fork -> %d\n", child);
            syscall_set_ret((uint32_t)child);
            break;
        }
        case SYS_EXEC: { // sys_exec(path)
            if (!sys_validate || !sys_validate(ebx, 1)) {
                serial_puts("[user] sys_exec: bad path\n");
                break;
            }
            {
                char kpath[256];
                uint32_t i = 0;
                const char *up = (const char*)ebx;
                // EBX TRACE (input-bug bisect 2026-09-08): raw user pointer
                // + first bytes AT TRAP TIME — caller garbage vs kernel-copy
                // corruption (see plan NEXT-1).
                serial_printf("[exec-ebx] ebx=%x pid=", ebx);
                {
                    extern void *process_current(void) __attribute__((weak));
                    uint32_t who = 0xFFFFFFFF;
                    if (process_current) {
                        uint32_t *pcb = (uint32_t*)process_current();
                        if (pcb) who = pcb[0];
                    }
                    serial_printf("%d bytes=", who);
                }
                for (uint32_t bi = 0; bi < 16; bi++) {
                    if (!sys_validate(ebx + bi, 1)) {
                        serial_puts("(unmapped)");
                        break;
                    }
                    uint8_t b = up[bi];
                    serial_putchar("0123456789abcdef"[b >> 4]);
                    serial_putchar("0123456789abcdef"[b & 15]);
                    serial_putchar(' ');
                    if (!b) break;
                }
                serial_putchar('\n');
                while (i < 255) {
                    if (!sys_validate(ebx + i, 1)) break;
                    kpath[i] = up[i];
                    if (up[i] == 0) break;
                    i++;
                }
                kpath[255] = 0;
                if (up[i] != 0 && i == 255) {
                    serial_puts("[user] sys_exec: path too long\n");
                    syscall_set_ret((uint32_t)-1);
                    break;
                }
                if (!sys_exec_hook) {
                    serial_puts("[user] sys_exec: unavailable in text mode\n");
                    syscall_set_ret((uint32_t)-1);
                    break;
                }
                int rc = sys_exec_hook(kpath);
                if (rc == 0) {
                    // Success: redirect THIS trap's iret into the new image.
                    // sys_proc_exec replaced user-low + set user_eip/esp on
                    // the CURRENT pcb. idt.c reads the redirect flag after
                    // dispatch and irets to user_eip/esp instead of the
                    // trapped EIP/ESP (same address space — CR3 unchanged).
                    // (Direct call: syscall.c and syscall_arm_exec_redirect
                    // live in the same TU — no weak-alias guard needed.)
                    syscall_arm_exec_redirect();
                    // EXEC-ARG TRACE (input-bug bisect 2026-09-08): log the
                    // validated path bytes hex — proves whether the garbage
                    // is already in EBX at trap time (caller corruption) or
                    // introduced later (kernel copy corruption).
                    serial_puts("[exec-arg] bytes=");
                    for (uint32_t bi = 0; bi < 16 && kpath[bi]; bi++) {
                        uint8_t b = (uint8_t)kpath[bi];
                        serial_putchar("0123456789abcdef"[b >> 4]);
                        serial_putchar("0123456789abcdef"[b & 15]);
                        serial_putchar(' ');
                    }
                    serial_putchar('\n');
                    // No set_ret: the new image starts fresh (EAX undefined,
                    // same as process entry). Serial first (ordering).
                    serial_printf("[user] exec '%s' rc=%d\n", kpath, rc);
                } else {
                    syscall_set_ret((uint32_t)rc);
                    serial_printf("[user] exec '%s' rc=%d\n", kpath, rc);
                    syscall_set_ret((uint32_t)rc);
                }
            }
            break;
        }
        case SYS_SBRK: { // sys_sbrk(inc)
            if (!sys_sbrk_hook) {
                serial_puts("[user] sys_sbrk: unavailable in text mode\n");
                syscall_set_ret(0);
                break;
            }
            uint32_t old = sys_sbrk_hook(ebx);
            syscall_set_ret(old);
            UTRACE("[user] sbrk inc=%d old=%x\n", ebx, old);
            syscall_set_ret(old);
            break;
        }
        case SYS_PIPE: { // sys_pipe(int[2])
            if (!sys_validate || !sys_validate(ebx, 8)) {
                serial_puts("[user] sys_pipe: bad pointer\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            if (!sys_pipe_hook) {
                serial_puts("[user] sys_pipe: unavailable in text mode\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            int rc = sys_pipe_hook((int*)ebx);
            syscall_set_ret((uint32_t)rc);
            UTRACE("[user] pipe rc=%d\n", rc);
            break;
        }
        case SYS_DUP: { // sys_dup(oldfd)
            if (!sys_dup_hook) {
                serial_puts("[user] sys_dup: unavailable in text mode\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            int nfd = sys_dup_hook((int)ebx);
            syscall_set_ret((uint32_t)nfd);
            UTRACE("[user] dup %d -> %d\n", ebx, nfd);
            syscall_set_ret((uint32_t)nfd);
            break;
        }
        case SYS_WAIT: { // sys_wait(pid)
            // Trap context can never sleep (stub must popa+iret): park the
            // caller as BLOCKED when a matching child exists but none is a
            // zombie yet (wakeup in sys_proc_exit_current via SIG_CHLD). No
            // child at all → -1 immediately (init/sh rely on this).
            if (!sys_wait_hook) {
                serial_puts("[user] sys_wait: unavailable in text mode\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            extern int sys_proc_wait_park(int pid) __attribute__((weak));
            int got;
            if (sys_proc_wait_park) {
                got = sys_proc_wait_park((int)ebx);
            } else {
                got = sys_wait_hook((int)ebx);
            }
            if (got == -2) {
                // No zombie yet: PARK (no iret) — same mechanism as yield.
                // The drain runs the queued child THIS iteration; on
                // re-entry EAX=-2 so ring 3 retries and reaps. (Returning
                // -2 via iret is what wedged forktest: the parent spun
                // 10M traps while the child sat queued behind a drain that
                // only runs on exits.)
                syscall_arm_park((uint32_t)-2);
                break;
            }
            syscall_set_ret((uint32_t)got);
            UTRACE("[user] wait %d -> %d\n", ebx, got);
            syscall_set_ret((uint32_t)got);
            break;
        }
        case SYS_KILL: { // sys_kill(pid, signo)
            if (!sys_kill_hook) {
                serial_puts("[user] sys_kill: unavailable in text mode\n");
                syscall_set_ret((uint32_t)-1);
                break;
            }
            int rc = sys_kill_hook((int)ebx, (int)ecx);
            syscall_set_ret((uint32_t)rc);
            UTRACE("[user] kill %d sig=%d rc=%d\n", ebx, ecx, rc);
            syscall_set_ret((uint32_t)rc);
            break;
        }
        case SYS_MMAP: { // sys_mmap(virt|0=auto)
            if (!sys_mmap_anon_hook) {
                serial_puts("[user] sys_mmap: unavailable in text mode\n");
                syscall_set_ret(0);
                break;
            }
            if (ebx != 0 && ((ebx & 0xFFF) || ebx >= KERNEL_VBASE)) {
                serial_puts("[user] sys_mmap: bad address\n");
                syscall_set_ret(0);
                break;
            }
            uint32_t got = sys_mmap_anon_hook(ebx);
            syscall_set_ret(got);
            UTRACE("[user] mmap -> %x\n", got);
            syscall_set_ret(got);
            break;
        }
        case SYS_MUNMAP: { // sys_munmap(virt)
            if ((ebx & 0xFFF) || ebx >= KERNEL_VBASE || ebx == 0) {
                serial_puts("[user] sys_munmap: bad address\n");
                break;
            }
            if (!sys_munmap_hook) {
                serial_puts("[user] sys_munmap: unavailable in text mode\n");
                break;
            }
            sys_munmap_hook(ebx);
            UTRACE("[user] munmap %x\n", ebx);
            break;
        }
        default:
            serial_printf("[user] unknown syscall %d\n", eax);
            break;
    }
}

#ifndef SYS_PROC_H
#define SYS_PROC_H

#include <stdint.h>

// Desktop-only process/file/IPC backend behind syscall hooks (needs
// process.o + paging.o + filesystem.o + window.o — desktop link only).
// syscall.c stays COMMON and calls through these; text mode never links
// this file (its syscalls fail safe via NULL hooks).

// fd-table over the VFS + pipes (per calling process).
int sys_proc_open(const char *path, uint32_t flags);
int sys_proc_close(int fd);
int sys_proc_write_fd(int fd, const char *buf, uint32_t len);
int sys_proc_read_fd(int fd, char *buf, uint32_t len);

// Keyboard line queue: shell offers completed lines, sys_read fd 0 drains.
void sys_proc_kbd_offer(const char *line, uint32_t len);
int sys_proc_kbd_read(char *buf, uint32_t len);

// Owning terminal window of the CURRENT process (term_win field, -1 none).
// Used by the SYS_WRITE fast path so stdio follows the spawner, not focus.
int sys_proc_term_win(void);

// Process lifecycle: fork (copy address space), exec (ELF from VFS),
// sbrk (user heap), wait/kill (zombies + signals).
int sys_proc_fork(void);
int sys_proc_exec(const char *path);
uint32_t sys_proc_sbrk(uint32_t inc);
int sys_proc_wait(int pid);
// Blocking wait for kernel-thread callers only (never trap context).
int sys_proc_wait_blocking(int pid);
// Trap-context wait: reap a ready zombie, -1 when no child matches, else
// -2 (WAIT_PARK — child alive but not exited; caller retries). Never blocks:
// the entry drain runs queued siblings on the next loop iteration, so a
// spinning retry converges in ~1ms without any tick or wakeup machinery.
// (An earlier draft parked as BLOCKED+wake-on-SIG_CHLD; that wedged the
// drain order — see sys_proc_wait_park.)
int sys_proc_wait_park(int pid);
// Yield-park: mark the caller BLOCKED-waiting-for-timeslice (woken by any
// tick once a sibling has run) and return 0. Lets a spinning waiter (init's
// wait/yield storm) give the entry drain + tick a chance to run queued
// siblings instead of owning ring 3 forever.
int sys_yield_park(void);
// Record current-process exit status + zombie + parent notify (trap-safe).
void sys_proc_exit_current(int code);
// Reparent a dead process's children to init (pid 1).
void sys_proc_reparent_to_init(uint32_t dead_pid);
int sys_proc_kill(int pid, int signo);

// Anonymous user mapping (fresh zero page at virt, or auto-pick) + unmap.
uint32_t sys_proc_mmap_anon(uint32_t virt);
void sys_proc_munmap(uint32_t virt);

// Pipes: pipe(), dup(), and the shared open-file refcounting.
int sys_proc_pipe(int *fds);
int sys_proc_dup(int oldfd);
// Signals: deliver + drain pending (called at safe points).
void sys_proc_signal_deliver(int pid, int signo);
void sys_proc_drain_signals(void);

// Spinlock/mutex self-test entry (shell command): exercises the primitives
// without SMP hardware and reports PASS/FAIL on serial.
void sys_proc_lock_selftest(void);

#endif

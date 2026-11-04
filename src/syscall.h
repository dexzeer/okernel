#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// Handle int 0x80 syscalls — called from isr_handler
void syscall_handler(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);

// Ring-3 return-value staging (handler -> idt.c saved-RAX slot).
void syscall_set_ret(uint64_t v);
uint64_t syscall_take_ret(void);

// Trapped ring-3 resume state (idt.c stashes pre-dispatch; fork consumes).
struct trapframe;
void syscall_stash_trap(const struct trapframe* f);
uint64_t syscall_trap_rip(void);
uint64_t syscall_trap_rsp(void);
void syscall_trap_regs(uint64_t out[6]);     // the trapped callee-saved registers: rbx rbp r12 r13 r14 r15
void syscall_load_park_regs(uint32_t pid);   // stage the registers of a parked program for its enter_user_mode

// Desktop-only services behind hooks (installed by desktop.c at boot):
// validate(user-virt range) + write-to-focused-terminal. Text mode leaves
// them NULL (pointer syscalls fail safe). Keeps COMMON linkable by text.
typedef int (*sys_validate_fn)(uint64_t virt, uint64_t len);
typedef int (*sys_write_hook_fn)(const char *buf, uint32_t len);
typedef int (*sys_write_win_hook_fn)(int win_id, const char *buf, uint32_t len);
typedef int (*sys_pid_hook_fn)(void);
typedef void (*sys_yield_hook_fn)(void);
typedef void (*sys_mmap_hook_fn)(uint64_t virt, uint64_t phys);
typedef void (*sys_munmap_hook_fn)(uint64_t virt);
typedef int (*sys_read_hook_fn)(char *buf, uint32_t len);
typedef int (*sys_open_hook_fn)(const char *path, uint32_t flags);
typedef int (*sys_close_hook_fn)(int fd);
typedef int (*sys_write_fd_hook_fn)(int fd, const char *buf, uint32_t len);
typedef int (*sys_read_fd_hook_fn)(int fd, char *buf, uint32_t len);
typedef int (*sys_fork_hook_fn)(void);
typedef int (*sys_exec_hook_fn)(const char *path);
typedef uint64_t (*sys_sbrk_hook_fn)(uint64_t inc);
typedef int (*sys_pipe_hook_fn)(int *fds);
typedef int (*sys_dup_hook_fn)(int oldfd);
typedef int (*sys_wait_hook_fn)(int pid);
typedef int (*sys_kill_hook_fn)(int pid, int signo);
typedef uint64_t (*sys_mmap_anon_hook_fn)(uint64_t virt);
void syscall_install(sys_validate_fn v, sys_write_hook_fn w);
void syscall_install_winwrite(sys_write_win_hook_fn w);
void syscall_install_proc(sys_pid_hook_fn p, sys_yield_hook_fn y);
void syscall_install_mmap(sys_mmap_hook_fn m);
void syscall_install_full(sys_munmap_hook_fn mu, sys_read_hook_fn r,
                           sys_open_hook_fn o, sys_close_hook_fn c,
                           sys_write_fd_hook_fn wf, sys_read_fd_hook_fn rf,
                           sys_fork_hook_fn fk, sys_exec_hook_fn ex,
                           sys_sbrk_hook_fn sb, sys_pipe_hook_fn pp,
                           sys_dup_hook_fn dp, sys_wait_hook_fn wt,
                           sys_kill_hook_fn kl, sys_mmap_anon_hook_fn ma);

// 1 while a user program runs (shell keeps redrawing behind it), 0 in shell.
// sys_exit clears it and latches completion; the main loop announces once.
extern volatile int syscall_can_exit;

// Entry run queue: stage pid+rip+rsp+owning-window per spawn; the main
// loop drains in order.
int entry_enqueue(int pid, uint64_t rip, uint64_t rsp, int win_id);
int entry_dequeue(int *pid, uint64_t *rip, uint64_t *rsp, int *win_id);
int entry_pending_any(void);

// Drain-side pid stash (main-loop IRET locals go stale across enter/exit).
void drain_stash_pid(int pid);
int drain_last_pid(void);

// Returns 1 exactly once per sys_exit (main-loop poll, non-blocking).
int user_mode_poll_finished(void);

// Latch completion for the idt.c SYS_EXIT hijack path (bypasses handler).
void syscall_note_exited(void);

// Exec redirect: SYS_EXEC success arms; idt.c consumes after dispatch and
// irets to the new image (user_rip/rsp) instead of the trapped RIP/RSP.
void syscall_arm_exec_redirect(void);
int syscall_take_exec_redirect(void);

// Park: blocking syscalls arm (retval + trapped RIP/RSP); idt.c consumes
// after dispatch and jmps to user_park_trampoline (trap discarded, main
// loop resumes). Drain re-enters the parker with park_ret in RAX.
// Per-slot like the trap stash. Drain-side getters take an explicit pid
// (the drain runs as pid 0 — self-slot would read the wrong slot).
void syscall_arm_park(uint32_t retval);
int syscall_take_park(void);
uint32_t syscall_take_park_ret(void);
void syscall_stash_park_ret(uint32_t v);
uint64_t syscall_park_rip(void);
uint64_t syscall_park_rsp(void);
uint64_t syscall_park_rip_for(uint32_t pid);
uint64_t syscall_park_rsp_for(uint32_t pid);
uint32_t syscall_take_park_ret_for(uint32_t pid);
#endif

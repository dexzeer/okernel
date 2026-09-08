#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// Handle int 0x80 syscalls — called from isr_handler
void syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

// Ring-3 return-value staging (handler -> idt.c saved-EAX slot).
void syscall_set_ret(uint32_t v);
uint32_t syscall_take_ret(void);

// Trapped ring-3 resume state (idt.c stashes pre-dispatch; fork consumes).
void syscall_stash_trap(uint32_t eip, uint32_t esp);
void syscall_stash_trap_full(uint32_t eip, uint32_t esp, uint32_t ebx,
                             uint32_t edi, uint32_t esi, uint32_t ebp,
                             int have_regs);
uint32_t syscall_trap_eip(void);
uint32_t syscall_trap_esp(void);
uint32_t syscall_trap_ebx(void);
uint32_t syscall_trap_edi(void);
uint32_t syscall_trap_esi(void);
uint32_t syscall_trap_ebp(void);

// Desktop-only services behind hooks (installed by desktop.c at boot):
// validate(user-virt range) + write-to-focused-terminal. Text mode leaves
// them NULL (pointer syscalls fail safe). Keeps COMMON linkable by text.
typedef int (*sys_validate_fn)(uint32_t virt, uint32_t len);
typedef int (*sys_write_hook_fn)(const char *buf, uint32_t len);
typedef int (*sys_write_win_hook_fn)(int win_id, const char *buf, uint32_t len);
typedef int (*sys_pid_hook_fn)(void);
typedef void (*sys_yield_hook_fn)(void);
typedef void (*sys_mmap_hook_fn)(uint32_t virt, uint32_t phys);
typedef void (*sys_munmap_hook_fn)(uint32_t virt);
typedef int (*sys_read_hook_fn)(char *buf, uint32_t len);
typedef int (*sys_open_hook_fn)(const char *path, uint32_t flags);
typedef int (*sys_close_hook_fn)(int fd);
typedef int (*sys_write_fd_hook_fn)(int fd, const char *buf, uint32_t len);
typedef int (*sys_read_fd_hook_fn)(int fd, char *buf, uint32_t len);
typedef int (*sys_fork_hook_fn)(void);
typedef int (*sys_exec_hook_fn)(const char *path);
typedef uint32_t (*sys_sbrk_hook_fn)(uint32_t inc);
typedef int (*sys_pipe_hook_fn)(int *fds);
typedef int (*sys_dup_hook_fn)(int oldfd);
typedef int (*sys_wait_hook_fn)(int pid);
typedef int (*sys_kill_hook_fn)(int pid, int signo);
typedef uint32_t (*sys_mmap_anon_hook_fn)(uint32_t virt);
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

// Main-loop IRET staging (shell arms, loop consumes once, then IRETs).
extern volatile uint32_t user_entry_eip;
extern volatile uint32_t user_entry_esp;
extern volatile int user_entry_pending;
extern volatile int user_entry_pid;

// Entry run queue (replaces the single slot above for new code): stage
// pid+eip+esp+owning-window per spawn; the main loop drains in order.
int entry_enqueue(int pid, uint32_t eip, uint32_t esp, int win_id);
int entry_dequeue(int *pid, uint32_t *eip, uint32_t *esp, int *win_id);
int entry_pending_any(void);

// Drain-side pid stash (main-loop IRET locals go stale across enter/exit).
void drain_stash_pid(int pid);
int drain_last_pid(void);

// Ring-3 test image source (installed by desktop.c at boot; syscall.c is
// COMMON and cannot reference the linked user_mode_test symbol directly).
void syscall_install_usertest(const uint8_t *page, uint32_t off);
void syscall_install_usertest_len(uint32_t len);
uint8_t* user_test_page_base(void);
uint32_t user_test_page_off(void);
uint32_t user_test_len(void);

// Returns 1 exactly once per sys_exit (main-loop poll, non-blocking).
int user_mode_poll_finished(void);

// Latch completion for the idt.c SYS_EXIT hijack path (bypasses handler).
void syscall_note_exited(void);

// Exec redirect: SYS_EXEC success arms; idt.c consumes after dispatch and
// irets to the new image (user_eip/esp) instead of the trapped EIP/ESP.
void syscall_arm_exec_redirect(void);
int syscall_take_exec_redirect(void);

// Park: blocking syscalls arm (retval + trapped EIP/ESP); idt.c consumes
// after dispatch and jmps to user_park_trampoline (trap discarded, main
// loop resumes). Drain re-enters the parker with park_ret in EAX.
// Per-slot like the trap stash. Drain-side getters take an explicit pid
// (the drain runs as pid 0 — self-slot would read the wrong slot).
void syscall_arm_park(uint32_t retval);
int syscall_take_park(void);
uint32_t syscall_take_park_ret(void);
void syscall_stash_park_ret(uint32_t v);
uint32_t syscall_park_eip(void);
uint32_t syscall_park_esp(void);
uint32_t syscall_park_eip_for(uint32_t pid);
uint32_t syscall_park_esp_for(uint32_t pid);
uint32_t syscall_take_park_ret_for(uint32_t pid);
#endif

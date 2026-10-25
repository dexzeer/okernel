#ifndef LX_H
#define LX_H

// The Linux subsystem: runs x86-64 Linux programs (static or dynamically linked ELF) natively in ring 3 of the long-mode
// CPU, translating their system calls to the kernel. See boot/lm.asm for the way in and out of ring 3.

#include <stdint.h>
#include "lxfs.h"

// ---- errno (Linux values) ---------------------------------------------------------------------------------------
#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_ESRCH 3
#define LX_EINTR 4
#define LX_EIO 5
#define LX_ENXIO 6
#define LX_E2BIG 7
#define LX_ENOEXEC 8
#define LX_EBADF 9
#define LX_ECHILD 10
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_EBUSY 16
#define LX_EEXIST 17
#define LX_EXDEV 18
#define LX_ENODEV 19
#define LX_ENOTDIR 20
#define LX_EISDIR 21
#define LX_EINVAL 22
#define LX_ENFILE 23
#define LX_EMFILE 24
#define LX_ENOTTY 25
#define LX_EFBIG 27
#define LX_ENOSPC 28
#define LX_ESPIPE 29
#define LX_EROFS 30
#define LX_EPIPE 32
#define LX_EDOM 33
#define LX_ERANGE 34
#define LX_ENAMETOOLONG 36
#define LX_ENOSYS 38
#define LX_ENOTEMPTY 39
#define LX_ELOOP 40
#define LX_ETIMEDOUT 110

// ---- the registers of a task: saved by boot/lm.asm, laid out as the CPU and the entry stubs push them ------------
struct lx_regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err;                       // vec: interrupt vector, 0x100 = SYSCALL entry
    uint64_t rip, cs, rflags, rsp, ss;
};

// ---- memory map of a task ---------------------------------------------------------------------------------------
#define LX_PROT_NONE 0
#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4

#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANON 0x20
#define LX_MAP_GROWSDOWN 0x100
#define LX_MAP_NORESERVE 0x4000

struct lx_vma {
    uint32_t start, end;                     // page aligned, end exclusive; the user space is below 3 GB
    uint32_t prot, flags;
    struct lx_node *node;                    // file backing (private mappings copy from it on first touch)
    uint64_t off;                            // file offset of `start`
    uint32_t fend;                           // file bytes are copied only below this address (the rest of a segment is bss)
    uint8_t  shared_fb;                      // maps the framebuffer window instead of anonymous memory
};

#define LX_USER_BASE   0x00010000u
#define LX_USER_TOP    0xC0000000u
#define LX_STACK_TOP   0xBFFFF000u
#define LX_STACK_SIZE  (8u << 20)
#define LX_MMAP_TOP    0xB0000000u

// ---- open files -------------------------------------------------------------------------------------------------
#define LX_O_RDONLY 0
#define LX_O_WRONLY 1
#define LX_O_RDWR 2
#define LX_O_ACCMODE 3
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_NOCTTY 0400
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_DIRECTORY 0200000
#define LX_O_CLOEXEC 02000000

struct lx_sock;
enum { LXF_NODE = 1, LXF_PIPE_R, LXF_PIPE_W, LXF_TTY, LXF_SOCK, LXF_EVENT };

struct lx_pipe {
    uint8_t *buf;
    uint32_t rd, wr, count;
    int readers, writers;
};
#define LX_PIPE_SIZE 65536

struct lx_file {
    int refs;
    int kind;
    int flags;                               // O_* as passed to open (access mode, O_APPEND, O_NONBLOCK)
    struct lx_node *node;
    uint64_t off;
    struct lx_pipe *pipe;
    struct lx_sock *sock;
    struct lx_event *ev;                     // epoll / eventfd / timerfd objects (lx_ev.c)
    void *priv;                              // device state (a /dev/okwin connection)
};

// ---- tasks ------------------------------------------------------------------------------------------------------
#define LX_MAX_TASKS 64
#define LX_MAX_FDS 128
#define LX_MAX_VMAS 128
#define LX_NSIG 65                           // signals 1..64

#define LXT_FREE 0
#define LXT_RUN 1                            // runnable
#define LXT_BLOCK 2                          // waiting for block_fn
#define LXT_ZOMBIE 3                         // exited, waiting for the parent's wait4

struct lx_sigaction { uint64_t handler, flags, restorer, mask; };

#define LX_SA_NOCLDSTOP 1
#define LX_SA_NOCLDWAIT 2
#define LX_SA_SIGINFO 4
#define LX_SA_ONSTACK 0x08000000u
#define LX_SA_RESTART 0x10000000u
#define LX_SA_NODEFER 0x40000000u
#define LX_SA_RESETHAND 0x80000000u

// What threads of one program can share (clone flags): the address space, the open files, the signal handlers.
struct lx_mm {
    int refs;
    uint32_t cr3;
    struct lx_vma *vma;
    int nvma;
    uint32_t brk_start, brk;
    uint32_t mmap_hint;
    uint32_t rss;                            // pages
    void *win;                               // Windows process data (lx_pe.c), or 0
};

struct lx_files {
    int refs;
    struct lx_file *fds[LX_MAX_FDS];
    uint8_t fdflags[LX_MAX_FDS];             // bit 0: close on exec
};

struct lx_sighand {
    int refs;
    struct lx_sigaction act[LX_NSIG];
};

struct lx_task {
    struct lx_regs regs;                     // FIRST: TSS.rsp0 points just past it (16-byte aligned page start + 0)
    uint8_t  fx[512] __attribute__((aligned(16)));   // x87/SSE state
    int      state;
    int      pid;                            // thread id
    int      tgid;                           // process id (the thread group leader's pid)
    int      ppid;
    int      exit_code;                      // wait4 status
    struct lx_mm *mm;
    struct lx_files *files;
    struct lx_sighand *sh;
    uint64_t fs_base, gs_base;               // gs_base: the TEB of a Windows thread
    int      is_win;                         // a Windows (PE) program: paths are case-insensitive
    struct lx_node *cwd;
    char     comm[16];
    int      term_win;                       // terminal window of its standard files (-1: serial only)
    int      (*block_fn)(struct lx_task*);   // wake-up test of a BLOCKED task
    uint64_t block_arg;
    uint64_t wake_ms;                        // nanosleep deadline (0 = none yet)
    uint64_t sysnr;                          // number of the syscall in progress (to restart it)
    uint64_t sig_pending, sig_blocked;
    uint64_t saved_mask;                     // mask to restore at rt_sigreturn
    uint64_t sigaltstack_sp, sigaltstack_size;
    uint64_t clear_child_tid;                // futex wake + zero at thread exit
    uint64_t itimer_next_ms, itimer_interval_ms;   // ITIMER_REAL
    int      futex_woken;
    int      fg;                             // the shell waits for it
    uint32_t umask;
    int      skip_ret;                       // the syscall replaced the registers (execve): do not store a result
    int      in_signal;
    int      blocked_rewound;                // the blocked call was rewound (rip points at the syscall) and will be repeated
    int      block_complete;                 // the call that blocked is done (result stored): do not repeat it on wake-up
    int      execd;                          // has called execve (a vfork parent waits for it)
    uint64_t futex_addr;
    int      futex_state;                    // 0 none, 1 waiting, 2 woken, 3 timed out
    char     exe[128];                       // path of the running program (/proc/self/exe)
};

// lx_sig.c
void lx_signal(struct lx_task *t, int sig);
void lx_signal_group(int tgid, int sig);

// lx.c
void lx_init(void);
// Start a program (path in the Linux file system) with argv/envp; returns the pid or a negative errno.
int lx_spawn(const char *path, const char *const *argv, const char *const *envp, int term_win);
// Main-loop hook: runs every runnable task for a slice. Returns 1 if any task is alive (the loop must not sleep long).
int lx_poll(void);
int lx_alive(void);
int lx_busy(void);                           // a task is runnable: the main loop must not sleep
// Is the task `pid` still running? (the foreground wait of the shell)
int lx_pid_alive(int pid);
void lx_report(char *out, int cap);

// lx_pe.c
int lx_pe_load(struct lx_task *t, struct lx_node *exe, const char *path, const char *const *argv, const char *const *envp);
int lx_pe_missing_import(struct lx_task *t, uint64_t rip);
int64_t lx_pe_loadlibrary(struct lx_task *t, uint64_t uname);
int64_t lx_pe_getproc(struct lx_task *t, uint64_t base, uint64_t uname, uint64_t ordinal);
extern int lxfs_case_insensitive;

// lx_elf.c
int lx_exec_load(struct lx_task *t, const char *path, const char *const *argv, const char *const *envp);

// helpers shared by the lx_*.c files
struct lx_task *lx_current(void);
struct lx_vma *lx_vma_find(struct lx_task *t, uint32_t addr);
int lx_map_region(struct lx_task *t, uint32_t start, uint32_t len, uint32_t prot, uint32_t flags, struct lx_node *node, uint64_t off);
int lx_unmap_region(struct lx_task *t, uint32_t start, uint32_t len);
int lx_protect_region(struct lx_task *t, uint32_t start, uint32_t len, uint32_t prot);
uint32_t lx_find_free(struct lx_task *t, uint32_t len);
void lx_free_user(struct lx_task *t);
// Make [addr, addr+len) readable (write=0) or writable (write=1) from the kernel: pages are brought in. 0 or -EFAULT.
int lx_user_range(struct lx_task *t, uint64_t addr, uint64_t len, int write);
int lx_copy_in(struct lx_task *t, void *dst, uint64_t src, uint64_t len);
int lx_copy_out(struct lx_task *t, uint64_t dst, const void *src, uint64_t len);
int lx_copy_str(struct lx_task *t, char *dst, uint64_t src, uint32_t max);   // length, or -EFAULT / -ENAMETOOLONG
void lx_block(struct lx_task *t, int (*fn)(struct lx_task*), uint64_t arg);
int64_t lx_now_ms(void);

// lx_net.c / lx_ev.c
void lx_sock_close(struct lx_sock *s);
int lx_sock_readable(struct lx_sock *s);
int lx_sock_writable(struct lx_sock *s);
int lx_sock_hup(struct lx_sock *s);
struct lx_event;
void lx_event_free(struct lx_event *e);
int lx_event_ready(struct lx_task *t, struct lx_file *f, int want);

// lx_sys.c
int64_t lx_syscall_dispatch(struct lx_task *t);
struct lx_file *lx_file_new(int kind, int flags, struct lx_node *node);
void lx_file_put(struct lx_file *f);
int lx_fd_alloc(struct lx_task *t, struct lx_file *f, int minfd, int cloexec);
void lx_task_exit(struct lx_task *t, int status);

#endif

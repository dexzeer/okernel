#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define MAX_PROCESSES 16
#define PROCESS_STACK_SIZE 4096

// Process states
#define PROC_UNUSED   0
#define PROC_READY    1
#define PROC_RUNNING  2
#define PROC_BLOCKED  3
#define PROC_EXITED   4
#define PROC_ZOMBIE   5   // exited, exit code kept for wait()

// FD table per process: 0 = keyboard/serial-in, 1 = terminal-out,
// 2 = terminal-out (stderr alias), 3+ = files/pipes. -1 = free slot.
#define PROC_MAX_FDS 16
#define PROC_FD_FREE   -1
#define PROC_FD_TERM   -2   // terminal output (window)
#define PROC_FD_KBD    -3   // keyboard input (line queue)

// Open file description (shared across dup/fork via refcount).
#define OFD_FILE  1
#define OFD_PIPE  2
// CLOEXEC-lite: fds flagged here close on exec (see sys_proc_exec). Syscalls
// to set/clear it don't exist yet — the shell/run path sets it on pipes it
// creates for pipelines (future); default 0 = keep-open (POSIX default).
#define OFD_CLOEXEC 0x10000
struct open_file {
    int kind;            // OFD_FILE / OFD_PIPE
    int flags;           // open flags (files) / unused (pipes)
    int refcount;
    // File backing: VFS name (NUL-terminated, FS_MAX_NAME) + offset.
    char name[32];
    uint32_t offset;
    // Pipe backing: index into the pipe table (sys_proc.c owns it).
    int pipe_idx;
    int pipe_write;      // 1 = write end, 0 = read end
};

// Signals (bitmask in pending_signals). Only a handful: TERM kills,
// CHLD notifies wait(), USR1 is user-defined. Handlers are not supported
// (no ring-3 signal frames yet) — default actions only.
#define SIG_TERM 0
#define SIG_CHLD 1
#define SIG_USR1 2
#define PROC_NSIG 3

// Process control block.
// Lifetime: created by process_create (usermode command / fork), reaped by
// process_destroy (sys_exit path / shell). page_dir is the PHYS address of a
// private directory (PD 0-767 private user-low, PD 768-1023 shared kernel).
// kernel_stack is a HIGH virtual address (kmalloc'd); esp0_top is the TSS
// value (stack top) used on ring-3 -> ring-0 traps for THIS process.
struct process {
    uint32_t pid;
    uint32_t state;        // PROC_*
    uint32_t page_dir;     // Physical address of page directory (for CR3)
    uint32_t esp;          // Kernel stack pointer (saved on context switch)
    uint32_t ebp;          // Base pointer (saved on context switch)
    uint32_t eip;          // Switch-seed flag: 0 = never switched-to (seed on
                           // first switch_to), 1 = seeded/live (never an addr)
    uint32_t entered_ring3; // 1 once the main-loop entry drain IRETed here
    uint32_t user_esp;     // User-mode stack pointer
    uint32_t user_eip;     // User-mode instruction pointer
    uint32_t kernel_stack; // Kernel thread stack HIGH virt (kmalloc'd 4K).
                           // Preemption frames live here (seed/saves).
    uint32_t trap_stack;   // Ring-3 trap stack HIGH virt (kmalloc'd 4K).
                           // TSS.ESP0 points here (pusha frames on syscalls).
    uint32_t esp0_top;     // TSS ESP0 value = trap_stack + SIZE (NOT kstack)
    uint32_t ticks_left;   // Scheduler time slice remaining (timer ticks)
    uint32_t parent_pid;   // fork parent (0 = spawned from shell)
    int exit_code;         // wait() status once ZOMBIE/EXITED
    uint32_t pending_signals; // bitmask (1<<SIG_*)
    uint32_t heap_break;   // sbrk-managed user heap top (0 = not started)
    uint32_t mmap_next;    // next auto-mmap hint (grows down from stack gap)
    int fds[PROC_MAX_FDS]; // open-file indices, PROC_FD_* or -1 free
    int term_win;        // owning terminal window for stdio (PROC_FD_TERM
                          // backing); -1 = none (falls back to focused)
    uint32_t generation;   // per-slot lifetime counter: bumped on every
                            // create (never cleared by destroy) so a tick that
                            // snapshots a PCB before its cli can detect slot
                            // reuse under it (see process_switch_to). LAST
                            // field by ABI convention: idt.c's exec-redirect
                            // reads user_eip/esp by offsetof (see below), but
                            // keeping growth at the tail minimizes churn for
                            // any future raw-offset readers.
};

// Initialize the process table
void process_init(void);

// Re-capture pid 0's live thread ESP/EBP (call right before sti).
void process_capture_idle_esp(void);

// Arm pid 0's slice once before sti (first tick decrements, not expires).
void process_idle_arm(void);

// Create a new process with a fresh page directory
// Returns PID, or -1 on failure
int process_create(void);

// Destroy a process and free its resources
void process_destroy(uint32_t pid);

// Get the process control block for a PID
struct process* process_get(uint32_t pid);

// Get the currently running process
struct process* process_current(void);

// Switch to a different process (updates CR3, saves/restores registers)
void process_switch(uint32_t pid);

// Full kernel-thread switch (Phase 2 preemption): saves the outgoing
// thread's frame (EBX/ESI/EDI/EBP/ESP+EIP via context_switch stub), loads
// CR3 + TSS.ESP0 + ESP for the incoming thread. IRQs OFF across the whole
// switch. Safe from timer IRQ AND from kernel threads — NEVER from ring-3
// trap context (the isr_common_stub frame must popa+iret, not be switched
// under — see the sched_yield trap-safe rule).
void process_switch_to(uint32_t pid);

// Drain-deferral guard (see desktop.c entry drain): 1 while the main loop
// holds a dequeued-but-not-yet-entered child — the tick must not switch
// mid-drain (it would capture the drain's half-built frame as the child's
// thread). Owned by process_switch_to (checks+sets); the drain sets it
// across dequeue→IRET and clears it post-exit/park.
extern volatile int switch_busy;

// Get the next ready process (round-robin)
uint32_t process_next(void);

// Translate a user fd to an open-file index (or PROC_FD_* national value).
// Returns PROC_FD_FREE (-1) when the slot is free/invalid.
int process_fd_get(struct process *p, int fd);

// Allocate the lowest free fd slot >= 3 for an open-file index.
int process_fd_alloc(struct process *p, int ofd_idx);

// Free an fd slot (caller drops the open-file refcount separately).
void process_fd_free(struct process *p, int fd);

// Resolve a user fd for the CURRENT process (syscall fast path).
int sys_fd_resolve(int fd);

#endif
struct process* process_get_by_slot(int slot);

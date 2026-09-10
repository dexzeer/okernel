#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

// Cooperative + timer-driven round-robin scheduler over struct process.
// Slice length in timer ticks (PIT = 100Hz: 10 = ~100ms per process).
#define SCHED_SLICE_TICKS 10

// Called from the timer IRQ (IRQ0) on every tick. PREEMPTIVE round-robin:
// counts the running slice down and switches (process_switch_to) on expiry.
// Safe: the context_switch stub saves/restores full kernel-thread frames.
// NEVER call process_switch_to from ring-3 trap context (use sched_yield's
// slice-reset there). No-op with no user process live (pid 0 keeps running).
void sched_tick(void);

// Voluntarily give up the rest of the time slice (sys_yield path).
void sched_yield(void);

// Spawn an ELF program from a VFS path as a process: allocates PCB + private
// address space, loads PT_LOAD segments + fresh user stack IN THAT SPACE,
// builds argc/argv/env on the user stack, marks READY. Returns pid or -1.
// Does NOT enter ring 3 (entry goes through the main-loop drain like any
// spawn). argv/envp are kernel-side string arrays (may be NULL/empty).
int sched_spawn_elf(const char *path, char *const argv[], char *const envp[],
                    int win_id);

// Switch address space + ESP0 to a process WITHOUT touching the running
// thread (used to BUILD the space: map + copy while kernel PD is live).
// After building, sched_activate(pid) makes it current for ring-3 entry.
void sched_prepare(uint32_t pid);

// Undo sched_prepare: back to the kernel PD + pid-0 idle trap stack.
// Called on the trampoline-resumed frame BEFORE touching kernel state.
void sched_unprepare(void);

// Reap a finished user process: frees its user-low pages + PD + PCB slot.
// Kernel-high mappings are shared and never freed (see process_destroy).
// Reparents the dead process's children to init first (no orphan waits).
void sched_reap(uint32_t pid);

// Stage a fork child's ring-3 entry (enqueue + fork-return-0 flag).
// Trap-safe (static slots only). Returns 0 on success, -1 when full.
int sched_fork_child_entry(uint32_t child_pid);

// Flag a pid as a fork child (its next entry IRET forces saved-EAX 0).
void sched_fork_mark_child(uint32_t pid);

// 1 if pid is a flagged fork child (consumes the flag), else 0.
int sched_fork_take_child(uint32_t pid);

// Stage a parked thread's resume (pid + trapped EIP/ESP + EAX retval).
// The drain re-enters it via enter_user_mode once woken (tick WAKE pass or
// SIG_CHLD); the re-entry forces EAX=park_ret (separate flag from fork).
void sched_park_stage(uint32_t pid, uint32_t eip, uint32_t esp, uint32_t ret);

// 1 + EAX retval if pid has a staged park resume (consumes the flag; the
// drain calls this before enter_user_mode), else 0.
int sched_park_take(uint32_t pid, uint32_t *ret_out);

// Peek a staged park resume's EIP/ESP (drain re-entry needs them; take()
// returns the retval — call this first, then take()).
int sched_park_resume(uint32_t pid, uint32_t *eip_out, uint32_t *esp_out);

// Drop a pid's staged park resume without consuming it (fork hygiene: fresh
// children must not inherit a reused slot's stale resume; destroy hygiene:
// the dead must not leave resumes behind). Safe to call with no entry.
void sched_park_drop(uint32_t pid);

// Ring-3 trap reconcile: if the current thread is pid 0 (kernel) but a
// RUNNING user process exists (preempted ring-3 thread stolen by the main
// loop), switch CR3+ESP0+current back to it. Trap-safe (no frame changes,
// shared-high-map stacks stay valid; future traps land correctly).
void syscall_reconcile_ring3(void);

#endif

#include "process.h"
#include "sched.h"
#include "memory.h"
#include "memlayout.h"
#include "paging.h"
#include "gdt.h"
#include "serial.h"
#include <stdint.h>
#include <string.h>

static struct process processes[MAX_PROCESSES];
static uint32_t current_slot = 0; // SLOT index 0..15 (NOT a pid)
static uint32_t next_pid = 1;
// Preemption re-entrancy guard (file scope — NOT function-static: the fault
// EIP c010735c proved the block-scoped static lived at address 0x4 (BSS
// mislink under -O2) and every switch wrote ESI+4==4 → #PF e=0002 on write.
// File-scope statics link correctly; function-static volatile did not.)
volatile int switch_busy = 0;

// PID 0 (kernel idle) trap stack: ESP0 fallback whenever no user process is
// prepared. Same 4K as every other kernel_stack; never freed (pid 0 is
// immortal). Set as ESP0 at init so any early ring transition lands valid.
static uint8_t idle_stack[PROCESS_STACK_SIZE] __attribute__((aligned(16)));

void process_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].state = PROC_UNUSED;
        processes[i].pid = 0;
        processes[i].generation = 0;
    }
    // PID 0 is the kernel (idle process)
    processes[0].pid = 0;
    processes[0].state = PROC_RUNNING;
    processes[0].page_dir = 0; // Kernel uses the boot page directory
    processes[0].kernel_stack = (uint32_t)idle_stack;
    processes[0].esp0_top = (uint32_t)(idle_stack + PROCESS_STACK_SIZE);
    // The main loop runs on the BOOT stack (start.asm), NOT idle_stack — but
    // the preemption stub saves/restores p->esp per thread. Capture the live
    // boot ESP as pid 0's thread ESP so timer switches preserve the main
    // loop's frame (the old code left esp=0/ebp=0 → first switch_to pid 0
    // loaded ESP=0 and triple-faulted in context_switch, bisected 2026-09-08
    // via -d int: #PF at CR2=EIP inside context_switch after mov cr3).
    // NOTE: process_init runs on the boot stack itself (called from
    // kernel_main before the loop), so ESP here == the thread's live ESP
    // minus this frame — close enough: the stub only needs a valid frame
    // chain, and the first real save overwrites it on first preemption.
    // Better: main loop re-captures right before sti (see desktop.c).
    __asm__ volatile("mov %%esp, %0" : "=r"(processes[0].esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(processes[0].ebp));
    processes[0].ticks_left = 0;
    processes[0].parent_pid = 0;
    processes[0].exit_code = 0;
    processes[0].pending_signals = 0;
    processes[0].heap_break = 0;
    processes[0].mmap_next = 0;
    processes[0].term_win = -1;
    for (int f = 0; f < PROC_MAX_FDS; f++) processes[0].fds[f] = PROC_FD_FREE;
    processes[0].fds[0] = PROC_FD_KBD;
    processes[0].fds[1] = PROC_FD_TERM;
    processes[0].fds[2] = PROC_FD_TERM;
    tss_set_kernel_stack(processes[0].esp0_top);
    current_slot = 0;
    serial_puts("[proc] process table initialized\n");
}

// Idle slice re-arm (called once before sti): pid 0 starts with ticks 0
// (never scheduled by anyone) — arm it so the first tick decrements
// instead of falling into the expired path immediately at boot.
void process_idle_arm(void) {
    processes[0].ticks_left = 10; // == SCHED_SLICE_TICKS (see sched.h)
}

// Re-capture pid 0's live thread ESP/EBP (called from kernel_main right
// before sti — the exact frame the timer will preempt). process_init's own
// capture is one frame up the boot stack; this one is precise.
void process_capture_idle_esp(void) {
    __asm__ volatile("mov %%esp, %0" : "=r"(processes[0].esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(processes[0].ebp));
}

// Create a new page directory sharing the kernel high map (PD 768-1023).
// User low space (PD 0-767) stays zero/private. CR3 values are PHYS; tables
// are dereferenced through P2V (high kernel map covers all phys).
static uint32_t create_page_directory(void) {
    // Allocate a new page for the page directory
    uint32_t pd_phys = pmm_alloc_page();
    if (!pd_phys) return 0;

    uint32_t *new_pd = (uint32_t *)P2V_U32(pd_phys);
    // Clear the new page directory
    for (int i = 0; i < 1024; i++) new_pd[i] = 0;

    // Get the current page directory (CR3 is phys)
    uint32_t cr3_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
    uint32_t *cur_pd = (uint32_t *)P2V_U32(cr3_phys);

    // Share kernel high map + FB/MMIO supervisor entries (PD 768-1023).
    // Never copy low entries: kernel-low no longer exists, and user-low
    // must start private (the old code copied PD 0-255, leaking kernel).
    for (int i = PD_KERNEL_BASE; i < 1024; i++) {
        if (cur_pd[i] & 0x01) {
            new_pd[i] = cur_pd[i];
        }
    }

    return pd_phys;
}

int process_create(void) {
    // Find a free slot
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        serial_puts("[proc] no free process slots\n");
        return -1;
    }

    struct process *p = &processes[slot];
    p->pid = next_pid++;
    p->state = PROC_READY;
    // Lifetime bump: destroy never clears this, so any tick holding a
    // pre-cli snapshot of this slot detects reuse (generation mismatch).
    // Wraps to 0 are skipped (0 = never-used).
    p->generation++;
    if (p->generation == 0) p->generation++;

    // Create a new page directory with kernel mappings
    p->page_dir = create_page_directory();
    if (!p->page_dir) {
        serial_puts("[proc] failed to create page directory\n");
        return -1;
    }

    // Allocate a kernel stack for this process
    p->kernel_stack = (uint32_t)kmalloc(PROCESS_STACK_SIZE);
    if (!p->kernel_stack) {
        pmm_free_page(p->page_dir);
        serial_puts("[proc] failed to allocate kernel stack\n");
        return -1;
    }
    // Allocate a SEPARATE trap stack (TSS.ESP0) for ring-3 traps. It MUST
    // differ from kernel_stack: the preemption stub seeds/saves thread
    // frames at kstack_top-20, while trap frames (pusha+ds+int_num ≈ 48B+)
    // grow down from esp0_top on EVERY syscall/IRQ during ring 3. Sharing
    // one page lets trap frames overwrite the seed (and vice versa) —
    // bisected 2026-09-08: post-Back EIP=0 on pid1 kstack (seed ret slot
    // smashed by trap frames, then a switch loaded the smashed frame).
    uint32_t trap_stack = (uint32_t)kmalloc(PROCESS_STACK_SIZE);
    if (!trap_stack) {
        pmm_free_page(p->page_dir);
        serial_puts("[proc] failed to allocate trap stack\n");
        return -1;
    }

    // Set up initial stack frame (as if the process was just interrupted)
    // The kernel stack grows DOWN, so we start at the top
    uint32_t kstack_top = p->kernel_stack + PROCESS_STACK_SIZE;

    // Initial register state: all zero except eip (user code) and esp (user stack)
    p->eip = 0;      // Switch-seed flag (see process.h): 0 = unseeded
    p->entered_ring3 = 0; // Set by the main-loop entry drain on first IRET
    p->esp = kstack_top;
    p->ebp = kstack_top;
    p->user_eip = 0;
    p->user_esp = 0;
    // Trap stack is SEPARATE from kernel_stack (see above) — ESP0 points
    // there, never at the thread frame area.
    p->esp0_top = trap_stack + PROCESS_STACK_SIZE;
    p->ticks_left = 0;
    // Fork/exec/IPC bookkeeping: parent filled by fork/spawn paths (0 here =
    // spawned from the shell); standard fds pre-wired, rest free.
    p->parent_pid = 0;
    p->exit_code = 0;
    p->pending_signals = 0;
    p->heap_break = 0;
    p->mmap_next = 0;
    p->term_win = -1; // bound by the spawner (run/init/fork inherit below)
    for (int f = 0; f < PROC_MAX_FDS; f++) p->fds[f] = PROC_FD_FREE;
    p->fds[0] = PROC_FD_KBD;
    p->fds[1] = PROC_FD_TERM;
    p->fds[2] = PROC_FD_TERM;

    serial_printf("[proc] created process pid=%d (slot=%d)\n", p->pid, slot);
    return p->pid;
}

void process_destroy(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == pid && processes[i].state != PROC_UNUSED) {
            processes[i].state = PROC_EXITED;
            // Free page directory pages: USER LOW ONLY (PD 0-767). PD
            // 768-1023 are shared kernel-high tables — freeing them would
            // unmap the kernel for every other process. page_dir is PHYS.
            // USE-AFTER-FREE GUARD (bisected 2026-09-09: post-hello #PF — the
            // timer tick snapshots next_esp/next_cr3 with IF SET, then a
            // ring-3 wait() reaps the target (PMM-frees its PD pages) BEFORE
            // the tick's cli+context_switch loads the freed PD. PMM instantly
            // recycles the pages (pmm_alloc hands them to the next fork) so
            // the switch loads HALF-REUSED tables → #PF err=0 cr2=0. Fix:
            // NEVER free user pages here (leak ~8-40KB/exit; 16 slots max,
            // PMM has MBs — the hobby-OS trade). The PD/PT memory stays
            // mapped-but-orphaned; a racing tick loads STALE-BUT-VALID tables
            // (worst case: switches into a dead address space for one slice,
            // then the zombie-target guard refuses it next tick). Poison + park
            // drop below still run (slot hygiene unchanged).
            if (0 && processes[i].page_dir) {
                uint32_t *pd = (uint32_t *)P2V_U32(processes[i].page_dir);
                for (int j = 0; j < PD_KERNEL_BASE; j++) {
                    if (pd[j] & 0x01) {
                        uint32_t *pt = (uint32_t *)P2V_U32(pd[j] & 0xFFFFF000);
                        // Free user page table pages
                        for (int k = 0; k < 1024; k++) {
                            if (pt[k] & 0x01) {
                                pmm_free_page(pt[k] & 0xFFFFF000);
                            }
                        }
                        pmm_free_page(pd[j] & 0xFFFFF000);
                    }
                }
                pmm_free_page(processes[i].page_dir);
            }
            // Free kernel stack
            if (processes[i].kernel_stack) {
                kfree((void *)processes[i].kernel_stack);
            }
            // Poison schedule-relevant fields so a use-after-reap faults LOUD
            // (serial-visible) instead of switching onto a freed stack/PD:
            // ticks 0 (never picked by elevators), esp/ebp/esp0 0 (stub seed
            // path re-seeds only via eip==0 — and eip stays poisoned-nonzero
            // so no reseed either; any switch attempt triple-faults fast).
            // Also drop any staged park resume for this pid (destroy hygiene:
            // a parked-then-killed thread must not leave a dead EIP/ESP that
            // hijacks a future same-pid... same-slot child entry).
            sched_park_drop(pid);
            processes[i].ticks_left = 0;
            processes[i].esp = 0;
            processes[i].ebp = 0;
            processes[i].esp0_top = 0;
            processes[i].page_dir = 0;
            processes[i].kernel_stack = 0;
            processes[i].state = PROC_UNUSED;
            serial_printf("[proc] destroyed process pid=%d\n", pid);
            return;
        }
    }
}

struct process *process_get(uint32_t pid) {
    // Linear scan by pid field (NOT slot — pids diverge from slots on
    // reuse; every preemption-bisect fault with ESI=0 traced to a lookup
    // that assumed pid==slot). Skips UNUSED (reaped/poisoned) slots.
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == pid && processes[i].state != PROC_UNUSED) {
            return &processes[i];
        }
    }
    return 0;
}

struct process *process_current(void) {
    // current_slot is a SLOT index 0..15 (NOT a pid — pids diverge from
    // slots once next_pid exceeds 15 or slots are reused). Index directly;
    // fall back to slot 0 on corruption (never return garbage).
    if (current_slot >= MAX_PROCESSES) return &processes[0];
    return &processes[current_slot];
}

// Slot of a PCB pointer (pointer-diff; the table is a static array).
static uint32_t slot_of(struct process *p) {
    return (uint32_t)(p - processes);
}

uint32_t process_next(void) {
    // Round-robin: find the next READY process after current.
    // BLOCKED (parked waiters) and un-entered (no live frame) processes are
    // never selected — the former resume via their own trap iret on wakeup,
    // the latter run solely via the main-loop entry drain.
    uint32_t start = (current_slot + 1) % MAX_PROCESSES;
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        uint32_t idx = (start + i) % MAX_PROCESSES;
        if (processes[idx].state == PROC_READY && processes[idx].pid != 0 &&
            processes[idx].entered_ring3) {
            return processes[idx].pid;
        }
    }
    return 0; // Fall back to kernel (pid 0)
}

void process_switch(uint32_t pid) {
    struct process *next = process_get(pid);
    if (!next) return;
    // BLOCKED processes must never be switched INTO via the lightweight
    // path (bisected 2026-09-09: kbd-wake IRET into pid 1's stale park
    // resume faulted #PF err=4 at the resume EIP with pid=0 live — the
    // parked thread has no live kernel frame; its resume runs through the
    // entry drain (prepare + enter_user_mode), never a CR3-only handoff.
    // The drain's wake path sets READY before entering; the tick never
    // picks BLOCKED (process_next skips them). SILENT refuse (no serial:
    // process_switch runs in the drain's cli-held window and on tick-adjacent
    // paths — serial_printf there re-enters the formatter; the refusal is
    // observable via the missing switch, not a line).
    if (next->state == PROC_BLOCKED) {
        return;
    }
    if (next->state != PROC_READY && next->state != PROC_RUNNING) return;

    struct process *prev = process_current();
    if (prev->pid == pid) return; // Already running
    // LIGHTWEIGHT handoff (entry/exit paths only): switches CR3 + ESP0 but
    // does NOT save/restore the outgoing kernel-thread frame. Safe ONLY when
    // the outgoing thread will never resume mid-frame (ring-3 entry: the
    // main loop parks in enter_user_mode's IRET, resumed by the trampoline;
    // exit: sys_exit never returns). NEVER call this to preempt a live
    // kernel thread — that strands it (use process_switch_to). The timer
    // tick path (sched_tick → process_switch_to) is the only preempter.
    uint32_t next_cr3 = next->page_dir ? next->page_dir
                                       : paging_kernel_pd_phys();
    __asm__ volatile("mov %0, %%cr3" : : "r"(next_cr3));

    // ESP0 follows the kernel stack: per-process top if known, else derive
    // from the allocated stack base. Never leave a stale ESP0 behind — a
    // ring-3 trap would land on another process's stack.
    if (next->esp0_top)
        tss_set_kernel_stack(next->esp0_top);
    else if (next->kernel_stack)
        tss_set_kernel_stack(next->kernel_stack + PROCESS_STACK_SIZE);

    // Demote outgoing ONLY when it was RUNNING: a ZOMBIE/EXITED/BLOCKED
    // previous must keep its state (exit_current may have zombied it while
    // current_slot still pointed at it — blindly marking READY resurrects
    // the dead, and the reap check then misses it; bisected 2026-09-08 via
    // [reapchk] state=1 post-exit: unprepare demoted the just-zombied pid1
    // back to READY, reap took the else-branch, and the next tick switched
    // into the half-dead slot → #PF EIP=0).
    if (prev->state == PROC_RUNNING)
        prev->state = PROC_READY;
    next->state = PROC_RUNNING;
    current_slot = slot_of(next);

    // No serial here (timer-IRQ context with IF clear: serial_putchar spins
    // on the UART THR-empty bit, and a nested tick inside a serial_printf
    // re-enters the formatter's statics mid-string → garbled log + deep
    // nesting → stack exhaustion. Switches are silent; spawn/reap (thread
    // context) carry the serial story.
}

// Naked asm stub (boot/isr.asm): saves outgoing frame, loads CR3+ESP.
extern void context_switch(uint32_t *old_esp_p, uint32_t new_esp,
                           uint32_t new_cr3);

void process_switch_to(uint32_t pid) {
    // Snapshot ALL PCB reads into locals BEFORE any call that can sleep or
    // re-enter (serial, tss_set, nested tick): -O2 keeps PCB pointers in
    // registers across calls, and a nested switch_to can free/reuse the slot
    // (reap) between our read and our write → stale-pointer write to a dead
    // slot (bisected 2026-09-08: ESI=0 write ESI+4 after a clean run+Back —
    // prev/next pointers went stale across the trap-guard block).
    struct process *next = process_get(pid);
    if (!next) return;
    uint32_t next_state = next->state;
    if (next_state != PROC_READY && next_state != PROC_RUNNING) return;
    uint32_t target_pid = next->pid;
    uint32_t next_gen = next->generation;
    uint32_t next_esp = next->esp;
    uint32_t next_eip = next->eip;
    uint32_t next_pd = next->page_dir;
    uint32_t next_kstack = next->kernel_stack;
    uint32_t next_esp0 = next->esp0_top ? next->esp0_top
                       : next_kstack ? next_kstack + PROCESS_STACK_SIZE : 0;

    struct process *prev = process_current();
    uint32_t prev_pid = prev->pid;
    if (prev_pid == pid) return; // Already running
    uint32_t prev_gen = prev->generation;
    uint32_t prev_esp0 = prev->esp0_top;
    uint32_t *prev_esp_p = &prev->esp;
    uint32_t prev_slot = slot_of(prev);
    // Preemption guard: NEVER switch while a ring-3 trap frame is live on
    // the current trap stack. The timer IRQ fires on whatever stack is live
    // — including the TSS.esp0 trap stack MID-syscall (ring 3 -> INT 0x80 ->
    // timer nests on the SAME esp0 stack, above the stub's pusha frame).
    // Switching CR3+ESP there strands the stub frame on the old stack: the
    // iret would popa from the NEW stack (garbage) and iret into garbage.
    // Detect: compare current ESP against the current process's trap-stack
    // range [esp0_top-4096, esp0_top). If inside → defer (re-arm slice,
    // return; the next tick retries after the syscall irets back to ring 3).
    {
        uint32_t cur_esp;
        __asm__ volatile("mov %%esp, %0" : "=r"(cur_esp));
        uint32_t ttop = prev_esp0;
        // pid 0's esp0_top is the idle stack; the main loop runs on the BOOT
        // stack, not the trap stack — only treat ESP as trapped when it is
        // inside the CURRENT trap-stack window AND a user process is current.
        if (prev_pid != 0 && ttop != 0 &&
            cur_esp < ttop && cur_esp >= ttop - PROCESS_STACK_SIZE) {
            // Mid-trap: defer the switch one tick (keep the slice expired so
            // the very next tick retries immediately after iret).
            return;
        }
    }

    // Full switch, IRQs OFF throughout (a trap on a half-switched stack —
    // new CR3 but old ESP, or vice versa — faults or corrupts).
    //
    // RE-ENTRANCY GUARD (bisected 2026-09-08 via -d int: #PF at CR2==EIP
    // inside context_switch after mov cr3, then #DF): IRQ gates are 0x8E
    // (interrupt gates → CPU clears IF on entry), so a tick CANNOT nest
    // inside process_switch_to's prologue-to-cli window... EXCEPT the window
    // between process_next/process_get (IF SET, thread context — sched_tick
    // itself runs with IF set on kernel threads) and the stub's cli. Two
    // ticks CAN'T nest (second tick waits for first iret — IF clear in stub),
    // but sched_tick→switch_to→(iret→sti)→tick→switch_to sequences can
    // interleave with the MAIN LOOP's own switch_to callers (prepare/reap
    // paths call process_switch, not _to — safe). The in_switch flag covers
    // the residual race: tick-during-prologue returns immediately (the outer
    // switch already picked process_next fresh).
    // (The trap-stack guard above covers ring-3-trap nesting; this covers
    // kernel-thread nesting. Both must hold.)
    uint32_t next_cr3 = next_pd ? next_pd : paging_kernel_pd_phys();
    // First switch ever for this thread: the stub pops ebp/edi/esi/ebx
    // then ret → needs [ebp][edi][esi][ebx][ret] low→high with esp at
    // [ebp]. Seed regs 0, ret = switch_resume below (uniform: every thread
    // enters through THIS function). ebp seed 0 = bottom of thread stack.
    // Detect via LOCAL next_eip snapshot (never otherwise written —
    // process_create zeroes it; no other path sets p->eip). Do NOT key on
    // esp==kstack_top: pid 0's esp IS live (idle capture) and re-seeding it
    // would discard the main-loop frame (bisected 2026-09-08: re-seed →
    // ESP=c1d92f28 garbage → #PF at CR2==EIP in context_switch, then #DF).
    // Seed writes go to the LOCAL snapshot stack (always mapped: kernel
    // kstacks are kernel-high shared), then publish via re-resolved PCB
    // pointer under cli below (locals may be stale by then — see below).
    uint32_t seed_esp = next_esp;
    int need_seed = (next_eip == 0);
    // SEED-SANITY (bisect 2026-09-09: post-hello cs=8 NULL-EIP with no rogue
    // enter — a poisoned esp==0 (destroy zeroes esp, and the zombie-target
    // guard below runs AFTER this write) would make sp-=5 underflow to
    // 0xFFFFFFEC and the five stores corrupt the IDT/GDT region... then
    // publish esp=0xFFFFFFEC and resume into garbage. Refuse to seed a zero
    // (or absurdly-low, <64K — null-adjacent) ESP: return with the guard
    // clear (the target is dead; its waiter reaps it; tick retries).
    if (need_seed && seed_esp < 65536) {
        return;
    }
    if (need_seed) {
        uint32_t *sp = (uint32_t*)seed_esp;
        sp -= 5;
        sp[0] = 0; // ebp
        sp[1] = 0; // edi
        sp[2] = 0; // esi
        sp[3] = 0; // ebx
        sp[4] = (uint32_t)&&switch_resume;
        seed_esp = (uint32_t)sp;
    }
    {
        if (switch_busy) return; // nested tick: outer switch covers it
        switch_busy = 1;
        __asm__ volatile("cli" ::: "memory");
        if (next_esp0) tss_set_kernel_stack(next_esp0);
        // Re-resolve BOTH threads NOW (under cli, no nesting possible past
        // this point): locals above may be stale — a tick between our
        // prologue and cli could have reaped/reused slots (reap runs on the
        // main loop with IF SET). prev by slot (slots never move), next by
        // pid (slots get reused; pid lookup skips UNUSED/poisoned).
        struct process *pv = &processes[prev_slot];
        struct process *nx = process_get(target_pid);
        if (!nx || nx->state == PROC_UNUSED) {
            switch_busy = 0;
            __asm__ volatile("sti" ::: "memory");
            return; // target died while we set up — drop the switch
        }
        // If WE were reaped while setting up (our slot is UNUSED — the main
        // loop reaped us between prologue and cli), there is no thread to
        // save: just clear the guard, re-enable, and return WITHOUT
        // touching context_switch (our stack may already be someone else's).
        if (pv->state == PROC_UNUSED) {
            switch_busy = 0;
            __asm__ volatile("sti" ::: "memory");
            return;
        }
        // GENERATION GUARD (2026-09-09 overnight: post-hello #PF narrowed to
        // tick-into-reused-slot — the pid lookup above misses the case where
        // OUR OWN slot was reaped AND reused between prologue and cli (same
        // slot index, new pid+generation, state READY: passes every check
        // above, but pv->esp is someone else's stack and saving there
        // corrupts the new occupant). The target side is covered by the pid
        // lookup (pids are monotonic, never reused until 2^32 wraps); still
        // re-check its generation for symmetry. Abort on any mismatch —
        // the waiter reaps, the tick retries next slice.
        if (pv->generation != prev_gen) {
            switch_busy = 0;
            __asm__ volatile("sti" ::: "memory");
            return;
        }
        if (nx->generation != next_gen) {
            switch_busy = 0;
            __asm__ volatile("sti" ::: "memory");
            return;
        }
        if (need_seed) {
            nx->esp = seed_esp;
            nx->eip = 1; // seeded (opaque flag, never executed as address)
        }
        // Same zombie rule as process_switch (see above): only demote a
        // RUNNING outgoing thread. A ZOMBIE/EXITED/BLOCKED prev keeps its
        // state (exit_current races the tick — the tick must not resurrect).
        // ZOMBIE-TARGET GUARD (bisected 2026-09-09: post-hello #PF err=0
        // cr2=eip=esp=0 — the tick's prologue snapshots next_esp BEFORE the
        // drain reaps the target (IF SET on kernel threads); the re-resolve
        // checks UNUSED but a ZOMBIE slot is NOT unused (reap deferred to
        // waiters) — without this the switch loads poisoned esp=0 and resumes
        // into nothing. Refuse ZOMBIE/EXITED/esp==0 targets (waiter reaps;
        // tick retries next slice).
        if (nx->state == PROC_ZOMBIE || nx->state == PROC_EXITED ||
            nx->esp == 0) {
            switch_busy = 0;
            __asm__ volatile("sti" ::: "memory");
            return;
        }
        if (pv->state == PROC_RUNNING)
            pv->state = PROC_READY;
        nx->state = PROC_RUNNING;
        current_slot = slot_of(nx);
        // No serial here (timer-IRQ context: nested-tick re-entrancy into
        // the formatter + deep va_list frames on 4K stacks; spawn/reap in
        // thread context carry the story).
        context_switch(&pv->esp, nx->esp, next_cr3);
    switch_resume:
        // Woken thread lands here (same code path both directions):
        // clear the guard + re-enable. current_slot was set pre-switch and
        // is already correct for the woken thread. No serial (prints twice
        // per switch — once per direction; WORSE: serial_printf's deep
        // va_list frames + static formatter state corrupt the switch frame
        // itself — bisected 2026-09-09: a resume-trace here moved the crash
        // INTO switch_to's prologue. NEVER serial inside switch paths).
        switch_busy = 0;
        __asm__ volatile("sti" ::: "memory");
    }
}

struct process* process_get_by_slot(int slot) {
    if (slot < 0 || slot >= MAX_PROCESSES) return 0;
    if (processes[slot].state == PROC_UNUSED) return 0;
    return &processes[slot];
}

int process_fd_get(struct process *p, int fd) {
    if (!p || fd < 0 || fd >= PROC_MAX_FDS) return PROC_FD_FREE;
    return p->fds[fd];
}

int process_fd_alloc(struct process *p, int ofd_idx) {
    if (!p || ofd_idx < 0) return -1;
    for (int fd = 3; fd < PROC_MAX_FDS; fd++) {
        if (p->fds[fd] == PROC_FD_FREE) {
            p->fds[fd] = ofd_idx;
            return fd;
        }
    }
    return -1;
}

void process_fd_free(struct process *p, int fd) {
    if (!p || fd < 0 || fd >= PROC_MAX_FDS) return;
    p->fds[fd] = PROC_FD_FREE;
}

// Resolve a user fd for the CURRENT process (syscall fast path):
// returns PROC_FD_TERM (-2) for terminal fds, the open-file index (>=0)
// for files/pipes, PROC_FD_FREE (-1) for keyboard/free/invalid.
int sys_fd_resolve(int fd) {
    struct process *cur = process_current();
    if (!cur) return PROC_FD_FREE;
    return process_fd_get(cur, fd);
}

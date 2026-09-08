#include "sched.h"
#include "process.h"
#include "paging.h"
#include "memlayout.h"
#include "memory.h"
#include "gdt.h"
#include "serial.h"
#include "filesystem.h"
#include "elf.h"
#include "syscall.h"
#include <stdint.h>

// Round-robin PREEMPTIVE scheduler (timer IRQ context, Phase 2).
// The context_switch stub + per-thread kernel ESP make timer-IRQ switches
// safe: the preempted kernel thread's frame (EBX/ESI/EDI/EBP/ESP+EIP) is
// saved into its PCB and restored when it runs again. Rules that keep it
// safe (do NOT regress):
// - NEVER switch from ring-3 trap context (isr_common_stub frame must
//   popa+iret — sched_yield stays slice-reset-only for that reason).
// - The IDLE thread (pid 0, main loop) is a normal thread: it gets
//   preempted and resumed like any other (its ESP lives in processes[0]).
// - process_switch_to runs cli across the whole switch (stub requirement).
void sched_tick(void) {
    struct process *cur = process_current();
    if (!cur) return;
    // WAKE PASS (runs even when current isn't RUNNING — e.g. a parked
    // waiter owns ring 3 while pid 0 is READY): if the entry queue holds an
    // un-entered spawn OR a READY+entered sibling exists OR the kbd queue
    // holds an unread line (a parked reader's wake condition — the line it
    // waits for is already there; 2026-09-09: sh slept through offers),
    // wake ONE yield/read-parker (BLOCKED + ticks_left == 1) to READY so its
    // next trap iret resumes it. Wait-parkers (ticks_left ==
    // SCHED_SLICE_TICKS) only wake on SIG_CHLD (see exit_current) — never
    // here. Trap-safe: PCB state only, no switches (the switch happens on
    // slice expiry below, or the drain runs the sibling on the next loop
    // iteration).
    {
        extern int entry_pending_any(void);
        extern int sys_proc_kbd_pending(void);
        int sibling_ready = 0;
        for (int s = 0; s < MAX_PROCESSES; s++) {
            struct process *c = process_get_by_slot(s);
            if (!c) continue;
            if (c->pid == 0) continue;
            if (c->state == PROC_READY && c->entered_ring3) {
                sibling_ready = 1;
                break;
            }
        }
        if (entry_pending_any() || sibling_ready || sys_proc_kbd_pending()) {
            for (int s = 0; s < MAX_PROCESSES; s++) {
                struct process *c = process_get_by_slot(s);
                if (!c) continue;
                if (c->pid == 0) continue;
                if (c->state == PROC_BLOCKED && c->ticks_left == 1) {
                    c->state = PROC_READY;
                    c->ticks_left = SCHED_SLICE_TICKS;
                    break; // one per tick (round-robin fairness)
                }
            }
        }
    }
    if (cur->state != PROC_RUNNING) return;
    // LIVE-ESP REFRESH (bisected 2026-09-08: post-Back EIP=0 with ESP 84B
    // above the live depth — pid0.esp captured at prepare/unprepare/boot
    // goes stale as the main-loop boot stack grows/shrinks across poll/draw
    // frames; the next switch TO pid0 loads the ancient frame → pops garbage
    // → ret into 0): when the current thread IS pid 0, re-capture its live
    // ESP/EBP on EVERY tick (2 movs, IRQ context, always safe — we ARE the
    // thread). Other threads' ESP is maintained by the stub itself.
    if (cur->pid == 0) {
        __asm__ volatile("mov %%esp, %0" : "=r"(cur->esp));
        __asm__ volatile("mov %%ebp, %0" : "=r"(cur->ebp));
    }
    // ENTRY-DRAIN GUARD (bisected 2026-09-08: EIP=0 on pid1 kstack BEFORE
    // 'entering ring 3' serial — tick fired between shell spawn (pid1 READY)
    // and the main-loop drain, switched to the UNSPAWNED... unentered pid1
    // via its SEEDED frame, then iret popped the IRQ stub frame from the
    // WRONG stack → EIP=0): never preempt INTO a process that has never
    // entered ring 3 (its kstack holds only the seed frame, not a live
    // switch_to frame — the stub ret path assumes a real saved caller).
    // Such processes run SOLELY via the main-loop entry drain (prepare +
    // enter_user_mode IRET). Skip them here (re-arm slice, retry next tick
    // — by then they have either entered or exited).
    if (cur->ticks_left > 0) {
        cur->ticks_left--;
        if (cur->ticks_left > 0) return;
    }
    // Slice expired: round-robin to the next READY process (skips pid 0
    // unless nothing else is runnable — process_next falls back to 0).
    // ENTRY-DRAIN RULE: never preempt INTO a process with entered_ring3==0
    // (spawned but never IRETed — its kstack holds only the seed frame, not
    // a live switch_to frame; switching there then iret-ing the IRQ stub
    // frame from the wrong stack faults EIP=0, bisected 2026-09-08). Such
    // processes run SOLELY via the main-loop entry drain. Defer (re-arm +
    // retry next tick — the drain runs on the next loop iteration, ~1ms).
    // BLOCKED processes also never get switched into (parked waiters resume
    // via their own trap iret on wakeup, not via a fresh switch frame).
    uint32_t nxt = process_next();
    {
        struct process *cand = process_get(nxt);
        if (cand && cand->pid != 0 && !cand->entered_ring3) {
            cur->ticks_left = SCHED_SLICE_TICKS;
            return;
        }
        if (cand && cand->state == PROC_BLOCKED) {
            cur->ticks_left = SCHED_SLICE_TICKS;
            return;
        }
    }
    if (nxt == cur->pid) {
        cur->ticks_left = SCHED_SLICE_TICKS;
        return;
    }
    cur->ticks_left = SCHED_SLICE_TICKS;
    struct process *np = process_get(nxt);
    if (np) np->ticks_left = SCHED_SLICE_TICKS;
    // Full preemptive switch (timer IRQ, IRQs already off in the stub —
    // irq_common_stub runs with IF clear until iret; process_switch_to's
    // cli is redundant-but-harmless here and required on thread paths).
    process_switch_to(nxt);
}

void sched_yield(void) {
    // SAFE-POINT YIELD (trap-stack context: ring-3 INT 0x80 entered via
    // isr_common_stub, which will popa+iret back to ring 3 on return). NEVER
    // switch CR3/ESP0 here: the stub's frame (pusha/ds/int_num) lives on the
    // CURRENT trap stack, and the iret target is the CURRENT user space.
    // Switching spaces underneath it faults the resume (present+U/S #PF at
    // the next ring-3 EIP — bisected 2026-09-08: yield switched pid 1 -> 0,
    // iret landed in the kernel PD where 0x08048046 is unmapped). So: only
    // reset the slice here; the next process runs at the next tick or ENTRY
    // event (preemption via process_switch_to handles the rest).
    struct process *cur = process_current();
    if (!cur || cur->pid == 0) return;
    cur->ticks_left = SCHED_SLICE_TICKS;
}

// Ring-3 trap reconcile (see syscall_handler call site): if current is pid 0
// but a user process sits READY with entered_ring3 (a ring-3 thread stolen
// mid-user-code by the main loop — the steal demoted it RUNNING→READY while
// pid 0 runs), restore it as current (CR3 + ESP0 + current_slot, lightweight
// — the live trap frame sits in shared-high-map memory so it stays valid
// across the CR3 switch; ESP0 affects future traps only). No-ops when current
// is already a user process, when no entered READY user proc exists, or when
// several do (ambiguous — leave it; the tick resolves).
void syscall_reconcile_ring3(void) {
    struct process *cur = process_current();
    if (!cur || cur->pid != 0) return; // already home (fast path)
    // TRAP-ESP0 DISAMBIGUATION (bisected 2026-09-08: forkexec/sh children
    // exec'd GARBAGE — the scan below picked the WRONG READY candidate when
    // two ring-3 threads were live (parent + queued fork child both READY),
    // stashing the trap under the wrong pid and forking the child with a
    // dead ESP). The trap itself ran on the OWNER's ESP0 page (TSS switch is
    // by CR3-live... no — by TSS.ESP0 at trap time). Identify the owner by
    // CURRENT ESP: it lies inside exactly one process's trap-stack window
    // [esp0_top-4096, esp0_top). That process IS the trapper — switch to it
    // directly, no scan, no ambiguity.
    {
        uint32_t cur_esp;
        __asm__ volatile("mov %%esp, %0" : "=r"(cur_esp));
        for (int s = 0; s < MAX_PROCESSES; s++) {
            struct process *c = process_get_by_slot(s);
            if (!c) continue;
            if (c->pid == 0 || !c->esp0_top) continue;
            if (cur_esp < c->esp0_top && cur_esp >= c->esp0_top - 4096) {
                if (c->state == PROC_READY || c->state == PROC_RUNNING)
                    process_switch(c->pid);
                return; // identified (or dead — either way no scan)
            }
        }
    }
    struct process *cand = 0;
    for (int s = 0; s < MAX_PROCESSES; s++) {
        struct process *c = process_get_by_slot(s);
        if (!c) continue;
        if (c->pid == 0) continue;
        // Stolen ring-3 threads are READY (demoted by the stealing switch);
        // a RUNNING candidate means the books are already correct elsewhere
        // (or two threads race — either way don't touch: prefer RUNNING).
        if (c->state == PROC_RUNNING) return;
        if (c->state != PROC_READY) continue;
        if (!c->entered_ring3) continue;
        if (cand) return; // ambiguous (2+ candidates) — leave to the tick
        cand = c;
    }
    if (!cand) return; // no stolen thread (kernel-only trap — keep pid 0)
    process_switch(cand->pid);
}

// Build a private address space for a user image WITHOUT switching to it:
// map code + stack pages into the fresh PD (no TLB flush needed — not
// running), then copy the image through the HIGH alias while the kernel PD
// is still live. process_switch() flushes the TLB when it activates.
int sched_spawn_user(const uint8_t *img, uint32_t img_len,
                     uint32_t u_code, uint32_t u_stack_top) {
    if (!img || img_len == 0) {
        serial_puts("[sched] spawn: null image\n");
        return -1;
    }
    // u_code must be page-aligned (mapping granularity); u_stack_top is a TOP
    // (exclusive end) — it may equal KERNEL_VBASE exactly (legacy top
    // 0xBFFFF000+4096 = 0xC0000000); only mapped pages must stay below it.
    if (u_code >= KERNEL_VBASE || u_stack_top > KERNEL_VBASE) {
        serial_puts("[sched] spawn: addr in kernel half\n");
        return -1;
    }
    if (u_code & 0xFFF) {
        serial_puts("[sched] spawn: u_code unaligned\n");
        return -1;
    }
    if ((u_stack_top - 4096) & 0xFFF) {
        serial_puts("[sched] spawn: stack base unaligned\n");
        return -1;
    }

    int pid = process_create();
    if (pid < 0) return -1;
    struct process *p = process_get((uint32_t)pid);
    if (!p) return -1;

    // Stack page + enough code pages for the image (rounded up).
    uint32_t stack_phys = pmm_alloc_page();
    if (!stack_phys) { process_destroy((uint32_t)pid); return -1; }
    uint32_t code_pages = (img_len + 0xFFF) >> 12;
    if (!code_pages) code_pages = 1;

    // Map stack first (so a later code-page failure still reaps cleanly).
    paging_map_user_pd(p->page_dir, u_stack_top - 4096, stack_phys);

    // The image is a slice of its source page starting at the entry offset:
    // byte i of the image lives at src_page[entry_off + i]. Copy relative
    // to the page base (img points AT the page base, not the entry).
    extern uint32_t user_test_page_off(void);
    uint32_t src_off = user_test_page_off();
    uint32_t off = 0;
    for (uint32_t i = 0; i < code_pages; i++) {
        uint32_t cp = pmm_alloc_page();
        if (!cp) { process_destroy((uint32_t)pid); return -1; }
        paging_map_user_pd(p->page_dir, u_code + i * 4096, cp);
        // Copy through the high alias (kernel PD live — P2V always works).
        uint8_t *dst = (uint8_t*)P2V_U32(cp);
        for (uint32_t j = 0; j < 4096 && off < img_len; j++, off++)
            dst[j] = img[src_off + off];
    }

    p->user_eip = u_code;
    p->user_esp = u_stack_top;
    p->ticks_left = SCHED_SLICE_TICKS;
    // esp0_top already points at the SEPARATE trap stack (process_create) —
    // do NOT reset it to kstack_top here (that re-merges the stacks the
    // split was created to separate; see process.c).
    p->state = PROC_READY;
    serial_printf("[sched] spawned pid=%d code=%x stack=%x pages=%d\n",
                  pid, u_code, u_stack_top, code_pages + 1);
    return pid;
}

// Target PD for the exec/ELF map callback below (elf_load takes a plain
// function pointer — no closure — so the PD rides a static, set for the
// duration of the load; single-threaded here, main-loop/shell context).
static uint32_t spawn_elf_target_pd = 0;
static void spawn_elf_map_cb(uint32_t virt_page, uint32_t phys_page) {
    paging_map_user_pd(spawn_elf_target_pd, virt_page, phys_page);
}

// Count NUL-terminated strings in a kernel-side vector (NULL = empty).
static uint32_t strvec_count(char *const vec[]) {
    uint32_t n = 0;
    if (vec) {
        while (vec[n]) {
            n++;
            if (n > 64) break; // sanity cap (exec arg lists are small)
        }
    }
    return n;
}

// Kernel strlen (no libc in COMMON-adjacent code; sched.o is desktop-only
// but keep it dependency-light anyway).
static uint32_t kstrlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
        if (n > 4096) break;
    }
    return n;
}

// Spawn an ELF program from a VFS path (Phase 3: shell `run`, init).
// Loads PT_LOAD segments IN THAT SPACE via elf_load, maps a fresh user
// stack page, then builds the standard i386 SysV entry stack:
//
//   [argc][argv0][argv1]...[NULL][envp0]...[NULL][strings...]
//
// with argv[i]/envp[i] pointing at the in-stack string copies. Entry EIP =
// ELF entry, ESP = stack top after the build. Returns pid or -1.
int sched_spawn_elf(const char *path, char *const argv[], char *const envp[],
                    int win_id) {
    if (!path || !path[0]) return -1;
    if (!fs_exists(path)) {
        serial_puts("[spawn_elf] not found: ");
        serial_puts(path);
        serial_putchar('\n');
        return -1;
    }
    // VFS cap is 32KB; ELF test programs are a few KB. Static buffer is
    // wrong (trap stack is 4KB) — but this runs in main-loop/shell thread
    // context (deep boot stack, 256KB), NOT trap context, so a static image
    // buffer is safe (same pattern as sys_proc_exec's elf_img).
    static uint8_t elf_img[32768];
    int len = fs_read(path, elf_img, sizeof(elf_img));
    if (len <= 0) return -1;
    if (elf_validate(elf_img, (uint32_t)len)) {
        serial_puts("[spawn_elf] bad ELF: ");
        serial_puts(path);
        serial_putchar('\n');
        return -1;
    }
    int pid = process_create();
    if (pid < 0) return -1;
    struct process *p = process_get((uint32_t)pid);
    if (!p) return -1;

    // Fresh user stack page (top of user-low, same ABI as spawn_user).
    uint32_t u_stack_top = 0xBFFFF000 + 4096;
    uint32_t stack_phys = pmm_alloc_page();
    if (!stack_phys) { process_destroy((uint32_t)pid); return -1; }
    paging_map_user_pd(p->page_dir, u_stack_top - 4096, stack_phys);

    // Load segments into the fresh space.
    spawn_elf_target_pd = p->page_dir;
    uint32_t entry = 0;
    int rc = elf_load(elf_img, (uint32_t)len, spawn_elf_map_cb, &entry);
    spawn_elf_target_pd = 0;
    if (rc) { process_destroy((uint32_t)pid); return -1; }

    uint32_t argc = strvec_count(argv);
    uint32_t envc = strvec_count(envp);
    if (argc > 16) argc = 16;
    if (envc > 16) envc = 16;

    // Build argc/argv/envp on the user stack (through the HIGH alias while
    // the kernel PD is live — the target space is NOT running yet).
    // Layout (all LE, 16B-aligned string area, single deterministic pass):
    //   final_esp -> [argc][argv_tab][envp_tab]
    //   argv_tab  -> [arg0*]...[NULL]
    //   envp_tab  -> [env0*]...[NULL]
    //   strings   -> arg/env copies (top of page, growing down to `str_base`)
    // Two passes: measure strings first, then lay tables + copies once.
    uint8_t *sp0 = (uint8_t*)P2V_U32(stack_phys); // base of the stack page
    {
        uint32_t used = 0;
        for (uint32_t i = 0; i < argc; i++) {
            uint32_t sl = kstrlen(argv[i]) + 1;
            if (sl > 256) sl = 256;
            used += sl;
        }
        for (uint32_t i = 0; i < envc; i++) {
            uint32_t sl = kstrlen(envp[i]) + 1;
            if (sl > 256) sl = 256;
            used += sl;
        }
        // Tables: (argc+1 + envc+1 + 3 header words) * 4 bytes.
        uint32_t need = used + 4 * (argc + 1) + 4 * (envc + 1) + 12;
        need = (need + 15) & ~15u;
        if (need > 4096) { process_destroy((uint32_t)pid); return -1; }
        uint32_t base = 4096 - need; // page offset of final_esp
        uint32_t final_esp = (u_stack_top - 4096) + base;
        uint32_t a_tab = final_esp + 12;
        uint32_t e_tab = a_tab + 4 * (argc + 1);
        uint32_t s_base = e_tab + 4 * (envc + 1);
        s_base &= ~15u; // string area stays 16B-aligned (pad between)
        // Header
        uint32_t h = base;
        sp0[h] = argc & 0xFF; sp0[h+1] = (argc >> 8) & 0xFF;
        sp0[h+2] = (argc >> 16) & 0xFF; sp0[h+3] = (argc >> 24) & 0xFF;
        sp0[h+4] = a_tab & 0xFF; sp0[h+5] = (a_tab >> 8) & 0xFF;
        sp0[h+6] = (a_tab >> 16) & 0xFF; sp0[h+7] = (a_tab >> 24) & 0xFF;
        sp0[h+8] = e_tab & 0xFF; sp0[h+9] = (e_tab >> 8) & 0xFF;
        sp0[h+10] = (e_tab >> 16) & 0xFF; sp0[h+11] = (e_tab >> 24) & 0xFF;
        // Argv table + string copies
        uint32_t w = a_tab - (u_stack_top - 4096);
        uint32_t s = s_base - (u_stack_top - 4096);
        for (uint32_t i = 0; i < argc; i++) {
            uint32_t sl = kstrlen(argv[i]) + 1;
            if (sl > 256) sl = 256;
            for (uint32_t b = 0; b < sl; b++) sp0[s + b] = (uint8_t)argv[i][b];
            uint32_t v = (u_stack_top - 4096) + s;
            sp0[w] = v & 0xFF; sp0[w+1] = (v >> 8) & 0xFF;
            sp0[w+2] = (v >> 16) & 0xFF; sp0[w+3] = (v >> 24) & 0xFF;
            w += 4; s += sl;
        }
        sp0[w] = sp0[w+1] = sp0[w+2] = sp0[w+3] = 0; w += 4;
        // Envp table + string copies
        for (uint32_t i = 0; i < envc; i++) {
            uint32_t sl = kstrlen(envp[i]) + 1;
            if (sl > 256) sl = 256;
            for (uint32_t b = 0; b < sl; b++) sp0[s + b] = (uint8_t)envp[i][b];
            uint32_t v = (u_stack_top - 4096) + s;
            sp0[w] = v & 0xFF; sp0[w+1] = (v >> 8) & 0xFF;
            sp0[w+2] = (v >> 16) & 0xFF; sp0[w+3] = (v >> 24) & 0xFF;
            w += 4; s += sl;
        }
        sp0[w] = sp0[w+1] = sp0[w+2] = sp0[w+3] = 0;
        p->user_eip = entry;
        p->user_esp = final_esp;
    }

    p->ticks_left = SCHED_SLICE_TICKS;
    p->state = PROC_READY;
    // Bind stdio to the spawning terminal (fd 1/2 write path uses this, not
    // focus — output stays in the window that ran it even if focus moves).
    p->term_win = win_id;
    serial_printf("[spawn_elf] pid=%d '%s' entry=%x esp=%x argc=%d\n",
                  pid, path, entry, p->user_esp, argc);
    return pid;
}

void sched_prepare(uint32_t pid) {
    // Activate address space + ESP0 for ring-3 entry of pid.
    // NOTE: process_switch (lightweight) does NOT save the outgoing
    // thread's ESP — pid 0's p->esp would go stale across entry/exit
    // cycles, and the first preemptive switch TO pid 0 would load the
    // ancient frame (pops garbage → ret into EIP=0, bisected 2026-09-08:
    // post-Back #PF CR2=0/EIP=0 on the boot stack). Refresh pid 0's live
    // ESP/EBP right here (we ARE pid 0's thread, ring-0 thread context).
    {
        extern void process_capture_idle_esp(void);
        process_capture_idle_esp();
    }
    process_switch(pid);
}

void sched_unprepare(void) {
    // Back to pid 0 (kernel idle): kernel PD + idle trap stack. The current
    // user process just exited via sys_exit; switch home first, then the
    // caller reaps the dead slot (sched_reap). pid 0 is permanently RUNNING
    // so it always accepts the handoff; prev is demoted to READY and the
    // reap converts it to UNUSED right after.
    // NOTE: refresh pid 0's ESP/EBP AFTER the switch (we are pid 0's thread
    // again here — same staleness reason as sched_prepare; the trampoline
    // restored the caller frame but p->esp still holds the pre-entry value,
    // one main-loop iteration stale).
    struct process *cur = process_current();
    if (cur && cur->pid == 0) return;
    process_switch(0);
    {
        extern void process_capture_idle_esp(void);
        process_capture_idle_esp();
    }
}

void sched_reap(uint32_t pid) {
    if (pid == 0) return; // never reap the kernel idle process
    // Orphans must not wait forever: children of the dead process are
    // adopted by init (pid 1) when it exists, else by pid 0 (whose
    // wait loop reaps them — the shell path reaps via wait(-1)).
    //
    // USE-AFTER-SWITCH RULE (bisected 2026-09-08: #PF at CR2==EIP in
    // context_switch AFTER a clean usermode run + Back): the reaped
    // process's kernel_stack page is freed here (kfree is a bump no-op so
    // the page is NOT reused — but its PD pages ARE pmm-freed and reusable).
    // That is fine. What is NOT fine: if the CURRENT thread (pid 0's main
    // loop, running on the boot stack) had ever been switched THROUGH the
    // dead process's ESP... it can't be (dead threads never run). The real
    // hazard is ticks_left/state: process_next must never pick UNUSED slots
    // (it checks READY) and process_get must never return them (checks
    // != UNUSED) — both already hold. Document + keep.
    {
        extern void sys_proc_reparent_to_init(uint32_t dead_pid);
        sys_proc_reparent_to_init(pid);
    }
    process_destroy(pid);
}

// Stage a fork child's ring-3 entry: enqueue (child pid, copied EIP/ESP,
// owning window of the parent) with the fork-return-0 flag. The main loop
// entry site forces the saved-EAX slot to 0 for flagged entries, so the
// child observes fork() == 0 while the parent observed the child pid.
// Runs in trap context (called from sys_proc_fork before the parent irets)
// — enqueue only touches static slots, no switches, no waits.
int sched_fork_child_entry(uint32_t child_pid) {
    extern int entry_enqueue(int pid, uint32_t eip, uint32_t esp, int win_id);
    struct process *child = process_get(child_pid);
    if (!child) return -1;
    // Owning window: inherit the parent's current binding (focused window
    // at fork time — best effort; the fd table carries real redirection).
    extern int window_get_focused(void);
    int win = window_get_focused();
    if (entry_enqueue((int)child_pid, child->user_eip, child->user_esp, win))
        return -1;
    sched_fork_mark_child(child_pid);
    return 0;
}

// Fork-child flags: small static set (pids are small ints; linear scan).
// Marked at fork, consumed at the child's first entry IRET.
#define FORK_CHILD_MAX 8
static uint32_t fork_children[FORK_CHILD_MAX];

void sched_fork_mark_child(uint32_t pid) {
    for (int i = 0; i < FORK_CHILD_MAX; i++) {
        if (fork_children[i] == 0) {
            fork_children[i] = pid;
            return;
        }
    }
    // Full (8 unentered fork children — absurd): drop the flag; the child
    // still runs, it just observes a stale EAX instead of 0. Serial-note it.
    serial_puts("[sched] fork-child flag table full\n");
}

int sched_fork_take_child(uint32_t pid) {
    for (int i = 0; i < FORK_CHILD_MAX; i++) {
        if (fork_children[i] == pid) {
            fork_children[i] = 0;
            return 1;
        }
    }
    return 0;
}

// Parked-resume staging: one slot per parked thread (16 max, static).
// Staged at park time (drain, thread context); consumed at re-entry
// (drain, before enter_user_mode — forces EAX=ret like the fork path).
// Trap-safe (static slots only); the tick WAKE pass only flips BLOCKED→
// READY, never touches these (no race: drain is the sole consumer).
#define PARK_RESUME_MAX 16
static uint32_t park_pids[PARK_RESUME_MAX];
static uint32_t park_eips[PARK_RESUME_MAX];
static uint32_t park_esps[PARK_RESUME_MAX];
static uint32_t park_rets[PARK_RESUME_MAX];

void sched_park_stage(uint32_t pid, uint32_t eip, uint32_t esp, uint32_t ret) {
    if (pid == 0) return; // never stage pid 0 (reused-park collision guard)
    for (int i = 0; i < PARK_RESUME_MAX; i++) {
        if (park_pids[i] == 0 || park_pids[i] == pid) {
            park_pids[i] = pid;
            park_eips[i] = eip;
            park_esps[i] = esp;
            park_rets[i] = ret;
            return;
        }
    }
    serial_puts("[sched] park-resume table full (dropping resume)\n");
}
// Drop a pid's staged resume WITHOUT consuming its return value (fork-path
// hygiene: a fresh fork child inherits nothing — but its SLOT may be reused
// and the previous occupant may have left a staged resume behind; the drain
// keys resumes by pid only, so a stale same-pid... same-slot entry would
// hijack the child's first entry. Called from sys_proc_fork after the child
// pid is assigned, and from process_destroy via sched_park_drop).
void sched_park_drop(uint32_t pid) {
    if (pid == 0) return;
    for (int i = 0; i < PARK_RESUME_MAX; i++) {
        if (park_pids[i] == pid) park_pids[i] = 0;
    }
}

int sched_park_take(uint32_t pid, uint32_t *ret_out) {
    for (int i = 0; i < PARK_RESUME_MAX; i++) {
        if (park_pids[i] == pid) {
            park_pids[i] = 0;
            if (ret_out) *ret_out = park_rets[i];
            return 1;
        }
    }
    return 0;
}

// Parked-resume EIP/ESP lookup (drain re-entry needs them; take() only
// returns the retval — EIP/ESP ride here, peeked before take()).
int sched_park_resume(uint32_t pid, uint32_t *eip_out, uint32_t *esp_out) {
    for (int i = 0; i < PARK_RESUME_MAX; i++) {
        if (park_pids[i] == pid) {
            if (eip_out) *eip_out = park_eips[i];
            if (esp_out) *esp_out = park_esps[i];
            return 1;
        }
    }
    return 0;
}

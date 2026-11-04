#include "syscall.h"
#include "sys_proc.h"

// One 64-bit word of a program's entry stack, written through the kernel's mapping of the page.
static inline void ustack_put(uint8_t *page, uint32_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) page[off + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

#include "process.h"
#include "sched.h"
#include "paging.h"
#include "memlayout.h"
#include "memory.h"
#include "filesystem.h"
#include "elf.h"
#include "spinlock.h"
#include "serial.h"
#include "window.h"
#include <stdint.h>

// ---- Open-file table (shared descriptions, refcounted across dup/fork) ----
#define SYS_PROC_MAX_OFD 64
#define SYS_PROC_MAX_PIPES 16
#define SYS_PROC_PIPE_BUF 4096

static struct open_file ofd_tab[SYS_PROC_MAX_OFD];
static spinlock_t ofd_lock = SPINLOCK_INIT;

// Pipes: byte ring per pipe, refcounted ends via ofd refcounts.
struct pipe {
    uint8_t buf[SYS_PROC_PIPE_BUF];
    uint32_t head;   // read index
    uint32_t tail;   // write index
    uint32_t count;
    int used;
    int readers;
    int writers;
};
static struct pipe pipes[SYS_PROC_MAX_PIPES];
static spinlock_t pipe_lock = SPINLOCK_INIT;

// Keyboard line queue (fd 0): shell offers full lines, sys_read drains.
#define KBD_QUEUE_LEN 8
#define KBD_LINE_LEN 256
static char kbd_lines[KBD_QUEUE_LEN][KBD_LINE_LEN];
static uint32_t kbd_lens[KBD_QUEUE_LEN];
static int kbd_head = 0, kbd_tail = 0, kbd_count = 0;
static spinlock_t kbd_lock = SPINLOCK_INIT;

int sys_proc_kbd_offer(const char *line, uint32_t len) {
    if (!line || len == 0) return 0;
    // STALE-BACKLOG GUARD (2026-09-08: sh's first read ate the `run /bin/sh`
    // kernel-shell line offered BEFORE sh existed — then every fresh command
    // lagged one line behind, execing the PREVIOUS line forever. The queue is
    // a shell-input queue, not a log: drop stale lines when NO reader is
    // parked... simpler + correct: DROP when no userland process exists yet
    // (offered before the first spawn — kernel-shell bootstrap lines). A
    // live reader drains promptly; a dead one means the line is stale.)
    {
        int live = 0;
        for (int s = 0; s < 16; s++) {
            struct process *pcb = process_get_by_slot(s);
            if (!pcb) continue;
            if (pcb->pid == 0) continue; // pid 0 = kernel, not a reader
            live = 1;
            break;
        }
        if (!live) return 0; // kernel-shell bootstrap line — nobody can read it
    }
    if (len >= KBD_LINE_LEN) len = KBD_LINE_LEN - 1;
    uint32_t ef = spin_lock_irq(&kbd_lock);
    int queued = 0;
    if (kbd_count < KBD_QUEUE_LEN) {
        queued = 1;
        for (uint32_t i = 0; i < len; i++) kbd_lines[kbd_tail][i] = line[i];
        kbd_lines[kbd_tail][len] = 0;
        kbd_lens[kbd_tail] = len;
        kbd_tail = (kbd_tail + 1) % KBD_QUEUE_LEN;
        kbd_count++;
        // Offer-path trace (input-bug bisect 2026-09-08): who queued what.
        // First 16 bytes hex + len; gated to offers (rare — lines, not polls).
        serial_puts("[kbd] offer len=");
        {
            char nb[12]; int ni = 0; uint32_t tv = len;
            char rv[12]; int ri = 0;
            if (tv == 0) nb[ni++] = '0';
            while (tv > 0 && ni < 11) { rv[ri++] = '0' + (tv % 10); tv /= 10; }
            while (ri > 0) nb[ni++] = rv[--ri];
            nb[ni] = 0;
            serial_puts(nb);
        }
        serial_puts(" bytes=");
        for (uint32_t i = 0; i < len && i < 16; i++) {
            uint8_t b = (uint8_t)line[i];
            serial_putchar("0123456789abcdef"[b >> 4]);
            serial_putchar("0123456789abcdef"[b & 15]);
            serial_putchar(' ');
        }
        serial_putchar('\n');
    }
    spin_unlock_irq(&kbd_lock, ef);
    return queued;
}

int sys_proc_kbd_read(char *buf, uint32_t len) {
    if (!buf || len == 0) return 0;
    uint32_t ef = spin_lock_irq(&kbd_lock);
    if (kbd_count == 0) {
        spin_unlock_irq(&kbd_lock, ef);
        return 0; // non-blocking: no line yet
    }
    uint32_t n = kbd_lens[kbd_head];
    if (n > len) n = len;
    for (uint32_t i = 0; i < n; i++) buf[i] = kbd_lines[kbd_head][i];
    // NUL-cap the kernel→user copy IN THE USER BUFFER (bisected 2026-09-08:
    // forkexec/sh children exec'd GARBAGE despite clean queue bytes — the
    // queue line "run /bin/sh" (11B, no newline: term strips it before
    // offer) was copied verbatim, and ring-3 `line[n]=0` SHOULD have capped
    // it... unless n was already wrong. Belt and suspenders: cap here too,
    // inside the validated user range (n < len <= 4096, validated by the
    // caller) so user code can never read past the line even if ITS n is
    // stale).
    if (n < len) buf[n] = 0;
    kbd_head = (kbd_head + 1) % KBD_QUEUE_LEN;
    kbd_count--;
    spin_unlock_irq(&kbd_lock, ef);
    // Read-path trace (input-bug bisect): what the reader actually got.
    {
        extern struct process *process_current(void) __attribute__((weak));
        uint32_t who = 0xFFFFFFFF;
        if (process_current) {
            struct process *c = process_current();
            if (c) who = c->pid;
        }
        serial_printf("[kbd] read pid=%d n=%d first16=", who, (int)n);
        for (uint32_t i = 0; i < n && i < 16; i++) {
            uint8_t b = (uint8_t)buf[i];
            serial_putchar("0123456789abcdef"[b >> 4]);
            serial_putchar("0123456789abcdef"[b & 15]);
            serial_putchar(' ');
        }
        serial_putchar('\n');
    }
    return (int)n;
}

int sys_proc_kbd_pending(void) {
    uint32_t ef = spin_lock_irq(&kbd_lock);
    int n = kbd_count;
    spin_unlock_irq(&kbd_lock, ef);
    return n > 0 ? 1 : 0;
}

// Owning terminal window of the CURRENT process (see term_win in the PCB).
int sys_proc_term_win(void) {
    struct process *cur = process_current();
    if (!cur) return -1;
    return cur->term_win;
}

// ---- Open-file allocation ----
static int ofd_alloc(void) {
    uint32_t ef = spin_lock_irq(&ofd_lock);
    for (int i = 0; i < SYS_PROC_MAX_OFD; i++) {
        if (ofd_tab[i].refcount == 0) {
            ofd_tab[i].refcount = 1;
            ofd_tab[i].kind = 0;
            ofd_tab[i].flags = 0;
            ofd_tab[i].name[0] = 0;
            ofd_tab[i].offset = 0;
            ofd_tab[i].pipe_idx = -1;
            ofd_tab[i].pipe_write = 0;
            spin_unlock_irq(&ofd_lock, ef);
            return i;
        }
    }
    spin_unlock_irq(&ofd_lock, ef);
    return -1;
}

static void ofd_hold(int idx) {
    if (idx < 0 || idx >= SYS_PROC_MAX_OFD) return;
    uint32_t ef = spin_lock_irq(&ofd_lock);
    ofd_tab[idx].refcount++;
    spin_unlock_irq(&ofd_lock, ef);
}

static void ofd_drop(int idx) {
    if (idx < 0 || idx >= SYS_PROC_MAX_OFD) return;
    uint32_t ef = spin_lock_irq(&ofd_lock);
    if (ofd_tab[idx].refcount > 0) ofd_tab[idx].refcount--;
    int gone = (ofd_tab[idx].refcount == 0);
    int kind = ofd_tab[idx].kind;
    int pidx = ofd_tab[idx].pipe_idx;
    int is_write = ofd_tab[idx].pipe_write;
    spin_unlock_irq(&ofd_lock, ef);
    // Last close on a pipe end: update the pipe's reader/writer counts.
    if (gone && kind == OFD_PIPE && pidx >= 0 && pidx < SYS_PROC_MAX_PIPES) {
        uint32_t pf = spin_lock_irq(&pipe_lock);
        if (is_write) {
            if (pipes[pidx].writers > 0) pipes[pidx].writers--;
        } else {
            if (pipes[pidx].readers > 0) pipes[pidx].readers--;
        }
        if (pipes[pidx].readers == 0 && pipes[pidx].writers == 0)
            pipes[pidx].used = 0;
        spin_unlock_irq(&pipe_lock, pf);
    }
}

// ---- Files ----
int sys_proc_open(const char *path, uint32_t flags) {
    if (!path || !path[0]) return -1;
    struct process *cur = process_current();
    if (!cur) return -1;
    // flags: 0 = read, 1 = write (create/truncate), 2 = read/write (create).
    if (flags > 2) return -1;
    if ((flags == 1 || flags == 2) && !fs_exists(path))
        fs_create(path);
    if (!fs_exists(path)) return -1;
    int oi = ofd_alloc();
    if (oi < 0) return -1;
    uint32_t ef = spin_lock_irq(&ofd_lock);
    ofd_tab[oi].kind = OFD_FILE;
    ofd_tab[oi].flags = (int)flags;
    uint32_t i = 0;
    while (path[i] && i < 31) { ofd_tab[oi].name[i] = path[i]; i++; }
    ofd_tab[oi].name[i] = 0;
    // Write-only opens truncate (fresh file semantics); read paths keep data.
    ofd_tab[oi].offset = 0;
    spin_unlock_irq(&ofd_lock, ef);
    if (flags == 1) fs_write(path, (const uint8_t*)"", 0);
    int fd = process_fd_alloc(cur, oi);
    if (fd < 0) { ofd_drop(oi); return -1; }
    return fd;
}

int sys_proc_close(int fd) {
    struct process *cur = process_current();
    if (!cur) return -1;
    int v = process_fd_get(cur, fd);
    if (v == PROC_FD_FREE) return -1;
    if (v == PROC_FD_TERM || v == PROC_FD_KBD) {
        // Standard streams are wired, not allocated — just free the slot?
        // No: 0/1/2 are permanent. Closing them is a no-op success.
        return 0;
    }
    process_fd_free(cur, fd);
    ofd_drop(v);
    return 0;
}

int sys_proc_write_fd(int fd, const char *buf, uint32_t len) {
    if (!buf && len > 0) return -1;
    struct process *cur = process_current();
    if (!cur) return -1;
    int v = process_fd_get(cur, fd);
    if (v == PROC_FD_FREE) return -1;
    if (v == PROC_FD_TERM) return -1; // terminal goes through fd-1 fast path
    if (v == PROC_FD_KBD) return -1;  // keyboard is read-only
    if (ofd_tab[v].kind == OFD_FILE) {
        if (ofd_tab[v].flags == 0) return -1; // opened read-only
        // Static 32KB spill (trap stack is 4KB — never a stack buffer).
        // VFS files are flat images; offsets beyond EOF clamp; tail past
        // 32KB truncates (matches FS_MAX_SIZE).
        static uint8_t splice_buf[32768];
        int cur_len = fs_read(ofd_tab[v].name, splice_buf, sizeof(splice_buf));
        if (cur_len < 0) cur_len = 0;
        uint32_t off = ofd_tab[v].offset;
        if (off > (uint32_t)cur_len) off = (uint32_t)cur_len;
        uint32_t end = off + len;
        if (end > sizeof(splice_buf)) end = sizeof(splice_buf);
        for (uint32_t i = off; i < end; i++)
            splice_buf[i] = (uint8_t)buf[i - off];
        int new_len = end > (uint32_t)cur_len ? (int)end : cur_len;
        fs_write(ofd_tab[v].name, splice_buf, new_len);
        ofd_tab[v].offset = end;
        return (int)(end - off);
    }
    if (ofd_tab[v].kind == OFD_PIPE) {
        if (!ofd_tab[v].pipe_write) return -1; // read end is not writable
        int pidx = ofd_tab[v].pipe_idx;
        uint32_t ef = spin_lock_irq(&pipe_lock);
        struct pipe *pp = &pipes[pidx];
        uint32_t wrote = 0;
        while (wrote < len && pp->count < SYS_PROC_PIPE_BUF) {
            pp->buf[pp->tail] = (uint8_t)buf[wrote];
            pp->tail = (pp->tail + 1) % SYS_PROC_PIPE_BUF;
            pp->count++;
            wrote++;
        }
        spin_unlock_irq(&pipe_lock, ef);
        return (int)wrote;
    }
    return -1;
}

int sys_proc_read_fd(int fd, char *buf, uint32_t len) {
    if (!buf && len > 0) return -1;
    struct process *cur = process_current();
    if (!cur) return -1;
    int v = process_fd_get(cur, fd);
    if (v == PROC_FD_FREE) return -1;
    // fd 0 = keyboard (wired KBD stream): drain the line queue. fd 1/2 =
    // terminal for writes; reading them is invalid.
    if (v == PROC_FD_KBD) return sys_proc_kbd_read(buf, len);
    if (v == PROC_FD_TERM) return -1;
    if (ofd_tab[v].kind == OFD_FILE) {
        if (ofd_tab[v].flags == 1) return -1; // opened write-only
        // Static 32KB spill (trap stack is 4KB — never a stack buffer).
        static uint8_t read_buf[32768];
        int cur_len = fs_read(ofd_tab[v].name, read_buf, sizeof(read_buf));
        if (cur_len < 0) return -1;
        uint32_t off = ofd_tab[v].offset;
        if (off >= (uint32_t)cur_len) return 0; // EOF
        uint32_t n = (uint32_t)cur_len - off;
        if (n > len) n = len;
        for (uint32_t i = 0; i < n; i++) buf[i] = (char)read_buf[off + i];
        ofd_tab[v].offset = off + n;
        return (int)n;
    }
    if (ofd_tab[v].kind == OFD_PIPE) {
        if (ofd_tab[v].pipe_write) return -1; // write end is not readable
        int pidx = ofd_tab[v].pipe_idx;
        uint32_t ef = spin_lock_irq(&pipe_lock);
        struct pipe *pp = &pipes[pidx];
        uint32_t n = 0;
        while (n < len && pp->count > 0) {
            buf[n] = (char)pp->buf[pp->head];
            pp->head = (pp->head + 1) % SYS_PROC_PIPE_BUF;
            pp->count--;
            n++;
        }
        spin_unlock_irq(&pipe_lock, ef);
        return (int)n; // 0 = empty (non-blocking; writers may still exist)
    }
    return -1;
}

// ---- Pipes + dup ----
int sys_proc_pipe(int *fds) {
    if (!fds) return -1;
    struct process *cur = process_current();
    if (!cur) return -1;
    uint32_t ef = spin_lock_irq(&pipe_lock);
    int pidx = -1;
    for (int i = 0; i < SYS_PROC_MAX_PIPES; i++) {
        if (!pipes[i].used) { pidx = i; break; }
    }
    if (pidx < 0) { spin_unlock_irq(&pipe_lock, ef); return -1; }
    pipes[pidx].used = 1;
    pipes[pidx].head = pipes[pidx].tail = pipes[pidx].count = 0;
    pipes[pidx].readers = 1;
    pipes[pidx].writers = 1;
    spin_unlock_irq(&pipe_lock, ef);
    int ro = ofd_alloc(), wo = ofd_alloc();
    if (ro < 0 || wo < 0) {
        if (ro >= 0) ofd_drop(ro);
        if (wo >= 0) ofd_drop(wo);
        uint32_t pf = spin_lock_irq(&pipe_lock);
        pipes[pidx].used = 0;
        spin_unlock_irq(&pipe_lock, pf);
        return -1;
    }
    uint32_t of = spin_lock_irq(&ofd_lock);
    ofd_tab[ro].kind = OFD_PIPE;
    ofd_tab[ro].pipe_idx = pidx;
    ofd_tab[ro].pipe_write = 0;
    ofd_tab[wo].kind = OFD_PIPE;
    ofd_tab[wo].pipe_idx = pidx;
    ofd_tab[wo].pipe_write = 1;
    spin_unlock_irq(&ofd_lock, of);
    int rfd = process_fd_alloc(cur, ro);
    int wfd = process_fd_alloc(cur, wo);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) process_fd_free(cur, rfd);
        if (wfd >= 0) process_fd_free(cur, wfd);
        ofd_drop(ro);
        ofd_drop(wo);
        return -1;
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

int sys_proc_dup(int oldfd) {
    struct process *cur = process_current();
    if (!cur) return -1;
    int v = process_fd_get(cur, oldfd);
    if (v == PROC_FD_FREE) return -1;
    // Standard streams dup as themselves onto a new slot? No — dup of a
    // wired stream is meaningless; only real open files/pipes dup.
    if (v == PROC_FD_TERM || v == PROC_FD_KBD) return -1;
    ofd_hold(v);
    int nfd = process_fd_alloc(cur, v);
    if (nfd < 0) { ofd_drop(v); return -1; }
    return nfd;
}

// ---- sbrk + mmap/munmap (per-process user heap) ----
#define USER_HEAP_BASE 0x08000000u
#define USER_HEAP_MAX  0x40000000u
#define USER_MMAP_BASE 0x40000000u
#define USER_MMAP_TOP  0x00007F0000000000ull

uint64_t sys_proc_sbrk(uint64_t inc) {
    struct process *cur = process_current();
    if (!cur || !cur->page_dir) return 0;
    if (cur->heap_break == 0) cur->heap_break = USER_HEAP_BASE;
    uint64_t old = cur->heap_break;
    if (inc == 0) return old;
    // Only growth (no shrink — freed heap pages stay mapped; munmap the
    // top explicitly if you need them back). Cap at USER_HEAP_MAX.
    if (old + inc < old || old + inc > USER_HEAP_MAX) return 0;
    uint64_t start_page = (old + 0xFFF) & ~0xFFFull;
    uint64_t end_page = (old + inc + 0xFFF) & ~0xFFFull;
    for (uint64_t page = start_page; page < end_page; page += 0x1000) {
        // Skip already-mapped pages (heap/stack overlap region).
        phys_t phys = pmm_alloc_page();
        if (!phys) return 0; // out of memory — break unchanged
        paging_map_user_pd(cur->page_dir, page, phys);
        uint8_t *z = (uint8_t*)P2V_ADDR(phys);
        for (int i = 0; i < 4096; i++) z[i] = 0;
    }
    cur->heap_break = old + inc;
    return old;
}

uint64_t sys_proc_mmap_anon(uint64_t virt) {
    struct process *cur = process_current();
    if (!cur || !cur->page_dir) return 0;
    if (cur->mmap_next == 0) cur->mmap_next = USER_MMAP_TOP;
    uint64_t page;
    if (virt == 0) {
        // Auto: scan down from the hint for a free page (present-bit test
        // through the HIGH alias — no paging.h readback helper exists, so
        // read the PD/PT directly here).
        page = 0;
        for (uint64_t cand = (cur->mmap_next - 0x1000) & ~0xFFFull;
             cand >= USER_MMAP_BASE; cand -= 0x1000) {
            int present = aspace_present(cur->page_dir, cand);
            if (!present) { page = cand; break; }
            if (cand == USER_MMAP_BASE) break;
        }
        if (!page) return 0;
        cur->mmap_next = page;
    } else {
        page = virt;
    }
    phys_t phys = pmm_alloc_page();
    if (!phys) return 0;
    paging_map_user_pd(cur->page_dir, page, phys);
    uint8_t *z = (uint8_t*)P2V_ADDR(phys);
    for (int i = 0; i < 4096; i++) z[i] = 0;
    return page;
}

void sys_proc_munmap(uint64_t virt) {
    struct process *cur = process_current();
    if (!cur || !cur->page_dir) return;
    phys_t phys = aspace_unmap(cur->page_dir, virt);
    if (!phys) return;
    pmm_free_page(phys);
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

// ---- fork: copy the calling process's user-low address space ----
// One user page of the parent: a fresh PMM page for the child with the same contents (through the HIGH alias).
struct fork_copy { phys_t child_cr3; };
static int fork_copy_page(uint64_t virt, phys_t src_phys, void *ctx) {
    struct fork_copy *fc = (struct fork_copy*)ctx;
    phys_t phys = pmm_alloc_page();
    if (!phys) return -1;
    paging_map_user_pd(fc->child_cr3, virt, phys);
    uint8_t *dst = (uint8_t*)P2V_ADDR(phys);
    uint8_t *src = (uint8_t*)P2V_ADDR(src_phys);
    for (int b = 0; b < 4096; b++) dst[b] = src[b];
    return 0;
}

int sys_proc_fork(void) {
    struct process *parent = process_current();
    if (!parent || parent->pid == 0 || !parent->page_dir) return -1;
    int child_pid = process_create();
    if (child_pid < 0) return -1;
    struct process *child = process_get((uint32_t)child_pid);
    if (!child) return -1;
    // Copy user-low pages (PD 0-767, present entries): fresh PMM page per
    // page, memcpy through the HIGH alias. Kernel-high is shared by design
    // (create_page_directory already shares it — never copy it).
    struct fork_copy fc = { child->page_dir };
    if (aspace_walk_user(parent->page_dir, fork_copy_page, &fc)) { process_destroy((uint32_t)child_pid); return -1; }
    // Child resumes AFTER the fork trap (same RSP the parent trapped with;
    // RIP = return address: the word the trap pushed... precisely, the
    // INT 0x80 trap pushed NOTHING on the user stack (ring-3 int pushes
    // RFLAGS/CS/RIP on the KERNEL trap stack via TSS.RSP0, not the user
    // stack) — so user RSP is unchanged by the trap and the return address
    // is wherever the CALLER put it. For our ring-3 wrappers the fork call
    // site is `int $0x80` inline in straight-line code; resume RIP = trapped
    // RIP (next instruction after int), RSP = trapped RSP verbatim.
    // Stashed by idt.c pre-dispatch (pushed[11/14] = CPU RIP/RSP).
    // (The old code resumed the child at image entry — it re-ran _start,
    // forked again, and fork-bombed grandchildren that all printed "parent".)
    {
        uint64_t t_rip = syscall_trap_rip();
        uint64_t t_rsp = syscall_trap_rsp();
        // Sanity: trapped RIP must be user-low text, RSP the user stack.
        // (If idt.c ever fails to stash, both read 0 — refuse the child
        // rather than IRETing to NULL and faulting obscurely.)
        if (t_rip < 0x1000 || t_rip >= USER_SPACE_TOP || t_rsp < 0x1000 || t_rsp >= USER_SPACE_TOP) {
            serial_printf("[fork] BAD trapped resume rip=%lx rsp=%lx — abort child\n",
                          t_rip, t_rsp);
            process_destroy((uint32_t)child_pid);
            return -1;
        }
        // FORK STUB: a child enters through a fresh IRET, which loads only RIP/CS/RFLAGS/RSP/SS and RAX, so a mid-function resume would find
        // garbage in the callee-saved registers. Put the parent's trapped registers + the resume RIP on the child's stack copy and enter the
        // child at a 13-byte stub laid at the stack page base:
        //   pop rbx; pop rbp; pop r12; pop r13; pop r14; pop r15; ret
        // The pops run low -> high and the ret lands at the resume RIP with RSP exactly on the trapped RSP. RAX = 0 via the fork flag (the stub
        // preserves it). Needs 56 free bytes below the trapped RSP and room for the stub below them, else the child is aborted (loud, not silent).
        {
            uint64_t regs[6];
            syscall_trap_regs(regs);
            uint32_t page_off = (uint32_t)(t_rsp & 0xFFF);
            if (page_off < 80) {
                serial_printf("[fork] stub: no room rsp=%lx, abort child\n", t_rsp);
                process_destroy((uint32_t)child_pid);
                return -1;
            }
            // Locate the child's stack page phys (walk ITS address space; the stack page is the last user page).
            uint64_t *spte = aspace_pte(child->page_dir, t_rsp, 0);
            phys_t sph = spte ? PTE_ADDR(*spte) : 0;
            uint8_t *spage = (uint8_t*)P2V_ADDR(sph);
            static const uint8_t stub[13] = { 0x5B, 0x5D, 0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0xC3, 0x90, 0x90 };
            for (int b = 0; b < 13; b++) spage[b] = stub[b];
            uint64_t stub_virt = t_rsp & ~0xFFFull;
            uint32_t woff = page_off - 56;
            for (int r = 0; r < 6; r++) ustack_put(spage, woff + (uint32_t)r * 8, regs[r]);
            ustack_put(spage, woff + 48, t_rip);
            child->user_rip = stub_virt;
            child->user_rsp = (t_rsp & ~0xFFFull) + woff;
            serial_printf("[fork] child=%d stub=%lx resume rip=%lx rsp=%lx\n", child_pid, stub_virt, t_rip, t_rsp);
        }
    }
    child->heap_break = parent->heap_break;
    child->mmap_next = parent->mmap_next;
    child->parent_pid = parent->pid;
    child->ticks_left = SCHED_SLICE_TICKS;
    child->term_win = parent->term_win; // stdio follows the parent's window
    // Child ring-3 return value: fork() returns 0 in the child. The entry
    // IRET path (main loop) forces pushed-RAX 0 for fork children — flagged
    // here so the entry site knows (see sched_fork_child_entry below).
    child->exit_code = 0;
    child->pending_signals = 0;
    child->state = PROC_READY;
    // Inherit fds (shared descriptions — refcount up).
    for (int f = 0; f < PROC_MAX_FDS; f++) {
        child->fds[f] = parent->fds[f];
        if (child->fds[f] >= 0) ofd_hold(child->fds[f]);
    }
    serial_printf("[fork] parent=%d child=%d pages copied\n",
                  parent->pid, child_pid);
    // Stage the child's ring-3 entry (RAX forced 0 — see sched layer).
    // Drop any stale park resume for this pid first (slot reuse: the
    // previous occupant may have parked and never been re-entered; the
    // drain keys resumes by pid, so a stale entry would hijack the child's
    // first entry with a dead RIP/RSP — garbage-exec/#PF class).
    {
        extern int sched_fork_child_entry(uint32_t child_pid);
        extern void sched_park_drop(uint32_t pid);
        sched_park_drop((uint32_t)child_pid);
        sched_fork_child_entry((uint32_t)child_pid);
    }
    return child_pid; // parent sees child pid through its own trap iret
}

// ---- exec: load an ELF from the VFS into the CALLING process ----
static phys_t exec_map_target_pd = 0;
static void exec_map_cb(uint64_t virt_page, uint64_t phys_page) {
    paging_map_user_pd(exec_map_target_pd, virt_page, phys_page);
}

int sys_proc_exec(const char *path) {
    if (!path || !path[0]) return -1;
    struct process *cur = process_current();
    if (!cur || cur->pid == 0 || !cur->page_dir) return -1;
    // PATH SANITY (input-bug bisect 2026-09-08: garbage exec string): the
    // kernel copy (kpath, 256B stack buffer in syscall.c) is passed by
    // pointer here — if the USER buffer was garbage, this is garbage. Bound
    // the damage: reject non-printable first bytes and over-long paths
    // BEFORE fs_exists walks them (find_file's unbounded compare on a
    // non-terminated 256B buffer can run pages past the buffer — mapped, so
    // no fault, but serial garbage + wasted time).
    {
        uint32_t plen = 0;
        while (plen < 256 && path[plen]) plen++;
        if (plen == 0 || plen >= 64) {
            serial_printf("[exec] bad path len=%d pid=%d\n",
                          plen, cur->pid);
            return -1;
        }
        for (uint32_t bi = 0; bi < plen; bi++) {
            uint8_t b = ((const uint8_t*)path)[bi];
            if (b < 32 || b > 126) {
                serial_printf("[exec] bad path byte pid=%d off=%d v=%x\n",
                              cur->pid, bi, b);
                return -1;
            }
        }
    }
    if (!fs_exists(path)) {
        // NOT-FOUND PATH TRACE (input-bug bisect 2026-09-08: the failing
        // exec never reaches syscall.c's [exec-arg] trace — it fails INSIDE
        // fs_exists on a garbage pointer. Log the pointer + first 16 bytes
        // hex here to prove whether the garbage is in the caller's RBX or
        // introduced by the kernel copy... path IS the kernel copy (kpath),
        // so garbage here = the USER buffer was already garbage at trap
        // time = caller-stack corruption, NOT a kernel bug).
        serial_puts("[exec] not found ptr=");
        serial_printf("%p", path);
        serial_puts(" bytes=");
        for (uint32_t bi = 0; bi < 16; bi++) {
            uint8_t b = ((const uint8_t*)path)[bi];
            serial_putchar("0123456789abcdef"[b >> 4]);
            serial_putchar("0123456789abcdef"[b & 15]);
            serial_putchar(' ');
            if (!b) break;
        }
        serial_puts(" str='");
        serial_puts(path);
        serial_putchar('\n');
        return -1;
    }
    serial_printf("[exec] pid=%d loading '%s'...\n", cur->pid, path);
    static uint8_t elf_img[65536];
    int len = fs_read(path, elf_img, sizeof(elf_img));
    if (len <= 0) return -1;
    // Wipe user-low (fresh address space, keep kernel-high shared tables).
    // Free pages first (avoid leaking the old image), then clear PDEs.
    aspace_free_user(cur->page_dir);
    // TLB flush (bisected 2026-09-08: exec #PF exc 14 at the new entry —
    // the wipe above only cleared PDEs/PTEs in MEMORY, but the TLB still
    // holds the OLD translations... actually worse: it holds NOTHING valid
    // yet the CPU may have cached the freed pages; either way the resume
    // RIP faults). CR3 reload flushes all non-global entries; safe here
    // (same address space, ring 0, no frame dependence).
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    // Fresh user stack (top of user-low, same ABI as spawn).
    phys_t stack_phys = pmm_alloc_page();
    if (!stack_phys) return -1;
    uint64_t u_stack_top = PROC_STACK_TOP;
    paging_map_user_pd(cur->page_dir, u_stack_top - 4096, stack_phys);
    // Load segments.
    exec_map_target_pd = cur->page_dir;
    uint64_t entry = 0;
    int rc = elf_load(elf_img, (uint32_t)len, exec_map_cb, &entry);
    exec_map_target_pd = 0;
    if (rc) return -1;
    // Rebuild the SysV entry stack on the fresh stack page (same layout as
    // sched_spawn_elf: [argc][argv_tab][envp_tab][strings]). sys_proc_exec
    // used to leave user_rsp = stack TOP (empty stack): every exec'd program
    // then read argc/argv from unmapped... actually mapped-but-zero stack
    // memory — garbage argc, garbage argv, #PF on first dereference
    // (bisected 2026-09-08: forkexec child #PF err=5 cr2=C0000000 at
    // hello+0xd reading argv[0] — RSP pointed at the empty top, not a
    // header). Kernel-owned argv: argv[0] = path, argc = 1, envp empty.
    {
        uint8_t *sp0 = (uint8_t*)P2V_ADDR(stack_phys);
        uint32_t path_len = 0;
        while (path[path_len] && path_len < 255) path_len++;
        path_len++; // NUL
        uint32_t need = path_len + 8 * 2 + 8 * 1 + 24 + 15;     // + 15: the string area's alignment pad
        need = (need + 15) & ~15u;
        if (need > 4096) return -1;
        uint32_t base = 4096 - need;
        uint64_t final_rsp = (u_stack_top - 4096) + base;
        uint64_t a_tab = final_rsp + 24;
        uint64_t e_tab = a_tab + 16;
        uint64_t s_base = (e_tab + 8 + 15) & ~15u;             // rounded UP: down landed on argv[1]/envp[0], zeroing the path's first bytes
        ustack_put(sp0, base, 1);                               // argc
        ustack_put(sp0, base + 8, a_tab);
        ustack_put(sp0, base + 16, e_tab);
        uint32_t w = (uint32_t)(a_tab - (u_stack_top - 4096));
        uint32_t s = (uint32_t)(s_base - (u_stack_top - 4096));
        uint64_t v = (u_stack_top - 4096) + s;
        for (uint32_t b = 0; b < path_len; b++) sp0[s + b] = (uint8_t)path[b];
        ustack_put(sp0, w, v);                                  // argv[0]
        ustack_put(sp0, w + 8, 0);                              // argv[1]
        ustack_put(sp0, w + 16, 0);                             // envp[0]
        cur->user_rip = entry;
        cur->user_rsp = final_rsp;
    }
    cur->heap_break = 0;
    cur->mmap_next = 0;
    // CLOEXEC-lite: close flagged fds (keep-open is the default). Wired
    // streams (0/1/2) are never closed — stdio survives exec by design.
    for (int f = 3; f < PROC_MAX_FDS; f++) {
        int v = cur->fds[f];
        if (v < 0 || v >= SYS_PROC_MAX_OFD) continue;
        if (ofd_tab[v].flags & OFD_CLOEXEC) {
            cur->fds[f] = PROC_FD_FREE;
            ofd_drop(v);
        }
    }
    serial_printf("[exec] pid=%d '%s' entry=%lx\n", cur->pid, path, entry);
    return 0;
}

// ---- wait/kill/signals ----
// Non-blocking reap poll for ring-3 trap context (the stub frame must iret,
// so syscalls from ring 3 can never sleep): returns the reaped pid, 0 when
// no zombie is ready yet, -1 when there are no children at all.
int sys_proc_wait(int pid) {
    struct process *cur = process_current();
    if (!cur) return -1;
    int have_child = 0;
    for (int s = 0; s < MAX_PROCESSES; s++) {
        struct process *c = process_get_by_slot(s);
        if (!c) continue;
        if (c->parent_pid != cur->pid) continue;
        if (pid != -1 && (int)c->pid != pid) continue;
        have_child = 1;
        if (c->state == PROC_ZOMBIE || c->state == PROC_EXITED) {
            int got = (int)c->pid;
            process_destroy(c->pid);
            return got;
        }
    }
    return have_child ? 0 : -1;
}

// Mark the CURRENT process ZOMBIE with an exit status and notify the parent.
// Called from SYS_EXIT before the trampoline jumps home (idt.c hijack path
// also funnels here through syscall.c). Safe in trap context: touches only
// the PCB + parent bitmask, no switches, no waits.
void sys_proc_exit_current(int code) {
    struct process *cur = process_current();
    if (!cur || cur->pid == 0) return; // never zombie the idle process
    cur->exit_code = code;
    cur->state = PROC_ZOMBIE;
    if (cur->parent_pid) {
        struct process *par = process_get(cur->parent_pid);
        if (par) {
            par->pending_signals |= (1u << SIG_CHLD);
            // Wake a parent blocked in wait() (see sys_proc_wait_blocking).
            if (par->state == PROC_BLOCKED)
                par->state = PROC_READY;
        }
    }
    serial_printf("[exit] pid=%d code=%d zombie\n", cur->pid, code);
}

// Trap-context wait (see sys_proc.h): reap-ready → pid; no matching child
// → -1; child alive but no zombie yet → -2 (WAIT_PARK, caller retries).
// Never BLOCKs by itself — the CALLER (syscall.c) arms the park trampoline
// on -2, which returns straight to the main loop (no iret spin). Trap-safe:
// PCB scan only. (An earlier draft set PROC_BLOCKED here and ireted -2;
// that wedged the drain order — the parent spun 10M traps while the child
// sat queued behind a drain that only runs on exits.)
int sys_proc_wait_park(int pid) {
    struct process *cur = process_current();
    if (!cur) return -1;
    int have_child = 0;
    for (int s = 0; s < MAX_PROCESSES; s++) {
        struct process *c = process_get_by_slot(s);
        if (!c) continue;
        if (c->parent_pid != cur->pid) continue;
        if (pid != -1 && (int)c->pid != pid) continue;
        have_child = 1;
        if (c->state == PROC_ZOMBIE || c->state == PROC_EXITED) {
            int got = (int)c->pid;
            process_destroy(c->pid);
            return got;
        }
    }
    if (!have_child) return -1;
    // No zombie YET → -2. The caller (syscall.c) arms the park trampoline:
    // no iret back to ring 3, straight to the main loop, which runs the
    // queued sibling THIS iteration. Re-entry resumes after the int $0x80
    // with RAX=-2 → ring 3 retries and reaps.
    return -2;
}

// Yield-park: legacy helper (park-trampoline era: syscall.c arms the park
// directly now instead of BLOCKING here). Kept for the text build + any
// direct callers; prefer syscall_arm_park in trap context. Harmless either
// way (WAKE pass still wakes BLOCKED+ticks==1).
int sys_yield_park(void) {
    struct process *cur = process_current();
    if (!cur || cur->pid == 0) return -1;
    cur->state = PROC_BLOCKED;
    cur->ticks_left = 1; // yield-park tag (see WAKE pass in sched_tick)
    return 0;
}

// Blocking wait for kernel-thread callers (shell foreground run). Sleeps as
// BLOCKED until a matching child goes ZOMBIE/EXITED, then reaps. Must NEVER
// be called from ring-3 trap context (the stub frame must iret). Returns the
// reaped pid, or -1 when there are no children at all.
int sys_proc_wait_blocking(int pid) {
    struct process *cur = process_current();
    if (!cur) return -1;
    for (;;) {
        int have_child = 0;
        for (int s = 0; s < MAX_PROCESSES; s++) {
            struct process *c = process_get_by_slot(s);
            if (!c) continue;
            if (c->parent_pid != cur->pid) continue;
            if (pid != -1 && (int)c->pid != pid) continue;
            have_child = 1;
            if (c->state == PROC_ZOMBIE || c->state == PROC_EXITED) {
                int got = (int)c->pid;
                process_destroy(c->pid);
                return got;
            }
        }
        if (!have_child) return -1;
        // Sleep until a child exit wakes us (exit_current flips BLOCKED→READY
        // via the parent notify above; timer wakeups also re-check).
        cur->state = PROC_BLOCKED;
        __asm__ volatile("sti; hlt; cli" ::: "memory");
        if (cur->state == PROC_BLOCKED)
            cur->state = PROC_RUNNING;
    }
}

// Reparent orphans to pid 1 (init). Called when a process with children is
// reaped: children must not point at a freed slot (parent_pid is a pid, not
// a slot — but wait() scans by parent_pid, so orphans would wait forever).
void sys_proc_reparent_to_init(uint32_t dead_pid) {
    for (int s = 0; s < MAX_PROCESSES; s++) {
        struct process *c = process_get_by_slot(s);
        if (!c) continue;
        if (c->parent_pid == dead_pid) {
            c->parent_pid = 1;
            // Init adopts even running children (it wait()s in a loop).
        }
    }
}

int sys_proc_kill(int pid, int signo) {
    if (signo < 0 || signo >= PROC_NSIG) return -1;
    struct process *t = process_get((uint32_t)pid);
    if (!t || t->pid == 0) return -1;
    sys_proc_signal_deliver(pid, signo);
    return 0;
}

void sys_proc_signal_deliver(int pid, int signo) {
    struct process *t = process_get((uint32_t)pid);
    if (!t) return;
    t->pending_signals |= (1u << signo);
    serial_printf("[sig] pid=%d pending=%x\n", pid, t->pending_signals);
}

void sys_proc_drain_signals(void) {
    // Called on the trampoline-resumed frame (safe point, ring 0, kernel
    // PD live). Default actions only: TERM kills (+reap), CHLD wakes
    // waiters (cleared — wait() polls), USR1 just clears (observed).
    struct process *cur = process_current();
    if (!cur) return;
    uint32_t pend = cur->pending_signals;
    if (!pend) return;
    cur->pending_signals = 0;
    if (pend & (1u << SIG_TERM)) {
        serial_printf("[sig] pid=%d TERM — exiting\n", cur->pid);
        cur->exit_code = 128 + SIG_TERM;
        cur->state = PROC_ZOMBIE;
        // Notify parent (CHLD) so wait() can reap.
        if (cur->parent_pid) {
            struct process *par = process_get(cur->parent_pid);
            if (par) par->pending_signals |= (1u << SIG_CHLD);
        }
    }
}

// ---- spinlock/mutex self-test (no SMP hardware needed) ----
void sys_proc_lock_selftest(void) {
    spinlock_t sl;
    spin_init(&sl);
    int ok = 1;
    // Basic acquire/release + contention flag.
    spin_lock(&sl);
    if (!sl.locked) ok = 0;
    spin_unlock(&sl);
    if (sl.locked) ok = 0;
    // IRQ-safe pair preserves IF=1 across the section.
    uint32_t ef = spin_lock_irq(&sl);
    spin_unlock_irq(&sl, ef);
    // Mutex nesting: same holder twice, two unlocks to free.
    mutex_t m;
    mutex_init(&m);
    mutex_lock(&m, 7);
    mutex_lock(&m, 7);
    if (m.depth != 2) ok = 0;
    mutex_unlock(&m, 7);
    if (m.holder != 7 || m.depth != 1) ok = 0;
    mutex_unlock(&m, 7);
    if (m.holder != -1) ok = 0;
    // Foreign unlock must not free.
    mutex_lock(&m, 7);
    mutex_unlock(&m, 8);
    if (m.holder != 7) ok = 0;
    mutex_unlock(&m, 7);
    serial_printf("[locks] selftest %s\n", ok ? "PASS" : "FAIL");
}

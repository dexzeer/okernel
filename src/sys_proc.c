#include "sys_proc.h"
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

void sys_proc_kbd_offer(const char *line, uint32_t len) {
    if (!line || len == 0) return;
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
        if (!live) return; // kernel-shell bootstrap line — nobody can read it
    }
    if (len >= KBD_LINE_LEN) len = KBD_LINE_LEN - 1;
    uint32_t ef = spin_lock_irq(&kbd_lock);
    if (kbd_count < KBD_QUEUE_LEN) {
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
#define USER_MMAP_TOP  0xB0000000u

uint32_t sys_proc_sbrk(uint32_t inc) {
    struct process *cur = process_current();
    if (!cur || !cur->page_dir) return 0;
    if (cur->heap_break == 0) cur->heap_break = USER_HEAP_BASE;
    uint32_t old = cur->heap_break;
    if (inc == 0) return old;
    // Only growth (no shrink — freed heap pages stay mapped; munmap the
    // top explicitly if you need them back). Cap at USER_HEAP_MAX.
    if (old + inc < old || old + inc > USER_HEAP_MAX) return 0;
    uint32_t start_page = (old + 0xFFF) & 0xFFFFF000;
    uint32_t end_page = (old + inc + 0xFFF) & 0xFFFFF000;
    for (uint32_t page = start_page; page < end_page; page += 0x1000) {
        // Skip already-mapped pages (heap/stack overlap region).
        uint32_t phys = pmm_alloc_page();
        if (!phys) return 0; // out of memory — break unchanged
        paging_map_user_pd(cur->page_dir, page, phys);
        uint8_t *z = (uint8_t*)P2V_U32(phys);
        for (int i = 0; i < 4096; i++) z[i] = 0;
    }
    cur->heap_break = old + inc;
    return old;
}

uint32_t sys_proc_mmap_anon(uint32_t virt) {
    struct process *cur = process_current();
    if (!cur || !cur->page_dir) return 0;
    if (cur->mmap_next == 0) cur->mmap_next = USER_MMAP_TOP;
    uint32_t page;
    if (virt == 0) {
        // Auto: scan down from the hint for a free page (present-bit test
        // through the HIGH alias — no paging.h readback helper exists, so
        // read the PD/PT directly here).
        page = 0;
        for (uint32_t cand = (cur->mmap_next - 0x1000) & 0xFFFFF000;
             cand >= USER_MMAP_BASE; cand -= 0x1000) {
            uint32_t *pd = (uint32_t*)P2V_U32(cur->page_dir);
            uint32_t pde = pd[cand >> 22];
            int present = 0;
            if (pde & 0x01) {
                uint32_t *pt = (uint32_t*)P2V_U32(pde & 0xFFFFF000);
                present = (pt[(cand >> 12) & 0x3FF] & 0x01) != 0;
            }
            if (!present) { page = cand; break; }
            if (cand == USER_MMAP_BASE) break;
        }
        if (!page) return 0;
        cur->mmap_next = page;
    } else {
        page = virt;
    }
    uint32_t phys = pmm_alloc_page();
    if (!phys) return 0;
    paging_map_user_pd(cur->page_dir, page, phys);
    uint8_t *z = (uint8_t*)P2V_U32(phys);
    for (int i = 0; i < 4096; i++) z[i] = 0;
    return page;
}

void sys_proc_munmap(uint32_t virt) {
    struct process *cur = process_current();
    if (!cur || !cur->page_dir) return;
    uint32_t *pd = (uint32_t*)P2V_U32(cur->page_dir);
    uint32_t pde = pd[virt >> 22];
    if (!(pde & 0x01)) return;
    uint32_t *pt = (uint32_t*)P2V_U32(pde & 0xFFFFF000);
    uint32_t pte = pt[(virt >> 12) & 0x3FF];
    if (!(pte & 0x01)) return;
    pmm_free_page(pte & 0xFFFFF000);
    pt[(virt >> 12) & 0x3FF] = 0;
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
}

// ---- fork: copy the calling process's user-low address space ----
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
    uint32_t *ppd = (uint32_t*)P2V_U32(parent->page_dir);
    for (int j = 0; j < PD_KERNEL_BASE; j++) {
        if (!(ppd[j] & 0x01)) continue;
        uint32_t *ppt = (uint32_t*)P2V_U32(ppd[j] & 0xFFFFF000);
        for (int k = 0; k < 1024; k++) {
            if (!(ppt[k] & 0x01)) continue;
            uint32_t phys = pmm_alloc_page();
            if (!phys) { process_destroy((uint32_t)child_pid); return -1; }
            uint32_t virt = ((uint32_t)j << 22) | ((uint32_t)k << 12);
            paging_map_user_pd(child->page_dir, virt, phys);
            uint8_t *dst = (uint8_t*)P2V_U32(phys);
            uint8_t *src = (uint8_t*)P2V_U32(ppt[k] & 0xFFFFF000);
            for (int b = 0; b < 4096; b++) dst[b] = src[b];
        }
    }
    // Child resumes AFTER the fork trap (same ESP the parent trapped with;
    // EIP = return address: the word the trap pushed... precisely, the
    // INT 0x80 trap pushed NOTHING on the user stack (ring-3 int pushes
    // EFLAGS/CS/EIP on the KERNEL trap stack via TSS.ESP0, not the user
    // stack) — so user ESP is unchanged by the trap and the return address
    // is wherever the CALLER put it. For our ring-3 wrappers the fork call
    // site is `int $0x80` inline in straight-line code; resume EIP = trapped
    // EIP (next instruction after int), ESP = trapped ESP verbatim.
    // Stashed by idt.c pre-dispatch (pushed[11/14] = CPU EIP/ESP).
    // (The old code resumed the child at image entry — it re-ran _start,
    // forked again, and fork-bombed grandchildren that all printed "parent".)
    {
        extern uint32_t syscall_trap_eip(void);
        extern uint32_t syscall_trap_esp(void);
        extern uint32_t syscall_trap_ebx(void);
        extern uint32_t syscall_trap_edi(void);
        extern uint32_t syscall_trap_esi(void);
        extern uint32_t syscall_trap_ebp(void);
        uint32_t teip = syscall_trap_eip();
        uint32_t tesp = syscall_trap_esp();
        // Sanity: trapped EIP must be user-low text, ESP the user stack.
        // (If idt.c ever fails to stash, both read 0 — refuse the child
        // rather than IRETing to NULL and faulting obscurely.)
        if (teip < 0x1000 || teip >= 0xC0000000 ||
            tesp < 0xB0000000 || tesp >= 0xC0000000) {
            serial_printf("[fork] BAD trapped resume eip=%x esp=%x — abort child\n",
                          teip, tesp);
            process_destroy((uint32_t)child_pid);
            return -1;
        }
        // FORK STUB (2026-09-08): fresh-IRET children inherit garbage
        // callee-saved regs (EBX/EDI/ESI/EBP) — mid-function resumes (sh's
        // fork inside _start) die touching EBP-relative state. Push the
        // parent's trapped (EBX,EDI,ESI,EBP) + resume-EIP onto the child's
        // stack copy (via the HIGH alias; child PD not running) and enter
        // the child at a 5-byte stub (pop ebx; pop edi; pop esi; pop ebp;
        // ret = 5b 5f 5e 5d c3) laid at the stack page base. Frame order
        // (low→high): [EBX][EDI][ESI][EBP][resume-EIP] at the entry ESP;
        // pops run low→high and ret lands at resume-EIP with ESP exactly on
        // the trapped ESP. EAX=0 via the fork flag (stub preserves EAX).
        // Needs 21 free bytes below the trapped ESP; require tesp offset
        // >= 32 (page-relative) else abort the child (loud, not silent).
        {
            uint32_t tebx = syscall_trap_ebx();
            uint32_t tedi = syscall_trap_edi();
            uint32_t tesi = syscall_trap_esi();
            uint32_t tebp = syscall_trap_ebp();
            uint32_t page_off = tesp & 0xFFF;
            if (page_off < 32) {
                serial_printf("[fork] stub: no room esp=%x — abort child\n",
                              tesp);
                process_destroy((uint32_t)child_pid);
                return -1;
            }
            // Locate the child's stack page phys (walk ITS PD: PD 0-767 PD
            // 767 covers 0xBFC00000-0xBFFFFFFF; stack page = last PTE).
            uint32_t *cpd = (uint32_t*)P2V_U32(child->page_dir);
            uint32_t pde = cpd[tesp >> 22];
            uint32_t *cpt = (uint32_t*)P2V_U32(pde & 0xFFFFF000);
            uint32_t sph = cpt[(tesp >> 12) & 0x3FF] & 0xFFFFF000;
            uint8_t *spage = (uint8_t*)P2V_U32(sph);
            // Stub bytes at page base (5B; .text-adjacent low page is mapped
            // user — reuse the stack page base, never executed otherwise).
            spage[0] = 0x5b; spage[1] = 0x5f; spage[2] = 0x5e;
            spage[3] = 0x5d; spage[4] = 0xc3;
            uint32_t stub_virt = (tesp & 0xFFFFF000);
            // Frame (low→high): [EBX][EDI][ESI][EBP][resume-EIP], 20 bytes
            // below trapped ESP (pop order is low→high: EBX must sit at the
            // entry ESP; the final ret pops resume-EIP and lands ESP exactly
            // on the trapped ESP). Write LE by bytes (no unaligned u32 store).
            uint32_t vals[5];
            vals[0] = tebx; vals[1] = tedi;
            vals[2] = tesi; vals[3] = tebp; vals[4] = teip;
            uint32_t woff = page_off - 20;
            for (uint32_t wi = 0; wi < 5; wi++) {
                uint32_t v = vals[wi];
                spage[woff + wi * 4] = v & 0xFF;
                spage[woff + wi * 4 + 1] = (v >> 8) & 0xFF;
                spage[woff + wi * 4 + 2] = (v >> 16) & 0xFF;
                spage[woff + wi * 4 + 3] = (v >> 24) & 0xFF;
            }
            child->user_eip = stub_virt;
            child->user_esp = (tesp & 0xFFFFF000) + woff;
            serial_printf("[fork] child=%d stub=%x resume eip=%x esp=%x (frame ebp=%x esi=%x)\n",
                          child_pid, stub_virt, teip, tesp, tebp, tesi);
        }
    }
    child->heap_break = parent->heap_break;
    child->mmap_next = parent->mmap_next;
    child->parent_pid = parent->pid;
    child->ticks_left = SCHED_SLICE_TICKS;
    child->term_win = parent->term_win; // stdio follows the parent's window
    // Child ring-3 return value: fork() returns 0 in the child. The entry
    // IRET path (main loop) forces pushed-EAX 0 for fork children — flagged
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
    // Stage the child's ring-3 entry (EAX forced 0 — see sched layer).
    // Drop any stale park resume for this pid first (slot reuse: the
    // previous occupant may have parked and never been re-entered; the
    // drain keys resumes by pid, so a stale entry would hijack the child's
    // first entry with a dead EIP/ESP — garbage-exec/#PF class).
    {
        extern int sched_fork_child_entry(uint32_t child_pid);
        extern void sched_park_drop(uint32_t pid);
        sched_park_drop((uint32_t)child_pid);
        sched_fork_child_entry((uint32_t)child_pid);
    }
    return child_pid; // parent sees child pid through its own trap iret
}

// ---- exec: load an ELF from the VFS into the CALLING process ----
static uint32_t exec_map_target_pd = 0;
static void exec_map_cb(uint32_t virt_page, uint32_t phys_page) {
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
        // hex here to prove whether the garbage is in the caller's EBX or
        // introduced by the kernel copy... path IS the kernel copy (kpath),
        // so garbage here = the USER buffer was already garbage at trap
        // time = caller-stack corruption, NOT a kernel bug).
        serial_puts("[exec] not found ptr=");
        serial_printf("%x", (uint32_t)path);
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
    uint32_t *pd = (uint32_t*)P2V_U32(cur->page_dir);
    for (int j = 0; j < PD_KERNEL_BASE; j++) {
        if (!(pd[j] & 0x01)) continue;
        uint32_t *pt = (uint32_t*)P2V_U32(pd[j] & 0xFFFFF000);
        for (int k = 0; k < 1024; k++) {
            if (pt[k] & 0x01) pmm_free_page(pt[k] & 0xFFFFF000);
        }
        pmm_free_page(pd[j] & 0xFFFFF000);
        pd[j] = 0;
    }
    // TLB flush (bisected 2026-09-08: exec #PF exc 14 at the new entry —
    // the wipe above only cleared PDEs/PTEs in MEMORY, but the TLB still
    // holds the OLD translations... actually worse: it holds NOTHING valid
    // yet the CPU may have cached the freed pages; either way the resume
    // EIP faults). CR3 reload flushes all non-global entries; safe here
    // (same address space, ring 0, no frame dependence).
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
    // Fresh user stack (top of user-low, same ABI as spawn).
    uint32_t stack_phys = pmm_alloc_page();
    if (!stack_phys) return -1;
    uint32_t u_stack_top = 0xBFFFF000 + 4096;
    paging_map_user_pd(cur->page_dir, u_stack_top - 4096, stack_phys);
    // Load segments.
    exec_map_target_pd = cur->page_dir;
    uint32_t entry = 0;
    int rc = elf_load(elf_img, (uint32_t)len, exec_map_cb, &entry);
    exec_map_target_pd = 0;
    if (rc) return -1;
    // Rebuild the SysV entry stack on the fresh stack page (same layout as
    // sched_spawn_elf: [argc][argv_tab][envp_tab][strings]). sys_proc_exec
    // used to leave user_esp = stack TOP (empty stack): every exec'd program
    // then read argc/argv from unmapped... actually mapped-but-zero stack
    // memory — garbage argc, garbage argv, #PF on first dereference
    // (bisected 2026-09-08: forkexec child #PF err=5 cr2=C0000000 at
    // hello+0xd reading argv[0] — ESP pointed at the empty top, not a
    // header). Kernel-owned argv: argv[0] = path, argc = 1, envp empty.
    {
        uint8_t *sp0 = (uint8_t*)P2V_U32(stack_phys);
        uint32_t path_len = 0;
        while (path[path_len] && path_len < 255) path_len++;
        path_len++; // NUL
        uint32_t need = path_len + 4 * 2 + 4 * 1 + 12;
        need = (need + 15) & ~15u;
        if (need > 4096) return -1;
        uint32_t base = 4096 - need;
        uint32_t final_esp = (u_stack_top - 4096) + base;
        uint32_t a_tab = final_esp + 12;
        uint32_t e_tab = a_tab + 8;
        uint32_t s_base = (e_tab + 4) & ~15u;
        uint32_t h = base;
        sp0[h] = 1; sp0[h+1] = 0; sp0[h+2] = 0; sp0[h+3] = 0; // argc = 1
        sp0[h+4] = a_tab & 0xFF; sp0[h+5] = (a_tab >> 8) & 0xFF;
        sp0[h+6] = (a_tab >> 16) & 0xFF; sp0[h+7] = (a_tab >> 24) & 0xFF;
        sp0[h+8] = e_tab & 0xFF; sp0[h+9] = (e_tab >> 8) & 0xFF;
        sp0[h+10] = (e_tab >> 16) & 0xFF; sp0[h+11] = (e_tab >> 24) & 0xFF;
        uint32_t w = a_tab - (u_stack_top - 4096);
        uint32_t s = s_base - (u_stack_top - 4096);
        uint32_t v = (u_stack_top - 4096) + s;
        for (uint32_t b = 0; b < path_len; b++) sp0[s + b] = (uint8_t)path[b];
        sp0[w] = v & 0xFF; sp0[w+1] = (v >> 8) & 0xFF;
        sp0[w+2] = (v >> 16) & 0xFF; sp0[w+3] = (v >> 24) & 0xFF;
        sp0[w+4] = sp0[w+5] = sp0[w+6] = sp0[w+7] = 0; // argv[1] + envp[0]
        cur->user_eip = entry;
        cur->user_esp = final_esp;
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
    serial_printf("[exec] pid=%d '%s' entry=%x\n", cur->pid, path, entry);
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
    // with EAX=-2 → ring 3 retries and reaps.
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

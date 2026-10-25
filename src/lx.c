// The Linux subsystem core: tasks, the scheduler hook, user memory (VMAs and demand paging).
#include "lx.h"
#include "cpufreq.h"
#include "fpu.h"
#include "gdt.h"
#include "lapic.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"
#include "string.h"
#include "window.h"

extern uint32_t lx_enter(void);                 // boot/lm.asm
extern void lx_syscall_entry(void);
extern uint32_t lx_ktop;                        // TSS.rsp0 / the registers of the task to run
extern uint64_t lx_fault_cr2;
extern uint32_t tick_count;

#define LX_ENTER_SYSCALL 1
#define LX_ENTER_IRQ 2
#define LX_ENTER_FAULT 3
#define LX_ENTER_INT80 4

static struct lx_task *tasks[LX_MAX_TASKS];
static struct lx_task *cur;
static int next_pid = 100;                      // Linux pids start well above the old system's
static int lx_ready;
static int rr_next;

struct lx_task *lx_current(void) { return cur; }

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int64_t lx_now_ms(void) {
    static uint64_t base;
    uint32_t mhz = cpufreq_tsc_mhz();
    if (!mhz) return (int64_t)tick_count * 10;
    uint64_t t = rdtsc64();
    if (!base) base = t;
    return (int64_t)((t - base) / ((uint64_t)mhz * 1000));
}

// ---- user memory ------------------------------------------------------------------------------------------------

static uint32_t page_alloc_zero(void) {
    uint32_t ph = pmm_alloc_page();
    if (!ph) return 0;
    memset((void*)P2V_U32(ph), 0, 4096);
    return ph;
}

struct lx_vma *lx_vma_find(struct lx_task *t, uint32_t addr) {
    for (int i = 0; i < t->mm->nvma; i++)
        if (addr >= t->mm->vma[i].start && addr < t->mm->vma[i].end) return &t->mm->vma[i];
    return 0;
}

static int vma_insert(struct lx_task *t, const struct lx_vma *v) {
    if (t->mm->nvma >= LX_MAX_VMAS) return -LX_ENOMEM;
    int i = t->mm->nvma++;
    while (i > 0 && t->mm->vma[i - 1].start > v->start) { t->mm->vma[i] = t->mm->vma[i - 1]; i--; }
    t->mm->vma[i] = *v;
    return 0;
}

static void vma_remove_at(struct lx_task *t, int i) {
    if (t->mm->vma[i].node) lxfs_unref(t->mm->vma[i].node);
    for (; i + 1 < t->mm->nvma; i++) t->mm->vma[i] = t->mm->vma[i + 1];
    t->mm->nvma--;
}

// Does [start, end) overlap an existing mapping?
static int vma_overlaps(struct lx_task *t, uint32_t start, uint32_t end) {
    for (int i = 0; i < t->mm->nvma; i++)
        if (start < t->mm->vma[i].end && end > t->mm->vma[i].start) return 1;
    return 0;
}

uint32_t lx_find_free(struct lx_task *t, uint32_t len) {
    len = (len + 0xFFF) & ~0xFFFu;
    uint32_t top = t->mm->mmap_hint ? t->mm->mmap_hint : LX_MMAP_TOP;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t end = pass == 0 ? top : LX_MMAP_TOP;
        // first fit going down from `end`
        uint32_t cand = end;
        for (;;) {
            if (cand < LX_USER_BASE + len) break;
            uint32_t s = cand - len;
            int hit = -1;
            for (int i = 0; i < t->mm->nvma; i++)
                if (s < t->mm->vma[i].end && cand > t->mm->vma[i].start) hit = i;
            if (hit < 0) return s;
            cand = t->mm->vma[hit].start;
            if (cand < len) break;
        }
        if (t->mm->mmap_hint == 0) break;
    }
    return 0;
}

// Free the pages of [start, end) (those that were touched).
static void free_pages(struct lx_task *t, uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; a += 0x1000) {
        uint32_t ph = aspace_unmap(t->mm->cr3, a);
        struct lx_vma *fv = lx_vma_find(t, a);
        if (ph && fv && fv->shared_fb) continue;                // the framebuffer window owns these pages
        if (ph) { pmm_free_page(ph); if (t->mm->rss) t->mm->rss--; }
    }
}

int lx_map_region(struct lx_task *t, uint32_t start, uint32_t len, uint32_t prot, uint32_t flags, struct lx_node *node, uint64_t off) {
    len = (len + 0xFFF) & ~0xFFFu;
    if (!len || start < LX_USER_BASE || start + len > LX_USER_TOP || start + len < start) return -LX_ENOMEM;
    if (vma_overlaps(t, start, start + len)) lx_unmap_region(t, start, len);          // MAP_FIXED replaces
    struct lx_vma v = { start, start + len, prot, flags, node, off, start + len, 0 };
    if (node) lxfs_ref(node);
    int r = vma_insert(t, &v);
    if (r && node) lxfs_unref(node);
    return r;
}

int lx_unmap_region(struct lx_task *t, uint32_t start, uint32_t len) {
    uint32_t end = start + ((len + 0xFFF) & ~0xFFFu);
    for (int i = 0; i < t->mm->nvma; ) {
        struct lx_vma *v = &t->mm->vma[i];
        if (v->end <= start || v->start >= end) { i++; continue; }
        uint32_t cs = v->start > start ? v->start : start, ce = v->end < end ? v->end : end;
        free_pages(t, cs, ce);
        if (cs <= v->start && ce >= v->end) { vma_remove_at(t, i); continue; }       // wholly inside
        if (cs <= v->start) {                                                        // cut the front
            if (v->node) v->off += ce - v->start;
            v->start = ce;
        } else if (ce >= v->end) {                                                   // cut the tail
            v->end = cs;
        } else {                                                                     // a hole in the middle: split
            struct lx_vma tail = *v;
            tail.start = ce;
            if (tail.node) { tail.off += ce - v->start; lxfs_ref(tail.node); }
            v->end = cs;
            if (vma_insert(t, &tail)) return -LX_ENOMEM;
        }
        i++;
    }
    return 0;
}

int lx_protect_region(struct lx_task *t, uint32_t start, uint32_t len, uint32_t prot) {
    uint32_t end = start + ((len + 0xFFF) & ~0xFFFu);
    // every page must be mapped
    for (uint32_t a = start; a < end; a += 0x1000) if (!lx_vma_find(t, a)) return -LX_ENOMEM;
    for (int i = 0; i < t->mm->nvma; i++) {
        struct lx_vma *v = &t->mm->vma[i];
        if (v->end <= start || v->start >= end || v->prot == prot) continue;
        uint32_t cs = v->start > start ? v->start : start, ce = v->end < end ? v->end : end;
        // split off the parts outside [cs, ce)
        if (cs > v->start) {
            struct lx_vma head = *v;
            head.end = cs;
            if (head.node) lxfs_ref(head.node);
            if (v->node) v->off += cs - v->start;
            v->start = cs;
            if (vma_insert(t, &head)) return -LX_ENOMEM;
            i = -1; continue;
        }
        if (ce < v->end) {
            struct lx_vma tail = *v;
            tail.start = ce;
            if (tail.node) { tail.off += ce - v->start; lxfs_ref(tail.node); }
            v->end = ce;
            if (vma_insert(t, &tail)) return -LX_ENOMEM;
        }
        v->prot = prot;
        for (uint32_t a = v->start; a < v->end; a += 0x1000) aspace_set_writable(t->mm->cr3, a, (prot & LX_PROT_WRITE) != 0);
    }
    return 0;
}

static void mm_free_user(struct lx_mm *m);
void lx_free_user(struct lx_task *t) { mm_free_user(t->mm); }

// helpers for devices that keep a mapping in some address space (src/lx_okwin.c)
static void mm_put(struct lx_mm *m);
void lx_mm_hold(struct lx_mm *m) { m->refs++; }
void lx_mm_release(struct lx_mm *m) { mm_put(m); }
void lx_mm_unmap(struct lx_mm *m, uint32_t va, uint32_t len) {
    static struct lx_task stand;                   // lx_unmap_region only looks at ->mm
    stand.mm = m;
    lx_unmap_region(&stand, va, len);
}

// Bring the page at `addr` in. Returns the physical address of the page, or 0 if the access is not allowed.
static uint32_t page_in(struct lx_task *t, uint32_t addr, int write) {
    uint32_t page = addr & ~0xFFFu;
    struct lx_vma *v = lx_vma_find(t, addr);
    if (!v) return 0;
    if (!(v->prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC))) return 0;
    if (write && !(v->prot & LX_PROT_WRITE)) return 0;
    uint64_t *e = aspace_pte(t->mm->cr3, page, 0);
    if (e && (*e & 1)) {                       // present: only a write to a page that was mapped read-only gets here
        if (write && !(*e & 2)) aspace_set_writable(t->mm->cr3, page, 1);
        return (uint32_t)*e & 0xFFFFF000u;
    }
    if (pmm_get_free_pages() < 4096) { serial_puts("[lx] out of memory\n"); return 0; }
    uint32_t ph = page_alloc_zero();
    if (!ph) return 0;
    if (v->node && v->node->type == LXN_REG) {
        uint64_t foff = v->off + (page - v->start);
        if (foff < v->node->size && page < v->fend) {
            uint64_t n = v->node->size - foff;
            if (n > 4096) n = 4096;
            if (v->fend - page < n) n = v->fend - page;
            memcpy((void*)P2V_U32(ph), v->node->data + foff, (uint32_t)n);
        }
        // bytes past the file end inside the segment's last page stay zero; bytes of the page that precede the
        // segment's file part (an unaligned start) are handled by the loader (it maps whole pages from the file)
    }
    if (aspace_map(t->mm->cr3, page, ph, (v->prot & LX_PROT_WRITE) != 0)) { pmm_free_page(ph); return 0; }
    t->mm->rss++;
    return ph;
}

// Physical address of user byte `va` (bringing the page in); 0 if it cannot be accessed.
static uint32_t user_phys(struct lx_task *t, uint64_t va, int write) {
    if (va < LX_USER_BASE || va >= LX_USER_TOP) return 0;
    uint32_t ph = page_in(t, (uint32_t)va, write);
    if (!ph) return 0;
    return ph | ((uint32_t)va & 0xFFF);
}

int lx_user_range(struct lx_task *t, uint64_t addr, uint64_t len, int write) {
    if (!len) return 0;
    if (addr + len < addr) return -LX_EFAULT;
    for (uint64_t a = addr & ~0xFFFull; a < addr + len; a += 0x1000)
        if (!user_phys(t, a < addr ? addr : a, write)) return -LX_EFAULT;
    return 0;
}

int lx_copy_in(struct lx_task *t, void *dst, uint64_t src, uint64_t len) {
    uint8_t *d = (uint8_t*)dst;
    while (len) {
        uint32_t ph = user_phys(t, src, 0);
        if (!ph) return -LX_EFAULT;
        uint32_t n = 0x1000 - (ph & 0xFFF);
        if (n > len) n = (uint32_t)len;
        memcpy(d, (void*)P2V_U32(ph), n);
        d += n; src += n; len -= n;
    }
    return 0;
}

int lx_copy_out(struct lx_task *t, uint64_t dst, const void *src, uint64_t len) {
    const uint8_t *s = (const uint8_t*)src;
    while (len) {
        uint32_t ph = user_phys(t, dst, 1);
        if (!ph) return -LX_EFAULT;
        uint32_t n = 0x1000 - (ph & 0xFFF);
        if (n > len) n = (uint32_t)len;
        memcpy((void*)P2V_U32(ph), s, n);
        s += n; dst += n; len -= n;
    }
    return 0;
}

int lx_copy_str(struct lx_task *t, char *dst, uint64_t src, uint32_t max) {
    for (uint32_t i = 0; i < max; ) {
        uint32_t ph = user_phys(t, src + i, 0);
        if (!ph) return -LX_EFAULT;
        const char *p = (const char*)P2V_U32(ph);
        uint32_t n = 0x1000 - (ph & 0xFFF);
        for (uint32_t k = 0; k < n && i < max; k++, i++) {
            dst[i] = p[k];
            if (!p[k]) return (int)i;
        }
    }
    return -LX_ENAMETOOLONG;
}

// ---- address spaces, files, signal handlers: shared by the threads of a program -----------------------------------

static void mm_free_user(struct lx_mm *m) {
    for (int i = 0; i < m->nvma; i++)                           // detach shared pages first: aspace_free_user would free them
        if (m->vma[i].shared_fb)
            for (uint32_t a = m->vma[i].start; a < m->vma[i].end; a += 0x1000) aspace_unmap(m->cr3, a);
    for (int i = 0; i < m->nvma; i++) if (m->vma[i].node) lxfs_unref(m->vma[i].node);
    m->nvma = 0;
    aspace_free_user(m->cr3);
    m->rss = 0;
}

static struct lx_mm *mm_new(void) {
    struct lx_mm *m = (struct lx_mm*)kcalloc(1, sizeof *m);
    if (!m) return 0;
    m->vma = (struct lx_vma*)kcalloc(LX_MAX_VMAS, sizeof(struct lx_vma));
    m->cr3 = aspace_create();
    if (!m->vma || !m->cr3) {
        if (m->vma) kfree(m->vma);
        if (m->cr3) aspace_destroy(m->cr3);
        kfree(m);
        return 0;
    }
    m->refs = 1;
    return m;
}

static void mm_put(struct lx_mm *m) {
    if (--m->refs > 0) return;
    mm_free_user(m);
    if (m->win) kfree(m->win);
    aspace_destroy(m->cr3);
    kfree(m->vma);
    kfree(m);
}

static int fork_page(uint32_t va, uint32_t src_phys, void *ctx) {
    struct lx_task *child = (struct lx_task*)ctx;
    struct lx_vma *v = lx_vma_find(child, va);
    uint32_t ph = pmm_alloc_page();
    if (!ph) return -1;
    memcpy((void*)P2V_U32(ph), (void*)P2V_U32(src_phys), 4096);
    if (aspace_map(child->mm->cr3, va, ph, v ? (v->prot & LX_PROT_WRITE) != 0 : 1)) { pmm_free_page(ph); return -1; }
    child->mm->rss++;
    return 0;
}

// fork: a copy of the address space (eager)
static struct lx_mm *mm_copy(struct lx_mm *src) {
    struct lx_mm *m = mm_new();
    if (!m) return 0;
    m->brk_start = src->brk_start; m->brk = src->brk; m->mmap_hint = src->mmap_hint;
    for (int i = 0; i < src->nvma; i++) {
        m->vma[i] = src->vma[i];
        if (m->vma[i].node) lxfs_ref(m->vma[i].node);
    }
    m->nvma = src->nvma;
    // the page walk finds VMAs through a task: a stand-in with just the new mm is enough
    struct lx_task *stand = (struct lx_task*)kcalloc(1, sizeof *stand);
    if (!stand) { mm_put(m); return 0; }
    stand->mm = m;
    int r = aspace_walk_user(src->cr3, fork_page, stand);
    kfree(stand);
    if (r) { mm_put(m); return 0; }
    return m;
}

static struct lx_files *files_new(void) {
    struct lx_files *f = (struct lx_files*)kcalloc(1, sizeof *f);
    if (f) f->refs = 1;
    return f;
}

static void files_put(struct lx_files *f) {
    if (--f->refs > 0) return;
    for (int i = 0; i < LX_MAX_FDS; i++) if (f->fds[i]) lx_file_put(f->fds[i]);
    kfree(f);
}

static struct lx_files *files_copy(struct lx_files *src) {
    struct lx_files *f = files_new();
    if (!f) return 0;
    for (int i = 0; i < LX_MAX_FDS; i++) {
        f->fds[i] = src->fds[i];
        f->fdflags[i] = src->fdflags[i];
        if (f->fds[i]) f->fds[i]->refs++;
    }
    return f;
}

static struct lx_sighand *sh_new(void) {
    struct lx_sighand *s = (struct lx_sighand*)kcalloc(1, sizeof *s);
    if (s) s->refs = 1;
    return s;
}

static void sh_put(struct lx_sighand *s) { if (--s->refs <= 0) kfree(s); }

// ---- tasks ------------------------------------------------------------------------------------------------------

void lx_block(struct lx_task *t, int (*fn)(struct lx_task*), uint64_t arg) {
    t->state = LXT_BLOCK;
    t->block_fn = fn;
    t->block_arg = arg;
}

int lx_alive(void) {
    for (int i = 0; i < LX_MAX_TASKS; i++)
        if (tasks[i] && tasks[i]->state != LXT_FREE) return 1;
    return 0;
}

int lx_busy(void) {
    for (int i = 0; i < LX_MAX_TASKS; i++)
        if (tasks[i] && tasks[i]->pid && tasks[i]->state == LXT_RUN) return 1;
    return 0;
}

// Is the process (thread group) with this id still running?
int lx_pid_alive(int pid) {
    for (int i = 0; i < LX_MAX_TASKS; i++)
        if (tasks[i] && tasks[i]->pid && tasks[i]->tgid == pid && (tasks[i]->state == LXT_RUN || tasks[i]->state == LXT_BLOCK)) return 1;
    return 0;
}

// An empty task slot: nothing shared yet, a default FP state.
static struct lx_task *task_slot(void) {
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        if (!tasks[i]) {
            uint32_t ph = pmm_alloc_page();
            if (!ph) return 0;
            struct lx_task *t = (struct lx_task*)P2V_U32(ph);
            memset(t, 0, 4096);
            tasks[i] = t;
        }
        if (tasks[i]->state == LXT_FREE && tasks[i]->pid == 0) {
            struct lx_task *t = tasks[i];
            memset(t, 0, sizeof *t);
            t->pid = next_pid++;
            t->tgid = t->pid;
            t->term_win = -1;
            t->umask = 022;
            t->cwd = lxfs_root();
            t->fx[0] = 0x7F; t->fx[1] = 0x03;               // FCW 0x37F, MXCSR 0x1F80
            t->fx[24] = 0x80; t->fx[25] = 0x1F;
            t->fx[28] = 0xFF; t->fx[29] = 0xFF;
            return t;
        }
    }
    return 0;
}

// A task with its own address space, files and signal handlers.
static struct lx_task *task_alloc(void) {
    struct lx_task *t = task_slot();
    if (!t) return 0;
    t->mm = mm_new();
    t->files = files_new();
    t->sh = sh_new();
    if (!t->mm || !t->files || !t->sh) {
        if (t->mm) mm_put(t->mm);
        if (t->files) files_put(t->files);
        if (t->sh) sh_put(t->sh);
        t->pid = 0;
        return 0;
    }
    return t;
}

int lx_task_by_pid(int pid, struct lx_task **out) {
    for (int i = 0; i < LX_MAX_TASKS; i++)
        if (tasks[i] && tasks[i]->pid == pid && tasks[i]->state != LXT_FREE) { *out = tasks[i]; return 1; }
    return 0;
}

static struct lx_task *group_leader(int tgid) {
    for (int i = 0; i < LX_MAX_TASKS; i++)
        if (tasks[i] && tasks[i]->pid && tasks[i]->pid == tgid && tasks[i]->state != LXT_FREE) return tasks[i];
    return 0;
}

static int group_count(int tgid, struct lx_task *except) {
    int n = 0;
    for (int i = 0; i < LX_MAX_TASKS; i++)
        if (tasks[i] && tasks[i]->pid && tasks[i]->tgid == tgid && tasks[i] != except && tasks[i]->state != LXT_ZOMBIE && tasks[i]->state != LXT_FREE) n++;
    return n;
}

// A thread's slot is free again (its leader may live on as a zombie until waited for)
static void slot_free(struct lx_task *t) {
    t->state = LXT_FREE;
    t->pid = 0;
}

// What a task gives up when it exits: its references on the shared parts.
static void task_drop(struct lx_task *t) {
    if (t->clear_child_tid && t->mm) {                       // CLONE_CHILD_CLEARTID: tell the joiner
        uint32_t zero = 0;
        lx_copy_out(t, t->clear_child_tid, &zero, 4);
        extern void lx_futex_wake_addr(struct lx_mm *mm, uint64_t addr, int n);
        lx_futex_wake_addr(t->mm, t->clear_child_tid, 1);
        t->clear_child_tid = 0;
    }
    if (t->mm) { mm_put(t->mm); t->mm = 0; }
    if (t->files) { files_put(t->files); t->files = 0; }
    if (t->sh) { sh_put(t->sh); t->sh = 0; }
}

// A process (or the last thread of it) is gone: its children are orphaned, the parent hears of it.
static void process_gone(struct lx_task *leader, int status) {
    leader->exit_code = status;
    leader->state = LXT_ZOMBIE;
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        struct lx_task *c = tasks[i];
        if (c && c->pid && c != leader && c->ppid == leader->tgid && c->tgid == c->pid) {
            c->ppid = 1;
            if (c->state == LXT_ZOMBIE) slot_free(c);
        }
    }
    struct lx_task *p = 0;
    for (int i = 0; i < LX_MAX_TASKS; i++)
        if (tasks[i] && tasks[i]->pid && tasks[i]->tgid == leader->ppid && tasks[i]->state != LXT_ZOMBIE && tasks[i]->state != LXT_FREE) { p = tasks[i]; break; }
    if (!p || leader->ppid <= 1) { slot_free(leader); return; }          // nobody waits for it
    extern void lx_signal(struct lx_task *t, int sig);
    lx_signal(p, 17);                                                    // SIGCHLD
}

// exit_group / the death of a whole process
void lx_task_exit(struct lx_task *t, int status) {
    int tgid = t->tgid;
    serial_printf("[lx] pid %d exited, status %d\n", tgid, status);
    { extern void lx_fb_owner_gone(int pid); lx_fb_owner_gone(t->pid); lx_fb_owner_gone(tgid); }
    for (int i = 0; i < LX_MAX_TASKS; i++) {                             // the other threads
        struct lx_task *o = tasks[i];
        if (o && o->pid && o != t && o->tgid == tgid && o->state != LXT_FREE) {
            int is_leader = o->pid == tgid;
            task_drop(o);
            if (is_leader) o->state = LXT_ZOMBIE; else slot_free(o);
        }
    }
    task_drop(t);
    struct lx_task *leader = t->pid == tgid ? t : group_leader(tgid);
    if (t != leader) slot_free(t);
    if (leader) process_gone(leader, status);
}

// exit of one thread: the process lives on while another thread does
void lx_thread_exit(struct lx_task *t, int status) {
    if (group_count(t->tgid, t) == 0) { lx_task_exit(t, status); return; }
    task_drop(t);
    if (t->pid == t->tgid) t->state = LXT_ZOMBIE; else slot_free(t);   // the leader is reported when the group is gone
}

// A free pid slot reaped by wait4
void lx_reap(struct lx_task *t) { slot_free(t); }

static void apply_exit_signal(struct lx_task *c, uint32_t flags) { (void)c; (void)flags; }

// clone(): fork (copies) and threads (shares). flags are the Linux CLONE_* bits.
#define CL_VM 0x100
#define CL_FS 0x200
#define CL_FILES 0x400
#define CL_SIGHAND 0x800
#define CL_VFORK 0x4000
#define CL_PARENT 0x8000
#define CL_THREAD 0x10000
#define CL_SETTLS 0x80000
#define CL_PARENT_SETTID 0x100000
#define CL_CHILD_CLEARTID 0x200000
#define CL_CHILD_SETTID 0x1000000

struct lx_task *lx_clone(struct lx_task *p, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls) {
    if ((flags & CL_THREAD) && !((flags & CL_VM) && (flags & CL_SIGHAND))) return 0;
    struct lx_task *c = task_slot();
    if (!c) return 0;
    c->mm = (flags & CL_VM) ? p->mm : 0;
    if (c->mm) c->mm->refs++; else c->mm = mm_copy(p->mm);
    c->files = (flags & CL_FILES) ? p->files : 0;
    if (c->files) c->files->refs++; else c->files = files_copy(p->files);
    if (flags & CL_SIGHAND) { c->sh = p->sh; c->sh->refs++; }
    else { c->sh = sh_new(); if (c->sh) memcpy(c->sh->act, p->sh->act, sizeof c->sh->act); }
    if (!c->mm || !c->files || !c->sh) {
        if (c->mm) mm_put(c->mm);
        if (c->files) files_put(c->files);
        if (c->sh) sh_put(c->sh);
        c->mm = 0; c->files = 0; c->sh = 0;
        slot_free(c);
        return 0;
    }
    c->regs = p->regs;
    c->regs.rax = 0;
    if (stack) c->regs.rsp = stack;
    memcpy(c->fx, p->fx, 512);
    c->fs_base = (flags & CL_SETTLS) ? tls : p->fs_base;
    c->gs_base = p->gs_base; c->is_win = p->is_win;
    memcpy(c->comm, p->comm, 16);
    memcpy(c->exe, p->exe, sizeof c->exe);
    c->cwd = p->cwd;
    c->term_win = p->term_win;
    c->umask = p->umask;
    c->sig_blocked = p->sig_blocked;
    if (flags & CL_THREAD) { c->tgid = p->tgid; c->ppid = p->ppid; }
    else { c->tgid = c->pid; c->ppid = p->tgid; }
    if (flags & CL_PARENT_SETTID) { int32_t tid = c->pid; lx_copy_out(p, ptid, &tid, 4); }
    if (flags & CL_CHILD_SETTID) { int32_t tid = c->pid; lx_copy_out(c, ctid, &tid, 4); }
    if (flags & CL_CHILD_CLEARTID) c->clear_child_tid = ctid;
    apply_exit_signal(c, (uint32_t)flags);
    c->state = LXT_RUN;
    return c;
}

int lx_spawn(const char *path, const char *const *argv, const char *const *envp, int term_win) {
    struct lx_task *t = task_alloc();
    if (!t) return -LX_ENOMEM;
    t->ppid = 0;
    t->term_win = term_win;
    // standard files: the terminal
    for (int i = 0; i < 3; i++) {
        struct lx_file *f = lx_file_new(LXF_TTY, i == 0 ? LX_O_RDONLY : LX_O_WRONLY, 0);
        if (!f) { lx_task_exit(t, 0); return -LX_ENOMEM; }
        t->files->fds[i] = f;
    }
    int r = lx_exec_load(t, path, argv, envp);
    if (r) { serial_printf("[lx] cannot start %s: error %d", path, r); serial_putchar(10); t->ppid = 0; lx_task_exit(t, 127 << 8); return r; }
    t->state = LXT_RUN;
    serial_printf("[lx] pid %d started: %s\n", t->pid, path);
    return t->pid;
}

int lx_has_child(struct lx_task *p, int pid) {
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        struct lx_task *c = tasks[i];
        if (c && c->pid && c->tgid == c->pid && c->ppid == p->tgid && c != p && (pid < 0 || c->pid == pid)) return 1;
    }
    return 0;
}

struct lx_task *lx_find_zombie_child(struct lx_task *p, int pid) {
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        struct lx_task *c = tasks[i];
        if (c && c->pid && c->tgid == c->pid && c->ppid == p->tgid && c->state == LXT_ZOMBIE && (pid < 0 || c->pid == pid)) return c;
    }
    return 0;
}


// ---- running ----------------------------------------------------------------------------------------------------

extern int lx_sig_check(struct lx_task *t);
extern int lx_sig_wake_blocked(struct lx_task *t);
extern void lx_sig_timers(void);
extern void lx_signal_group(int tgid, int sig);

// A hardware fault the kernel cannot repair: the matching signal, caught or fatal.
static void fault_signal(struct lx_task *t, uint64_t vec) {
    int sig = 11;                                   // SIGSEGV
    if (vec == 6) sig = 4;                          // #UD  SIGILL
    else if (vec == 0 || vec == 16 || vec == 19) sig = 8;   // #DE, x87, SSE  SIGFPE
    else if (vec == 17) sig = 7;                    // #AC  SIGBUS
    else if (vec == 3 || vec == 1) sig = 5;         // SIGTRAP
    if (vec == 14 && t->is_win && lx_pe_missing_import(t, t->regs.rip)) { lx_task_exit(t, 3 << 8); return; }
    uint64_t h = t->sh->act[sig].handler;
    if (h <= 1 || (t->sig_blocked & (1ull << (sig - 1)))) {
        serial_printf("[lx] pid %d (%s): fault vec=%d err=%x rip=%x addr=%x -> signal %d\n", t->tgid, t->comm, (int)vec,
                      (uint32_t)t->regs.err, (uint32_t)t->regs.rip, (uint32_t)lx_fault_cr2, sig);
        lx_task_exit(t, sig);
        return;
    }
    t->sig_pending |= 1ull << (sig - 1);            // delivered by lx_sig_check before the task runs again
}

static inline void load_user_state(struct lx_task *t) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(t->mm->cr3) : "memory");
    wrmsr(0xC0000100, t->fs_base);                  // IA32_FS_BASE
    if (t->is_win) wrmsr(0xC0000101, t->gs_base);   // IA32_GS_BASE: the TEB
}

static inline void load_kernel_state(void) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(paging_kernel_pd_phys()) : "memory");
}

static void run_slice(struct lx_task *t) {
    cur = t;
    uint32_t ktop = (uint32_t)&t->regs + sizeof t->regs;
    lx_ktop = ktop;
    tss_set_kernel_stack(ktop);
    fpu_restore(t->fx);
    load_user_state(t);
    for (int budget = 0; budget < 256; budget++) {
        if (t->sig_pending & ~t->sig_blocked) {
            load_kernel_state();
            if (lx_sig_check(t)) break;               // the signal killed it
            load_user_state(t);
        }
        uint32_t reason = lx_enter();
        if (reason == LX_ENTER_SYSCALL) {
            t->sysnr = t->regs.rax;
            load_kernel_state();
            lxfs_case_insensitive = t->is_win;
            int64_t r = lx_syscall_dispatch(t);
            lxfs_case_insensitive = 0;
            if (t->state == LXT_FREE || t->state == LXT_ZOMBIE) break;
            if (t->state == LXT_BLOCK) {
                if (t->block_complete) { t->block_complete = 0; t->blocked_rewound = 0; t->regs.rax = (uint64_t)r; }       // done; only the waiting remains
                else { t->regs.rip -= 2; t->regs.rax = t->sysnr; t->blocked_rewound = 1; }                                 // repeat the call once it can proceed
                break;
            }
            if (t->skip_ret) t->skip_ret = 0; else t->regs.rax = (uint64_t)r;
            load_user_state(t);
            continue;
        }
        if (reason == LX_ENTER_IRQ) break;                // preempted: the interrupt was served on the way out
        if (reason == LX_ENTER_INT80) { t->regs.rax = (uint64_t)-LX_ENOSYS; continue; }
        // a fault: page it in if it is a legal access, else a signal
        load_kernel_state();
        if (t->regs.vec == 14) {
            int write = (int)(t->regs.err & 2);
            if (user_phys(t, lx_fault_cr2, write)) { load_user_state(t); continue; }
        }
        fault_signal(t, t->regs.vec);
        if (t->state == LXT_FREE || t->state == LXT_ZOMBIE) break;
        load_user_state(t);
    }
    fpu_save(t->fx);
    load_kernel_state();
    cur = 0;
}

int lx_poll(void) {
    if (!lx_ready) return 0;
    int any = 0;
    lx_sig_timers();
    for (int n = 0; n < LX_MAX_TASKS; n++) {
        int i = (rr_next + n) % LX_MAX_TASKS;
        struct lx_task *t = tasks[i];
        if (!t || !t->pid) continue;
        if (t->state == LXT_BLOCK) {
            if (!lx_sig_wake_blocked(t)) {
                if (t->block_fn && !t->block_fn(t)) { any = 1; continue; }
                t->state = LXT_RUN;
            }
        }
        if (t->state == LXT_RUN) {
            run_slice(t);
            any = 1;
        } else if (t->state == LXT_ZOMBIE) any = 1;
    }
    rr_next = (rr_next + 1) % LX_MAX_TASKS;
    return any;
}

static void mk_node(const char *path, int type, int mode, uint32_t dev) {
    struct lx_node *dir; char name[LX_NAME_MAX + 1]; int err = 0;
    if (lxfs_resolve_parent(0, path, &dir, name)) return;
    struct lx_node *n = lxfs_create(dir, name, type, mode, &err);
    if (n && type == LXN_CHR) n->dev = dev;
}

static void mk_text(const char *path, const char *text) {
    struct lx_node *dir; char name[LX_NAME_MAX + 1]; int err = 0;
    if (lxfs_resolve_parent(0, path, &dir, name)) return;
    struct lx_node *n = lxfs_create(dir, name, LXN_REG, 0644, &err);
    if (n) lxfs_write(n, 0, text, strlen(text));
}

void lx_init(void) {
    lxfs_init();
    const uint8_t *mod; uint32_t mlen;
    if ((mod = memory_boot_module(0, &mlen, 0)) != 0) lxfs_load_tar(mod, mlen);
    static const char *const dirs[] = { "/bin", "/sbin", "/usr", "/usr/bin", "/usr/sbin", "/usr/lib", "/lib", "/lib64", "/etc", "/dev", "/dev/input", "/tmp", "/proc", "/sys", "/home", "/root", "/var", "/var/tmp", "/run", "/mnt", 0 };
    for (int i = 0; dirs[i]; i++) mk_node(dirs[i], LXN_DIR, 0755, 0);
    mk_node("/dev/null", LXN_CHR, 0666, LXDEV_NULL);
    mk_node("/dev/zero", LXN_CHR, 0666, LXDEV_ZERO);
    mk_node("/dev/full", LXN_CHR, 0666, LXDEV_FULL);
    mk_node("/dev/random", LXN_CHR, 0666, LXDEV_RANDOM);
    mk_node("/dev/urandom", LXN_CHR, 0666, LXDEV_URANDOM);
    mk_node("/dev/tty", LXN_CHR, 0666, LXDEV_TTY);
    mk_node("/dev/console", LXN_CHR, 0600, LXDEV_CONSOLE);
    mk_node("/dev/fb0", LXN_CHR, 0666, LXDEV_FB0);
    mk_node("/dev/input/event0", LXN_CHR, 0666, LXDEV_EVENT0);
    mk_node("/dev/okwin", LXN_CHR, 0666, LXDEV_OKWIN);
    mk_text("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n");
    mk_text("/etc/group", "root:x:0:\n");
    mk_text("/etc/hostname", "kanarchy\n");
    mk_text("/etc/os-release", "NAME=KAnarchy\nID=kanarchy\nPRETTY_NAME=\"KAnarchy OS\"\n");
    // SYSCALL/SYSRET: EFER.SCE, kernel CS/SS = 0x30/0x38, user bases 0x18 (64-bit CS = 0x28, SS = 0x20)
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);
    wrmsr(0xC0000081, ((uint64_t)0x18 << 48) | ((uint64_t)0x30 << 32));      // STAR
    wrmsr(0xC0000082, (uint32_t)lx_syscall_entry);                            // LSTAR
    wrmsr(0xC0000084, 0x44700);                                               // SFMASK: IF TF DF NT AC cleared on entry
    lx_ready = 1;
    serial_puts("[lx] Linux subsystem ready\n");
}

void lx_report(char *out, int cap) {
    int n = 0;
    for (int i = 0; i < LX_MAX_TASKS && n < cap - 80; i++) {
        struct lx_task *t = tasks[i];
        if (!t || !t->pid) continue;
        const char *st = t->state == LXT_RUN ? "run" : t->state == LXT_BLOCK ? "wait" : t->state == LXT_ZOMBIE ? "zombie" : "?";
        char line[96];
        int l = 0;
        const char *s = "pid ";
        while (*s) line[l++] = *s++;
        int pid = t->pid; char d[12]; int dn = 0;
        while (pid) { d[dn++] = (char)('0' + pid % 10); pid /= 10; }
        while (dn) line[l++] = d[--dn];
        line[l++] = ' ';
        for (const char *c = t->comm; *c && l < 40; c++) line[l++] = *c;
        line[l++] = ' ';
        for (; *st; st++) line[l++] = *st;
        line[l++] = '\n';
        for (int k = 0; k < l && n < cap - 1; k++) out[n++] = line[k];
    }
    out[n] = 0;
}

// ---- lookups used by the system calls ---------------------------------------------------------------------------

uint64_t lx_now_ns(void) {
    uint32_t mhz = cpufreq_tsc_mhz();
    if (!mhz) return (uint64_t)tick_count * 10000000ull;
    static uint64_t base;
    uint64_t t = rdtsc64();
    if (!base) base = t;
    return (t - base) * 1000 / mhz;
}

// The task that owns terminal window `win` (its standard files), for key routing
int lx_tty_owner(int win, struct lx_task **out) {
    struct lx_task *best = 0;
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        struct lx_task *c = tasks[i];
        if (c && c->pid && c->term_win == win && (c->state == LXT_RUN || c->state == LXT_BLOCK)) best = c;
    }
    if (!best) return 0;
    *out = best;
    return 1;
}

void lx_signal_fg(int win, int sig) {
    for (int i = 0; i < LX_MAX_TASKS; i++) {
        struct lx_task *c = tasks[i];
        if (c && c->pid && c->tgid == c->pid && c->term_win == win && (c->state == LXT_RUN || c->state == LXT_BLOCK)) lx_signal_group(c->tgid, sig);
    }
}

struct lx_task *lx_task_at(int i) { return (i >= 0 && i < LX_MAX_TASKS) ? tasks[i] : 0; }

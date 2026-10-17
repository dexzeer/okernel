// gc.c — ojs heap: size-class pages + large objects, non-moving
// mark-sweep with a conservative C-stack scan.
//
// Small objects (<= 4096 bytes) live in 16 KB pages of one size class
// each; pages are carved from 1 MB chunks so they can be page aligned.
// A bitmap over the 32-bit address space (one bit per page) says which
// page addresses are ours, so the conservative scan can map any word to
// "the object containing it" in O(1). Large objects are sys-malloc'd and
// kept in an address-sorted table (binary search).
//
// Marking: roots (intrinsics, registered slots, VM stack, jobs, modules),
// then every word of the C stack between the current stack pointer and
// the entry's stack top (callee-saved registers are spilled first).
// Objects are traced precisely by type. Weak structures (atom table,
// WeakMap ephemerons, WeakRef targets, FinalizationRegistry cells) are
// processed after marking, then unmarked objects are swept onto per-class
// free lists (objects with GCF_FINAL get obj_finalize first).

#include "ojs_int.h"
#include "gc_int.h"
#if !defined(KERNEL) || !KERNEL
#include <stdio.h>
#include <stdlib.h>
#endif

#if UINTPTR_MAX != 0xFFFFFFFFu
#error "ojs's conservative collector assumes 32-bit pointers"
#endif

#define PAGE_SHIFT 14   // 16 KB pages: a realm starts with one page per size class in use
#define PAGE_SIZE  (1u << PAGE_SHIFT)
#define CHUNK_PAGES 64   // 1 MB chunks
#define LARGE_MIN  4097u

static const uint16_t SIZE_CLASSES[] = {
    16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096
};
#define NCLASSES ((int)(sizeof SIZE_CLASSES / sizeof SIZE_CLASSES[0]))

struct page {
    uint32_t objsize;
    uint32_t nobj;
    uint32_t nfree;
    uint16_t cls;
    uint16_t swept;
    struct page* next;          // all pages of this class
    struct page* anext;         // pages of this class with free objects (the allocation stack)
    struct gch* free;           // free list (linked through the object body)
    uint8_t* data;              // first object
};

struct chunk { struct chunk* next; void* raw; uintptr_t base; int nspare; };

struct large {
    uint8_t* p;                 // object start (the struct gch)
    size_t size;
    void* raw;
};

// The embedder may offer worker threads for marking (ojs_set_parallel); without it, or with one CPU, the collector marks alone.
static int (*par_ncpu_fn)(void);
void (*ojs_gc_report)(int parallel, double total_ms, double mark_ms, size_t live, size_t heap);   // optional: called after every collection
void (*ojs_par_run)(void (*fn)(void* ctx, int item, int is_main), void* ctx, int n);
void ojs_set_parallel(int (*ncpu)(void), void (*run)(void (*fn)(void*, int, int), void*, int)) { par_ncpu_fn = ncpu; ojs_par_run = run; }
static int ojs_par_ncpu(void) { return par_ncpu_fn ? par_ncpu_fn() : 1; }
static size_t par_min_heap = 24u << 20;   // below this a collection is too short to be worth waking the other CPUs
void ojs_set_parallel_min(size_t bytes) { par_min_heap = bytes; }
#define PAR_MIN_HEAP par_min_heap

// A mark stack. The collector has one for the calling thread; parallel marking gives every worker its own (see par_worker).
struct marker {
    void** stack;
    int sp, cap;
    int overflow;               // a push failed (out of memory): the heap is rescanned for marked objects after the phase
    int par;                    // marking in parallel: mark bits are set atomically
    unsigned traced;            // objects traced (statistics)
    uint8_t pad[104];           // 128 bytes in all: workers update their own sp on every object, neighbours must not share its cache line
};


struct gc_heap {
    uint32_t owned[(1u << (32 - PAGE_SHIFT)) / 32];   // page bitmap over 4 GB
    struct page* pages[NCLASSES];        // class -> page list
    struct page* avail[NCLASSES];        // class -> stack of pages with free objects (linked by anext)
    struct chunk* chunks;
    struct page* spare;                  // carved, unused pages
    struct large* large;
    int nlarge, caplarge;
    struct marker m0;                    // the calling thread's mark stack
    int mark_overflow;
    size_t live_after_gc;
    size_t live_by_type[GT_COUNT];
    uint8_t size_to_class[(4096 / 8) + 1];
};

// ---------------------------------------------------------------- pages

static inline int page_owned(struct gc_heap* H, uintptr_t addr) {
    uint32_t pi = (uint32_t)(addr >> PAGE_SHIFT);
    return (H->owned[pi >> 5] >> (pi & 31)) & 1;
}
static inline void page_set_owned(struct gc_heap* H, uintptr_t addr, int on) {
    uint32_t pi = (uint32_t)(addr >> PAGE_SHIFT);
    if (on) H->owned[pi >> 5] |= 1u << (pi & 31);
    else H->owned[pi >> 5] &= ~(1u << (pi & 31));
}

#define CHUNK_BYTES ((size_t)CHUNK_PAGES * PAGE_SIZE + PAGE_SIZE)

// the realm's memory limit applies to what is taken from the system: free slots
// and spare pages are always reusable
static int over_limit(ojs* J, size_t bytes) {
    if (J->mem_limit && J->heap_bytes + J->ext_bytes + bytes > J->mem_limit) return 1;
    return J->mem_guard && !J->mem_guard(bytes, J->mem_guard_ud);
}

static struct page* page_new(ojs* J, int cls) {
    struct gc_heap* H = J->heap;
    if (!H->spare) {
        size_t bytes = CHUNK_BYTES;
        if (over_limit(J, bytes)) return 0;
        void* raw = ojs_sys_malloc(bytes);
        if (!raw) return 0;
        struct chunk* c = (struct chunk*)ojs_sys_malloc(sizeof *c);
        if (!c) { ojs_sys_free(raw); return 0; }
        c->raw = raw;
        c->next = H->chunks;
        H->chunks = c;
        uintptr_t a = ((uintptr_t)raw + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
        c->base = a;
        for (int i = 0; i < CHUNK_PAGES; i++) {
            struct page* p = (struct page*)(a + (uintptr_t)i * PAGE_SIZE);
            p->next = H->spare;
            H->spare = p;
        }
        J->heap_bytes += bytes;
    }
    struct page* p = H->spare;
    H->spare = p->next;
    uint32_t os = SIZE_CLASSES[cls];
    uintptr_t data = ((uintptr_t)p + sizeof(struct page) + 15) & ~(uintptr_t)15;
    p->objsize = os;
    p->data = (uint8_t*)data;
    p->nobj = (uint32_t)(((uintptr_t)p + PAGE_SIZE - data) / os);
    p->nfree = p->nobj;
    p->cls = (uint16_t)cls;
    p->swept = 1;
    p->free = 0;
    // free list in address order (low addresses first: better locality)
    for (int i = (int)p->nobj - 1; i >= 0; i--) {
        struct gch* g = (struct gch*)(p->data + (size_t)i * os);
        g->type = GT_FREE;
        g->gcflags = 0;
        *(struct gch**)(g + 1) = p->free;
        p->free = g;
    }
    p->next = H->pages[cls];
    H->pages[cls] = p;
    p->anext = H->avail[cls];
    H->avail[cls] = p;
    page_set_owned(H, (uintptr_t)p, 1);
    return p;
}

// O(1): the class keeps a stack of the pages that still have free objects (rebuilt by the sweep). Scanning
// the page list for one made every allocation O(pages) once the older pages were full.
static void* alloc_small(ojs* J, int cls) {
    struct gc_heap* H = J->heap;
    struct page* p = H->avail[cls];
    if (!p) {
        p = page_new(J, cls);
        if (!p) return 0;
    }
    struct gch* g = p->free;
    p->free = *(struct gch**)(g + 1);
    p->nfree--;
    if (!p->free) H->avail[cls] = p->anext;      // exhausted: pop
    return g;
}

// ---------------------------------------------------------------- large objects

static int large_find(struct gc_heap* H, uintptr_t addr) {   // index of block containing addr, -1
    int lo = 0, hi = H->nlarge - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        struct large* L = &H->large[m];
        if (addr < (uintptr_t)L->p) hi = m - 1;
        else if (addr > (uintptr_t)L->p + L->size) lo = m + 1;   // one-past-end counts as inside
        else return m;
    }
    return -1;
}

static void* alloc_large(ojs* J, size_t size) {
    struct gc_heap* H = J->heap;
    if (H->nlarge >= H->caplarge) {
        int nc = H->caplarge ? H->caplarge * 2 : 64;
        struct large* t = (struct large*)ojs_sys_realloc(H->large, (size_t)nc * sizeof *t);
        if (!t) return 0;
        H->large = t;
        H->caplarge = nc;
    }
    if (over_limit(J, size + 16)) return 0;
    void* raw = ojs_sys_malloc(size + 16);
    if (!raw) return 0;
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    // insert sorted
    int lo = 0, hi = H->nlarge;
    while (lo < hi) { int m = (lo + hi) >> 1; if ((uintptr_t)H->large[m].p < (uintptr_t)p) lo = m + 1; else hi = m; }
    memmove(&H->large[lo + 1], &H->large[lo], (size_t)(H->nlarge - lo) * sizeof(struct large));
    H->large[lo].p = p;
    H->large[lo].size = size;
    H->large[lo].raw = raw;
    H->nlarge++;
    J->heap_bytes += size + 16;
    return p;
}

// ---------------------------------------------------------------- allocation

void gc_init(ojs* J) {
    struct gc_heap* H = (struct gc_heap*)ojs_sys_malloc(sizeof *H);
    if (!H) return;
    memset(H, 0, sizeof *H);
    int c = 0;
    for (int s = 0; s <= 4096 / 8; s++) {
        while (SIZE_CLASSES[c] < s * 8) c++;
        H->size_to_class[s] = (uint8_t)c;
    }
    J->heap = H;
    J->gc_threshold = 8u << 20;
    J->mk = &H->m0;
}

size_t gc_size(const void* p) {
    // only used for objects we allocated: small objects know their page
    uintptr_t a = (uintptr_t)p;
    struct page* pg = (struct page*)(a & ~(uintptr_t)(PAGE_SIZE - 1));
    return pg->objsize;   // callers never ask this of large objects (see gc_size_of)
}

size_t gc_size_of(ojs* J, const void* p) {
    struct gc_heap* H = J->heap;
    if (page_owned(H, (uintptr_t)p)) return gc_size(p);
    int i = large_find(H, (uintptr_t)p);
    return i >= 0 ? H->large[i].size : 0;
}

// ArrayBuffer storage lives outside the GC heap but counts against the limit,
// and drives collections like any allocation (dead buffers are only freed by one)
void* gc_ext_alloc(ojs* J, size_t n) {
    if (!n) n = 1;
    J->bytes_since_gc += n;
    for (int tries = 0;; tries++) {
        if (!over_limit(J, n)) {
            void* p = ojs_sys_malloc(n);
            if (p) { J->ext_bytes += n; memset(p, 0, n); return p; }
        }
        if (tries || J->gc_disabled || J->in_gc || !J->stack_top) return 0;
        gc_collect(J);
    }
}

void gc_ext_free(ojs* J, void* p, size_t n) {
    if (!p) return;
    if (!n) n = 1;
    ojs_sys_free(p);
    J->ext_bytes = J->ext_bytes >= n ? J->ext_bytes - n : 0;
}

void* gc_alloc(ojs* J, int type, size_t size) {
    struct gc_heap* H = J->heap;
    if (size < sizeof(struct gch) + sizeof(void*)) size = sizeof(struct gch) + sizeof(void*);
    if (J->bytes_since_gc + size > J->gc_threshold && !J->gc_disabled && !J->in_gc && J->stack_top)
        gc_collect(J);
    else if (J->gc_stress && ++J->gc_stress_n >= J->gc_stress && !J->gc_disabled && !J->in_gc && J->stack_top) {
        J->gc_stress_n = 0;
        gc_collect(J);
    }
    void* p;
    for (int tries = 0;; tries++) {
        if (size < LARGE_MIN) p = alloc_small(J, H->size_to_class[(size + 7) >> 3]);
        else p = alloc_large(J, size);
        // at the limit (or out of system memory): collect once and retry
        if (p || tries || J->gc_disabled || J->in_gc || !J->stack_top) break;
        gc_collect(J);
    }
    if (!p) { throw_oom(J); return 0; }
    size_t real = size < LARGE_MIN ? SIZE_CLASSES[H->size_to_class[(size + 7) >> 3]] : size;
    memset(p, 0, real);
    ((struct gch*)p)->type = (uint8_t)type;
    J->bytes_since_gc += real;
    J->total_alloc += real;
    return p;
}

// ---------------------------------------------------------------- marking

static void mpush(struct marker* mk, void* p) {
    if (mk->sp >= mk->cap) {
        int nc = mk->cap ? mk->cap * 2 : 4096;
        void** t = (void**)ojs_sys_realloc(mk->stack, (size_t)nc * sizeof(void*));
        if (!t) { mk->overflow = 1; return; }   // rescan later
        mk->stack = t;
        mk->cap = nc;
    }
    mk->stack[mk->sp++] = p;
}

void gc_mark_ptr(ojs* J, void* p) {
    if (!p) return;
    struct gch* g = (struct gch*)p;
    if (g->gcflags & GCF_MARK) return;
    struct marker* mk = J->mk;
    g->gcflags |= GCF_MARK;      // (parallel marking: two workers may both see it unmarked and both trace it - harmless, marking is idempotent, and no other flag changes during a collection)
    mpush(mk, p);
}

void gc_mark_value(ojs* J, jv v) {
    uint32_t t = JV_TAG(v);
    if (t >= TAG_OBJ) gc_mark_ptr(J, JV_PTR(v));
}

void gc_mark_pkey(ojs* J, pkey k) {
    if (!PK_IS_INDEX(k) && k) gc_mark_ptr(J, (void*)(uintptr_t)k);
}

static void mark_values(ojs* J, const jv* v, size_t n) {
    for (size_t i = 0; i < n; i++) gc_mark_value(J, v[i]);
}

// the object containing address w (conservative), or NULL
static struct gch* find_object(ojs* J, uintptr_t w) {
    struct gc_heap* H = J->heap;
    if (page_owned(H, w)) {
        struct page* pg = (struct page*)(w & ~(uintptr_t)(PAGE_SIZE - 1));
        if (w < (uintptr_t)pg->data) return 0;
        uint32_t idx = (uint32_t)((w - (uintptr_t)pg->data) / pg->objsize);
        if (idx >= pg->nobj) return 0;
        struct gch* g = (struct gch*)(pg->data + (size_t)idx * pg->objsize);
        return g->type == GT_FREE ? 0 : g;
    }
    if (H->nlarge && w >= (uintptr_t)H->large[0].p) {
        int i = large_find(H, w);
        if (i >= 0) return (struct gch*)H->large[i].p;
    }
    return 0;
}

void gc_scan_conservative(ojs* J, const void* lo, const void* hi);
static void scan_conservative(ojs* J, const void* lo, const void* hi) { gc_scan_conservative(J, lo, hi); }

// reads raw stack words: AddressSanitizer's redzones between locals would be reported otherwise
__attribute__((no_sanitize_address)) void gc_scan_conservative(ojs* J, const void* lo, const void* hi) {
    uintptr_t a = ((uintptr_t)lo + 3) & ~(uintptr_t)3;
    for (; a + 4 <= (uintptr_t)hi; a += 4) {
        uintptr_t w = *(const uint32_t*)a;
        if (w < 4096) continue;
        struct gch* g = find_object(J, w);
        if (!g && w) g = find_object(J, w - 1);     // one-past-the-end pointers
        if (g) gc_mark_ptr(J, g);
    }
}

static void trace(ojs* J, struct gch* g) {
    switch (g->type) {
    case GT_STR: case GT_BIGINT: case GT_BYTES: break;
    case GT_ROPE: {
        struct rope* r = (struct rope*)g;
        gc_mark_value(J, r->left);
        gc_mark_value(J, r->right);
        gc_mark_ptr(J, r->flat);
        break;
    }
    case GT_SYM: gc_mark_value(J, ((struct sym*)g)->desc); break;
    case GT_SHAPE: shape_trace(J, (struct shape*)g); break;
    case GT_OBJ: obj_trace(J, (struct obj*)g); break;
    case GT_FTEMPL: ftempl_trace(J, g); break;
    case GT_UPVAL: upval_trace(J, g); break;
    case GT_VALARR: {
        struct valarr* a = (struct valarr*)g;
        mark_values(J, a->v, a->cap);
        break;
    }
    case GT_PTRARR: {
        struct ptrarr* a = (struct ptrarr*)g;
        for (uint32_t i = 0; i < a->n; i++) gc_mark_ptr(J, a->p[i]);
        break;
    }
    case GT_MAPTAB: maptab_trace(J, g); break;
    case GT_MODULE: module_trace(J, g); break;
    case GT_FRAME: frame_trace(J, g); break;
    default: break;
    }
}

static void drain(ojs* J) {
    struct gc_heap* H = J->heap;
    struct marker* mk = J->mk;
    uint32_t traced = 0;
    for (;;) {
        while (mk->sp > 0) {
            trace(J, (struct gch*)mk->stack[--mk->sp]);
            // marking a heap of 100+ MB takes 100-300 ms: let the host keep its sound card and video going (its answer is ignored:
            // a script past its deadline is stopped by the next check of the interpreter)
            if (!(++traced & 8191) && J->interrupt) (void)J->interrupt(J, J->interrupt_op);
        }
        if (mk->overflow) { mk->overflow = 0; H->mark_overflow = 1; }
        if (!H->mark_overflow) break;
        // the mark stack overflowed: rescan the heap for marked objects
        // and retrace them (their children may have been dropped)
        H->mark_overflow = 0;
        for (int c = 0; c < NCLASSES; c++)
            for (struct page* p = H->pages[c]; p; p = p->next)
                for (uint32_t i = 0; i < p->nobj; i++) {
                    struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                    if (g->type != GT_FREE && (g->gcflags & GCF_MARK)) trace(J, g);
                }
        for (int i = 0; i < H->nlarge; i++) {
            struct gch* g = (struct gch*)H->large[i].p;
            if (g->gcflags & GCF_MARK) trace(J, g);
        }
    }
}


// ---------------------------------------------------------------- parallel marking
//
// Marking is 80% of a collection and bound by memory latency, so it scales with the number of CPUs that chase pointers at the same time.
// The embedder supplies the executor (ojs_set_parallel): n workers run concurrently, one of them on the calling thread. Each worker has a
// private mark stack and marks through a private copy of the runtime struct (only J->mk differs), so gc_mark_ptr needs no thread-local
// storage. Mark bits are set with an atomic OR (only for objects that looked unmarked). Idle workers take chunks of pointers from a shared
// pool; busy workers put chunks there when somebody is idle. The trace functions are pure (they only read objects and call gc_mark_*), the
// roots, the conservative stack scan, ephemerons, finalization registries and the sweep stay on the calling thread.

#define PAR_CHUNK 256
#define PAR_MAXW 8

struct gc_par {
    volatile int lock;
    void** buf;                     // pool storage: cap chunks of PAR_CHUNK pointers
    int* nbuf;                      // entries in each chunk
    int np, cap;
    volatile int nstarted, nidle, done, overflow;
    ojs* J;
    struct marker mk[PAR_MAXW];
};

static inline void par_lock(volatile int* l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) while (*l) __asm__ volatile("pause");
}
static inline void par_unlock(volatile int* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

// room for n more entries on the marker's stack
static int mk_reserve(struct marker* mk, int n) {
    if (mk->sp + n <= mk->cap) return 1;
    int nc = mk->cap ? mk->cap : 4096;
    while (nc < mk->sp + n) nc *= 2;
    void** t = (void**)ojs_sys_realloc(mk->stack, (size_t)nc * sizeof(void*));
    if (!t) return 0;
    mk->stack = t;
    mk->cap = nc;
    return 1;
}

// give half of a long stack (its oldest entries: the biggest subtrees) to the pool
static void par_share(struct gc_par* P, struct marker* mk) {
    par_lock(&P->lock);
    if (P->np < P->cap && mk->sp > 2 * PAR_CHUNK) {
        void** dst = P->buf + (size_t)P->np * PAR_CHUNK;
        memcpy(dst, mk->stack, PAR_CHUNK * sizeof(void*));
        memmove(mk->stack, mk->stack + PAR_CHUNK, (size_t)(mk->sp - PAR_CHUNK) * sizeof(void*));
        mk->sp -= PAR_CHUNK;
        P->nbuf[P->np++] = PAR_CHUNK;
    }
    par_unlock(&P->lock);
}

// (P->lock held, P->np > 0) move the newest chunk to the marker's stack
static int par_take(struct gc_par* P, struct marker* mk) {
    int n = P->nbuf[P->np - 1];
    if (!mk_reserve(mk, n)) { mk->overflow = 1; P->np--; return 0; }       // dropped: the serial rescan after the phase picks it up
    memcpy(mk->stack + mk->sp, P->buf + (size_t)(P->np - 1) * PAR_CHUNK, (size_t)n * sizeof(void*));
    mk->sp += n;
    P->np--;
    return 1;
}

static void par_worker(void* ctx, int item, int is_main) {
    struct gc_par* P = (struct gc_par*)ctx;
    if (item < 0 || item >= PAR_MAXW) return;
    struct marker* mk = &P->mk[item];
    ojs Jw = *P->J;                                   // this worker's view of the runtime: same everything, its own marker
    Jw.mk = mk;
    mk->par = 1;
    par_lock(&P->lock);
    P->nstarted++;
    par_unlock(&P->lock);
    uint32_t traced = 0;
    for (;;) {
        while (mk->sp > 0) {
            struct gch* g = (struct gch*)mk->stack[--mk->sp];
            trace(&Jw, g);
            mk->traced++;
            if (mk->sp > 2 * PAR_CHUNK && P->np == 0 && P->nidle > 0) par_share(P, mk);   // only when somebody waits and nothing is waiting for them
            // the calling thread keeps the host's pointer, sound card and video going, like the serial marker does
            if (is_main && !(++traced & 8191) && P->J->interrupt) (void)P->J->interrupt(P->J, P->J->interrupt_op);
        }
        par_lock(&P->lock);
        if (P->np > 0) { par_take(P, mk); par_unlock(&P->lock); continue; }
        P->nidle++;
        if (P->nidle == P->nstarted) P->done = 1;      // everybody who ever started is idle and nothing is waiting: finished
        par_unlock(&P->lock);
        int again = 0;
        while (!P->done) {
            if (P->np > 0) {
                par_lock(&P->lock);
                if (P->np > 0) { P->nidle--; par_take(P, mk); again = 1; }
                par_unlock(&P->lock);
                if (again) break;
            }
            __asm__ volatile("pause");
            if (is_main && P->J->interrupt && !(++traced & 4095)) (void)P->J->interrupt(P->J, P->J->interrupt_op);
        }
        if (!again) break;
    }
    if (mk->overflow) P->overflow = 1;
    mk->par = 0;
}

// Drain the main marker's stack with all CPUs. Returns 1 if it ran in parallel.
static int par_drain(ojs* J) {
    struct gc_heap* H = J->heap;
    int nw = ojs_par_ncpu();
    if (nw < 2 || !ojs_par_run || J->mk->sp < 1) return 0;
    if (nw > PAR_MAXW) nw = PAR_MAXW;
    struct marker* m0 = J->mk;
    int chunks = (m0->sp + PAR_CHUNK - 1) / PAR_CHUNK + 64;
    struct gc_par* P = (struct gc_par*)ojs_sys_malloc(sizeof *P);
    if (!P) return 0;
    memset(P, 0, sizeof *P);
    P->buf = (void**)ojs_sys_malloc((size_t)chunks * PAR_CHUNK * sizeof(void*));
    P->nbuf = (int*)ojs_sys_malloc((size_t)chunks * sizeof(int));
    if (!P->buf || !P->nbuf) { ojs_sys_free(P->buf); ojs_sys_free(P->nbuf); ojs_sys_free(P); return 0; }
    P->cap = chunks;
    P->J = J;
    while (m0->sp > 0) {                               // the roots become chunks for whoever starts first
        int n = m0->sp < PAR_CHUNK ? m0->sp : PAR_CHUNK;
        memcpy(P->buf + (size_t)P->np * PAR_CHUNK, m0->stack + (m0->sp - n), (size_t)n * sizeof(void*));
        P->nbuf[P->np++] = n;
        m0->sp -= n;
    }
    ojs_par_run(par_worker, P, nw);
#if !defined(KERNEL) || !KERNEL
    if (getenv("OJS_GCPAR_TRACE")) { fprintf(stderr, "[gc]   workers traced:"); for (int i = 0; i < PAR_MAXW; i++) fprintf(stderr, " %u", P->mk[i].traced); fprintf(stderr, "  started %d\n", P->nstarted); }
#endif
    if (P->overflow) H->mark_overflow = 1;
    for (int i = 0; i < PAR_MAXW; i++) ojs_sys_free(P->mk[i].stack);
    ojs_sys_free(P->buf);
    ojs_sys_free(P->nbuf);
    ojs_sys_free(P);
    return 1;
}


#if !defined(KERNEL) || !KERNEL
// Debug check of the parallel marker (OJS_GCPAR_VERIFY=1): trace every marked object again, serially; any child that is still unmarked was
// missed by the workers. Aborts with a report.
static void par_verify(ojs* J) {
    struct gc_heap* H = J->heap;
    struct marker* mk = &H->m0;
    int missed = 0;
    for (int c = 0; c < NCLASSES; c++)
        for (struct page* p = H->pages[c]; p; p = p->next)
            for (uint32_t i = 0; i < p->nobj; i++) {
                struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                if (g->type != GT_FREE && (g->gcflags & GCF_MARK)) { trace(J, g); missed += mk->sp; while (mk->sp > 0) { struct gch* m = (struct gch*)mk->stack[--mk->sp]; (void)m; } }
            }
    for (int i = 0; i < H->nlarge; i++) {
        struct gch* g = (struct gch*)H->large[i].p;
        if (g->gcflags & GCF_MARK) { trace(J, g); missed += mk->sp; mk->sp = 0; }
    }
    if (missed) { fprintf(stderr, "[gc] PARALLEL MARK MISSED %d objects\n", missed); abort(); }
}
#endif

static void __attribute__((noinline)) mark_stack(ojs* J) {
    // spill callee-saved registers into this frame, then scan from here
    volatile uint32_t regs[4];
    __asm__ volatile("movl %%ebx, 0(%0)\n\tmovl %%esi, 4(%0)\n\tmovl %%edi, 8(%0)\n\tmovl %%ebp, 12(%0)"
                     :: "r"(regs) : "memory");
    void* sp = (void*)regs;
    if (J->stack_top && (uintptr_t)sp < (uintptr_t)J->stack_top) scan_conservative(J, sp, J->stack_top);
}

static void mark_roots(ojs* J) {
    void** ip = (void**)&J->I;
    for (size_t i = 0; i < sizeof(struct intrinsics) / sizeof(void*); i++) gc_mark_ptr(J, ip[i]);
    for (int i = 0; i < WK_COUNT; i++) gc_mark_ptr(J, J->wk[i]);
    atoms_mark_common(J);
    if (J->has_exc) gc_mark_value(J, J->exc);
    gc_mark_value(J, J->sym_registry);
    gc_mark_value(J, J->template_cache);
    gc_mark_value(J, J->global_lex);
    for (struct root_range* r = J->roots; r; r = r->next) mark_values(J, r->base, (size_t)r->n);
    for (int i = 0; i < J->nroot_ptrs; i++) gc_mark_value(J, *J->root_ptrs[i]);
    for (struct job* jb = J->jobs_head; jb; jb = jb->next) {
        gc_mark_value(J, jb->fn);
        mark_values(J, jb->argv, (size_t)jb->argc);
    }
    vm_mark_roots(J);
    module_mark_roots(J);
    mark_stack(J);
}

// ---------------------------------------------------------------- sweep

static size_t sweep(ojs* J) {
    struct gc_heap* H = J->heap;
    size_t live = 0;
    for (int i = 0; i < GT_COUNT; i++) H->live_by_type[i] = 0;
    for (int c = 0; c < NCLASSES; c++) {
        struct page** pp = &H->pages[c];
        struct page** atail = &H->avail[c];
        H->avail[c] = 0;
        while (*pp) {
            struct page* p = *pp;
            p->free = 0;
            p->nfree = 0;
            for (int i = (int)p->nobj - 1; i >= 0; i--) {
                struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                if (g->type != GT_FREE) {
                    if (g->gcflags & (GCF_MARK | GCF_PINNED)) {
                        g->gcflags &= (uint8_t)~GCF_MARK;
                        live += p->objsize;
                        if (g->type < GT_COUNT) H->live_by_type[g->type] += p->objsize;
                        continue;
                    }
                    if (g->gcflags & GCF_FINAL) gc_finalize(J, g);
                    g->type = GT_FREE;
                    g->gcflags = 0;
                }
                *(struct gch**)(g + 1) = p->free;
                p->free = g;
                p->nfree++;
            }
            if (p->nfree == p->nobj) {
                // empty page: back to the spare pool
                *pp = p->next;
                page_set_owned(H, (uintptr_t)p, 0);
                p->next = H->spare;
                H->spare = p;
                continue;
            }
            if (p->free) { *atail = p; atail = &p->anext; p->anext = 0; }
            pp = &p->next;
        }
    }
    int w = 0;
    for (int i = 0; i < H->nlarge; i++) {
        struct large* L = &H->large[i];
        struct gch* g = (struct gch*)L->p;
        if (g->gcflags & (GCF_MARK | GCF_PINNED)) {
            g->gcflags &= (uint8_t)~GCF_MARK;
            live += L->size;
            if (g->type < GT_COUNT) H->live_by_type[g->type] += L->size;
            H->large[w++] = *L;
            continue;
        }
        if (g->gcflags & GCF_FINAL) gc_finalize(J, g);
        J->heap_bytes -= L->size + 16;
        ojs_sys_free(L->raw);
    }
    H->nlarge = w;
    return live;
}

static void release_chunks(ojs* J) {
    struct gc_heap* H = J->heap;
    int nchunks = 0;
    for (struct chunk* c = H->chunks; c; c = c->next) { c->nspare = 0; nchunks++; }
    if (nchunks <= 1) return;
    for (struct page* p = H->spare; p; p = p->next)
        for (struct chunk* c = H->chunks; c; c = c->next)
            if ((uintptr_t)p >= c->base && (uintptr_t)p < c->base + (uintptr_t)CHUNK_PAGES * PAGE_SIZE) { c->nspare++; break; }
    int kept_free = 0, released = 0;
    for (struct chunk* c = H->chunks; c; c = c->next) {
        if (c->nspare != CHUNK_PAGES) continue;
        if (!kept_free) { kept_free = 1; continue; }   // one empty chunk stays for the next burst
        c->nspare = -1;                                 // to release
        released++;
    }
    if (!released) return;
    struct page** pp = &H->spare;
    while (*pp) {
        struct page* p = *pp;
        int drop = 0;
        for (struct chunk* c = H->chunks; c; c = c->next)
            if (c->nspare < 0 && (uintptr_t)p >= c->base && (uintptr_t)p < c->base + (uintptr_t)CHUNK_PAGES * PAGE_SIZE) { drop = 1; break; }
        if (drop) *pp = p->next; else pp = &p->next;
    }
    struct chunk** cp = &H->chunks;
    while (*cp) {
        struct chunk* c = *cp;
        if (c->nspare < 0) {
            *cp = c->next;
            ojs_sys_free(c->raw);
            ojs_sys_free(c);
            J->heap_bytes -= CHUNK_BYTES;
        } else cp = &c->next;
    }
}

double ojs_sys_time_ms(void);

void gc_collect(ojs* J) {
    struct gc_heap* H = J->heap;
    if (J->in_gc || !H) return;
    J->in_gc = 1;
    double t0 = ojs_sys_time_ms();
    H->m0.sp = 0;
    H->m0.overflow = 0;
    H->m0.par = 0;
    H->mark_overflow = 0;
    J->mk = &H->m0;
    mark_roots(J);
    double tp0 = ojs_sys_time_ms();
    int was_par = 0;
    if (J->heap_bytes >= PAR_MIN_HEAP) was_par = par_drain(J);       // the big part of the work, on every CPU the embedder offers
    double tp1 = ojs_sys_time_ms();
#if !defined(KERNEL) || !KERNEL
    if (was_par && getenv("OJS_GCPAR_VERIFY")) par_verify(J);
#endif
    drain(J);
#if !defined(KERNEL) || !KERNEL
    if (getenv("OJS_GCPAR_TRACE")) fprintf(stderr, "[gc] roots %.1f ms, parallel phase %.1f ms (%d), rest %.1f ms\n", tp0 - t0, tp1 - tp0, was_par, ojs_sys_time_ms() - tp1);
#endif
    // weak structures: ephemerons may mark more (loop until stable)
    for (;;) {
        int more = weak_process_ephemerons(J);
        drain(J);
        if (!more) break;
    }
    finreg_process(J);      // may mark held values of dead targets (callbacks queued)
    drain(J);
    weak_clear_dead(J);     // WeakMap entries / WeakRef targets that died
    J->gc_mark_ms += ojs_sys_time_ms() - t0;
    double tm1 = ojs_sys_time_ms();
    atoms_sweep(J);
    size_t live = sweep(J);
    release_chunks(J);
    H->live_after_gc = live;
    J->bytes_since_gc = 0;
    // next collection after 1.5x the live size is allocated: a mark costs about the live
    // size, so this bounds GC work per allocated byte; the realm limit caps the peak
    // (an allocation that would pass it collects first)
    size_t th = live + live / 2;
    if (th < (8u << 20)) th = 8u << 20;
    J->gc_threshold = th;
    J->gc_count++;
    J->gc_ms += ojs_sys_time_ms() - t0;
    if (ojs_gc_report) ojs_gc_report(was_par, ojs_sys_time_ms() - t0, tm1 - t0, live, J->heap_bytes);
    J->in_gc = 0;
}

size_t gc_live_bytes(ojs* J) { return J->heap->live_after_gc; }
size_t gc_live_of_type(ojs* J, int type) { return type > 0 && type < GT_COUNT ? J->heap->live_by_type[type] : 0; }

int gc_is_marked(const void* p) { return p && (((const struct gch*)p)->gcflags & (GCF_MARK | GCF_PINNED)) != 0; }

void gc_free_all(ojs* J) {
    struct gc_heap* H = J->heap;
    if (!H) return;
    // finalize everything that asked for it
    for (int c = 0; c < NCLASSES; c++)
        for (struct page* p = H->pages[c]; p; p = p->next)
            for (uint32_t i = 0; i < p->nobj; i++) {
                struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                if (g->type != GT_FREE && (g->gcflags & GCF_FINAL)) gc_finalize(J, g);
            }
    for (int i = 0; i < H->nlarge; i++) {
        struct gch* g = (struct gch*)H->large[i].p;
        if (g->gcflags & GCF_FINAL) gc_finalize(J, g);
        ojs_sys_free(H->large[i].raw);
    }
    for (struct chunk* c = H->chunks; c;) {
        struct chunk* n = c->next;
        ojs_sys_free(c->raw);
        ojs_sys_free(c);
        c = n;
    }
    ojs_sys_free(H->large);
    ojs_sys_free(H->m0.stack);
    ojs_sys_free(H);
    J->heap = 0;
}

void gc_write_barrier(ojs* J, void* holder) { (void)J; (void)holder; }

// ---------------------------------------------------------------- value arrays

jv* valarr_new(ojs* J, uint32_t cap) {
    struct valarr* a = (struct valarr*)gc_alloc(J, GT_VALARR, sizeof(struct valarr) + (size_t)cap * sizeof(jv));
    if (!a) return 0;
    a->cap = cap;
    for (uint32_t i = 0; i < cap; i++) a->v[i] = JV_UNDEFINED;
    return a->v;
}

jv* valarr_grow(ojs* J, jv* old, uint32_t oldcap, uint32_t newcap, jv fill) {
    struct valarr* a = (struct valarr*)gc_alloc(J, GT_VALARR, sizeof(struct valarr) + (size_t)newcap * sizeof(jv));
    if (!a) return 0;
    a->cap = newcap;
    uint32_t keep = oldcap < newcap ? oldcap : newcap;
    if (old && keep) memcpy(a->v, old, (size_t)keep * sizeof(jv));
    for (uint32_t i = keep; i < newcap; i++) a->v[i] = fill;
    return a->v;
}

void* bytes_new(ojs* J, size_t n) {
    struct gbytes* b = (struct gbytes*)gc_alloc(J, GT_BYTES, sizeof(struct gbytes) + n);
    if (!b) return 0;
    b->n = (uint32_t)n;
    return b->d;
}

struct ptrarr* ptrarr_new(ojs* J, uint32_t n) {
    struct ptrarr* a = (struct ptrarr*)gc_alloc(J, GT_PTRARR, sizeof(struct ptrarr) + (size_t)n * sizeof(void*));
    if (a) a->n = n;
    return a;
}

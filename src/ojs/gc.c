// gc.c — ojs heap: size-class pages + large objects, non-moving
// mark-sweep with a conservative C-stack scan.
//
// Small objects (<= 4096 bytes) live in 16 KB pages of one size class
// each; pages are carved from 1 MB chunks so they can be page aligned.
// A hash set of page numbers says which page addresses are ours, so the
// conservative scan can map any word to "the object containing it" in O(1). Large objects are sys-malloc'd and
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

// the debug/opt-in knobs are env vars on the host; in the kernel they are off
// (the embedder flips the engine-level switches instead)
#if defined(KERNEL) && KERNEL
static inline int gc_env_on(const char* name) { (void)name; return 0; }
#else
static inline int gc_env_on(const char* name) { const char* g = getenv(name); return g && *g == '1'; }
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
    uint16_t in_young;          // the page is in the heap's young list (holds at least one young object)
    struct page* next;          // all pages of this class
    struct page* anext;         // pages of this class with free objects (the allocation stack)
    struct page* ynext;         // pages of this class holding young objects (the minor GC walks these)
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
    uintptr_t* owned;                    // open-addressing set of owned page numbers (0 = empty, 1 = deleted)
    uint32_t ownedsize, ownedused;       // table size (power of two); slots not empty, deleted ones included
    uintptr_t ownedlo, ownedhi;          // address range of every page ever owned: most stack words fail this test at once
    struct page* pages[NCLASSES];        // class -> page list
    struct page* avail[NCLASSES];        // class -> stack of pages with free objects (linked by anext)
    struct page* young[NCLASSES];        // class -> pages holding young objects (linked by ynext)
    struct chunk* chunks;
    struct page* spare;                  // carved, unused pages
    struct large* large;
    int nlarge, caplarge;
    struct gch** remset;                 // remembered old objects (generational: written since the last minor GC)
    int nrem, caprem;
    uint32_t minor_count;                // statistics
    struct marker m0;                    // the calling thread's mark stack
    int mark_overflow;
    size_t live_after_gc;
    size_t live_by_type[GT_COUNT];
    uint8_t size_to_class[(4096 / 8) + 1];
};

#define REMSET_MAX 65536   // beyond this a minor GC costs more than a major one: force the major instead

// ---------------------------------------------------------------- pages

static inline uint32_t page_hash(uintptr_t pn) { return (uint32_t)(((uint64_t)pn * 0x9E3779B97F4A7C15ull) >> 32); }

static inline int page_owned(struct gc_heap* H, uintptr_t addr) {
    if (addr < H->ownedlo || addr >= H->ownedhi) return 0;
    uintptr_t pn = addr >> PAGE_SHIFT;
    uint32_t mask = H->ownedsize - 1;
    for (uint32_t i = page_hash(pn) & mask;; i = (i + 1) & mask) {
        uintptr_t e = H->owned[i];
        if (e == pn) return 1;
        if (!e) return 0;
    }
}

static int page_table_grow(struct gc_heap* H) {
    uint32_t nsize = H->ownedsize ? H->ownedsize * 2 : 256;
    uintptr_t* t = (uintptr_t*)ojs_sys_malloc((size_t)nsize * sizeof(uintptr_t));
    if (!t) return -1;
    memset(t, 0, (size_t)nsize * sizeof(uintptr_t));
    uint32_t used = 0;
    for (uint32_t k = 0; k < H->ownedsize; k++) {
        uintptr_t e = H->owned[k];
        if (e < 2) continue;
        uint32_t i = page_hash(e) & (nsize - 1);
        while (t[i]) i = (i + 1) & (nsize - 1);
        t[i] = e;
        used++;
    }
    ojs_sys_free(H->owned);
    H->owned = t;
    H->ownedsize = nsize;
    H->ownedused = used;
    return 0;
}

// 0 on success. Page numbers are never 0 or 1: those would be the first 32 KB of the address space.
static int page_set_owned(struct gc_heap* H, uintptr_t addr, int on) {
    uintptr_t pn = addr >> PAGE_SHIFT;
    if (on) {
        if ((H->ownedused + 1) * 4 >= H->ownedsize * 3 && page_table_grow(H) < 0) return -1;
        uint32_t mask = H->ownedsize - 1;
        uint32_t i = page_hash(pn) & mask;
        while (H->owned[i] >= 2) i = (i + 1) & mask;
        if (!H->owned[i]) H->ownedused++;
        H->owned[i] = pn;
        if (!H->ownedlo || addr < H->ownedlo) H->ownedlo = addr;
        if (addr + PAGE_SIZE > H->ownedhi) H->ownedhi = addr + PAGE_SIZE;
        return 0;
    }
    if (!H->ownedsize) return 0;
    uint32_t mask = H->ownedsize - 1;
    for (uint32_t i = page_hash(pn) & mask;; i = (i + 1) & mask) {
        if (H->owned[i] == pn) { H->owned[i] = 1; return 0; }
        if (!H->owned[i]) return 0;
    }
}

#define CHUNK_BYTES ((size_t)CHUNK_PAGES * PAGE_SIZE + PAGE_SIZE)

void gc_collect(ojs* J);
void gc_collect_minor(ojs* J);

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
    p->in_young = 0;
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
    if (page_set_owned(H, (uintptr_t)p, 1) < 0) {      // no room in the table: the stack scan could not find this page, so it must not be used
        H->pages[cls] = p->next;
        H->avail[cls] = p->anext;
        p->next = H->spare;
        H->spare = p;
        return 0;
    }
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
    if (!p->in_young) {                          // the page now holds a young object: the minor GC must see it
        p->in_young = 1;
        p->ynext = H->young[cls];
        H->young[cls] = p;
    }
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
    if (!J->gc_disabled && !J->in_gc && J->stack_top) {
        if (J->gc_stress && ++J->gc_stress_n >= J->gc_stress) {
            J->gc_stress_n = 0;
            // stress alternates: three minor collections, then a major one
            static int gen_on = -1;
            if (gen_on < 0) gen_on = gc_env_on("OJS_GC_GEN");
            if (++J->gc_stress_rounds % 4 == 0 || !gen_on) gc_collect(J);
            else gc_collect_minor(J);
        } else if (J->force_major || J->bytes_since_gc + size > J->gc_threshold) {
            gc_collect(J);
        } else {
            // the nursery: a quarter of the major threshold (floor 1 MB); the
            // minor collection reclaims it so a full mark does not run on every burst.
            // The generational path is opt-in (OJS_GC_GEN=1) until its remaining
            // interaction bug is found: minors alone corrupt plain churn workloads.
            static int gen_on = -1;
            if (gen_on < 0) gen_on = gc_env_on("OJS_GC_GEN");
            if (gen_on) {
                size_t minor_budget = J->gc_threshold / 4;
                if (minor_budget < (1u << 20)) minor_budget = 1u << 20;
                if (J->bytes_since_minor + size > minor_budget) gc_collect_minor(J);
            }
        }
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
    J->bytes_since_minor += real;
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
    // Words are pointer-sized and aligned. A word may be a NaN-boxed value: its low 48 bits sign-extended give the pointer back, and a plain
    // pointer is unchanged by that.
    const uintptr_t sz = sizeof(uintptr_t);
    uintptr_t a = ((uintptr_t)lo + sz - 1) & ~(sz - 1);
    for (; a + sz <= (uintptr_t)hi; a += sz) {
        uintptr_t w = *(const uintptr_t*)a;
#if UINTPTR_MAX > 0xFFFFFFFFu
        w = (uintptr_t)((int64_t)((uint64_t)w << 16) >> 16);
#endif
        if (w < 4096) continue;
        struct gch* g = find_object(J, w);
        if (!g && w) g = find_object(J, w - 1);     // one-past-the-end pointers
        if (g) gc_mark_ptr(J, g);
    }
}

// trace one object. thru_old: the caller is the minor GC's remembered-set drain
// (old objects are normally skipped: their young children come from the remset).
static void trace1(ojs* J, struct gch* g, int thru_old) {
    if (!thru_old && (g->gcflags & GCF_OLD)) return;   // minor marking: old objects are only marked, not traced
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

static void trace(ojs* J, struct gch* g) { trace1(J, g, 0); }

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
#if defined(__x86_64__)
    volatile uint64_t regs[6];
    __asm__ volatile("movq %%rbx, 0(%0)\n\tmovq %%rbp, 8(%0)\n\tmovq %%r12, 16(%0)\n\tmovq %%r13, 24(%0)\n\tmovq %%r14, 32(%0)\n\tmovq %%r15, 40(%0)"
                     :: "r"(regs) : "memory");
#else
    volatile uint32_t regs[4];
    __asm__ volatile("movl %%ebx, 0(%0)\n\tmovl %%esi, 4(%0)\n\tmovl %%edi, 8(%0)\n\tmovl %%ebp, 12(%0)"
                     :: "r"(regs) : "memory");
#endif
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
            int has_young = 0;
            for (int i = (int)p->nobj - 1; i >= 0; i--) {
                struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                if (g->type != GT_FREE) {
                    if (g->gcflags & (GCF_MARK | GCF_PINNED)) {
                        // a full collection demotes every survivor: the young/old
                        // split rebuilds from here, so the remembered-set invariant
                        // ("old objects with young refs are recorded") holds vacuously
                        g->gcflags &= (uint8_t)~(GCF_MARK | GCF_OLD | GCF_AGE | GCF_REM);
                        live += p->objsize;
                        if (g->type < GT_COUNT) H->live_by_type[g->type] += p->objsize;
                        has_young = 1;
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
            if (has_young && !p->in_young) {
                p->in_young = 1;
                p->ynext = H->young[c];
                H->young[c] = p;
            } else if (!has_young && p->in_young) {
                p->in_young = 0;
                // remove from the young list: walk it (the full sweep is rare)
                struct page** yp = &H->young[c];
                while (*yp && *yp != p) yp = &(*yp)->ynext;
                if (*yp) *yp = p->ynext;
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
            g->gcflags &= (uint8_t)~(GCF_MARK | GCF_OLD | GCF_AGE | GCF_REM);
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
    H->nrem = 0;   // the survivors are all young again
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
    J->force_major = 0;
    J->bytes_since_minor = 0;
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
    ojs_sys_free(H->owned);
    ojs_sys_free(H->remset);
    ojs_sys_free(H);
    J->heap = 0;
}

// ---------------------------------------------------------------- generational

// record an old object whose fields were written (it may now point at young
// objects); the next minor GC traces it. GCF_REM deduplicates.
void gc_remember(ojs* J, void* holder) {
    if (gc_env_on("OJS_GC_NOBARRIER")) return;
    struct gc_heap* H = J->heap;
    if (!H) return;
    if (H->nrem >= REMSET_MAX) { J->force_major = 1; return; }
    if (H->nrem >= H->caprem) {
        int nc = H->caprem ? H->caprem * 2 : 256;
        struct gch** t = (struct gch**)ojs_sys_realloc(H->remset, (size_t)nc * sizeof(struct gch*));
        if (!t) { J->force_major = 1; return; }
        H->remset = t;
        H->caprem = nc;
    }
    ((struct gch*)holder)->gcflags |= GCF_REM;
    H->remset[H->nrem++] = (struct gch*)holder;
}

// age a surviving young object; promote it once it has seen GCF_AGE_MAX minor
// collections. A promoted object joins the remembered set once: its young
// children (written while it was young, when no barrier fired) must be found.
static void minor_survive(ojs* J, struct gch* g) {
    if (gc_env_on("OJS_GC_NOAGE")) return;
    uint32_t age = (g->gcflags & GCF_AGE) >> 5;
    if (g->gcflags & GCF_PINNED || age >= GCF_AGE_MAX) {
        g->gcflags &= (uint8_t)~GCF_AGE;
        g->gcflags |= GCF_OLD;
        gc_remember(J, g);
        return;
    }
    g->gcflags = (uint8_t)((g->gcflags & ~GCF_AGE) | ((age + 1) << 5));
}

// a minor collection: mark from the roots but trace only young objects, drain
// the remembered set, sweep young objects only. Old objects keep their mark
// bits (the next full sweep clears them) and are never freed here.
void gc_collect_minor(ojs* J) {
    struct gc_heap* H = J->heap;
    if (J->in_gc || !H) return;
    if (J->force_major) { gc_collect(J); return; }
    J->in_gc = 1;
    double t0 = ojs_sys_time_ms();
    H->m0.sp = 0;
    H->m0.overflow = 0;
    H->m0.par = 0;
    H->mark_overflow = 0;
    J->mk = &H->m0;
    mark_roots(J);
    drain(J);
    // the remembered set: old objects written since the last minor GC (their
    // young children are reachable only through them)
    for (int i = 0; i < H->nrem; i++) {
        struct gch* g = H->remset[i];
        g->gcflags &= (uint8_t)~GCF_REM;
        trace1(J, g, 1);
    }
    H->nrem = 0;
    drain(J);
    // weak structures (a young WeakMap key may die here; targets are cleared)
    for (;;) {
        int more = weak_process_ephemerons(J);
        drain(J);
        if (!more) break;
    }
    finreg_process(J);
    drain(J);
    weak_clear_dead(J);
#if !defined(KERNEL) || !KERNEL
    // OJS_GC_VERIFY: re-trace everything marked, ignoring generations; any young
    // object that becomes marked now would have been swept while still reachable
    // (a missing write barrier). Names the holder chain by type counts.
    if (gc_env_on("OJS_GC_VERIFY")) {
        int fixed = 0;
        for (;;) {
            int before = 0;
            struct marker* mk = J->mk;
            for (int c = 0; c < NCLASSES; c++)
                for (struct page* p = H->pages[c]; p; p = p->next)
                    for (uint32_t i = 0; i < p->nobj; i++) {
                        struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                        if (g->type != GT_FREE && (g->gcflags & GCF_MARK)) { before += mk->sp; trace1(J, g, 1); fixed += mk->sp; while (mk->sp > 0) mk->sp--; }
                    }
            for (int i = 0; i < H->nlarge; i++) {
                struct gch* g = (struct gch*)H->large[i].p;
                if (g->gcflags & GCF_MARK) { trace1(J, g, 1); fixed += mk->sp; while (mk->sp > 0) mk->sp--; }
            }
            if (!fixed) break;   // stable: nothing new became marked
            fprintf(stderr, "[gc] VERIFY: minor marking missed objects (traced %d more this round)\n", fixed);
            (void)before;
            fixed = 0;
            // keep going: the loop ends when a full pass marks nothing new
        }
        fprintf(stderr, "[gc] VERIFY: stable\n");
    }
#endif
    J->gc_mark_ms += ojs_sys_time_ms() - t0;
    // sweep of the young generation. Pages already in avail[cls] are left
    // untouched (their free lists keep working exactly as the allocator left
    // them); slots freed here on other pages are collected onto those pages
    // own free lists and the pages re-enter allocation if they had been
    // exhausted. Nothing is returned to the spare pool and no avail entry is
    // removed: a fully dead page simply waits for the full sweep, which owns
    // every one of those lists. This keeps the allocators invariants trivially
    // true while the minor GC still reclaims the common case (fresh pages of
    // short-lived objects).
    size_t live = 0;
    for (int c = 0; c < NCLASSES; c++) {
        struct page** yp = &H->young[c];
        while (*yp) {
            struct page* p = *yp;
            int had_free = p->nfree > 0;   // in avail[c] (the alloc invariant)
            int has_young = 0;
            if (!had_free) {
                // an exhausted page: reclaim its dead young objects
                p->free = 0;
                p->nfree = 0;
                for (int i = (int)p->nobj - 1; i >= 0; i--) {
                    struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                    if (g->type == GT_FREE) {
                        *(struct gch**)(g + 1) = p->free;
                        p->free = g;
                        p->nfree++;
                        continue;
                    }
                    if (g->gcflags & GCF_OLD) continue;
                    if (g->gcflags & (GCF_MARK | GCF_PINNED)) {
                        g->gcflags &= (uint8_t)~GCF_MARK;
                        live += p->objsize;
                        minor_survive(J, g);
                        if (!(g->gcflags & GCF_OLD)) has_young = 1;
                        continue;
                    }
                    // young atom strings stay until the full sweep: the atom table
                    // (swept only by full collections) would hold dangling pointers
                    if (g->type == GT_STR && (((struct str*)g)->h.aux & SF_ATOM)) { has_young = 1; continue; }
#if !defined(KERNEL) || !KERNEL
                    // verify: no marked object may still point at a young object we are about to free
                    if (gc_env_on("OJS_GC_VERIFY")) {
                        int refd = 0;
                        for (int c2 = 0; c2 < NCLASSES && !refd; c2++)
                            for (struct page* p2 = H->pages[c2]; p2 && !refd; p2 = p2->next)
                                for (uint32_t i2 = 0; i2 < p2->nobj && !refd; i2++) {
                                    struct gch* g2 = (struct gch*)(p2->data + (size_t)i2 * p2->objsize);
                                    if (g2 == g || g2->type == GT_FREE || !(g2->gcflags & (GCF_MARK | GCF_PINNED))) continue;
                                    // crude: scan the raw body for the pointer (catches every container kind)
                                    uint64_t* w = (uint64_t*)((struct gch*)g2 + 1);
                                    size_t nw = (p2->objsize - sizeof(struct gch)) / 8;
                                    for (size_t k2 = 0; k2 < nw; k2++) {
                                        uintptr_t word = w[k2];
                                        uintptr_t ptr = (uintptr_t)((int64_t)(word << 16) >> 16);   // jv pointer or raw pointer
                                        if (ptr == (uintptr_t)g) { refd = 1; fprintf(stderr, "[gc] FREE of referenced object (type %d) from marked (type %d)\n", g->type, g2->type); break; }
                                    }
                                }
                        for (int i2 = 0; i2 < H->nlarge && !refd; i2++) {
                            struct gch* g2 = (struct gch*)H->large[i2].p;
                            if (g2 == g || !(g2->gcflags & (GCF_MARK | GCF_PINNED))) continue;
                            uint64_t* w = (uint64_t*)((struct gch*)g2 + 1);
                            size_t nw = (H->large[i2].size - sizeof(struct gch)) / 8;
                            for (size_t k2 = 0; k2 < nw; k2++) {
                                uintptr_t word = w[k2];
                                uintptr_t ptr = (uintptr_t)((int64_t)(word << 16) >> 16);
                                if (ptr == (uintptr_t)g) { refd = 1; fprintf(stderr, "[gc] FREE of referenced object from marked large\n"); break; }
                            }
                        }
                        if (refd) { fprintf(stderr, "[gc] ABORT: freeing a referenced young object\n"); abort(); }
                    }
#endif
                    if (g->gcflags & GCF_FINAL) gc_finalize(J, g);
#if !defined(KERNEL) || !KERNEL
                    if (gc_env_on("OJS_GC_POISON") && p->objsize <= 64) memset((uint8_t*)g + 2, 0xCC, p->objsize - 2 > 40 ? 40 : p->objsize - 2);
#endif
                    g->type = GT_FREE;
                    g->gcflags = 0;
                    J->minor_freed += p->objsize;
                    *(struct gch**)(g + 1) = p->free;
                    p->free = g;
                    p->nfree++;
                }
                if (p->nfree > 0) {              // back into allocation
                    p->anext = H->avail[c];
                    H->avail[c] = p;
                }
            } else {
                // in avail: survivors age, garbage waits for the full sweep
                for (int i = 0; i < (int)p->nobj; i++) {
                    struct gch* g = (struct gch*)(p->data + (size_t)i * p->objsize);
                    if (g->type == GT_FREE || (g->gcflags & GCF_OLD)) continue;
                    if (g->gcflags & (GCF_MARK | GCF_PINNED)) {
                        g->gcflags &= (uint8_t)~GCF_MARK;
                        live += p->objsize;
                        minor_survive(J, g);
                        if (!(g->gcflags & GCF_OLD)) has_young = 1;
                    } else if (g->type == GT_STR && (((struct str*)g)->h.aux & SF_ATOM)) {
                        has_young = 1;           // the atom table (major-only sweep) still points here
                    } else {
                        has_young = 1;           // dead but not reclaimable here: keep the page young-listed
                        if (g->gcflags & GCF_FINAL) gc_finalize(J, g);   // hmm: it dies at the full sweep instead
                    }
                }
                // (dead objects of in-avail pages are finalized by the full sweep)
            }
            if (has_young) yp = &p->ynext;         // kept in the young list (*yp is already p)
            else {
                struct page* done = *yp;
                *yp = done->ynext;
                done->in_young = 0;
                done->ynext = 0;
            }
        }
    }
    int w = 0;
    for (int i = 0; i < H->nlarge; i++) {
        struct gch* g = (struct gch*)H->large[i].p;
        if (g->gcflags & GCF_OLD) { H->large[w++] = H->large[i]; continue; }
        if (g->gcflags & (GCF_MARK | GCF_PINNED)) {
            g->gcflags &= (uint8_t)~GCF_MARK;
            live += H->large[i].size;
            minor_survive(J, g);
            H->large[w++] = H->large[i];     // keep the table address-sorted
            continue;
        }
        if (g->gcflags & GCF_FINAL) gc_finalize(J, g);
        J->heap_bytes -= H->large[i].size + 16;
        ojs_sys_free(H->large[i].raw);
        // dropped: the table stays sorted
    }
    H->nlarge = w;
    J->bytes_since_minor = 0;
    J->minor_count++;   // statistics (ojs.h)
    J->gc_ms += ojs_sys_time_ms() - t0;
    J->in_gc = 0;
}

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

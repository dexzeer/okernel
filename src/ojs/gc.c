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
    struct gch* free;           // free list (linked through the object body)
    uint8_t* data;              // first object
};

struct chunk { struct chunk* next; void* raw; uintptr_t base; int nspare; };

struct large {
    uint8_t* p;                 // object start (the struct gch)
    size_t size;
    void* raw;
};

struct gc_heap {
    uint32_t owned[(1u << (32 - PAGE_SHIFT)) / 32];   // page bitmap over 4 GB
    struct page* pages[NCLASSES];        // class -> page list
    struct page* avail[NCLASSES];        // class -> first page with free objects (hint)
    struct chunk* chunks;
    struct page* spare;                  // carved, unused pages
    struct large* large;
    int nlarge, caplarge;
    void** mstack;                       // mark stack
    int msp, mcap;
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
    page_set_owned(H, (uintptr_t)p, 1);
    return p;
}

static void* alloc_small(ojs* J, int cls) {
    struct gc_heap* H = J->heap;
    struct page* p = H->avail[cls];
    while (p && !p->free) p = p->next;
    if (!p) {
        for (p = H->pages[cls]; p && !p->free; p = p->next) {}
        if (!p) p = page_new(J, cls);
        if (!p) return 0;
    }
    H->avail[cls] = p;
    struct gch* g = p->free;
    p->free = *(struct gch**)(g + 1);
    p->nfree--;
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

static void mpush(ojs* J, void* p) {
    struct gc_heap* H = J->heap;
    if (H->msp >= H->mcap) {
        int nc = H->mcap ? H->mcap * 2 : 4096;
        void** t = (void**)ojs_sys_realloc(H->mstack, (size_t)nc * sizeof(void*));
        if (!t) { H->mark_overflow = 1; return; }   // rescan later
        H->mstack = t;
        H->mcap = nc;
    }
    H->mstack[H->msp++] = p;
}

void gc_mark_ptr(ojs* J, void* p) {
    if (!p) return;
    struct gch* g = (struct gch*)p;
    if (g->gcflags & GCF_MARK) return;
    g->gcflags |= GCF_MARK;
    mpush(J, p);
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

void gc_scan_conservative(ojs* J, const void* lo, const void* hi) {
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
    for (;;) {
        while (H->msp > 0) trace(J, (struct gch*)H->mstack[--H->msp]);
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
    H->msp = 0;
    H->mark_overflow = 0;
    mark_roots(J);
    drain(J);
    // weak structures: ephemerons may mark more (loop until stable)
    for (;;) {
        int more = weak_process_ephemerons(J);
        drain(J);
        if (!more) break;
    }
    finreg_process(J);      // may mark held values of dead targets (callbacks queued)
    drain(J);
    weak_clear_dead(J);     // WeakMap entries / WeakRef targets that died
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
    ojs_sys_free(H->mstack);
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

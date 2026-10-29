// okai on Windows -- the allocator, with an optional heap checker.
//
// Everything in the process allocates through here (malloc/calloc/realloc/free
// are defined in this file, and the kernel-named kmalloc/kfree go through the
// same core), so this is the one place that can see every block.
//
// Normal mode is a thin wrapper over the process heap: same semantics, no
// bookkeeping. OKAI_HCHK=1 switches to a guarded allocator on a PRIVATE heap:
//   [header][payload][16 x 0xCD]
// the header carries the size and the return address of the allocation site,
// live blocks are kept in a list, and win_heap_check() (called from the main
// loop, ~2 Hz) verifies every block's magic and trailing guard. A write past
// the end of a block is therefore reported with the size and the call site of
// the allocation that was overrun -- and it is reported BEFORE ntdll's heap
// validation turns it into 0xC0000409 in ntdll (which no handler can catch).
//
// Freed blocks are filled with 0xDD and their magic becomes "freed", so a
// double free or a write-after-free is also caught. The env switch keeps the
// release binary on the fast path.

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "win_plat.h"

#define H_GUARD 16
#define H_MAGIC 0x4F4B4850u   // "OKHP"
#define H_FREED 0x4F4B4846u   // "OKHF"
#define H_FILL_ALLOC 0xCD
#define H_FILL_FREE  0xDD

struct hhdr {
    uint32_t magic;
    uint32_t size;
    void* site;               // return address of the allocating call site
    struct hhdr* prev;
    struct hhdr* next;
    uint32_t pad;             // keeps the payload 8-byte aligned
};

static int hchk;              // 1 = guarded mode
static HANDLE gheap;          // private heap for guarded blocks
static SRWLOCK hlock = SRWLOCK_INIT;
static struct hhdr* live;
static volatile LONG nlive, nalloc, nfree;

static void h_report(const char* what, struct hhdr* h, void* ptr) {
    // No allocation, no locks of our own: the log path uses msvcrt's own heap.
    win_logf("\n[heap] %s: block size=%u site=%p ptr=%p live=%ld allocs=%ld frees=%ld\n",
             what, h ? h->size : 0, h ? h->site : 0, ptr, (long)nlive, (long)nalloc, (long)nfree);
}

static int guard_ok(const struct hhdr* h) {
    const uint8_t* g = (const uint8_t*)(h + 1) + h->size;
    for (int i = 0; i < H_GUARD; i++) if (g[i] != H_FILL_ALLOC) return 0;
    return 1;
}

// The payload starts right after the header (h + 1); every base computation
// must use the same size, which is why this is sizeof() and not a rounded-up
// variant (that mismatch made every free look like a foreign block).
static size_t hdr_bytes(void) { return sizeof(struct hhdr); }

void* win_h_alloc(size_t n, void* site) {
    if (n == 0) n = 1;
    size_t need = hdr_bytes() + n + H_GUARD;
    if (!hchk) {
        void* p = HeapAlloc(GetProcessHeap(), 0, need);
        return p ? (uint8_t*)p + hdr_bytes() : 0;
    }
    struct hhdr* h = (struct hhdr*)HeapAlloc(gheap, 0, need);
    if (!h) return 0;
    h->magic = H_MAGIC;
    h->size = (uint32_t)n;
    h->site = site;
    memset((uint8_t*)(h + 1), H_FILL_ALLOC, n + H_GUARD);   // payload + guard
    AcquireSRWLockExclusive(&hlock);
    h->prev = 0;
    h->next = live;
    if (live) live->prev = h;
    live = h;
    InterlockedIncrement(&nlive);
    InterlockedIncrement(&nalloc);
    ReleaseSRWLockExclusive(&hlock);
    return (uint8_t*)(h + 1);
}

void win_h_free(void* ptr, void* site) {
    (void)site;
    if (!ptr) return;
    uint8_t* base = (uint8_t*)ptr - hdr_bytes();
    struct hhdr* h = (struct hhdr*)base;
    if (!hchk) { HeapFree(GetProcessHeap(), 0, base); return; }
    if (h->magic == H_FREED) { h_report("DOUBLE FREE", h, ptr); ExitProcess(4); }
    if (h->magic != H_MAGIC) {
        // Not one of ours. Do NOT free it (freeing a pointer we do not own is
        // how a 0xC0000374 heap corruption starts) -- report it with the stack
        // so the caller chain can be resolved against okai-debug.exe.
        win_logf("\n[heap] BAD FREE ptr=%p free_site=%p magic=%08X\n",
                 ptr, site, h->magic);
        win_crash_log(0x42444652u /*BDFR*/, ptr, (uint32_t)(uintptr_t)site,
                      (uint32_t)(uintptr_t)&h, 0, (const uint32_t*)&h, 48);
        ExitProcess(4);
    }
    if (!guard_ok(h)) { h_report("BUFFER OVERRUN (guard destroyed)", h, ptr); ExitProcess(4); }
    AcquireSRWLockExclusive(&hlock);
    if (h->prev) h->prev->next = h->next;
    else live = h->next;
    if (h->next) h->next->prev = h->prev;
    InterlockedDecrement(&nlive);
    InterlockedIncrement(&nfree);
    ReleaseSRWLockExclusive(&hlock);
    h->magic = H_FREED;
    memset((uint8_t*)(h + 1), H_FILL_FREE, h->size);         // catch use-after-free
    HeapFree(gheap, 0, h);
}

void* win_h_realloc(void* ptr, size_t n, void* site) {
    if (!ptr) return win_h_alloc(n, site);
    if (n == 0) { win_h_free(ptr, site); return 0; }
    uint8_t* base = (uint8_t*)ptr - hdr_bytes();
    struct hhdr* h = (struct hhdr*)base;
    if (!hchk) {
        void* q = HeapReAlloc(GetProcessHeap(), 0, base, hdr_bytes() + n + H_GUARD);
        return q ? (uint8_t*)q + hdr_bytes() : 0;
    }
    if (h->magic != H_MAGIC) { h_report("BAD REALLOC", h, ptr); ExitProcess(4); }
    if (!guard_ok(h)) { h_report("BUFFER OVERRUN (guard destroyed, realloc)", h, ptr); ExitProcess(4); }
    size_t old = h->size;
    void* q = win_h_alloc(n, site);
    if (!q) return 0;
    memcpy(q, ptr, old < n ? old : n);
    win_h_free(ptr, site);
    return q;
}

void win_heap_check(void) {
    if (!hchk) return;
    AcquireSRWLockShared(&hlock);
    for (struct hhdr* h = live; h; h = h->next) {
        if (h->magic != H_MAGIC || !guard_ok(h)) {
            ReleaseSRWLockShared(&hlock);
            h_report(h->magic != H_MAGIC ? "LIVE BLOCK magic damaged" : "BUFFER OVERRUN (sweep)",
                     h, (uint8_t*)(h + 1));
            ExitProcess(4);
        }
    }
    ReleaseSRWLockShared(&hlock);
}

void win_heap_stats(void) {
    win_logf("[heap] mode=%s live=%ld allocs=%ld frees=%ld\n",
             hchk ? "guarded(private heap)" : "process heap",
             (long)nlive, (long)nalloc, (long)nfree);
}

int win_heap_guarded(void) { return hchk; }

static void h_setup(void) {
    if (gheap) return;
    const char* e = getenv("OKAI_HCHK");
    if (e && *e && *e != '0') {
        gheap = HeapCreate(0, 0, 0);
        if (gheap) hchk = 1;
    }
}

// Runs during CRT initialisation, before main: every allocation from the very
// first one is in one mode. Deciding in WinMain left the blocks allocated by
// the CRT startup on the process heap, and freeing one after the switch was
// reported as a foreign free.
__attribute__((constructor)) static void h_init_ctor(void) { h_setup(); }

void win_heap_init(void) {
    h_setup();
    // Ask the process heap to terminate on corruption with a clear code
    // instead of failing later in an unrelated place.
    HeapSetInformation(GetProcessHeap(), HeapEnableTerminationOnCorruption, 0, 0);
    if (hchk) win_logf("[heap] guarded allocator on (OKAI_HCHK=1)\n");
}

// ---- the C allocator, defined here so every block is accounted for ----------

void* malloc(size_t n) { return win_h_alloc(n, __builtin_return_address(0)); }
void  free(void* p) { win_h_free(p, __builtin_return_address(0)); }
void* realloc(void* p, size_t n) { return win_h_realloc(p, n, __builtin_return_address(0)); }

void* calloc(size_t n, size_t size) {
    size_t total = n * size;
    void* p = win_h_alloc(total, __builtin_return_address(0));
    if (p) memset(p, 0, total);               // guarded mode fills 0xCD first
    return p;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)win_h_alloc(n, __builtin_return_address(0));
    if (p) memcpy(p, s, n);
    return p;
}

char* strndup(const char* s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char* p = (char*)win_h_alloc(len + 1, __builtin_return_address(0));
    if (p) { memcpy(p, s, len); p[len] = 0; }
    return p;
}

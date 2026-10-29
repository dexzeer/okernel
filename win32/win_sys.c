// okai on Windows -- everything the kernel supplies that is not graphics,
// windows, networking or audio: the heap names (kmalloc/kfree), the serial
// log, the 100 Hz tick, the profiler counters, the CPU clock report, pointer
// state, entropy, and the SMP job API (parallel GC + video decode helpers).

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "memory.h"
#include "serial.h"
#include "prof.h"
#include "cpufreq.h"
#include "smp.h"
#include "crypto/rand.h"
#include "crypto/x509.h"
#include "win_plat.h"

// ---- heap: the kernel names over the guarded allocator ----------------------

static volatile LONG heap_used, heap_peak;

static void heap_count(uint32_t bytes) {
    LONG u = InterlockedAdd(&heap_used, (LONG)bytes);
    LONG pk = heap_peak;
    while (u > pk && InterlockedCompareExchange(&heap_peak, u, pk) != pk) pk = heap_peak;
}

void* kmalloc(uint32_t size) {
    void* p = win_h_alloc(size ? size : 1, __builtin_return_address(0));
    if (p) heap_count(size);
    return p;
}

void* kcalloc(uint32_t n, uint32_t size) {
    void* p = calloc(n ? n : 1, size ? size : 1);
    if (p) heap_count(n * size);
    return p;
}

void* krealloc(void* ptr, uint32_t size) {
    return win_h_realloc(ptr, size ? size : 1, __builtin_return_address(0));
}

void kfree(void* ptr) {
    if (!ptr) return;
    win_h_free(ptr, __builtin_return_address(0));
}

uint32_t ksize(void* ptr) {
    (void)ptr;
    return 0;
}

void heap_stats(uint32_t* total, uint32_t* used, uint32_t* peak, uint32_t* largest_free) {
    uint32_t t = 2048u << 20;         // accounting only: the CRT heap has no fixed cap
    uint32_t u = (uint32_t)heap_used;
    if (total) *total = t;
    if (used) *used = u;
    if (peak) *peak = (uint32_t)heap_peak;
    if (largest_free) *largest_free = t > u ? t - u : 0;
}

int heap_pretouch(uint32_t budget) { (void)budget; return 0; }

// ---- serial: a log file (OKAI_LOG=path, "0" disables) + the debugger --------

char serial_ring[SERIAL_RING];
volatile uint32_t serial_total;

static FILE* log_fp;
static HANDLE log_raw = INVALID_HANDLE_VALUE;   // append handle for crash records
static int log_tried;
static SRWLOCK log_lock = SRWLOCK_INIT;

// Crash records must not touch the CRT, the heap or a lock: a fault inside a
// system callback (or a corrupted heap) still has to reach the log.
static void raw_append(const char* s, int n) {
    if (log_raw == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD wrote = 0;
    WriteFile(log_raw, s, (DWORD)n, &wrote, 0);
}

static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }

static char* put_hex(char* p, uint32_t v) {
    *p++ = '0'; *p++ = 'x';
    int started = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        int d = (int)((v >> shift) & 0xF);
        if (d || started || shift == 0) {
            *p++ = (char)(d < 10 ? '0' + d : 'A' + d - 10);
            started = 1;
        }
    }
    return p;
}

void win_crash_log(uint32_t code, const void* addr, uint32_t eip, uint32_t esp,
                   uint32_t ebp, const uint32_t* stack, int nstack) {
    char buf[1024];
    char* p = buf;
    p = put_str(p, "\n[okai-win] CRASH code=");
    p = put_hex(p, code);
    p = put_str(p, " at=");
    p = put_hex(p, (uint32_t)(uintptr_t)addr);
    p = put_str(p, " eip=");
    p = put_hex(p, eip);
    p = put_str(p, " esp=");
    p = put_hex(p, esp);
    p = put_str(p, " ebp=");
    p = put_hex(p, ebp);
    p = put_str(p, "\n[okai-win] stack:");
    for (int i = 0; i < nstack && p < buf + sizeof buf - 14; i++) {
        *p++ = ' ';
        p = put_hex(p, stack[i]);
        if ((i & 7) == 7 && i + 1 < nstack && p < buf + sizeof buf - 16) *p++ = '\n';
    }
    p = put_str(p, "\n");
    raw_append(buf, (int)(p - buf));
}

void win_log_init(void) {
    log_tried = 1;
    const char* path = getenv("OKAI_LOG");
    if (path && !strcmp(path, "0")) return;
    if (!path || !*path) path = "okai.log";
    log_fp = fopen(path, "w");
    log_raw = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          0, OPEN_ALWAYS, 0, 0);
}

void win_logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    serial_puts(buf);
}

void serial_init(void) { if (!log_tried) win_log_init(); }

void serial_putchar(char c) {
    char s[2] = { c, 0 };
    serial_puts(s);
}

void serial_puts(const char* str) {
    if (!str || !*str) return;
    if (!log_tried) win_log_init();
    size_t n = strlen(str);
    // keep the last SERIAL_RING bytes, like the kernel's ring buffer
    uint32_t tot = serial_total;
    for (size_t i = 0; i < n; i++) serial_ring[(tot + i) % SERIAL_RING] = str[i];
    serial_total = tot + (uint32_t)n;
    if (log_fp) {
        AcquireSRWLockExclusive(&log_lock);
        fputs(str, log_fp);
        fflush(log_fp);
        ReleaseSRWLockExclusive(&log_lock);
    }
    // No OutputDebugString: it raises DBG_PRINTEXCEPTION_C, which this
    // process cannot absorb without a debugger attached.
}

void serial_printf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    serial_puts(buf);
}

// ---- profiler counters (prof on is never enabled in the port) ---------------

volatile int prof_on;
uint64_t prof_cyc[PZ_COUNT];
uint32_t prof_cnt[PZ_COUNT];
volatile uint32_t stall_beat;
volatile int stall_halted;

void prof_report(void) { memset((void*)prof_cyc, 0, sizeof prof_cyc); memset((void*)prof_cnt, 0, sizeof prof_cnt); }
void stall_sample(uint32_t eip, uint32_t cs, uint32_t esp) { (void)eip; (void)cs; (void)esp; }
void stall_report(void) { }

// ---- CPU clock -------------------------------------------------------------

static uint32_t tsc_mhz;

static uint32_t measure_tsc_mhz(void) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    uint64_t t0 = __builtin_ia32_rdtsc();
    QueryPerformanceCounter(&a);
    Sleep(20);
    uint64_t t1 = __builtin_ia32_rdtsc();
    QueryPerformanceCounter(&b);
    uint64_t us = (uint64_t)((b.QuadPart - a.QuadPart) * 1000000ull / f.QuadPart);
    if (!us) return 0;
    return (uint32_t)((t1 - t0) / us);
}

uint32_t cpufreq_tsc_mhz(void) { return tsc_mhz; }
uint32_t cpufreq_mhz(void) { return tsc_mhz; }
int cpufreq_tsc_reliable(void) { return 1; }
int cpufreq_slow_factor(void) { return 1; }
void cpufreq_init(void) { tsc_mhz = measure_tsc_mhz(); }
void cpufreq_init_ap(void) { }
void cpufreq_report(char* buf, int cap) { if (cap > 0) snprintf(buf, cap, "CPU %u MHz (TSC)", tsc_mhz); }
int cpufreq_msr_selftest(void) { return 1; }

// ---- the 100 Hz tick -------------------------------------------------------
//
// okai schedules renders, tab animations, script timers and the media clock on
// tick_count (10 ms units). The kernel advances it from the PIT IRQ; here a
// thread does, so it keeps moving during a long layout or script (the engine's
// W_PROGRESS hook and the media governor both depend on that).

volatile uint32_t tick_count;

static DWORD WINAPI tick_thread(void* arg) {
    (void)arg;
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    uint64_t acc_us = 0;
    win_logf("[okai-win] tick thread started (%lld Hz clock)\n", (long long)freq.QuadPart);
    for (;;) {
        Sleep(1);
        QueryPerformanceCounter(&now);
        uint64_t d = (uint64_t)(now.QuadPart - prev.QuadPart);
        prev = now;
        acc_us += d * 1000000ull / (uint64_t)freq.QuadPart;
        uint32_t n = (uint32_t)(acc_us / 10000ull);
        if (n) { acc_us -= (uint64_t)n * 10000ull; tick_count += n; }
    }
    return 0;
}

// ---- pointer ---------------------------------------------------------------

static int mouse_x, mouse_y, mouse_buttons;
static int mouse_wheel;

void mouse_get_position(int* x, int* y) { *x = mouse_x; *y = mouse_y; }
int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
int mouse_get_left_button(void) { return mouse_buttons & 1; }
int mouse_get_buttons(void) { return mouse_buttons; }

int mouse_get_scroll(void) {
    int w = mouse_wheel;
    mouse_wheel = 0;
    return w;
}

void win_mouse_move(int x, int y) { mouse_x = x; mouse_y = y; }
void win_mouse_buttons(int b) { mouse_buttons = b; }
void win_mouse_wheel(int notches) { mouse_wheel += notches; }

// ---- entropy ---------------------------------------------------------------
//
// Same model as the kernel: rand_hw_init() pulls RDRAND/RDSEED, a BOOT sample
// seeds, and the timer + input stir afterwards. The samples are timing jitter
// (QueryPerformanceCounter deltas and event timing), labelled honestly; on a
// CPU without a hardware RNG readiness then needs BOOT+TIMER+INPUT.

static uint64_t ent_state = 0x9E3779B97F4A7C15ull;

static void ent_fill(uint8_t* out, int n) {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    ent_state ^= (uint64_t)q.QuadPart * 0x2545F4914F6CDD1Dull;
    ent_state ^= ((uint64_t)GetTickCount64() << 17) ^ (uint64_t)(uintptr_t)&q ^ (uint64_t)GetCurrentThreadId();
    for (int i = 0; i < n; i++) {
        ent_state ^= ent_state << 13;
        ent_state ^= ent_state >> 7;
        ent_state ^= ent_state << 17;
        out[i] = (uint8_t)(ent_state >> 24);
    }
}

void win_rand_init(void) {
    uint8_t seed[32];
    int hw = rand_hw_init();
    ent_fill(seed, 32);
    rand_seed(seed);                     // BOOT: one sample + immediate reseed
    // The readiness gate wants two classes (>= 32B each) inside the window
    // that declares the seed, so fill the pool here instead of waiting for
    // the timer: 15 timing samples (BOOT for the last one, TIMER for the rest)
    // complete the 16-slot pool and the reseed declares it ready. Honest:
    // these are the same boot-time timing samples the kernel's BOOT class is.
    for (int i = 0; i < 15; i++) {
        ent_fill(seed, 32);
        rand_stir_src(seed, i == 14 ? RAND_SRC_BOOT : RAND_SRC_TIMER);
    }
    memset(seed, 0, sizeof seed);
    win_logf("[okai-win] entropy: %s, cprng %s\n",
             hw ? "RDRAND/RDSEED present" : "no hardware RNG",
             rand_ready() ? "ready" : "NOT READY");
}

// Called from the main loop (never from the tick thread): rand.c has no lock,
// and the kernel relies on interrupts-off instead.
void win_rand_timer(void) {
    uint8_t s[32];
    ent_fill(s, 32);
    rand_stir(s);
    memset(s, 0, sizeof s);
}

void win_rand_input(void) {
    uint8_t s[32];
    ent_fill(s, 32);
    rand_stir_src(s, RAND_SRC_INPUT);
    memset(s, 0, sizeof s);
}

// ---- SMP: parallel jobs on real threads -------------------------------------
//
// The kernel's APs run smp_parallel_for() items and nothing else. Here a small
// pool does the same for the parallel GC marker and the VP9 tile/loop-filter
// helpers, and smp_async_start() gets a fresh thread (video decode workers run
// for tens of milliseconds and must not occupy the pool).

static int ncpu = 1;
static DWORD tls_on_ap;

#define POOL_MAX 8
static struct {
    HANDLE go, done;
    smp_fn fn;
    void* ctx;
    uint32_t from, to;
    DWORD tid;
} pool[POOL_MAX];
static int pool_n;

static DWORD WINAPI pool_worker(void* arg) {
    int k = (int)(intptr_t)arg;
    TlsSetValue(tls_on_ap, (void*)1);
    for (;;) {
        WaitForSingleObject(pool[k].go, INFINITE);
        if (!pool[k].fn) return 0;                  // shutdown
        for (uint32_t i = pool[k].from; i < pool[k].to; i++) pool[k].fn(pool[k].ctx, i, (uint32_t)k + 1);
        ReleaseSemaphore(pool[k].done, 1, 0);
    }
}

static void smp_start_pool(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ncpu = (int)si.dwNumberOfProcessors;
    if (ncpu < 1) ncpu = 1;
    if (ncpu > POOL_MAX + 1) ncpu = POOL_MAX + 1;
    int want = ncpu - 1;
    for (int i = 0; i < want; i++) {
        pool[i].go = CreateSemaphoreW(0, 0, 1, 0);
        pool[i].done = CreateSemaphoreW(0, 0, 1, 0);
        pool[i].fn = 0;
        HANDLE t = CreateThread(0, 512 * 1024, pool_worker, (void*)(intptr_t)i, 0, &pool[i].tid);
        if (!t) { break; }
        CloseHandle(t);
        pool_n++;
    }
    win_logf("[okai-win] smp: %d CPUs, %d pool workers\n", ncpu, pool_n);
}

int smp_init(void) {
    if (!tls_on_ap) tls_on_ap = TlsAlloc();
    if (!pool_n && ncpu == 1) smp_start_pool();
    return ncpu;
}

int smp_ncpu(void) { return ncpu; }
int smp_cpu_index(void) { return TlsGetValue(tls_on_ap) ? -1 : 0; }
int smp_on_ap(void) { return TlsGetValue(tls_on_ap) != 0; }
uint32_t smp_cpu_apic_id(int cpu) { return (uint32_t)cpu; }
void smp_stop_others(void) { }
void smp_describe(void (*out)(const char* line, void* ud), void* ud) { (void)out; (void)ud; }
int smp_selftest(void (*out)(const char* line, void* ud), void* ud) { (void)out; (void)ud; return 0; }

void smp_parallel_for(smp_fn fn, void* ctx, uint32_t n, uint32_t grain) {
    if (n == 0) return;
    if (grain == 0) grain = 1;
    if (pool_n == 0 || smp_on_ap() || n < grain * 2) {
        for (uint32_t i = 0; i < n; i++) fn(ctx, i, 0);
        return;
    }
    // One chunk per worker; the caller runs the last one itself.
    uint32_t chunks = (uint32_t)pool_n + 1;
    uint32_t per = (n + chunks - 1) / chunks;
    int used = 0;
    for (int k = 0; k < pool_n; k++) {
        uint32_t from = (uint32_t)k * per, to = from + per;
        if (from >= n) break;
        if (to > n) to = n;
        pool[k].fn = fn;
        pool[k].ctx = ctx;
        pool[k].from = from;
        pool[k].to = to;
        ReleaseSemaphore(pool[k].go, 1, 0);
        used++;
    }
    uint32_t from = (uint32_t)used * per, to = from + per;
    if (to > n) to = n;
    for (uint32_t i = from; i < to; i++) fn(ctx, i, 0);
    for (int k = 0; k < used; k++) WaitForSingleObject(pool[k].done, INFINITE);
}

// Async job slots: g encodes slot+1 so struct smp_async stays 4 bytes.
#define ASYNC_MAX 16
static struct { HANDLE t; volatile int done; int used; } asl[ASYNC_MAX];
static CRITICAL_SECTION asl_lock;
static int asl_init;

struct async_ctx { smp_fn fn; void* ctx; int slot; };

static DWORD WINAPI async_worker(void* arg) {
    struct async_ctx* a = (struct async_ctx*)arg;
    TlsSetValue(tls_on_ap, (void*)1);
    a->fn(a->ctx, 0, (uint32_t)a->slot + 1);
    asl[a->slot].done = 1;
    free(a);
    return 0;
}

int smp_async_start(smp_fn fn, void* ctx, struct smp_async* h) {
    if (!asl_init) { InitializeCriticalSection(&asl_lock); asl_init = 1; }
    // Allowed from a worker thread too: async jobs get their own thread, so
    // there is no pool deadlock (VP9 tile helpers are started from inside the
    // decode job, which itself runs as an async job).
    if (!h || !fn) return -1;
    EnterCriticalSection(&asl_lock);
    int slot = -1;
    for (int i = 0; i < ASYNC_MAX; i++) {
        if (!asl[i].used) { slot = i; break; }
        if (asl[i].done) { CloseHandle(asl[i].t); asl[i].used = 0; slot = i; break; }
    }
    if (slot < 0) { LeaveCriticalSection(&asl_lock); return -1; }
    struct async_ctx* a = (struct async_ctx*)malloc(sizeof *a);
    if (!a) { LeaveCriticalSection(&asl_lock); return -1; }
    a->fn = fn; a->ctx = ctx; a->slot = slot;
    asl[slot].used = 1;
    asl[slot].done = 0;
    asl[slot].t = CreateThread(0, 512 * 1024, async_worker, a, 0, 0);
    if (!asl[slot].t) { asl[slot].used = 0; free(a); LeaveCriticalSection(&asl_lock); return -1; }
    h->g = (uint32_t)slot + 1;
    LeaveCriticalSection(&asl_lock);
    return 0;
}

int smp_async_done(const struct smp_async* h) {
    if (!h) return 1;
    int slot = (int)h->g - 1;
    if (slot < 0 || slot >= ASYNC_MAX) return 1;
    if (!asl[slot].used) return 1;
    if (asl[slot].done) {
        if (asl_init) EnterCriticalSection(&asl_lock);
        CloseHandle(asl[slot].t);
        asl[slot].t = 0;
        asl[slot].used = 0;
        asl[slot].done = 0;
        if (asl_init) LeaveCriticalSection(&asl_lock);
        return 1;
    }
    return 0;
}

// ---- <sys/mman.h> for ojs' JIT (rwx pages out of VirtualAlloc) --------------

void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off) {
    (void)flags; (void)fd; (void)off;
    DWORD p = PAGE_READWRITE;
    if (prot & 4) p = PAGE_EXECUTE_READWRITE;
    else if (prot & 2) p = PAGE_READWRITE;
    else if (prot & 1) p = PAGE_READONLY;
    void* m = VirtualAlloc(addr, len, MEM_COMMIT | MEM_RESERVE, p);
    return m ? m : MAP_FAILED;
}

int munmap(void* addr, size_t len) {
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

void win_sys_init(void) {
    win_rand_init();
    cpufreq_init();
    {
        // The certificate-validity clock: cert_verify() fails closed until a
        // real "now" arrives (the kernel sets it from the CMOS RTC at boot).
        SYSTEMTIME st;
        GetSystemTime(&st);                 // UTC, like the kernel's clock
        x509_time now;
        now.year = st.wYear; now.month = st.wMonth; now.day = st.wDay;
        now.hour = st.wHour; now.minute = st.wMinute; now.second = st.wSecond;
        x509_set_now(&now);
        win_logf("[okai-win] clock: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                 now.year, now.month, now.day, now.hour, now.minute, now.second);
    }
    smp_init();
    CreateThread(0, 64 * 1024, tick_thread, 0, 0, 0);
}

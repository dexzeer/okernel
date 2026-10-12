#include "smp.h"
#include "lapic.h"
#include "fpu.h"
#include "cpufreq.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "simd.h"
#include "serial.h"
#include "spinlock.h"
#include <stdarg.h>

// ---- state --------------------------------------------------------------

#define AP_STACK_SIZE (256u * 1024u)
#define TRAMP_PAGE    0x08u                 // SIPI vector: AP starts at phys 0x8000
#define NSLOT         16u

enum { CPU_NONE = 0, CPU_STARTING = 1, CPU_ONLINE = 2, CPU_FAILED = 3, CPU_DEAD = 4 };

struct cpu_state {
    uint32_t apic_id;
    volatile int state;
    uint8_t* stack;
    volatile uint32_t items;        // job items this CPU has run (written by that CPU only)
};
static struct cpu_state cpus[SMP_MAX_CPUS];
static int ncpu_total = 1;          // CPUs that run jobs (BSP + online APs)
static volatile int start_idx;      // which AP the trampoline is waking

// One job lives in a slot until NSLOT newer jobs have been published. `gen` is
// zeroed while a slot is being recycled, `active` counts CPUs inside it.
struct job {
    volatile uint32_t gen;
    volatile uint32_t active;
    volatile uint32_t next;
    volatile uint32_t done;
    smp_fn fn;
    void* ctx;
    uint32_t n, grain;
};
static struct job slots[NSLOT];
static volatile uint32_t smp_gen;   // newest published job generation
static uint32_t gen_counter;
static spinlock_t pub_lock = SPINLOCK_INIT;
static volatile int bsp_in_job;      // the boot CPU is inside smp_parallel_for (nested calls run inline)

extern uint8_t ap_tramp_start[], ap_tramp_end[];
extern uint8_t ap_tramp_cr3[], ap_tramp_stack[], ap_tramp_entry[];
extern void isr_ipi_wake(void);
extern void isr_spurious(void);

// ---- helpers ------------------------------------------------------------

static inline void mb(void) { __sync_synchronize(); }
static inline void relax(void) { __asm__ volatile("pause" ::: "memory"); }

int smp_ncpu(void) { return ncpu_total; }
uint32_t smp_cpu_apic_id(int cpu) { return (cpu >= 0 && cpu < SMP_MAX_CPUS) ? cpus[cpu].apic_id : 0; }

static int cpu_of_stack(uint32_t esp) {
    for (int i = 1; i < SMP_MAX_CPUS; i++) {
        uint32_t s = (uint32_t)cpus[i].stack;
        if (s && esp >= s && esp < s + AP_STACK_SIZE) return i;
    }
    return -1;
}

int smp_on_ap(void) {
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    return cpu_of_stack(esp) > 0;
}

int smp_cpu_index(void) {
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    int c = cpu_of_stack(esp);
    return c > 0 ? c : 0;
}

void smp_ipi_wake(void) { lapic_eoi(); }

// ---- job execution ------------------------------------------------------

// Join job generation `g` as CPU `cpu`: claim chunks until none are left.
static void job_join(uint32_t g, uint32_t cpu) {
    struct job* J = &slots[g % NSLOT];
    __sync_fetch_and_add(&J->active, 1);          // full barrier
    if (J->gen != g) {                            // slot was recycled for a newer job
        __sync_fetch_and_sub(&J->active, 1);
        return;
    }
    smp_fn fn = J->fn;
    void* ctx = J->ctx;
    uint32_t n = J->n, grain = J->grain;
    for (;;) {
        uint32_t i = __sync_fetch_and_add(&J->next, grain);
        if (i >= n) break;
        uint32_t e = i + grain;
        if (e > n || e < i) e = n;
        for (uint32_t k = i; k < e; k++) fn(ctx, k, cpu);
        cpus[cpu].items += e - i;
        __sync_fetch_and_add(&J->done, e - i);
    }
    __sync_fetch_and_sub(&J->active, 1);
}

void smp_parallel_for(smp_fn fn, void* ctx, uint32_t n, uint32_t grain) {
    if (!fn || n == 0) return;
    if (ncpu_total < 2 || n < 2 || bsp_in_job || smp_on_ap()) {
        // One CPU, a trivial loop, or called from inside a job: run inline.
        uint32_t cpu = (uint32_t)smp_cpu_index();
        for (uint32_t i = 0; i < n; i++) fn(ctx, i, cpu);
        return;
    }
    if (n > 0x10000000u) n = 0x10000000u;
    if (grain == 0) {
        grain = n / ((uint32_t)ncpu_total * 8u);
        if (grain == 0) grain = 1;
    }
    spin_lock(&pub_lock);
    uint32_t g = ++gen_counter;
    if (g == 0) g = ++gen_counter;                // 0 means "recycling"
    struct job* J = &slots[g % NSLOT];
    J->gen = 0;                                   // new joiners back off...
    mb();
    while (J->active) relax();                    // ...and the ones inside finish
    J->fn = fn; J->ctx = ctx; J->n = n; J->grain = grain;
    J->next = 0; J->done = 0;
    mb();
    J->gen = g;
    mb();
    smp_gen = g;
    bsp_in_job = 1;
    lapic_ipi_others(LAPIC_VEC_WAKE);
    job_join(g, 0);
    uint32_t spins = 0;
    while (J->done != n) {
        relax();
        if (++spins == 0x40000000u) serial_puts("[smp] job is waiting on a CPU that does not finish\n");
    }
    bsp_in_job = 0;
    spin_unlock(&pub_lock);
}

// ---- AP side ------------------------------------------------------------

static void ap_worker(int idx) {
    uint32_t seen = smp_gen;
    for (;;) {
        uint32_t g;
        for (;;) {
            __asm__ volatile("cli" ::: "memory");
            g = smp_gen;
            if (g != seen) break;
            __asm__ volatile("sti; hlt" ::: "memory");   // the IPI wakes us
        }
        seen = g;
        job_join(g, (uint32_t)idx);
    }
}

void smp_ap_entry(void) __attribute__((noreturn));
void smp_ap_entry(void) {
    gdt_load_ap();
    idt_load_ap();
    pat_init();                         // same page attribute table as the boot CPU
    fpu_init_cpu();                     // x87 + SSE as on the boot CPU
    cpufreq_init_ap();                  // same clock request as the boot CPU
    lapic_init(0);
    int idx = start_idx;
    cpus[idx].apic_id = lapic_id();
    mb();
    cpus[idx].state = CPU_ONLINE;
    ap_worker(idx);
    for (;;) __asm__ volatile("cli; hlt");
}

// Called from isr_handler for CPU exceptions. On an AP it is fatal for that
// CPU only: report it and park the CPU. Returns 0 on the boot CPU.
int smp_fault_hook(int int_num, uint32_t* frame) {
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    int c = cpu_of_stack(esp);
    if (c <= 0) return 0;
    cpus[c].state = CPU_DEAD;
    serial_printf("[smp] cpu %d (apic %d) fault vec=%d err=%x eip=%x -- cpu parked\n",
                  c, cpus[c].apic_id, int_num, frame[9], frame[10]);
    for (;;) __asm__ volatile("cli; hlt");
}

// ---- bring-up -----------------------------------------------------------

static int start_ap(int idx, uint32_t apic_id) {
    struct cpu_state* c = &cpus[idx];
    c->stack = (uint8_t*)kmalloc(AP_STACK_SIZE);
    if (!c->stack) { serial_puts("[smp] no memory for an AP stack\n"); return 0; }
    c->apic_id = apic_id;

    uint8_t* tr = (uint8_t*)P2V_U32(TRAMP_PAGE << 12);
    uint32_t len = (uint32_t)(ap_tramp_end - ap_tramp_start);
    for (uint32_t i = 0; i < len; i++) tr[i] = ap_tramp_start[i];
    *(volatile uint32_t*)(tr + (ap_tramp_cr3 - ap_tramp_start)) = paging_kernel_pd_phys();
    *(volatile uint32_t*)(tr + (ap_tramp_stack - ap_tramp_start)) = (uint32_t)c->stack + AP_STACK_SIZE - 4;
    *(volatile uint32_t*)(tr + (ap_tramp_entry - ap_tramp_start)) = (uint32_t)smp_ap_entry;

    start_idx = idx;
    c->state = CPU_STARTING;
    mb();
    lapic_ipi_init(apic_id);
    pit_delay_us(10000);
    lapic_ipi_startup(apic_id, TRAMP_PAGE);
    pit_delay_us(200);
    lapic_ipi_startup(apic_id, TRAMP_PAGE);       // ignored by a CPU that already woke

    for (int ms = 0; ms < 500 && c->state != CPU_ONLINE; ms++) pit_delay_us(1000);
    mb();
    if (c->state != CPU_ONLINE) {
        c->state = CPU_FAILED;
        serial_printf("[smp] cpu apic %d did not start\n", apic_id);
        return 0;
    }
    return 1;
}

#ifdef SMP_BOOT_SELFTEST
static void boot_line(const char* line, void* ud) { (void)ud; serial_printf("[smp] %s\n", line); }
#endif

int smp_init(void) {
    const struct acpi_info* a = acpi_get();
    cpus[0].state = CPU_ONLINE;
    for (int i = 0; i < a->ncpu; i++)             // ids above 254 only exist in x2APIC mode
        if (a->cpu[i].apic_id > 254) lapic_prefer_x2apic();
    if (!lapic_init(1)) {
        serial_puts("[smp] no local APIC -- single CPU\n");
        return 1;
    }
    cpus[0].apic_id = lapic_id();
    serial_printf("[smp] boot cpu apic id %d (%s)\n", cpus[0].apic_id,
                  lapic_is_x2apic() ? "x2APIC" : "xAPIC");
    if (!a->madt || a->ncpu < 2) {
        serial_puts("[smp] one CPU\n");
        return 1;
    }
    idt_set_vector(LAPIC_VEC_WAKE, isr_ipi_wake);
    idt_set_vector(LAPIC_VEC_SPUR, isr_spurious);

    int started = 0;
    for (int i = 0; i < a->ncpu && ncpu_total < SMP_MAX_CPUS; i++) {
        if (!a->cpu[i].enabled || a->cpu[i].apic_id == cpus[0].apic_id) continue;
        if (a->cpu[i].apic_id > 254 && !lapic_is_x2apic()) {
            serial_printf("[smp] cpu apic %d needs x2APIC; skipped\n", a->cpu[i].apic_id);
            continue;
        }
        if (start_ap(ncpu_total, a->cpu[i].apic_id)) { ncpu_total++; started++; }
    }
    serial_printf("[smp] %d CPUs running (%d application processors started)\n", ncpu_total, started);
#ifdef SMP_BOOT_SELFTEST
    smp_selftest(boot_line, 0);          // test builds: run the self test right here
#endif
    return ncpu_total;
}

// ---- reporting ----------------------------------------------------------

static void kfmt(char* dst, int cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    for (const char* p = fmt; *p && n < cap - 1; p++) {
        if (*p != '%') { dst[n++] = *p; continue; }
        p++;
        if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            while (s && *s && n < cap - 1) dst[n++] = *s++;
        } else if (*p == 'u' || *p == 'x') {
            uint32_t v = va_arg(ap, uint32_t), base = (*p == 'u') ? 10 : 16;
            char t[12]; int k = 0;
            if (!v) t[k++] = '0';
            while (v) { uint32_t d = v % base; t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; }
            while (k && n < cap - 1) dst[n++] = t[--k];
        } else if (*p == '%') dst[n++] = '%';
    }
    dst[n] = 0;
    va_end(ap);
}

void smp_describe(void (*out)(const char*, void*), void* ud) {
    static const char* names[] = { "unused", "starting", "online", "failed to start", "dead (fault)" };
    char b[96];
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        if (i > 0 && cpus[i].state == CPU_NONE) continue;
        kfmt(b, sizeof b, "cpu %u apic %u %s%s items %u", (uint32_t)i, cpus[i].apic_id,
             names[cpus[i].state], i == 0 ? " (boot)" : "", cpus[i].items);
        out(b, ud);
    }
}

// ---- self test ----------------------------------------------------------

static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

// CPU-bound filler: `iters` rounds of a xorshift chain.
static uint32_t burn(uint32_t seed, uint32_t iters) {
    uint32_t x = seed | 1;
    for (uint32_t i = 0; i < iters; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
    return x;
}

struct t_hash { uint32_t* out; uint32_t per_cpu[SMP_MAX_CPUS]; uint32_t iters; };
static void t_hash_fn(void* c, uint32_t i, uint32_t cpu) {
    struct t_hash* t = (struct t_hash*)c;
    t->out[i] = burn(hash32(i), t->iters) ^ i;
    __sync_fetch_and_add(&t->per_cpu[cpu], 1);
}

static volatile uint32_t t_counter;
static void t_count_fn(void* c, uint32_t i, uint32_t cpu) {
    (void)c; (void)i; (void)cpu;
    __sync_fetch_and_add(&t_counter, 1);
}

struct t_heap { volatile uint32_t bad; };
static void t_heap_fn(void* c, uint32_t i, uint32_t cpu) {
    struct t_heap* t = (struct t_heap*)c;
    (void)cpu;
    uint32_t sz = 16 + hash32(i) % 3000;
    uint8_t* p = (uint8_t*)kmalloc(sz);
    if (!p) { __sync_fetch_and_add(&t->bad, 1); return; }
    uint8_t pat = (uint8_t)(i * 7 + 1);
    for (uint32_t k = 0; k < sz; k++) p[k] = pat;
    for (uint32_t k = 0; k < sz; k++) if (p[k] != pat) { __sync_fetch_and_add(&t->bad, 1); break; }
    kfree(p);
}

// SSE2 on every CPU: each item copies and fills a block through the XMM helpers
// and verifies it (an AP without CR4.OSFXSR would take #UD and park).
struct t_sse { uint8_t* src; uint8_t* dst; volatile uint32_t bad; };
#define T_SSE_BLK 65536u
static void t_sse_fn(void* c, uint32_t i, uint32_t cpu) {
    struct t_sse* t = (struct t_sse*)c;
    (void)cpu;
    uint8_t* s = t->src + i * T_SSE_BLK;
    uint8_t* d = t->dst + i * T_SSE_BLK;
    simd_copy_stream(d, s, T_SSE_BLK - (i & 7));            // odd sizes too
    for (uint32_t k = 0; k < T_SSE_BLK - (i & 7); k++) if (d[k] != s[k]) { __sync_fetch_and_add(&t->bad, 1); break; }
    simd_fill32((uint32_t*)d, 0xA5A5A5A5u ^ i, T_SSE_BLK / 4);
    for (uint32_t k = 0; k < T_SSE_BLK / 4; k++) if (((uint32_t*)d)[k] != (0xA5A5A5A5u ^ i)) { __sync_fetch_and_add(&t->bad, 1); break; }
}

static void t_nest_inner(void* c, uint32_t i, uint32_t cpu) { (void)c; (void)i; (void)cpu; __sync_fetch_and_add(&t_counter, 1); }
static void t_nest_outer(void* c, uint32_t i, uint32_t cpu) {
    (void)c; (void)i; (void)cpu;
    smp_parallel_for(t_nest_inner, 0, 8, 1);       // runs inline on an AP, parallel on the boot CPU path
}

static inline uint64_t tsc(void) { uint32_t a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return ((uint64_t)d << 32) | a; }

int smp_selftest(void (*out)(const char*, void*), void* ud) {
    char b[120];
    int fails = 0;
    kfmt(b, sizeof b, "cpus running: %u", (uint32_t)ncpu_total); out(b, ud);

    // A: results and distribution
    {
        const uint32_t N = 2048;
        struct t_hash* t = (struct t_hash*)kcalloc(1, sizeof *t);
        uint32_t* res = (uint32_t*)kmalloc(N * 4);
        if (!t || !res) { out("A: out of memory", ud); return 1; }
        t->out = res; t->iters = 20000;
        smp_parallel_for(t_hash_fn, t, N, 0);
        int bad = 0; uint32_t total = 0, used = 0;
        for (uint32_t i = 0; i < N; i++) if (res[i] != (burn(hash32(i), t->iters) ^ i)) bad++;
        for (int c = 0; c < SMP_MAX_CPUS; c++) { total += t->per_cpu[c]; if (t->per_cpu[c]) used++; }
        kfmt(b, sizeof b, "A parallel results: %s (items %u, cpus used %u)",
             (bad == 0 && total == N) ? "ok" : "FAIL", total, used);
        out(b, ud);
        if (bad || total != N) fails++;
        if (ncpu_total > 1 && used < 2) { out("A: only one CPU took part", ud); fails++; }
        kfree(t); kfree(res);
    }
    // B: publish/recycle protocol under many tiny jobs
    {
        t_counter = 0;
        uint32_t expect = 0;
        for (uint32_t j = 0; j < 20000; j++) {
            uint32_t n = 1 + j % 37;
            smp_parallel_for(t_count_fn, 0, n, 1);
            expect += n;
        }
        kfmt(b, sizeof b, "B 20000 small jobs: %s (%u of %u items)", t_counter == expect ? "ok" : "FAIL", t_counter, expect);
        out(b, ud);
        if (t_counter != expect) fails++;
    }
    // C: heap lock under contention
    {
        uint32_t t0, u0, u1;
        heap_stats(&t0, &u0, 0, 0);
        struct t_heap h = { 0 };
        smp_parallel_for(t_heap_fn, &h, 20000, 16);
        heap_stats(&t0, &u1, 0, 0);
        kfmt(b, sizeof b, "C heap contention: %s (corrupt %u, heap used %u -> %u)",
             (h.bad == 0 && u0 == u1) ? "ok" : "FAIL", h.bad, u0, u1);
        out(b, ud);
        if (h.bad || u0 != u1) fails++;
    }
    // D: nested calls
    {
        t_counter = 0;
        smp_parallel_for(t_nest_outer, 0, 64, 1);
        kfmt(b, sizeof b, "D nested calls: %s (%u of 512)", t_counter == 512 ? "ok" : "FAIL", t_counter);
        out(b, ud);
        if (t_counter != 512) fails++;
    }
    // F: SSE2 on the other CPUs
    {
        const uint32_t N = 64;
        struct t_sse t = { (uint8_t*)kmalloc(N * T_SSE_BLK), (uint8_t*)kmalloc(N * T_SSE_BLK), 0 };
        if (t.src && t.dst) {
            for (uint32_t k = 0; k < N * T_SSE_BLK; k++) t.src[k] = (uint8_t)(hash32(k) >> 8);
            smp_parallel_for(t_sse_fn, &t, N, 1);
            kfmt(b, sizeof b, "F sse2 on every cpu: %s (bad %u)", t.bad == 0 ? "ok" : "FAIL", t.bad);
            out(b, ud);
            if (t.bad) fails++;
        }
        if (t.src) kfree(t.src);
        if (t.dst) kfree(t.dst);
    }
    // E: speed
    {
        const uint32_t N = 512;
        struct t_hash* t = (struct t_hash*)kcalloc(1, sizeof *t);
        uint32_t* res = (uint32_t*)kmalloc(N * 4);
        if (t && res) {
            t->out = res; t->iters = 20000;
            uint64_t a0 = tsc();
            for (uint32_t i = 0; i < N; i++) t_hash_fn(t, i, 0);
            uint32_t serial = (uint32_t)((tsc() - a0) >> 10);
            a0 = tsc();
            smp_parallel_for(t_hash_fn, t, N, 0);
            uint32_t par = (uint32_t)((tsc() - a0) >> 10);
            if (par == 0) par = 1;
            uint32_t x100 = (serial * 100u) / par;
            kfmt(b, sizeof b, "E speedup on %u cpus: %u.%u%ux", (uint32_t)ncpu_total, x100 / 100, (x100 / 10) % 10, x100 % 10);
            out(b, ud);
        }
        if (t) kfree(t);
        if (res) kfree(res);
    }
    kfmt(b, sizeof b, "smp selftest: %s", fails ? "FAILED" : "ALL OK"); out(b, ud);
    return fails;
}

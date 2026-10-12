// CPU clock: measure it, ask for the top ratio on Atom cores (see cpufreq.h).
#include "cpufreq.h"
#include "lapic.h"
#include "serial.h"
#include "devlog.h"
#include <stddef.h>

// Silvermont / Airmont family (Bay Trail 0x37, Cherry Trail + Braswell 0x4C, Moorefield 0x5A, SoFIA 0x5D).
#define MSR_PLATFORM_INFO  0x0CE
#define MSR_PERF_STATUS    0x198
#define MSR_PERF_CTL       0x199
#define MSR_MISC_ENABLE    0x1A0
#define MSR_ENERGY_BIAS    0x1B0
#define MSR_ATOM_RATIOS    0x66A     // bits 14:8 lowest-power ratio, 22:16 highest guaranteed ratio
#define MSR_ATOM_TURBO     0x66C     // bits 6:0 highest turbo ratio (one core active)

// A fault-safe RDMSR/WRMSR: isr_handler (idt.c) resumes at msr_fix_to when a #GP hits msr_fix_from.
volatile uint32_t msr_fix_from, msr_fix_to;

static int rdmsr_safe(uint32_t msr, uint64_t* v) {
    uint32_t lo = 0, hi = 0, ok;
    __asm__ volatile(
        "movl $1f, msr_fix_from\n\t"
        "movl $2f, msr_fix_to\n\t"
        "1: rdmsr\n\t"
        "movl $1, %[ok]\n\t"
        "jmp 3f\n"
        "2: movl $0, %[ok]\n"
        "3:\n\t"
        "movl $0, msr_fix_from\n\t"
        : "=a"(lo), "=d"(hi), [ok] "=&r"(ok)
        : "c"(msr)
        : "memory", "cc");
    *v = ((uint64_t)hi << 32) | lo;
    return (int)ok;
}

static int wrmsr_safe(uint32_t msr, uint64_t v) {
    uint32_t ok;
    __asm__ volatile(
        "movl $1f, msr_fix_from\n\t"
        "movl $2f, msr_fix_to\n\t"
        "1: wrmsr\n\t"
        "movl $1, %[ok]\n\t"
        "jmp 3f\n"
        "2: movl $0, %[ok]\n"
        "3:\n\t"
        "movl $0, msr_fix_from\n\t"
        : [ok] "=&r"(ok)
        : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))
        : "memory", "cc");
    return (int)ok;
}

static inline uint64_t tsc(void) {
    uint32_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}

static uint32_t tsc_mhz, core_mhz, core_mhz_before, model, family;
static uint32_t target_ratio;
static int bench_factor = 1;
static char rep[1200];
static int rep_n;

static void put_s(const char* s) { while (*s && rep_n < (int)sizeof rep - 1) rep[rep_n++] = *s++; rep[rep_n] = 0; }
static void put_u(uint32_t v) {
    char t[12]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) { if (rep_n < (int)sizeof rep - 1) rep[rep_n++] = t[--k]; else --k; }
    rep[rep_n] = 0;
}
static void put_x(uint64_t v) {
    char t[20]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = "0123456789abcdef"[v & 15]; v >>= 4; }
    while (k) { if (rep_n < (int)sizeof rep - 1) rep[rep_n++] = t[--k]; else --k; }
    rep[rep_n] = 0;
}

// iters * 8 dependent increments: one cycle each on every x86 core, so cycles = 8 * iters
static uint64_t spin(uint32_t iters) {
    uint64_t t0 = tsc();
    uint32_t a = 0;
    __asm__ volatile(
        "1: inc %%eax; inc %%eax; inc %%eax; inc %%eax\n\t"
        "inc %%eax; inc %%eax; inc %%eax; inc %%eax\n\t"
        "dec %%ecx; jnz 1b"
        : "+c"(iters), "+a"(a) :: "cc");
    return tsc() - t0;
}

static uint32_t measure_core_mhz(void) {
    if (!tsc_mhz) return 0;
    const uint32_t iters = 400000;                    // 3.2M cycles: 1-3 ms
    uint64_t best = ~0ull;
    for (int i = 0; i < 5; i++) {
        uint64_t d = spin(iters);
        if (d < best) best = d;
    }
    if (!best) return 0;
    return (uint32_t)(((uint64_t)tsc_mhz * 8ull * iters + best / 2) / best);
}

static void calibrate_tsc(void) {
    uint64_t t0 = tsc();
    pit_delay_us(20000);
    uint64_t d = tsc() - t0;
    tsc_mhz = (uint32_t)((d + 10000) / 20000);
}

// A scalar workload shaped like the interpreters it stands for: a dependent chain of table lookups with a data-dependent
// branch per step. Returns microseconds for a fixed amount of work.
static uint32_t bench_us(void) {
    static uint32_t tab[4096];                        // 16 KB: stays in L1/L2 like an interpreter's hot data
    uint32_t x = 12345;
    for (int i = 0; i < 4096; i++) { x = x * 1664525u + 1013904223u; tab[i] = x; }
    uint64_t t0 = tsc();
    uint32_t p = 1, acc = 0;
    for (int i = 0; i < 600000; i++) {
        uint32_t v = tab[p & 4095];
        if (v & 1) acc += v >> 3; else acc ^= v << 2;
        if (v & 0x100) p = v ^ acc; else p = v + i;
    }
    uint64_t d = tsc() - t0;
    __asm__ volatile("" :: "r"(acc));
    return tsc_mhz ? (uint32_t)(d / tsc_mhz) : 0;
}

#define BENCH_REF_US 1630       // microseconds bench_us takes on the development PC (4.2 GHz desktop core, see the log line "[cpu]"): factor 1

static int model_ok(void) {
    return family == 6 && (model == 0x37 || model == 0x4C || model == 0x5A || model == 0x5D);
}

static void request_ratio(void) {
    if (!target_ratio) return;
    uint64_t v;
    if (rdmsr_safe(MSR_MISC_ENABLE, &v) && !(v & (1ull << 16)))
        wrmsr_safe(MSR_MISC_ENABLE, v | (1ull << 16));          // enhanced SpeedStep on (the request below is ignored without it)
    wrmsr_safe(MSR_PERF_CTL, (uint64_t)target_ratio << 8);
}

void cpufreq_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    family = (a >> 8) & 15;
    model = ((a >> 4) & 15) | (((a >> 16) & 15) << 4);
    if (family == 15) family += (a >> 20) & 255;
    calibrate_tsc();
    core_mhz_before = measure_core_mhz();
    uint32_t us = bench_us();
    if (us) {
        int f = (int)((us + BENCH_REF_US / 2) / BENCH_REF_US);
        bench_factor = f < 1 ? 1 : f > 16 ? 16 : f;
    }
    put_s("family "); put_u(family); put_s(" model 0x"); put_x(model);
    put_s(": TSC "); put_u(tsc_mhz); put_s(" MHz, core "); put_u(core_mhz_before); put_s(" MHz, bench "); put_u(us); put_s(" us (x"); put_u((uint32_t)bench_factor); put_s(")");
    if (model_ok()) {
        uint64_t pi = 0, ratios = 0, turbo = 0, st = 0, misc = 0, epb = 0;
        int have_pi = rdmsr_safe(MSR_PLATFORM_INFO, &pi);
        int have_r = rdmsr_safe(MSR_ATOM_RATIOS, &ratios);
        int have_t = rdmsr_safe(MSR_ATOM_TURBO, &turbo);
        int have_st = rdmsr_safe(MSR_PERF_STATUS, &st);
        int have_m = rdmsr_safe(MSR_MISC_ENABLE, &misc);
        put_s("; platform "); if (have_pi) put_x(pi); else put_s("-");
        put_s(" ratios "); if (have_r) put_x(ratios); else put_s("-");
        put_s(" turbo "); if (have_t) put_x(turbo); else put_s("-");
        put_s(" status "); if (have_st) put_x(st); else put_s("-");
        put_s(" misc "); if (have_m) put_x(misc); else put_s("-");
        uint32_t maxr = have_r ? (uint32_t)((ratios >> 16) & 0x7F) : (have_pi ? (uint32_t)((pi >> 8) & 0xFF) : 0);
        uint32_t turbor = have_t ? (uint32_t)(turbo & 0x7F) : 0;
        uint32_t want = turbor > maxr ? turbor : maxr;
        if (!(have_m && (misc & (1ull << 38))) && want >= 4 && want <= 60) target_ratio = want;   // turbo allowed (bit 38 = disabled)
        else if (maxr >= 4 && maxr <= 60) target_ratio = maxr;
        if (rdmsr_safe(MSR_ENERGY_BIAS, &epb)) { put_s(" epb "); put_x(epb); wrmsr_safe(MSR_ENERGY_BIAS, 0); }
        put_s("; request ratio "); put_u(target_ratio);
        request_ratio();
        pit_delay_us(5000);
        core_mhz = measure_core_mhz();
        uint64_t st2 = 0;
        if (rdmsr_safe(MSR_PERF_STATUS, &st2)) { put_s("; status now "); put_x(st2); }
        put_s("; core now "); put_u(core_mhz); put_s(" MHz");
    } else {
        core_mhz = core_mhz_before;
        put_s("; clock control not used on this CPU");
    }
    serial_puts("[cpu] "); serial_puts(rep); serial_puts("\n");
    devlog('c', "cpu: %s", rep);
}

void cpufreq_init_ap(void) {
    if (model_ok()) request_ratio();
}

void cpufreq_report(char* buf, int cap) {
    int n = 0;
    const char* s = rep;
    while (*s && n < cap - 1) buf[n++] = *s++;
    buf[n] = 0;
}

uint32_t cpufreq_mhz(void) { return core_mhz; }
uint32_t cpufreq_tsc_mhz(void) { return tsc_mhz; }
int cpufreq_slow_factor(void) { return bench_factor; }


// Self test of the fault recovery that rdmsr_safe/wrmsr_safe use. Real emulators and CPUs differ in which MSRs fault, so the
// test provokes a #GP that is the same on all of them (loading a segment register with a selector far outside the GDT --
// nothing is changed, the load faults first) and checks that execution resumes at the recovery label.
int cpufreq_msr_selftest(void) {
    uint32_t ok;
    __asm__ volatile(
        "movl $1f, msr_fix_from\n\t"
        "movl $2f, msr_fix_to\n\t"
        "movw $0x7FF8, %%ax\n\t"
        "1: movw %%ax, %%gs\n\t"
        "movl $0, %[ok]\n\t"                      // the load worked: not what a bad selector should do
        "jmp 3f\n"
        "2: movl $1, %[ok]\n"                     // resumed here after the #GP
        "3:\n\t"
        "movl $0, msr_fix_from\n\t"
        : [ok] "=&r"(ok)
        :
        : "eax", "memory", "cc");
    uint64_t v = 0;
    return ok && rdmsr_safe(0x1B, &v);            // and a real MSR still reads
}

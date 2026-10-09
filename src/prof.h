#ifndef PROF_H
#define PROF_H

#include <stdint.h>

// Cycle profiler for the desktop loop. Off by default (shell: `prof on`);
// when on, a `[prof]` line with Mcycles per zone is logged once a second next
// to `[fps]`. Zones may nest (PAINT runs inside JOB inside OKAI).
enum {
    PZ_KEYS, PZ_OKAI, PZ_WINDOWS, PZ_TASKBAR, PZ_FLUSH, PZ_HALT,
    PZ_JOB, PZ_PAINT, PZ_SCROLL, PZ_BLIT,
    PZ_COUNT
};

extern volatile int prof_on;
extern uint64_t prof_cyc[PZ_COUNT];
extern uint32_t prof_cnt[PZ_COUNT];

static inline uint64_t prof_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define PROF_BEGIN(z) uint64_t _pt_##z = prof_on ? prof_tsc() : 0
#define PROF_END(z) do { if (prof_on) { prof_cyc[z] += prof_tsc() - _pt_##z; prof_cnt[z]++; } } while (0)

// Log and reset (called once a second by the main loop).
void prof_report(void);

#endif

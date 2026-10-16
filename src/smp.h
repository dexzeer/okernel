#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "acpi.h"

// Symmetric multiprocessing, deliberately small: every extra core (an AP)
// runs a worker that executes jobs handed over by the boot CPU. The AP never
// touches the scheduler, the process table or any window/browser state, so
// none of that code has to be reentrant. Jobs are data parallel loops:
//
//     smp_parallel_for(fn, ctx, n, grain)   // fn(ctx, i, cpu) for i in [0, n)
//
// The caller takes part in the loop and returns when every item is done.
// With one CPU (or on an AP) the loop simply runs inline.
//
// Rules for `fn`: it runs on a 256KB private stack with interrupts off; it may
// use kmalloc/kfree (the heap is locked) and memory the caller owns, but it
// must not touch serial/graphics/windows, block, or call smp_parallel_for
// (that runs inline on an AP). It may use x87 and, once enabled, SSE.

typedef void (*smp_fn)(void* ctx, uint32_t item, uint32_t cpu);

// Probe ACPI/APIC and start the other CPUs. Safe to call with 1 CPU or no
// ACPI; returns the number of CPUs running (>= 1).
int smp_init(void);
int smp_ncpu(void);                  // CPUs running jobs, including the boot CPU
int smp_cpu_index(void);             // 0 = boot CPU; -1 if unknown
uint32_t smp_cpu_apic_id(int cpu);
int smp_on_ap(void);                 // the caller runs on an AP stack

void smp_parallel_for(smp_fn fn, void* ctx, uint32_t n, uint32_t grain);

// Run fn(ctx, 0, cpu) ONCE on some idle AP and return at once (the caller does not take part and does not wait). Poll
// smp_async_done() for completion; `ctx` and everything fn touches must stay valid and unshared until then. The same rules as for
// parallel jobs apply to fn (no serial/graphics/windows, no blocking; kmalloc and SSE are fine). Returns -1 (nothing started) when
// there is no AP, the caller is one, or all 8 mailbox entries are busy: run the work inline instead. Several jobs may be outstanding at once;
// each runs exactly once (they do not travel through the broadcast slots of smp_parallel_for).
struct smp_async { uint32_t g; };
int smp_async_start(smp_fn fn, void* ctx, struct smp_async* h);
int smp_async_done(const struct smp_async* h);

// One line per CPU: apic id, state and how many items it has executed.
void smp_describe(void (*out)(const char* line, void* ud), void* ud);

// Self test (jobs, heap contention, publish/reuse protocol, speedup).
// Returns 0 when everything held; `out` receives the report lines.
int smp_selftest(void (*out)(const char* line, void* ud), void* ud);

#endif

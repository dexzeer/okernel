#ifndef CPUFREQ_H
#define CPUFREQ_H

#include <stdint.h>

// CPU clock control for Intel Atom (Silvermont / Airmont: Bay Trail, Cherry Trail, Braswell). The firmware leaves the P-state
// request wherever it likes and nothing here ever asked for more, so a laptop could run its cores far below the rated speed.
//   cpufreq_init:    boot CPU, before the other CPUs start: measure the real core clock (a dependent-instruction loop timed
//                    against the TSC, which runs at a fixed rate), read the ratio MSRs, request the highest ratio, measure
//                    again, log both.
//   cpufreq_init_ap: every other CPU (smp.c), repeats the request on its own core.
//   cpufreq_report:  text for the `cpu` shell command and the stick log.
// Only the models listed in cpufreq.c are touched; MSR accesses are fault-safe (#GP is caught and skipped).
void cpufreq_init(void);
void cpufreq_init_ap(void);
void cpufreq_report(char* buf, int cap);
uint32_t cpufreq_mhz(void);            // last measured core clock in MHz (0 = unknown)
uint32_t cpufreq_tsc_mhz(void);        // TSC rate in MHz (0 = unknown)
int cpufreq_slow_factor(void);         // 1 = fast machine ... 16 = very slow: JS and task budgets scale by this (see cpufreq.c)
int cpufreq_msr_selftest(void);        // 1 = a missing MSR faulted harmlessly and a present one read fine

#endif

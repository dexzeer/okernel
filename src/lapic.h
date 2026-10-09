#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

// Local APIC driver (xAPIC MMIO, or x2APIC MSRs when the firmware left the
// CPU in that mode). The legacy 8259 PIC stays the interrupt controller for
// devices; the LAPIC is used for inter-processor interrupts only.

#define LAPIC_VEC_WAKE   0xF0   // "look at the job queue" IPI
#define LAPIC_VEC_SPUR   0xFF   // spurious interrupt vector

// CPUID says an APIC exists.
int lapic_supported(void);
// Phys base the CPU currently uses (IA32_APIC_BASE), 0 if none. paging_init
// maps the window before any process exists.
uint32_t lapic_phys_base(void);

// Bring up the calling CPU's LAPIC. `bsp` selects the PIC pass-through
// (LINT0 = ExtINT) that only the boot CPU keeps. Returns 0 on failure.
int lapic_init(int bsp);
int lapic_ready(void);
int lapic_x2apic_supported(void);
// Ask for x2APIC mode when the boot CPU is initialised (call before lapic_init(1));
// ignored on CPUs without it. Required once APIC ids no longer fit in 8 bits.
void lapic_prefer_x2apic(void);
int lapic_is_x2apic(void);

uint32_t lapic_id(void);            // the calling CPU's APIC id
void lapic_eoi(void);

// Inter-processor interrupts. `dest` is an APIC id.
void lapic_ipi(uint32_t dest, uint32_t icr_lo);
void lapic_ipi_others(uint32_t vector);          // all CPUs except the caller
void lapic_ipi_init(uint32_t dest);
void lapic_ipi_startup(uint32_t dest, uint8_t page);   // SIPI: start at page*4096

// Busy wait for `us` microseconds (PIT channel 2, independent of the tick).
void pit_delay_us(uint32_t us);

// MSR access.
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

#endif

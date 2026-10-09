#include "lapic.h"
#include "io.h"
#include "serial.h"

#define MSR_APIC_BASE   0x1B
#define APIC_BASE_X2    (1u << 10)
#define APIC_BASE_EN    (1u << 11)

#define R_ID    0x020
#define R_VER   0x030
#define R_TPR   0x080
#define R_EOI   0x0B0
#define R_SVR   0x0F0
#define R_ESR   0x280
#define R_ICR   0x300
#define R_ICRH  0x310
#define R_LVT_TIMER 0x320
#define R_LVT_THERM 0x330
#define R_LVT_PERF  0x340
#define R_LVT_LINT0 0x350
#define R_LVT_LINT1 0x360
#define R_LVT_ERR   0x370

#define LVT_MASKED 0x00010000u

static volatile uint32_t* mmio;     // xAPIC window (identity-mapped uncached by paging_init)
static int x2;
static int ready;
static int want_x2;     // switch the CPUs to x2APIC mode (needed for APIC ids above 254)

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

int lapic_supported(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    return (d >> 9) & 1;
}

uint32_t lapic_phys_base(void) {
    if (!lapic_supported()) return 0;
    return (uint32_t)rdmsr(MSR_APIC_BASE) & 0xFFFFF000u;
}

static uint32_t rd(uint32_t reg) {
    return x2 ? (uint32_t)rdmsr(0x800 + (reg >> 4)) : mmio[reg >> 2];
}
static void wr(uint32_t reg, uint32_t v) {
    if (x2) wrmsr(0x800 + (reg >> 4), v);
    else mmio[reg >> 2] = v;
}

int lapic_ready(void) { return ready; }

int lapic_x2apic_supported(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    return (c >> 21) & 1;
}

void lapic_prefer_x2apic(void) {
#ifndef LAPIC_NO_X2APIC
    if (lapic_x2apic_supported()) want_x2 = 1;
#endif
}
int lapic_is_x2apic(void) { return x2; }

uint32_t lapic_id(void) {
    uint32_t v = rd(R_ID);
    return x2 ? v : (v >> 24);
}

void lapic_eoi(void) { wr(R_EOI, 0); }

int lapic_init(int bsp) {
    if (!lapic_supported()) return 0;
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (!(base & APIC_BASE_EN)) {                // firmware left it globally disabled
        base |= APIC_BASE_EN;
        wrmsr(MSR_APIC_BASE, base);
        base = rdmsr(MSR_APIC_BASE);
        if (!(base & APIC_BASE_EN)) return 0;
    }
    // Mode: stay in whatever the firmware chose, unless x2APIC was asked for
    // (boot CPU) or the boot CPU already switched (APs come out of INIT in
    // xAPIC mode and must follow it).
    int to_x2 = (base & APIC_BASE_X2) ? 1 : (bsp ? want_x2 : x2);
#ifdef LAPIC_FORCE_X2APIC
    if (bsp && lapic_x2apic_supported()) to_x2 = 1;      // test builds
#endif
    if (to_x2 && !(base & APIC_BASE_X2) && lapic_x2apic_supported()) {
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_X2);
        base = rdmsr(MSR_APIC_BASE);
    }
    x2 = (base & APIC_BASE_X2) ? 1 : 0;
    if (!x2) mmio = (volatile uint32_t*)((uint32_t)base & 0xFFFFF000u);

    // Software-enable and set the spurious vector; accept every priority.
    wr(R_SVR, 0x100 | LAPIC_VEC_SPUR);
    wr(R_TPR, 0);

    // Virtual wire: the boot CPU keeps taking the 8259's interrupts through
    // LINT0 (ExtINT, delivery mode 111b). Other CPUs must not see them.
    wr(R_LVT_LINT0, bsp ? 0x700u : LVT_MASKED);
    wr(R_LVT_LINT1, bsp ? 0x400u : LVT_MASKED);          // NMI
    wr(R_LVT_TIMER, LVT_MASKED);
    wr(R_LVT_ERR, LVT_MASKED | 0xFE);
    uint32_t maxlvt = (rd(R_VER) >> 16) & 0xFF;
    if (maxlvt >= 4) wr(R_LVT_PERF, LVT_MASKED);
    if (maxlvt >= 5) wr(R_LVT_THERM, LVT_MASKED);
    wr(R_ESR, 0);
    wr(R_ESR, 0);
    if (bsp) ready = 1;
    return 1;
}

// Wait for the ICR's delivery-status bit (xAPIC; x2APIC has none).
static void icr_wait(void) {
    if (x2) return;
    for (int i = 0; i < 1000000 && (rd(R_ICR) & 0x1000); i++) __asm__ volatile("pause");
}

void lapic_ipi(uint32_t dest, uint32_t icr_lo) {
    if (x2) {
        wrmsr(0x830, ((uint64_t)dest << 32) | icr_lo);
    } else {
        icr_wait();
        wr(R_ICRH, dest << 24);
        wr(R_ICR, icr_lo);
    }
}

void lapic_ipi_others(uint32_t vector) {
    // fixed delivery, level assert, destination shorthand "all excluding self"
    uint32_t lo = vector | (1u << 14) | (3u << 18);
    if (x2) wrmsr(0x830, lo);
    else { icr_wait(); wr(R_ICR, lo); }
}

void lapic_ipi_init(uint32_t dest) { lapic_ipi(dest, 0x4500); }

void lapic_ipi_startup(uint32_t dest, uint8_t page) { lapic_ipi(dest, 0x4600u | page); }

// PIT channel 2, mode 0 (interrupt on terminal count), gate driven from port
// 0x61. It is untouched by the 100 Hz tick programmed on channel 0.
void pit_delay_us(uint32_t us) {
    while (us) {
        uint32_t chunk = us > 50000 ? 50000 : us;
        uint32_t count = chunk * 1193u + (chunk * 182u) / 1000u;
        if (count == 0) count = 1;
        outb(0x61, (uint8_t)((inb(0x61) & 0xFD) | 0x01));      // gate on, speaker off
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(count & 0xFF));
        outb(0x42, (uint8_t)(count >> 8));
        for (uint32_t spin = 0; spin < 20000000u && !(inb(0x61) & 0x20); spin++) { }
        us -= chunk;
    }
}

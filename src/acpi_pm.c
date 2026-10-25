#include "acpi_pm.h"

static uint32_t pm_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// FADT layout (ACPI 6): DSDT 40, SMI_CMD 48, ACPI_ENABLE 52, PM1a_CNT_BLK 64, PM1b_CNT_BLK 68, RESET_REG 116 (a
// 12-byte Generic Address: space id, width, offset, access, 64-bit address) + RESET_VALUE 128, flags 112,
// X_DSDT 140, X_PM1a_CNT_BLK 172, SLEEP_CONTROL_REG 244.
int acpi_parse_fadt(const uint8_t* t, uint32_t len, struct acpi_pm* out) {
    if (len < 76) return 0;
    out->valid = 0; out->dsdt = 0; out->smi_cmd = 0; out->acpi_enable = 0; out->pm1a_cnt = out->pm1b_cnt = 0;
    out->hw_reduced = 0; out->sleep_ctl_port = 0; out->reset_ok = 0; out->reset_port = 0; out->reset_value = 0;
    out->have_s5 = 0; out->slp_typa = out->slp_typb = 0;
    out->dsdt = pm_rd32(t + 40);
    if (len >= 148 && pm_rd32(t + 140 + 4) == 0 && pm_rd32(t + 140)) out->dsdt = pm_rd32(t + 140);      // X_DSDT below 4GB
    out->smi_cmd = pm_rd32(t + 48) & 0xFFFF;
    out->acpi_enable = t[52];
    out->pm1a_cnt = pm_rd32(t + 64) & 0xFFFF;
    out->pm1b_cnt = pm_rd32(t + 68) & 0xFFFF;
    uint32_t flags = len >= 116 ? pm_rd32(t + 112) : 0;
    if (len >= 184 && !out->pm1a_cnt && t[172] == 1 && pm_rd32(t + 176 + 4) == 0) out->pm1a_cnt = pm_rd32(t + 176) & 0xFFFF;   // X_PM1a in I/O space
    if (flags & (1u << 20)) {                                   // hardware-reduced ACPI
        out->hw_reduced = 1;
        if (len >= 252 && t[244] == 1 && pm_rd32(t + 248 + 4) == 0) out->sleep_ctl_port = pm_rd32(t + 248) & 0xFFFF;
    }
    if ((flags & (1u << 10)) && len >= 129 && t[116] == 1 && pm_rd32(t + 120 + 4) == 0) {   // RESET_REG_SUP, I/O space
        out->reset_ok = 1; out->reset_port = pm_rd32(t + 120) & 0xFFFF; out->reset_value = t[128];
    }
    out->valid = out->pm1a_cnt != 0 || out->sleep_ctl_port != 0;
    return out->valid;
}

// AML: Name(_S5_, Package() { SLP_TYPa, SLP_TYPb, 0, 0 }) = 08 5F 53 35 5F 12 <PkgLength> <NumElements> then the
// elements, each a ZeroOp, OneOp or a BytePrefix (0A) + byte.
int acpi_find_s5(const uint8_t* a, uint32_t len, uint8_t* typa, uint8_t* typb) {
    for (uint32_t i = 1; i + 12 < len; i++) {
        if (a[i] != '_' || a[i + 1] != 'S' || a[i + 2] != '5' || a[i + 3] != '_') continue;
        if (!(a[i - 1] == 0x08 || (i >= 2 && a[i - 2] == 0x08 && a[i - 1] == '\\'))) continue;
        uint32_t p = i + 4;
        if (a[p] != 0x12) continue;                              // PackageOp
        p++;
        p += (uint32_t)((a[p] & 0xC0) >> 6) + 1;                 // PkgLength: the lead byte says how many follow
        p++;                                                     // NumElements
        uint8_t v[2];
        for (int k = 0; k < 2; k++) {
            if (p + 2 > len) return 0;
            if (a[p] == 0x0A) { v[k] = a[p + 1]; p += 2; }
            else if (a[p] == 0x00 || a[p] == 0x01) { v[k] = a[p]; p++; }
            else return 0;
        }
        *typa = v[0]; *typb = v[1];
        return 1;
    }
    return 0;
}

#ifdef KERNEL
#include "io.h"

static void io_delay_us(uint32_t us) { for (uint32_t i = 0; i < us; i++) (void)inb(0x80); }   // ~1us each

void acpi_pm_poweroff(const struct acpi_pm* pm) {
    __asm__ volatile("cli");
    if (pm && pm->valid && pm->have_s5) {
        if (pm->hw_reduced && pm->sleep_ctl_port) {
            outb((uint16_t)pm->sleep_ctl_port, (uint8_t)((pm->slp_typa << 2) | (1u << 5)));   // SLP_TYP | SLP_EN
        } else if (pm->pm1a_cnt) {
            uint16_t a = (uint16_t)pm->pm1a_cnt;
            // SCI_EN (bit 0) clear: the chipset is still in legacy mode; ask it to enter ACPI mode first
            if (!(inw(a) & 1) && pm->smi_cmd && pm->acpi_enable) {
                outb((uint16_t)pm->smi_cmd, pm->acpi_enable);
                for (int i = 0; i < 3000 && !(inw(a) & 1); i++) io_delay_us(1000);
            }
            uint16_t cur = inw(a) & (uint16_t)~((7u << 10) | (1u << 13));
            // like Linux: SLP_TYP first, then the same value with SLP_EN
            outw(a, (uint16_t)(cur | (pm->slp_typa << 10)));
            if (pm->pm1b_cnt) outw((uint16_t)pm->pm1b_cnt, (uint16_t)((inw((uint16_t)pm->pm1b_cnt) & ~((7u << 10) | (1u << 13))) | (pm->slp_typb << 10)));
            outw(a, (uint16_t)(cur | (pm->slp_typa << 10) | (1u << 13)));
            if (pm->pm1b_cnt) outw((uint16_t)pm->pm1b_cnt, (uint16_t)((inw((uint16_t)pm->pm1b_cnt) & ~((7u << 10) | (1u << 13))) | (pm->slp_typb << 10) | (1u << 13)));
        }
        for (int i = 0; i < 2000; i++) io_delay_us(1000);        // the machine should be gone within this
    }
    // emulators: QEMU (PIIX4 PM / ich9), Bochs, VirtualBox
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
}

void acpi_pm_reboot(const struct acpi_pm* pm) {
    __asm__ volatile("cli");
    extern void smp_stop_others(void) __attribute__((weak));   // smp.c (desktop only)
    if (smp_stop_others) smp_stop_others();
    if (pm && pm->reset_ok) { outb((uint16_t)pm->reset_port, pm->reset_value); io_delay_us(200000); }
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) io_delay_us(1);
    outb(0x64, 0xFE);                                            // pulse the CPU reset line of the keyboard controller
    io_delay_us(200000);
    outb(0xCF9, 0x02); io_delay_us(50); outb(0xCF9, 0x06);       // PCI reset control: hard reset
    io_delay_us(200000);
    struct { uint16_t limit; uint32_t base; uint32_t base_hi; } __attribute__((packed)) idt = { 0, 0, 0 };   // 10 bytes: the long-mode form
    __asm__ volatile("lidt %0; int3" :: "m"(idt));               // no IDT: the breakpoint triple-faults
    for (;;) __asm__ volatile("hlt");
}
#endif

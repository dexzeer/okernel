// Hardware report for the text-mode build: what a machine we have never seen is made of. It runs on
// plain VGA text with the PS/2 keyboard, so it works on any old PC even when no graphics mode does.
// Commands (shell.c): cpu, memmap, acpi, pci [page], pcidump B:D.F, gpu, all.

#include "hwinfo.h"
#include "terminal.h"
#include "vga.h"
#include "serial.h"
#include "io.h"
#include "pcix.h"
#include "i915.h"
#include "string.h"
#include "display.h"
#include "acpi_pm.h"
const struct display_ops* i915_probe(uint32_t* backbuffer, int w, int h);
#include <stdarg.h>

static uint32_t g_mboot;

void hwinfo_init(uint32_t mboot_phys) { g_mboot = mboot_phys; }

// ---- output: screen + serial -----------------------------------------------------------------
static void out(const char* s) {
    terminal_puts(s);
    serial_puts(s);
}

static void put_num(char** p, uint32_t v, unsigned base, int width, int upper) {
    char t[12]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { uint32_t d = v % base; t[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; }
    while (k < width) t[k++] = '0';
    while (k) *(*p)++ = t[--k];
}

// %s %u %x (%02x, %04x, %08x) %c
static void pf(const char* fmt, ...) {
    char b[200], *p = b;
    va_list ap;
    va_start(ap, fmt);
    for (const char* f = fmt; *f && p < b + 180; f++) {
        if (*f != '%') { *p++ = *f; continue; }
        f++;
        int width = 0;
        if (*f == '0') f++;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'u') put_num(&p, va_arg(ap, uint32_t), 10, width, 0);
        else if (*f == 'x') put_num(&p, va_arg(ap, uint32_t), 16, width, 0);
        else if (*f == 'c') *p++ = (char)va_arg(ap, int);
        else if (*f == 's') { const char* s = va_arg(ap, const char*); while (s && *s && p < b + 190) *p++ = *s++; }
        else *p++ = *f;
    }
    va_end(ap);
    *p = 0;
    out(b);
}

static void head(const char* t) {
    terminal_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    pf("== %s ==\n", t);
    terminal_set_color(VGA_WHITE, VGA_BLACK);
}

static void gpu_line(const char* s, void* ud) { (void)ud; pf("%s\n", s); }

// ---- cpu -------------------------------------------------------------------------------------
static int has_cpuid(void) {
    uint32_t a, b;
    __asm__ volatile("pushfl; pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; popfl"
                     : "=r"(a), "=r"(b) :: "cc");
    return ((a ^ b) & 0x200000) != 0;
}

static void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

void hwinfo_cpu(void) {
    head("CPU");
    if (!has_cpuid()) { pf("no CPUID instruction (a 486 or older)\n"); return; }
    uint32_t a, b, c, d, maxleaf;
    char vendor[13];
    cpuid(0, &maxleaf, &b, &c, &d);
    *(uint32_t*)(vendor + 0) = b; *(uint32_t*)(vendor + 4) = d; *(uint32_t*)(vendor + 8) = c; vendor[12] = 0;
    pf("vendor %s, highest basic leaf %u\n", vendor, maxleaf);
    uint32_t emax;
    cpuid(0x80000000u, &emax, &b, &c, &d);
    if (emax >= 0x80000004u) {
        char brand[49];
        for (int i = 0; i < 3; i++) cpuid(0x80000002u + (uint32_t)i, (uint32_t*)(brand + i * 16), (uint32_t*)(brand + i * 16 + 4), (uint32_t*)(brand + i * 16 + 8), (uint32_t*)(brand + i * 16 + 12));
        brand[48] = 0;
        const char* s = brand;
        while (*s == ' ') s++;
        pf("brand  %s\n", s);
    }
    if (maxleaf >= 1) {
        cpuid(1, &a, &b, &c, &d);
        uint32_t fam = (a >> 8) & 15, mod = (a >> 4) & 15, step = a & 15;
        if (fam == 15) fam += (a >> 20) & 255;
        if (fam == 6 || fam >= 15) mod |= ((a >> 16) & 15) << 4;
        pf("family %u model %u (0x%02x) stepping %u   signature %08x   logical CPUs/package %u\n", fam, mod, mod, step, a, (b >> 16) & 255);
        static const struct { int reg; int bit; const char* n; } F[] = {
            { 0, 0, "fpu" }, { 0, 4, "tsc" }, { 0, 5, "msr" }, { 0, 6, "pae" }, { 0, 8, "cx8" }, { 0, 9, "apic" }, { 0, 11, "sep" },
            { 0, 12, "mtrr" }, { 0, 13, "pge" }, { 0, 15, "cmov" }, { 0, 16, "pat" }, { 0, 19, "clflush" }, { 0, 23, "mmx" }, { 0, 24, "fxsr" },
            { 0, 25, "sse" }, { 0, 26, "sse2" }, { 0, 28, "htt" }, { 1, 0, "sse3" }, { 1, 9, "ssse3" }, { 1, 19, "sse4.1" }, { 1, 20, "sse4.2" },
            { 1, 5, "vmx" }, { 1, 21, "x2apic" }, { 1, 28, "avx" }, { 1, 30, "rdrand" },
        };
        out("features:");
        for (unsigned i = 0; i < sizeof F / sizeof *F; i++)
            if ((F[i].reg ? c : d) & (1u << F[i].bit)) pf(" %s", F[i].n);
        out("\n");
    }
    if (emax >= 0x80000001u) {
        cpuid(0x80000001u, &a, &b, &c, &d);
        pf("extended: %s%s%s\n", (d & (1u << 29)) ? "64-bit(lm) " : "", (d & (1u << 20)) ? "nx " : "", (d & (1u << 11)) ? "syscall" : "");
    }
}

// ---- memory map ------------------------------------------------------------------------------
void hwinfo_memmap(void) {
    head("Memory map (from the bootloader)");
    uint32_t flags = *(volatile uint32_t*)(g_mboot + 0);
    pf("lower %u KB, upper %u KB\n", *(volatile uint32_t*)(g_mboot + 4), *(volatile uint32_t*)(g_mboot + 8));
    if (!(flags & (1u << 6))) { pf("no memory map\n"); return; }
    uint32_t len = *(volatile uint32_t*)(g_mboot + 44), addr = *(volatile uint32_t*)(g_mboot + 48), off = 0;
    static const char* const T[] = { "?", "usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad" };
    while (off + 24 <= len && off < 4096) {
        uint32_t sz = *(volatile uint32_t*)(addr + off);
        uint32_t bl = *(volatile uint32_t*)(addr + off + 4), bh = *(volatile uint32_t*)(addr + off + 8);
        uint32_t ll = *(volatile uint32_t*)(addr + off + 12), lh = *(volatile uint32_t*)(addr + off + 16);
        uint32_t ty = *(volatile uint32_t*)(addr + off + 20);
        pf("  %08x%08x  %08x%08x  %s\n", bh, bl, lh, ll, ty < 6 ? T[ty] : "?");
        off += sz + 4;
    }
    if (flags & (1u << 12)) {
        pf("bootloader framebuffer: %x pitch %u %ux%u bpp %u type %u\n", *(volatile uint32_t*)(g_mboot + 88), *(volatile uint32_t*)(g_mboot + 96),
           *(volatile uint32_t*)(g_mboot + 100), *(volatile uint32_t*)(g_mboot + 104), (uint32_t)*(volatile uint8_t*)(g_mboot + 108), (uint32_t)*(volatile uint8_t*)(g_mboot + 109));
    }
}

// ---- ACPI ------------------------------------------------------------------------------------
static int sum_ok(const uint8_t* p, uint32_t n) { uint8_t s = 0; while (n--) s = (uint8_t)(s + *p++); return s == 0; }

// ---- ACPI tables (also on UEFI, where the RSDP is not in the BIOS areas) ----------------------
// The RSDP of a UEFI machine sits in an "ACPI reclaim"/"ACPI NVS" region of the bootloader's memory map.
static uint32_t find_rsdp(void) {
    uint32_t ebda = (uint32_t)(*(volatile uint16_t*)0x40E) << 4;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t lo = pass == 0 ? ebda : 0xE0000, hi = pass == 0 ? ebda + 1024 : 0x100000;
        if (pass == 0 && (ebda < 0x80000 || ebda > 0xA0000)) continue;
        for (uint32_t p = lo; p + 20 <= hi; p += 16)
            if (!memcmp((const void*)p, "RSD PTR ", 8) && sum_ok((const uint8_t*)p, 20)) return p;
    }
    uint32_t flags = *(volatile uint32_t*)(g_mboot + 0);
    if (!(flags & (1u << 6))) return 0;
    uint32_t len = *(volatile uint32_t*)(g_mboot + 44), addr = *(volatile uint32_t*)(g_mboot + 48);
    for (int want = 3; want <= 4; want++)                      // reclaim first, then NVS
        for (uint32_t off = 0; off + 24 <= len;) {
            volatile uint8_t* e = (volatile uint8_t*)(addr + off);
            uint32_t esz = *(volatile uint32_t*)e;
            uint64_t base = *(volatile uint64_t*)(e + 4), rl = *(volatile uint64_t*)(e + 12);
            uint32_t type = *(volatile uint32_t*)(e + 20);
            if (esz < 20) break;
            off += esz + 4;
            if ((int)type != want || base >= 0xFFFFF000ull) continue;
            uint32_t b = (uint32_t)base, n = rl > 0x1000000ull ? 0x1000000u : (uint32_t)rl;
            for (uint32_t p = (b + 15) & ~15u; p + 20 <= b + n; p += 16)
                if (!memcmp((const void*)p, "RSD PTR ", 8) && sum_ok((const uint8_t*)p, 20)) return p;
        }
    return 0;
}

// the table at a physical address when it is reachable (below 4GB) and plausible
static const uint8_t* table_at(uint64_t a, const char* sig) {
    if (a < 0x1000 || a >= 0xFFFFF000ull) return 0;
    const uint8_t* t = (const uint8_t*)(uint32_t)a;
    if (sig && memcmp(t, sig, 4)) return 0;
    uint32_t l = *(const uint32_t*)(t + 4);
    return l >= 36 && l < 0x400000 ? t : 0;
}

// Tables listed by the RSDT/XSDT, up to max; returns the count.
static int acpi_tables(const uint8_t** out_t, int max) {
    uint32_t rsdp = find_rsdp();
    if (!rsdp) return 0;
    const uint8_t* r = (const uint8_t*)rsdp;
    int n = 0;
    uint64_t xs = r[15] >= 2 ? *(const uint64_t*)(r + 24) : 0;
    const uint8_t* x = xs ? table_at(xs, "XSDT") : 0;
    if (x) {
        uint32_t l = *(const uint32_t*)(x + 4);
        for (uint32_t i = 36; i + 8 <= l && n < max; i += 8) {
            const uint8_t* t = table_at(*(const uint64_t*)(x + i), 0);
            if (t) out_t[n++] = t;
        }
        return n;
    }
    const uint8_t* rs = table_at(*(const uint32_t*)(r + 16), "RSDT");
    if (!rs) return 0;
    uint32_t l = *(const uint32_t*)(rs + 4);
    for (uint32_t i = 36; i + 4 <= l && n < max; i += 4) {
        const uint8_t* t = table_at(*(const uint32_t*)(rs + i), 0);
        if (t) out_t[n++] = t;
    }
    return n;
}

static int build_pm(struct acpi_pm* pm);

void hwinfo_acpi(void) {
    head("ACPI");
    uint32_t rsdp = find_rsdp();
    if (!rsdp) { pf("no RSDP found\n"); return; }
    const uint8_t* r = (const uint8_t*)rsdp;
    char oem[7];
    for (int i = 0; i < 6; i++) oem[i] = (char)r[9 + i];
    oem[6] = 0;
    pf("RSDP at %x, revision %u, OEM %s, RSDT %x\n", rsdp, (uint32_t)r[15], oem, *(const uint32_t*)(r + 16));
    const uint8_t* tabs[48];
    int n = acpi_tables(tabs, 48);
    if (!n) { pf("no readable RSDT/XSDT\n"); return; }
    out("tables:");
    for (int i = 0; i < n; i++) { const char* sg = (const char*)tabs[i]; pf(" %c%c%c%c", sg[0], sg[1], sg[2], sg[3]); }
    out("\n");
    struct acpi_pm pm;
    if (build_pm(&pm))
        pf("power: PM1a %x PM1b %x  smi %x/%x%s  S5 %s (%x,%x)  reset %s\n", pm.pm1a_cnt, pm.pm1b_cnt, pm.smi_cmd, (uint32_t)pm.acpi_enable,
           pm.hw_reduced ? "  HW-reduced" : "", pm.have_s5 ? "found" : "MISSING", (uint32_t)pm.slp_typa, (uint32_t)pm.slp_typb, pm.reset_ok ? "reg" : "none");
    else
        pf("power: no usable FADT\n");
}

// Power facts from the firmware tables (FADT + the DSDT's _S5_), for `acpi`, shutdown and reboot.
static int build_pm(struct acpi_pm* pm) {
    pm->have_s5 = 0; pm->slp_typa = pm->slp_typb = 0;
    const uint8_t* tabs[48];
    int n = acpi_tables(tabs, 48);
    pm->valid = 0;
    for (int i = 0; i < n; i++) {
        if (memcmp(tabs[i], "FACP", 4)) continue;
        if (!acpi_parse_fadt(tabs[i], *(const uint32_t*)(tabs[i] + 4), pm)) return 0;
        const uint8_t* d = pm->dsdt ? table_at(pm->dsdt, "DSDT") : 0;
        if (d) pm->have_s5 = acpi_find_s5(d, *(const uint32_t*)(d + 4), &pm->slp_typa, &pm->slp_typb);
        return 1;
    }
    return 0;
}

void hwinfo_poweroff(void) {
    struct acpi_pm pm;
    build_pm(&pm);
    acpi_pm_poweroff(&pm);
    pf("Power-off did not work: hold the power button.\n");
    for (;;) __asm__ volatile("hlt");
}

void hwinfo_reboot(void) {
    struct acpi_pm pm;
    build_pm(&pm);
    acpi_pm_reboot(&pm);
}

// ---- which input devices does the firmware describe? -------------------------------------------
// Scans the AML of the DSDT/SSDTs for _HID/_CID names (a string "ELAN0501" or a compressed EISA id "PNP0303").
#define MAX_HID 80
static char hid_list[MAX_HID][12];
static int hid_n;

static void hid_add(const char* s) {
    for (int i = 0; i < hid_n; i++) if (!strcmp(hid_list[i], s)) return;
    if (hid_n < MAX_HID) { strncpy(hid_list[hid_n], s, 11); hid_list[hid_n][11] = 0; hid_n++; }
}

static void scan_aml(const uint8_t* t) {
    uint32_t len = *(const uint32_t*)(t + 4);
    for (uint32_t i = 36; i + 12 < len; i++) {
        if (t[i] != '_' || !((t[i + 1] == 'H' && t[i + 2] == 'I' && t[i + 3] == 'D') || (t[i + 1] == 'C' && t[i + 2] == 'I' && t[i + 3] == 'D'))) continue;
        uint32_t p = i + 4;
        char s[12];
        if (t[p] == 0x0D) {                                      // string
            p++;
            int k = 0;
            while (k < 11 && t[p + k] >= 0x20 && t[p + k] < 0x7F) { s[k] = (char)t[p + k]; k++; }
            s[k] = 0;
            if (k >= 6) hid_add(s);
        } else if (t[p] == 0x0C) {                               // dword: compressed EISA id
            uint8_t b0 = t[p + 1], b1 = t[p + 2], b2 = t[p + 3], b3 = t[p + 4];
            static const char hx[] = "0123456789ABCDEF";
            s[0] = (char)(((b0 >> 2) & 0x1F) + 64);
            s[1] = (char)((((b0 & 3) << 3) | (b1 >> 5)) + 64);
            s[2] = (char)((b1 & 0x1F) + 64);
            s[3] = hx[b2 >> 4]; s[4] = hx[b2 & 15]; s[5] = hx[b3 >> 4]; s[6] = hx[b3 & 15]; s[7] = 0;
            if (s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' && s[2] >= 'A' && s[2] <= 'Z') hid_add(s);
        }
    }
}

static int starts(const char* s, const char* pre) { while (*pre) if (*s++ != *pre++) return 0; return 1; }

static int is_pointing(const char* s) {
    static const char* const pre[] = { "PNP0F", "PNP0C50", "ACPI0C50", "MSFT0001", "SYNA", "ELAN", "ALPS", "CYAP", "FTE", "DLL0", "HID", "PNP0303", "PNP030B", "PNP0320", "MSFT0002", 0 };
    for (int i = 0; pre[i]; i++) if (starts(s, pre[i])) return 1;
    return 0;
}

static void input_acpi(void) {
    head("Input devices described by ACPI");
    const uint8_t* tabs[48];
    int n = acpi_tables(tabs, 48);
    if (!n) { pf("no ACPI tables found\n"); return; }
    hid_n = 0;
    int nscan = 0;
    for (int i = 0; i < n; i++) {
        const char* sg = (const char*)tabs[i];
        if (!memcmp(sg, "FACP", 4)) {                              // the DSDT hangs off the FADT
            const uint8_t* d = table_at(*(const uint32_t*)(tabs[i] + 40), "DSDT");
            if (!d && *(const uint32_t*)(tabs[i] + 4) >= 148) d = table_at(*(const uint64_t*)(tabs[i] + 140), "DSDT");
            if (d) { scan_aml(d); nscan++; }
        } else if (!memcmp(sg, "SSDT", 4)) { scan_aml(tabs[i]); nscan++; }
    }
    pf("%u tables, %u scanned, %u device ids\n", (uint32_t)n, (uint32_t)nscan, (uint32_t)hid_n);
    out("mouse/touchpad/keyboard/hid: ");
    int any = 0;
    for (int i = 0; i < hid_n; i++) if (is_pointing(hid_list[i])) { pf("%s ", hid_list[i]); any = 1; }
    if (!any) out("(none)");
    out("\nall ids: ");
    for (int i = 0; i < hid_n; i++) pf("%s ", hid_list[i]);
    out("\n");
}

// ---- PS/2 controller and the pointing device ----------------------------------------------------
// Interrupts are off while this runs and time comes from PIT channel 0 (reprogrammed to a plain 65536 count).
static uint32_t pit_last, pit_acc;
static uint32_t pit_ms(void) {
    outb(0x43, 0x00);
    uint32_t lo = inb(0x40), hi = inb(0x40), c = lo | (hi << 8);
    pit_acc += (pit_last - c) & 0xFFFF;
    pit_last = c;
    return pit_acc / 1193;
}
static void pit_start(void) {
    outb(0x43, 0x34); outb(0x40, 0); outb(0x40, 0);             // channel 0, mode 2, count 65536
    pit_acc = 0;
    outb(0x43, 0x00);
    uint32_t lo = inb(0x40), hi = inb(0x40);
    pit_last = lo | (hi << 8);
}

static uint32_t last_wait_ms;
static int kbc_in_wait(void) {                                  // input buffer empty: we may write
    uint32_t t0 = pit_ms();
    while (inb(0x64) & 2) if (pit_ms() - t0 > 200) return 0;
    return 1;
}
static int kbc_read(uint32_t timeout_ms) {                       // output byte, or -1
    uint32_t t0 = pit_ms();
    for (;;) {
        if (inb(0x64) & 1) { last_wait_ms = pit_ms() - t0; return inb(0x60); }
        if (pit_ms() - t0 > timeout_ms) { last_wait_ms = timeout_ms; return -1; }
    }
}
static void kbc_cmd(uint8_t c) { kbc_in_wait(); outb(0x64, c); }
static void kbc_data(uint8_t d) { kbc_in_wait(); outb(0x60, d); }
static void kbc_flush(void) { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60); }
static int aux_cmd(uint8_t b) { kbc_cmd(0xD4); kbc_data(b); return kbc_read(300); }

static const char* hexs(int v) {
    static char b[4][5];
    static int k;
    char* s = b[k++ & 3];
    if (v < 0) { s[0] = '-'; s[1] = '-'; s[2] = 0; return s; }
    s[0] = "0123456789abcdef"[(v >> 4) & 15]; s[1] = "0123456789abcdef"[v & 15]; s[2] = 0;
    return s;
}

static void input_ps2(void) {
    head("PS/2 controller and pointing device");
    uint32_t fl;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(fl) :: "memory");
    pit_start();
    uint32_t st = inb(0x64);
    pf("status port 0x64 = %02x%s\n", st, st == 0xFF ? "  (0xFF: there may be no PS/2 controller at all)" : "");
    kbc_flush();
    kbc_cmd(0x20);
    int cfg = kbc_read(200);
    pf("controller config byte: %s  (bit1 = mouse irq, bit5 = mouse clock off, bit6 = translation)\n", hexs(cfg));
    kbc_cmd(0xAD); kbc_cmd(0xA7); kbc_flush();
    kbc_cmd(0xA9);
    int at = kbc_read(200);
    pf("mouse port self-test: %s  (00 = ok)\n", hexs(at));
    kbc_cmd(0xA8);
    int a = aux_cmd(0xFF), bat = -1, id = -1;
    uint32_t w_ack = last_wait_ms, w_bat = 0, w_id = 0;
    if (a == 0xFA) { bat = kbc_read(1500); w_bat = last_wait_ms; id = kbc_read(500); w_id = last_wait_ms; }
    pf("mouse reset: ack %s (%u ms)  self-test %s (%u ms)  id %s (%u ms)   [fa aa 00 = a PS/2 mouse]\n",
       hexs(a), w_ack, hexs(bat), w_bat, hexs(id), w_id);
    if (a == 0xFA && bat == 0xAA) {
        int r = aux_cmd(0xF2);
        int did = r == 0xFA ? kbc_read(200) : -1;
        pf("device id: %s\n", hexs(did));
        // Synaptics identify: resolution 0 four times, then a status request. The middle byte 47 = Synaptics.
        for (int i = 0; i < 4; i++) { aux_cmd(0xE8); aux_cmd(0x00); }
        int rs = aux_cmd(0xE9);
        int s0 = rs == 0xFA ? kbc_read(200) : -1;
        int s1 = kbc_read(200), s2 = kbc_read(200);
        pf("status request: %s %s %s  %s\n", hexs(s0), hexs(s1), hexs(s2), s1 == 0x47 ? "(a Synaptics touchpad)" : "");
        kbc_flush();
        int en = aux_cmd(0xF4);
        pf("enable reporting: %s\n", hexs(en));
        terminal_set_color(VGA_YELLOW, VGA_BLACK);
        out(">>> MOVE YOUR FINGER ON THE TOUCHPAD AND PRESS ITS BUTTONS FOR 10 SECONDS <<<\n");
        terminal_set_color(VGA_WHITE, VGA_BLACK);
        int cnt = 0;
        uint8_t first[18];
        uint32_t t0 = pit_ms();
        while (pit_ms() - t0 < 10000) {
            if (inb(0x64) & 1) { uint8_t v = inb(0x60); if (cnt < 18) first[cnt] = v; cnt++; }
        }
        pf("received %u bytes:", (uint32_t)cnt);
        for (int i = 0; i < cnt && i < 18; i++) pf(" %s", hexs(first[i]));
        out(cnt ? "\n" : "  (nothing: not a PS/2 touchpad, or it needs another mode)\n");
        aux_cmd(0xF5);
    }
    kbc_flush();
    kbc_cmd(0x60);
    kbc_data(cfg >= 0 ? (uint8_t)cfg : 0x47);                    // put the controller back as it was
    kbc_cmd(0xAE);
    kbc_flush();
    __asm__ volatile("pushl %0; popfl" :: "r"(fl) : "memory", "cc");
}

// ---- I2C-HID touchpad: what ACPI says and whether its controller answers ------------------------
// Read-only. The touchpad (e.g. SYNA3602 with compatible id PNP0C50) sits on an Intel LPSS DesignWare I2C controller.
// Its _CRS holds an I2cSerialBus descriptor (slave address, speed, the controller's ACPI path); the controller is
// another ACPI device whose _CRS has a Memory32Fixed range. We pattern-scan the AML instead of interpreting it.
static const uint8_t* aml_t[32];
static int aml_n;

static void aml_collect(void) {
    const uint8_t* tabs[48];
    int n = acpi_tables(tabs, 48);
    aml_n = 0;
    for (int i = 0; i < n && aml_n < 32; i++) {
        const char* sg = (const char*)tabs[i];
        if (!memcmp(sg, "FACP", 4)) {
            const uint8_t* d = table_at(*(const uint32_t*)(tabs[i] + 40), "DSDT");
            if (!d && *(const uint32_t*)(tabs[i] + 4) >= 148) d = table_at(*(const uint64_t*)(tabs[i] + 140), "DSDT");
            if (d) aml_t[aml_n++] = d;
        } else if (!memcmp(sg, "SSDT", 4)) aml_t[aml_n++] = tabs[i];
    }
}

static int find_bytes(const uint8_t* t, uint32_t len, uint32_t from, const void* pat, uint32_t pl) {
    for (uint32_t i = from; i + pl <= len; i++)
        if (t[i] == ((const uint8_t*)pat)[0] && !memcmp(t + i, pat, pl)) return (int)i;
    return -1;
}

static uint32_t aml_u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// a DesignWare I2C block: component type register 0xFC reads "DW" 0x44570140
static void i2c_probe_regs(uint32_t base) {
    volatile uint32_t* r = (volatile uint32_t*)base;
    uint32_t ctype = r[0xFC / 4], cver = r[0xF8 / 4];
    pf("    regs at %x: COMP_TYPE %x COMP_VERSION %x %s\n", base, ctype, cver,
       ctype == 0x44570140u ? "(DesignWare I2C)" : ctype == 0xFFFFFFFFu ? "(reads all ones: powered off or clock gated)" : "(not a DesignWare I2C?)");
    if (ctype == 0xFFFFFFFFu) return;
    pf("    IC_CON %x  IC_TAR %x  IC_ENABLE %x  IC_STATUS %x  ENABLE_STATUS %x  SS %x/%x  FS %x/%x\n", r[0x00], r[0x04 / 4], r[0x6C / 4], r[0x70 / 4], r[0x9C / 4],
       r[0x14 / 4], r[0x18 / 4], r[0x1C / 4], r[0x20 / 4]);
    pf("    LPSS clock %x  resets %x  general %x\n", r[0x200 / 4], r[0x204 / 4], r[0x208 / 4]);
}

void hwinfo_i2c(void) {
    head("I2C touchpad");
    aml_collect();
    if (!aml_n) { pf("no AML tables\n"); return; }
    char src[40] = "";
    int found = 0;
    for (int ti = 0; ti < aml_n && !found; ti++) {
        const uint8_t* t = aml_t[ti];
        uint32_t len = *(const uint32_t*)(t + 4);
        int hid = -1;
        static const char* const ids[] = { "SYNA3602", "SYNA", "ELAN", "ALPS", "CYAP", "MSFT0001", 0 };   // known touchpad ids, best first
        for (int k = 0; ids[k] && hid < 0; k++) hid = find_bytes(t, len, 36, ids[k], (uint32_t)strlen(ids[k]));
        if (hid < 0) continue;
        pf("touchpad id %c%c%c%c%c%c%c%c in %c%c%c%c at +%x\n", t[hid], t[hid + 1], t[hid + 2], t[hid + 3], t[hid + 4], t[hid + 5], t[hid + 6], t[hid + 7],
           t[0], t[1], t[2], t[3], (uint32_t)hid);
        // I2cSerialBusV2: 8E len(2) rev 00 type=01 flags tsflags(2) tsrev=01 tdlen=0006 speed(4) addr(2) source\0
        for (uint32_t j = (uint32_t)hid; j + 40 < len && j < (uint32_t)hid + 1500; j++) {
            const uint8_t* d = t + j;
            if (d[0] != 0x8E || d[3] != 0x01 || d[5] != 0x01 || d[9] != 0x01 || d[10] != 0x06 || d[11] != 0x00) continue;
            uint32_t speed = aml_u32(d + 12);
            uint32_t addr = d[16] | ((uint32_t)d[17] << 8);
            int k = 0;
            while (k < 39 && d[18 + k] >= 0x20 && d[18 + k] < 0x7F) { src[k] = (char)d[18 + k]; k++; }
            src[k] = 0;
            pf("  I2C slave address %x (%s-bit), %u Hz, controller \"%s\"\n", addr & 0x3FF, (d[7] & 1) ? "10" : "7", speed, src);
            found = 1;
            break;
        }
        if (!found) pf("  no I2cSerialBus descriptor within 1500 bytes of the id\n");
        // the HID descriptor register: _DSM (GUID 3CDFF6F7-4267-4555-AD05-B30A3D8938DE) usually returns 0x0001/0x0020
        static const uint8_t guid[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
        int g = find_bytes(t, len, hid > 1500 ? (uint32_t)hid - 1500 : 36, guid, 16);
        if (g >= 0) {
            out("  HID-I2C _DSM follows the GUID: ");
            for (int k = 16; k < 64 && (uint32_t)(g + k) < len; k++) pf("%02x ", (uint32_t)t[g + k]);
            out("\n");
        } else out("  no HID-I2C _DSM GUID near the id\n");
    }
    if (!found) { pf("no touchpad described\n"); }
    // the controller: a Device() named like the last four characters of the path
    if (src[0]) {
        int sl = (int)strlen(src);
        const char* nm = src + (sl >= 4 ? sl - 4 : 0);
        int dev_found = 0;
        for (int ti = 0; ti < aml_n; ti++) {
            const uint8_t* t = aml_t[ti];
            uint32_t len = *(const uint32_t*)(t + 4);
            for (uint32_t i = 36; i + 12 < len; i++) {
                if (t[i] != 0x5B || t[i + 1] != 0x82) continue;                         // DeviceOp
                uint32_t p = i + 2, lead = t[p], cnt = (lead >> 6) & 3;
                uint32_t pk = cnt == 0 ? (lead & 0x3F) : (lead & 0x0F);
                for (uint32_t k = 0; k < cnt; k++) pk |= (uint32_t)t[p + 1 + k] << (4 + 8 * k);
                uint32_t nstart = p + 1 + cnt;
                if (memcmp(t + nstart, nm, 4)) continue;
                dev_found = 1;
                uint32_t end = p + pk;
                if (end > len) end = len;
                pf("controller device %s in %c%c%c%c at +%x (scope length %u)\n", nm, t[0], t[1], t[2], t[3], i, pk);
                int h = find_bytes(t, end, nstart + 4, "_HID", 4);
                if (h >= 0) {
                    char b[12]; int k = 0;
                    if (t[h + 4] == 0x0D) while (k < 11 && t[h + 5 + k] >= 0x20 && t[h + 5 + k] < 0x7F) { b[k] = (char)t[h + 5 + k]; k++; }
                    b[k] = 0;
                    pf("  _HID %s\n", b);
                }
                for (uint32_t j = nstart + 4; j + 12 <= end; j++) {                       // Memory32Fixed(ReadWrite, base, len)
                    if (t[j] == 0x86 && t[j + 1] == 0x09 && t[j + 2] == 0x00) {
                        uint32_t base = aml_u32(t + j + 4), mlen = aml_u32(t + j + 8);
                        pf("  Memory32Fixed %x length %x\n", base, mlen);
                        if (base && base != 0xFFFFFFFFu && mlen >= 0x1000 && base < 0xFFFF0000u) i2c_probe_regs(base);
                    }
                }
            }
        }
        if (!dev_found) pf("controller device %s not found in the AML\n", nm);
    }
    // LPSS I2C controllers that the firmware exposes as PCI devices
    int npci = 0;
    for (uint16_t d = 0x22C1; d <= 0x22C7; d++) {
        struct pci_loc l;
        if (!pcix_find_id(0x8086, d, 0, &l)) continue;
        int is_mem, above;
        uint32_t bar = pcix_bar(l, 0, &is_mem, &above);
        pf("PCI I2C controller 8086:%x at %02x:%02x.%u BAR0 %x\n", (uint32_t)d, l.bus, l.dev, l.fn, bar);
        if (bar && bar != 0xFFFFFFFFu) i2c_probe_regs(bar);
        npci++;
    }
    if (!npci) pf("no 8086:22C1-22C7 PCI I2C controllers (ACPI mode)\n");
}

void hwinfo_input(void) {
    input_acpi();
    input_ps2();
}

// ---- PCI -------------------------------------------------------------------------------------
static const char* vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel"; case 0x10EC: return "Realtek"; case 0x14E4: return "Broadcom"; case 0x168C: return "Atheros";
    case 0x1969: return "Qualcomm Atheros"; case 0x10DE: return "NVIDIA"; case 0x1002: return "AMD/ATI"; case 0x1022: return "AMD";
    case 0x1106: return "VIA"; case 0x1039: return "SiS"; case 0x1180: return "Ricoh"; case 0x104C: return "Texas Instruments";
    case 0x1217: return "O2 Micro"; case 0x197B: return "JMicron"; case 0x11AB: return "Marvell"; case 0x10B7: return "3Com";
    case 0x1814: return "Ralink"; case 0x1B21: return "ASMedia"; case 0x1AF4: return "virtio"; case 0x1234: return "QEMU/Bochs";
    case 0x10EE: return "Xilinx";
    }
    return "";
}

static const char* class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x00: return "unclassified";
    case 0x01: return s == 1 ? "IDE controller" : s == 6 ? "SATA (AHCI)" : s == 8 ? "NVMe" : "storage";
    case 0x02: return "network";
    case 0x03: return s == 0 ? "VGA display" : "display";
    case 0x04: return s == 3 ? "HD audio" : "multimedia";
    case 0x05: return "memory ctrl";
    case 0x06: return s == 0 ? "host bridge" : s == 1 ? "ISA bridge" : s == 4 ? "PCI bridge" : s == 7 ? "CardBus bridge" : "bridge";
    case 0x07: return "comm ctrl";
    case 0x08: return "system peripheral";
    case 0x09: return "input device";
    case 0x0C: return s == 3 ? "USB controller" : s == 5 ? "SMBus" : s == 0 ? "FireWire" : "serial bus";
    case 0x0D: return "wireless";
    }
    return "other";
}

#define PCI_PAGE 20
void hwinfo_pci(int page) {
    head("PCI devices");
    int idx = 0, shown = 0, total = 0;
    int first = (page < 1 ? 0 : page - 1) * PCI_PAGE;
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                struct pci_loc l = { (uint8_t)bus, (uint8_t)dev, (uint8_t)fn };
                uint32_t id = pcix_read32(l, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                total++;
                if (idx++ < first || shown >= PCI_PAGE) continue;
                uint32_t cls = pcix_read32(l, 8);
                uint16_t ven = (uint16_t)id;
                pf("%02x:%02x.%u  %04x:%04x  %02x%02x%02x  %s %s\n", bus, dev, fn, (uint32_t)ven, id >> 16, cls >> 24, (cls >> 16) & 255, (cls >> 8) & 255,
                   vendor_name(ven), class_name((uint8_t)(cls >> 24), (uint8_t)(cls >> 16)));
                shown++;
                if (fn == 0 && !((pcix_read32(l, 0x0C) >> 16) & 0x80)) break;      // single-function device
            }
    pf("%u devices; page %u of %u%s\n", (uint32_t)total, (uint32_t)(first / PCI_PAGE + 1), (uint32_t)((total + PCI_PAGE - 1) / PCI_PAGE),
       first + PCI_PAGE < total ? "  (type 'pci 2' for the next page)" : "");
}

void hwinfo_pcidump(const char* arg) {
    // B:D.F in hex
    unsigned v[3] = { 0, 0, 0 };
    int k = 0;
    for (const char* s = arg; s && *s && k < 3; s++) {
        if (*s == ':' || *s == '.') { k++; continue; }
        int h = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
        if (h >= 0) v[k] = v[k] * 16 + (unsigned)h;
    }
    struct pci_loc l = { (uint8_t)v[0], (uint8_t)(v[1] & 31), (uint8_t)(v[2] & 7) };
    head("PCI config space");
    pf("%02x:%02x.%u\n", v[0], v[1] & 31, v[2] & 7);
    for (int off = 0; off < 256; off += 16) {
        pf("%02x:", off);
        for (int i = 0; i < 16; i += 4) pf(" %08x", pcix_read32(l, (uint8_t)(off + i)));
        out("\n");
    }
}

// ---- GPU -------------------------------------------------------------------------------------
void hwinfo_gpu(void) {
    head("Graphics");
    i915_probe(0, 0, 0);
    // the first display controller, whoever made it
    struct pci_loc loc;
    for (int n = 0; pcix_find_class(0x03, 0xFF, n, &loc); n++) {
        uint32_t id = pcix_read32(loc, 0);
        pf("display controller %02x:%02x.%u  %04x:%04x  %s   subsystem %08x\n", loc.bus, loc.dev, loc.fn, id & 0xFFFF, id >> 16,
           vendor_name((uint16_t)id), pcix_read32(loc, 0x2C));
        for (int b = 0; b < 6; b++) {
            int is_mem, above;
            uint32_t bar = pcix_bar(loc, b, &is_mem, &above);
            if (bar) pf("  BAR%u %08x  %s  %u KB%s\n", (uint32_t)b, bar, is_mem ? "mem" : "io ", pcix_bar_size(loc, b) >> 10, above ? "  (64-bit)" : "");
        }
    }
    i915_report(gpu_line, 0);
}

void hwinfo_all(void) {
    hwinfo_cpu();
    hwinfo_memmap();
    hwinfo_acpi();
    hwinfo_pci(1);
    hwinfo_pci(2);
    hwinfo_gpu();
}

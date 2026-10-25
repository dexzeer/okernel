// The panic screen of the desktop kernel: a CPU exception in the kernel (idt.c) or kpanic() ends here. The screen shows
// the cause and the call trace (function names from the .ksyms table); the full report (registers, cr2, ...) and the
// serial tail go into a crash record on the log partition of an installed disk (crash.h, klog.h), read back by the
// recovery desktop. GRUB is told to boot the recovery desktop once (grubenv, part.h) and the machine reboots after a
// countdown. Nothing here trusts the kernel's state: no heap, no locks, no IRQs, no other CPU; the screen is drawn with
// the 16x32 font into the backbuffer and flushed by the display backend (the framebuffer directly when that fails), the
// keyboard is polled on the PS/2 controller.
#include <stdarg.h>
#include <stdint.h>
#include "panic.h"
#include "acpi.h"
#include "acpi_pm.h"
#include "ata.h"
#include "cpufreq.h"
#include "crash.h"
#include "devlog.h"
#include "graphics.h"
#include "io.h"
#include "paging.h"
#include "klog.h"
#include "part.h"
#include "serial.h"
#include "string.h"

extern uint32_t tick_count;
extern const uint8_t font8x16[95][64];
extern const uint8_t _ksyms_start[], _ksyms_end[];
extern const uint8_t _kernel_start[], _rodata_start[];
struct process;
extern struct process* process_current(void) __attribute__((weak));
extern int smp_cpu_index(void) __attribute__((weak));

typedef char crash_rec_is_512[(sizeof(struct crash_rec) == 512) ? 1 : -1];
typedef char crash_hdr_is_512[(sizeof(struct crash_hdr) == 512) ? 1 : -1];

static volatile int panicking;
static char rep[CRASH_TEXT_SECTORS * 512];   // the report (also the text of the crash record)
static int rlen;
static char title[64];
static struct crash_rec rec;

static void rputs(const char* s) { while (*s && rlen < (int)sizeof rep - 1) rep[rlen++] = *s++; rep[rlen] = 0; }

static void rprintf(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    devlog_fmt(line, sizeof line, fmt, ap);
    va_end(ap);
    rputs(line);
}

static const char* exc_name(int v) {
    static const char* n[20] = {
        "divide error", "debug exception", "non-maskable interrupt", "breakpoint", "overflow", "bound range exceeded",
        "invalid opcode", "fpu not available", "double fault", "coprocessor segment overrun", "invalid tss",
        "segment not present", "stack-segment fault", "general protection fault", "page fault", "exception 15",
        "x87 floating-point error", "alignment check", "machine check", "simd floating-point exception",
    };
    return v >= 0 && v < 20 ? n[v] : "unknown exception";
}

// ---- symbols ----

static int is_code(uint32_t a) { return a >= (uint32_t)_kernel_start && a < (uint32_t)_rodata_start; }

static const char* sym_lookup(uint32_t addr, uint32_t* off) {
    const uint8_t* t = _ksyms_start;
    if (_ksyms_end - t < 8 || memcmp(t, "KSYM", 4) || !is_code(addr)) return 0;
    uint32_t n = *(const uint32_t*)(t + 4);
    const uint32_t* e = (const uint32_t*)(t + 8);
    const char* names = (const char*)(t + 8 + n * 8);
    if (!n || addr < e[0]) return 0;
    uint32_t lo = 0, hi = n;                                  // the last entry with address <= addr
    while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (e[m * 2] <= addr) lo = m; else hi = m; }
    *off = addr - e[lo * 2];
    return names + e[lo * 2 + 1];
}

static void put_addr(uint32_t a) {
    uint32_t off;
    const char* s = sym_lookup(a, &off);
    if (s) rprintf("%08x  %s+0x%x\n", a, s, off);
    else rprintf("%08x\n", a);
}

// Whether a code address follows a CALL instruction (rel32, or FF /2 through a register or memory operand).
static int after_call(uint32_t r) {
    if (!is_code(r - 7) || !is_code(r)) return 0;
    const uint8_t* p = (const uint8_t*)r;
    if (p[-5] == 0xE8) return 1;
    static const int back[5] = { 2, 3, 4, 6, 7 };
    for (int i = 0; i < 5; i++)
        if (p[-back[i]] == 0xFF && ((p[-back[i] + 1] >> 3) & 7) == 2) return 1;
    return 0;
}

// Whether a kernel address can be read without faulting (walks the current page tables).
static int mapped(uint32_t va) { return paging_va_mapped(va); }


// Return addresses on the stack, starting at esp (the frame pointer is omitted at -O2: this is a scan, like Linux's,
// so older entries can be stale). The first entry is the faulting address itself.
#define TRACE_MAX 12
static uint32_t trace[TRACE_MAX];
static int ntrace;

static void stack_trace(uint32_t esp, uint32_t eip) {
    rputs("\ncall trace:\n  ");
    put_addr(eip);
    trace[0] = eip;
    ntrace = 1;
    uint32_t page = 0xFFFFFFFFu;
    for (uint32_t a = esp & ~3u, k = 0; k < 2048 && ntrace < TRACE_MAX; a += 4, k++) {
        if ((a >> 12) != page) { if (!mapped(a)) break; page = a >> 12; }
        uint32_t v = *(const uint32_t*)a;
        uint32_t off;
        if (after_call(v) && (!sym_lookup(v, &off) || off)) {   // (a function's first byte is no return address)
            rputs("  ");
            put_addr(v);
            trace[ntrace++] = v;
        }
    }
}

// ---- screen ----

static uint32_t* bb;
static uint8_t* fbp;
static int fbpitch, W, H;

static void px(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    if (bb) bb[y * W + x] = c;
    else *(uint32_t*)(fbp + y * fbpitch + x * 4) = c;
}

static void rect(int x0, int y0, int w, int h, uint32_t c) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) px(x, y, c);
}

// The 16x32 terminal font at s times its size; only the set pixels are drawn (the background is already there).
static int str_at(int x, int y, const char* s, uint32_t fg, int sc) {
    for (; *s; s++, x += 16 * sc) {
        if (*s < 32 || *s > 126) continue;
        const uint8_t* g = font8x16[*s - 32];
        for (int gy = 0; gy < 32; gy++)
            for (int gx = 0; gx < 16; gx++)
                if ((g[gy * 2 + (gx >> 3)] >> (7 - (gx & 7))) & 1) rect(x + gx * sc, y + gy * sc, sc, sc, fg);
    }
    return x;
}

#define BG    0x0C0C0E
#define FG    0xE6E6E6
#define DIM   0x5C5C66
#define RED   0xE5484D
#define REDDK 0x3A1416

static char cause[160];                     // "invalid opcode", "page fault reading 00000000", or the kpanic message
static char status[96];                     // what happened to the crash record ("" when saved)

static void draw(int secs, int halted) {
    int m = W / 12, y = H / 7;
    rect(0, 0, W, H, BG);
    rect(0, 0, 6, H, RED);                                     // the red edge
    str_at(m, y, "kernel panic", RED, 2);
    y += 64 + 24;
    str_at(m, y, cause, FG, 1);
    y += 32 + 40;
    str_at(m, y, "call trace", DIM, 1);
    y += 32 + 8;
    for (int i = 0; i < ntrace && y < H - 200; i++, y += 32) {
        uint32_t off;
        const char* fn = sym_lookup(trace[i], &off);
        char a[16], o[16];
        int k = 0;
        for (int b = 28; b >= 0; b -= 4) a[k++] = "0123456789abcdef"[(trace[i] >> b) & 15];
        a[k] = 0;
        int x = str_at(m, y, a, DIM, 1) + 32;
        if (fn) {
            x = str_at(x, y, fn, i ? FG : RED, 1);
            k = 0;
            o[k++] = '+'; o[k++] = '0'; o[k++] = 'x';
            char d[9]; int n = 0;
            do { d[n++] = "0123456789abcdef"[off & 15]; off >>= 4; } while (off);
            while (n) o[k++] = d[--n];
            o[k] = 0;
            str_at(x, y, o, DIM, 1);
        } else str_at(x, y, "?", DIM, 1);
    }
    // the countdown: a draining bar, the seconds, the keys
    int by = H - 120, bw = W - 2 * m;
    rect(m, by, bw, 4, REDDK);
    if (!halted) rect(m, by, bw * (secs > 0 ? secs : 0) / 30, 4, RED);
    char t[64];
    int n = 0;
    const char* a = halted ? "held" : "recovery in ";
    while (*a) t[n++] = *a++;
    if (!halted) { if (secs >= 10) t[n++] = (char)('0' + secs / 10); t[n++] = (char)('0' + secs % 10); t[n++] = 's'; }
    t[n] = 0;
    str_at(m, by + 20, t, FG, 1);
    const char* keys = halted ? "r recovery   d desktop" : "r recovery   d desktop   h hold";
    str_at(m + bw - (int)strlen(keys) * 16, by + 20, keys, DIM, 1);
    if (status[0]) str_at(m, by + 60, status, DIM, 1);
    if (bb) { graphics_mark_rect(0, 0, W, H); graphics_flush(); }
}

// ---- disk ----

// The crash record (crash.h) on the log partition; "" or what went wrong.
static const char* save_record(void) {
    const struct part* p = part_role(PART_LOG);
    if (!p || p->count < CRASH_PART_SECTORS) return "not saved: no log partition";
    klog_panic_flush();
    static struct crash_hdr h;
    uint32_t base = p->start + CRASH_FIRST;
    if (ata_read(base, (uint8_t*)&h)) return "not saved: read error";
    if (memcmp(h.magic, CRASH_MAGIC, 16)) { memset(&h, 0, sizeof h); memcpy(h.magic, CRASH_MAGIC, 16); }
    h.count++;
    uint32_t slot = base + 1 + ((h.count - 1) % CRASH_SLOTS) * CRASH_SLOT_SECTORS;
    memcpy(rec.magic, CRASH_MAGIC, 16);
    rec.seq = h.count;
    rec.boot = klog_boot();
    rec.uptime_ms = tick_count * 10;
    rec.text_len = (uint32_t)rlen;
    memcpy(rec.title, title, sizeof rec.title);
    rec.title[63] = 0;
    // the serial tail: the newest CRASH_SER_SECTORS * 512 bytes of the ring
    static char ser[CRASH_SER_SECTORS * 512];
    uint32_t total = serial_total, n = total < sizeof ser ? total : sizeof ser;
    if (n > SERIAL_RING) n = SERIAL_RING;
    for (uint32_t i = 0; i < n; i++) ser[i] = serial_ring[(total - n + i) % SERIAL_RING];
    rec.ser_len = n;
    if (ata_write(slot, (const uint8_t*)&rec)) return "not saved: write error";
    for (uint32_t s = 0; s < CRASH_TEXT_SECTORS; s++)
        if (ata_write(slot + 1 + s, (const uint8_t*)rep + s * 512)) return "not saved: write error";
    for (uint32_t s = 0; s < (n + 511) / 512; s++)
        if (ata_write(slot + 1 + CRASH_TEXT_SECTORS + s, (const uint8_t*)ser + s * 512)) return "not saved: write error";
    if (ata_write(base, (const uint8_t*)&h)) return "not saved: write error";
    serial_printf("[panic] saved as crash record #%u\n", h.count);
    return "";
}

// ---- keys and time ----

static int poll_key(void) {
    uint8_t st = inb(0x64);
    if (!(st & 1)) return 0;
    uint8_t sc = inb(0x60);
    if (st & 0x20) return 0;                                  // a mouse byte
    return sc;
}

static uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

__attribute__((noreturn)) static void reboot_now(void) {
    serial_puts("[panic] rebooting\n");
    acpi_pm_reboot(acpi_get_pm());
    for (;;) __asm__ volatile("cli; hlt");
}

__attribute__((noreturn)) static void finish(uint32_t vec) {
    rec.vec = vec;
    const char* saved = save_record();
    int env = grubenv_set_next("recovery") == 0;
    serial_printf("[panic] grubenv next_entry=recovery: %s\n", env ? "set" : "no boot partition");
    int n = 0;
    for (const char* a = saved; *a && n < 60; a++) status[n++] = *a;
    if (!env) { const char* a = n ? ", no boot menu" : "no boot menu: restarts the desktop"; while (*a && n < 90) status[n++] = *a++; }
    status[n] = 0;
    uint32_t mhz = cpufreq_tsc_mhz();
    uint64_t per_s = (uint64_t)(mhz ? mhz : 2000) * 1000000u;
    uint64_t t0 = rdtsc();
    int halted = 0, last = -2;
    for (;;) {
        int secs = 30 - (int)((rdtsc() - t0) / per_s);
        if (secs != last) {
            draw(secs, halted);
            if (last == -2) serial_printf("[panic] countdown %d\n", secs);   // once: tests wait for it
            last = secs;
        }
        if (!halted && secs <= 0) reboot_now();
        int k = poll_key();
        if (k == 0x13 || k == 0x1C) { if (env) grubenv_set_next("recovery"); reboot_now(); }
        if (k == 0x20) { if (env) grubenv_set_next(""); serial_puts("[panic] restart the desktop\n"); reboot_now(); }
        if (k == 0x23 && !halted) { halted = 1; last = -3; serial_puts("[panic] held\n"); }
        for (volatile int i = 0; i < 2000; i++) {}
    }
}

static void setup_screen(void) {
    W = gfx_w; H = gfx_h;
    bb = graphics_get_buffer();
    fbp = graphics_fb_base();
    fbpitch = graphics_fb_pitch();
    if (panicking > 1) bb = 0;                                // the display backend itself failed: draw on the framebuffer
}

static void begin(void) {
    __asm__ volatile("cli");
    if (panicking++) {
        serial_puts("[panic] fault inside the panic screen\n");
        if (panicking > 3) reboot_now();
    }
    rlen = 0;
    rep[0] = 0;
    ntrace = 0;
    memset(&rec, 0, sizeof rec);
}

static void copy(char* d, int cap, const char* s) {
    int n = 0;
    while (*s && n < cap - 1) d[n++] = *s++;
    d[n] = 0;
}

void panic_exception(int vec, uint32_t* f) {
    begin();
    // f: pusha (EDI ESI EBP ESP EBX EDX ECX EAX), int no, error code, EIP, CS, EFLAGS [, ESP, SS from ring 3]
    uint32_t err = f[9], eip = f[10], cs = f[11] & 0xFFFF, efl = f[12], cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0; mov %%cr2, %1; mov %%cr3, %2; mov %%cr4, %3" : "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4));
    int user = (cs & 3) == 3;
    uint32_t esp = user ? f[13] : (uint32_t)&f[13];
    int pid = -1;
    if (process_current) { uint32_t* pcb = (uint32_t*)process_current(); if (pcb) pid = (int)pcb[0]; }
    copy(title, sizeof title, exc_name(vec));
    if (vec == 14) {
        int n = 0;
        const char* a = exc_name(vec);
        while (*a) cause[n++] = *a++;
        a = err & 16 ? " fetching " : err & 2 ? " writing " : " reading ";
        while (*a) cause[n++] = *a++;
        for (int b = 28; b >= 0; b -= 4) cause[n++] = "0123456789abcdef"[(cr2 >> b) & 15];
        cause[n] = 0;
    } else copy(cause, sizeof cause, exc_name(vec));
    if (user) { int n = (int)strlen(cause); copy(cause + n, (int)sizeof cause - n, " in a user program"); }
    rec.err = err; rec.eip = eip; rec.cr2 = cr2; rec.cs = cs; rec.eflags = efl;
    uint32_t off;
    const char* fn = sym_lookup(eip, &off);
    rprintf("%s, vector %d, error %x, %s, cpu %d, pid %d, up %us\n", exc_name(vec), vec, err,
            user ? "user" : "kernel", smp_cpu_index ? smp_cpu_index() : 0, pid, tick_count / 100);
    if (fn) rprintf("at %08x %s+0x%x\n", eip, fn, off);
    else rprintf("at %08x\n", eip);
    if (vec == 14)
        rprintf("cr2 %08x: %s, %s, %s%s\n", cr2, err & 1 ? "protection" : "not present",
                err & 2 ? "write" : "read", err & 4 ? "user" : "kernel", err & 16 ? ", fetch" : "");
    rprintf("\neax %08x  ebx %08x  ecx %08x  edx %08x  esi %08x  edi %08x\n", f[7], f[4], f[6], f[5], f[1], f[0]);
    rprintf("ebp %08x  esp %08x  eflags %08x  cs %x  cr0 %08x  cr3 %08x  cr4 %08x\n", f[2], esp, efl, cs, cr0, cr3, cr4);
    if (!user) stack_trace(esp, eip);
    else { trace[0] = eip; ntrace = 1; }
    serial_printf("[panic] %s\n", title);
    serial_puts(rep);
    setup_screen();
    finish((uint32_t)vec);
}

void kpanic(const char* fmt, ...) {
    begin();
    va_list ap;
    va_start(ap, fmt);
    devlog_fmt(cause, sizeof cause, fmt, ap);
    va_end(ap);
    copy(title, sizeof title, cause);
    uint32_t eip = (uint32_t)__builtin_return_address(0), esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    rec.eip = eip;
    rprintf("panic: %s\ncpu %d, up %us\n", cause, smp_cpu_index ? smp_cpu_index() : 0, tick_count / 100);
    stack_trace(esp, eip);
    serial_printf("[panic] %s\n%s", title, rep);
    setup_screen();
    finish(0xFFFFFFFFu);
}

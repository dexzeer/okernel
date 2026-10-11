// I2C-HID touchpad glue (see i2chid.h): ACPI lookup, controller hunt, bring-up, polling.
#include "i2chid.h"
#include <stdarg.h>
#include "acpi.h"
#include "cursor.h"
#include "devlog.h"
#include "dwi2c.h"
#include "graphics.h"
#include "hid.h"
#include "hidi2c.h"
#include "i2caml.h"
#include "klog.h"
#include "lapic.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "pcix.h"
#include "ptp.h"
#include "string.h"

extern uint32_t tick_count;           // 100 Hz since boot (desktop.c)

#define tlog(...) devlog('t', __VA_ARGS__)
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
#define MAXDEV 4
#define MAXBLK 12
#define MAXRNG 96

static struct rng { uint32_t lo, hi; } RNG[MAXRNG];     // [lo, hi) address ranges the scan skips
static int nrng;

static struct {
    int state;                         // 0 nothing to do, 1 searching, 2 running, 3 failed
    struct i2c_hid_dev dev[MAXDEV];
    int ndev, cur;
    uint32_t blk[MAXBLK];              // physical bases of the DesignWare blocks found
    int nblk;
    uint32_t base;                     // the one the touchpad answered on
    volatile uint8_t* regs;
    struct dwi2c dw;
    struct hidi2c h;
    struct hid_ptr lay[HID_MAXRPT];
    int nptr, ids;
    struct hid_touch touch;
    int have_touch;
    struct ptp ptp;
    uint32_t reports, errors, empty, unmatched, last_tick, fails;
    int button;
    int logged;
    int paused;                        // `touchpad off`
    int order[MAXDEV];                 // devices, touchpads first
    uint32_t dbar[MAXDEV];             // controller register block of each device, from the AML (0 = unknown)
    uint32_t dbar1[MAXDEV];            // its PCI-config mirror (power state at +0x84)
    int aml_stage;                     // 0 not started, 1 done
    volatile uint32_t* attn;           // the touchpad's interrupt pad (control register 0; bit 1 = the pin's level, low = data ready)
    int attn_last;
    uint32_t attn_low, attn_edges, reports_low;
    uint8_t mode_rep[16];              // the Input Mode feature report (id first when the device uses ids), kept to repeat it on a restart
    int mode_len, mode_ids;
    uint32_t garbage_tick;
    uint32_t garbage_run;              // consecutive input reads that were not a report
    uint32_t next_recover;             // tick before which no restart is attempted
    uint32_t recovers;                 // restarts so far
    int recover_logged;                // the first read after a restart is logged
} T;

static int cmdline_has(uint32_t mboot, const char* word) {
    uint32_t flags = *(volatile uint32_t*)(mboot + 0);
    if (!(flags & (1u << 2))) return 0;
    const char* c = (const char*)P2V_U32(*(volatile uint32_t*)(mboot + 16));
    uint32_t wl = (uint32_t)strlen(word);
    for (int i = 0; c[i] && i < 512; i++)
        if (!strncmp(c + i, word, wl) && (i == 0 || c[i - 1] == ' ') && (c[i + wl] == 0 || c[i + wl] == ' ')) return 1;
    return 0;
}

// ---- address ranges the scan must not read ------------------------------------------------------
static void rng_add(uint32_t lo, uint32_t hi) {
    if (hi <= lo || nrng >= MAXRNG) return;
    RNG[nrng].lo = lo; RNG[nrng].hi = hi; nrng++;
}

static int rng_has(uint32_t a) {
    for (int i = 0; i < nrng; i++) if (a >= RNG[i].lo && a < RNG[i].hi) return 1;
    return 0;
}

static void collect_ranges(uint32_t mboot) {
    nrng = 0;
    uint32_t flags = *(volatile uint32_t*)(mboot + 0);
    if (flags & (1u << 6)) {                                          // RAM, ACPI tables, NVS from the firmware map
        uint32_t mlen = *(volatile uint32_t*)(mboot + 44), maddr = *(volatile uint32_t*)(mboot + 48);
        for (uint32_t o = 0; o + 24 <= mlen && o < 4096; ) {
            const volatile uint32_t* e = (const volatile uint32_t*)P2V_U32(maddr + o);
            uint32_t esz = e[0], base = e[1], baseh = e[2], rl = e[3], rlh = e[4], type = e[5];
            if (esz < 20) break;
            devlog('p', "mem %x%08x +%x%08x type %u", baseh, base, rlh, rl, type);
            if ((type == 1 || type == 3 || type == 4) && baseh == 0) {
                uint32_t hi = (rlh || base + rl < base) ? 0xFFFFFFFFu : base + rl;
                rng_add(base, hi);
            }
            o += esz + 4;
        }
    }
    rng_add(0xE0000000u, 0xF0000000u);                                // the usual PCI config window
    struct pci_loc l;
    if (cmdline_has(mboot, "i2cbars")) return;                        // test option: scan the PCI BARs too (QEMU device models)
    for (int n = 0; n < 96 && pcix_find_any(n, &l); n++) {
        if (pcix_read32(l, 0) == 0xFFFFFFFFu) continue;
        devlog('p', "pci %x:%x.%x id %x class %x", l.bus, l.dev, l.fn, pcix_read32(l, 0), pcix_read32(l, 8) >> 8);
        int ht = pcix_read8(l, 0x0E) & 0x7F;
        int nb = ht == 0 ? 6 : ht == 1 ? 2 : 0;
        for (int b = 0; b < nb; b++) {
            uint32_t raw = pcix_read32(l, (uint8_t)(0x10 + 4 * b));
            if (raw & 1) continue;                                    // I/O BAR
            int is64 = ((raw >> 1) & 3) == 2;
            int mem, above;
            uint32_t base = pcix_bar(l, b, &mem, &above);
            uint32_t size = base ? pcix_bar_size(l, b) : 0;
            if (base) devlog('p', "  bar%d %x%s size %x", b, base, above ? " (above 4G)" : "", size);
            if (base && !above && size) rng_add(base, base + size < base ? 0xFFFFFFFFu : base + size);
            if (is64) b++;
        }
    }
}

// ---- register access ------------------------------------------------------------------------------
static uint32_t mm_rd(void* c, uint32_t off) { return *(volatile uint32_t*)((uint8_t*)c + off); }
static void mm_wr(void* c, uint32_t off, uint32_t v) { *(volatile uint32_t*)((uint8_t*)c + off) = v; }
static void us_delay(uint32_t us) { pit_delay_us(us); }
static void ms_delay(uint32_t ms) { while (ms--) pit_delay_us(1000); }
static int bus_xfer(void* ctx, int addr, const uint8_t* w, int wl, uint8_t* r, int rl) {
    (void)ctx;
    return dw_xfer(&T.dw, addr, w, wl, r, rl);
}

// ---- the controller hunt ----------------------------------------------------------------------------
// Reads one dword per 4 KB page of the PCI hole (not RAM, not a PCI BAR, not the config window) through the scratch window
// and records every page whose component type register reads "DesignWare I2C".
static int try_block(uint32_t phys);
static int bring_up_device(void);
static void dump_ctrl_aml(const char* path);
static void hexlog(const char* what, const uint8_t* b, uint32_t n);

// The hunt runs from the main loop, a few hundred microseconds per tick, starting a few seconds after boot: a bus that
// stalls on a read can then be seen (the breadcrumb on screen names the 4 MB block being read) and never delays the boot.
#define SCAN_START_TICK 300
#define SCAN_BUDGET 1200000u              // cycles of reading per tick
static struct {
    uint32_t w, p;                        // next 4 MB window and byte offset inside it
    uint32_t pages;
    uint64_t cycles;
    int active;                           // the window is mapped and p/w are valid
    uint32_t ticks;
} S;

static volatile uint8_t* blk_map(uint32_t phys) {
    volatile uint8_t* v = (volatile uint8_t*)paging_probe_window(phys & 0xFFC00000u);
    return v ? v + (phys & 0x003FFFFFu) : 0;
}

// A line at the top left of the screen, flushed (and the write buffers drained) BEFORE the reads it describes. Only while the
// blind memory hunt is on (menu 2h / `touchpad scan`): the normal start-up has nothing to show. Every line also goes to the log.
static int crumbs_on;
static void crumb(const char* fmt, ...) {
    char s[72];
    va_list ap;
    va_start(ap, fmt);
    devlog_fmt(s, sizeof s, fmt, ap);
    va_end(ap);
    if (!crumbs_on) return;
    draw_string_1x(4, 4, s, 0xFFFFFF, 0x000000);
    graphics_flush();
    __asm__ volatile("sfence" ::: "memory");
}

static int aml_rd(uint32_t phys, uint32_t* v) { return acpi_phys_read(phys, v, 4); }

// ---- bring-up on the address the AML gave (state 5) -----------------------------------------------------
// Step by step, each logged AND written to the stick before the next access: the first real run froze on a blind read.
#define AML_START_TICK 250                // 2.5 s after boot (the stick log of 5 failing boots all had the boot stick on a SuperSpeed link, see xhci.c ss_power_off)
static void aml_bringup(void) {
    if (tick_count < AML_START_TICK || T.paused) return;
    for (int oi = 0; oi < T.ndev; oi++) {
        int d = T.order[oi];
        uint32_t bar = T.dbar[d];
        if (!bar) continue;
        int dup = 0;
        for (int k = 0; k < oi; k++) if (T.dbar[T.order[k]] == bar) dup = 1;
        if (dup) continue;                                              // this controller was tried for an earlier device
        if (T.dbar1[d]) {                                               // _PS0: clear the D-state bits of the PCI-config mirror (PSAT at +0x84)
            klog_set_scan_window(T.dbar1[d] & 0xFFC00000u);
            crumb("touchpad: power state of %x", T.dbar1[d]);
            klog_flush();
            volatile uint8_t* m = blk_map(T.dbar1[d]);
            if (m) {
                uint32_t ps = mm_rd((void*)m, 0x84);
                tlog("%s: PSAT %x", T.dev[d].ctrl, ps);
                klog_flush();
                if (ps != 0xFFFFFFFFu && (ps & 3)) {
                    mm_wr((void*)m, 0x84, ps & ~3u);
                    ms_delay(20);
                    tlog("  set to D0: PSAT %x", mm_rd((void*)m, 0x84));
                }
            }
        }
        klog_set_scan_window(bar & 0xFFC00000u);
        crumb("touchpad: controller at %x", bar);
        klog_flush();
        if (try_block(bar)) { T.state = 2; klog_set_scan_window(0); return; }
        tlog("%s at %x: the touchpad did not answer", T.dev[d].ctrl, bar);
    }
    klog_set_scan_window(0);
    paging_probe_window(0);
    T.state = 3;
    tlog("NOT WORKING: no controller answered (`touchpad aml` and the stick log have the details)");
}

static void scan_restart(void) {
    crumbs_on = 1;
    S.w = 0x40000000u; S.p = 0; S.pages = 0; S.cycles = 0; S.active = 0; S.ticks = 0;
    T.nblk = 0;
    T.state = 1;
}

static void scan_finish(void) {
    klog_set_scan_window(0);
    if (T.state != 2) paging_probe_window(0);                    // a running touchpad keeps the window pointed at its block
    S.active = 0;
    tlog("scan: %u pages read in %u Mcycles, %d DesignWare I2C block(s)", S.pages, (uint32_t)(S.cycles >> 20), T.nblk);
    if (T.state != 2) {
        T.state = 3;
        tlog("the touchpad did not answer on any controller block (`touchpad aml` shows what the AML says)");
        if (T.ndev) dump_ctrl_aml(T.dev[0].ctrl);
    }
}

static void scan_step(void) {
    if (T.paused) return;
    if (tick_count < SCAN_START_TICK) return;
    uint64_t t0 = rdtsc();
    volatile uint32_t* v = 0;
    if (++S.ticks % 500 == 0) tlog("  hunt: %x+%x, %u pages, %u Mcycles", S.w, S.p, S.pages, (uint32_t)(S.cycles >> 20));
    while (S.w < 0xFEC00000u && T.nblk < MAXBLK) {
        if (S.p == 0) {                                          // entering a window: skip it when nothing in it may be read
            int any = 0;
            for (uint32_t p = 0; p < 0x400000u && !any; p += 0x1000) if (!rng_has(S.w + p)) any = 1;
            if (!any) { S.w += 0x400000u; continue; }
        }
        if (!v) {
            klog_set_scan_window(S.w);                           // on the stick BEFORE the read: a freeze leaves the culprit in the log
            crumb("touchpad scan %x", S.w);
            t0 = rdtsc();                                        // the budget is for reading, not for drawing the breadcrumb
            v = (volatile uint32_t*)paging_probe_window(S.w);
            if (!v) { tlog("scan: no probe window available"); S.w = 0xFEC00000u; break; }
        }
        for (; S.p < 0x400000u; S.p += 0x1000) {
            if (rng_has(S.w + S.p)) continue;
            S.pages++;
            if (v[(S.p + 0xFC) / 4] == DW_COMP_TYPE && T.nblk < MAXBLK) {
                uint32_t phys = S.w + S.p;
                S.p += 0x1000;
                T.blk[T.nblk++] = phys;
                tlog("  DesignWare I2C block at %x", phys);
                crumb("touchpad: trying the I2C block at %x", phys);
                if (try_block(phys)) { T.state = 2; S.cycles += rdtsc() - t0; scan_finish(); return; }   // found: the window stays on this block
                v = (volatile uint32_t*)paging_probe_window(S.w);                    // try_block re-pointed it at the same block
            }
            if ((S.pages & 7) == 0 && rdtsc() - t0 > SCAN_BUDGET) { S.p += 0x1000; S.cycles += rdtsc() - t0; paging_probe_window(0); return; }
        }
        S.p = 0; S.w += 0x400000u;
        if (!(S.w & 0x0FFFFFFFu)) tlog("  hunt at %x: %u pages read, %u Mcycles", S.w, S.pages, (uint32_t)(S.cycles >> 20));
        if (rdtsc() - t0 > SCAN_BUDGET) { S.cycles += rdtsc() - t0; paging_probe_window(0); return; }
        v = 0;
    }
    S.cycles += rdtsc() - t0;
    scan_finish();
}

static void hexline(const char* what, const uint8_t* b, int n) {
    char s[40]; int k = 0;
    for (int i = 0; i < n && i < 12; i++) { s[k++] = "0123456789abcdef"[b[i] >> 4]; s[k++] = "0123456789abcdef"[b[i] & 15]; s[k++] = ' '; }
    s[k] = 0;
    tlog("%s %s", what, s);
}

// LPSS wrapper registers at +0x800: reset release (+0x804, bits 0-1) and the clock gate (+0x800, bit 0)
static void lpss_kick(volatile uint8_t* regs) {
    uint32_t rst = mm_rd((void*)regs, 0x804), clk = mm_rd((void*)regs, 0x800);
    tlog("  LPSS clock %x resets %x general %x", clk, rst, mm_rd((void*)regs, 0x808));
    if ((rst & 3) != 3) mm_wr((void*)regs, 0x804, rst | 3);
    if (!(clk & 1) && clk != 0xFFFFFFFFu) mm_wr((void*)regs, 0x800, clk | 1);
    ms_delay(2);
}

// Does the touchpad answer on the controller block at `phys`? Two passes: as the firmware left it, then after the LPSS kick.
static int try_block(uint32_t phys) {
    volatile uint8_t* regs = blk_map(phys);
    if (!regs) { tlog("block %x: cannot map", phys); return 0; }
    T.dw.rd = mm_rd; T.dw.wr = mm_wr; T.dw.delay_us = us_delay; T.dw.ctx = (void*)regs;
    if (!dw_present(&T.dw)) { tlog("block %x: not a DesignWare I2C now (type %x)", phys, mm_rd((void*)regs, 0xFC)); return 0; }
    tlog("block %x: version %x param %x enable-status %x", phys, mm_rd((void*)regs, 0xF8), mm_rd((void*)regs, 0xF4), mm_rd((void*)regs, 0x9C));
    T.regs = regs;
    T.h.bus.xfer = bus_xfer; T.h.bus.delay_ms = ms_delay; T.h.bus.ctx = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) lpss_kick(regs);
        if (dw_setup(&T.dw, 0) < 0) { tlog("  controller will not switch off (pass %d)", pass); continue; }
        for (int oi = 0; oi < T.ndev; oi++) {
            int d = T.order[oi];
            const struct i2c_hid_dev* dev = &T.dev[d];
            if (T.dbar[d] && T.dbar[d] != phys) continue;               // the AML says this device sits on another controller
            static const uint16_t fallback[] = { 0x0020, 0x0001, 0x0000, 0x0002, 0x0010, 0x0030, 0x0100 };
            uint16_t cand[8 + 7];
            int nc = 0;
            for (int i = 0; i < dev->nreg; i++) cand[nc++] = dev->reg[i];
            for (unsigned i = 0; i < sizeof fallback / sizeof fallback[0]; i++) {
                int dup = 0;
                for (int k = 0; k < nc; k++) if (cand[k] == fallback[i]) dup = 1;
                if (!dup) cand[nc++] = fallback[i];
            }
            T.h.addr = dev->addr;
            int firstrc = 0;
            for (int i = 0; i < nc; i++) {
                int rc = hidi2c_get_desc(&T.h, cand[i]);
                if (i == 0) firstrc = rc;
                if (rc == -1) break;                                   // the bus is not answering at all: no point trying more registers
                if (rc == 0) {
                    tlog("  %s at %x on block %x: HID descriptor at register %x (pass %d)", dev->hid, dev->addr, phys, cand[i], pass);
                    T.cur = d; T.base = phys;
                    if (bring_up_device()) return 1;                  // a touchpad or mouse: running. Anything else (a touchscreen): next device
                    break;
                }
            }
            tlog("  addr %x: no HID descriptor (first try rc %d, abort %x, pass %d)", dev->addr, firstrc, T.dw.last_abort, pass);
        }
    }
    return 0;
}

static void try_all_blocks(void) {
    for (int i = 0; i < T.nblk; i++) {
        if (try_block(T.blk[i])) { T.state = 2; return; }
    }
    if (T.state != 2) T.state = 3;
}

// The device answered: power it, reset it, read and parse its report descriptor, choose the report format.
static int bring_up_device(void) {
    struct hidi2c* h = &T.h;
    tlog("  desc: vendor %x product %x version %x, report desc %u bytes, input reg %x max %u, command reg %x",
         h->vid, h->pid, h->ver, h->rdesc_len, h->in_reg, h->max_in, h->cmd_reg);
    int r = hidi2c_power(h, 1);
    if (r < 0) tlog("  power on: bus error %d", r);
    r = hidi2c_reset(h);
    tlog("  reset: %s", r == 0 ? "acknowledged" : r > 0 ? "no answer (carrying on)" : "bus error");
    uint8_t* rd = (uint8_t*)kmalloc(h->rdesc_len);
    if (!rd) { tlog("  out of memory"); return 0; }
    int n = hidi2c_report_desc(h, rd, h->rdesc_len);
    if (n < 0) { tlog("  report descriptor read failed (%d)", n); kfree(rd); return 0; }
    hexline("  rd[0..]:", rd, n);
    hexlog("rd ", rd, (uint32_t)n);                                // the whole report descriptor goes to the log ('a' lines)
    T.nptr = hid_parse_pointer(rd, n, T.lay, HID_MAXRPT, &T.ids);
    int tids = 0;
    T.have_touch = hid_parse_touch(rd, n, &T.touch, &tids);
    if (T.have_touch) T.ids = T.ids || tids;
    for (int i = 0; i < T.nptr; i++)
        tlog("  mouse report id %d: %s x%d@%d y%d@%d btn%d len %d", T.lay[i].id, T.lay[i].absolute ? "abs" : "rel", T.lay[i].x.bits, T.lay[i].x.off,
             T.lay[i].y.bits, T.lay[i].y.off, T.lay[i].btn[0].bits ? 1 : 0, T.lay[i].bytes);
    if (T.have_touch) {
        tlog("  touchpad report id %d: %d finger(s) x%d..%d y%d..%d len %d count %d button %d mode-feature %d", T.touch.id, T.touch.nf,
             T.touch.f[0].x.lmin, T.touch.f[0].x.lmax, T.touch.f[0].y.lmin, T.touch.f[0].y.lmax, T.touch.bytes, T.touch.count.bits, T.touch.button.bits,
             T.touch.mode.bits);
        int xr = T.touch.f[0].x.lmax - T.touch.f[0].x.lmin, yr = T.touch.f[0].y.lmax - T.touch.f[0].y.lmin;
        ptp_init(&T.ptp, xr > 0 ? xr : 1000, yr > 0 ? yr : 600, SCREEN_W);
        uint8_t mr[16];
        int ml = hid_touch_mode_report(&T.touch, tids, mr, sizeof mr);
        if (ml) {
            T.mode_len = ml; T.mode_ids = tids;
            for (int i = 0; i < ml && i < (int)sizeof T.mode_rep; i++) T.mode_rep[i] = mr[i];
            int rc = hidi2c_set_feature(h, tids ? mr[0] : 0, mr + (tids ? 1 : 0), ml - (tids ? 1 : 0));
            tlog("  Input Mode = touchpad: %s (%d)", rc == 0 ? "sent" : "failed", rc);
        }
    }
    kfree(rd);
    if (!T.nptr && !T.have_touch) { tlog("  neither a mouse nor a touchpad report in the descriptor"); return 0; }
    // try the fast bus speed; keep it only when the descriptor still reads back
    if (dw_setup(&T.dw, 1) == 0) {
        if (hidi2c_get_desc(h, h->desc_reg) != 0) { tlog("  fast mode failed: back to standard"); dw_setup(&T.dw, 0); }
        else tlog("  bus in fast mode");
    }
    hidi2c_power(h, 1);                                            // a command write points the device back at its input register
    T.fails = 0;
    tlog("  touchpad running");
    return 1;
}

// The device keeps answering with something that is not a report (seen on the real laptop when the bring-up ran in the
// first moments after power-up): start it over. Every third attempt also drops the bus back to standard speed.
static void recover_device(uint32_t t) {
    struct hidi2c* h = &T.h;
    T.recovers++;
    T.next_recover = t + (T.recovers < 12 ? 100 : 500);
    int slow = (T.recovers % 3) == 0;
    if (slow) dw_setup(&T.dw, 0);
    int r = hidi2c_restart(h);
    int rc = -9;
    if (T.mode_len) rc = hidi2c_set_feature(h, T.mode_ids ? T.mode_rep[0] : 0, T.mode_rep + (T.mode_ids ? 1 : 0), T.mode_len - (T.mode_ids ? 1 : 0));
    hidi2c_power(h, 1);
    tlog("restart %u (%s speed): reset %d, Input Mode %d", T.recovers, slow ? "standard" : "same", r, rc);
    T.garbage_run = 0;
    T.recover_logged = 1;
}

// ---- what the AML says about the controller (shown by `touchpad aml` when the hunt fails) -------------
#define alog(...) devlog('a', __VA_ARGS__)

static void hexlog(const char* what, const uint8_t* b, uint32_t n) {
    for (uint32_t o = 0; o < n; o += 20) {
        char s[64]; int k = 0;
        for (uint32_t i = o; i < o + 20 && i < n; i++) { s[k++] = "0123456789abcdef"[b[i] >> 4]; s[k++] = "0123456789abcdef"[b[i] & 15]; }
        s[k] = 0;
        alog("%s%x %s", what, o, s);
    }
}

static int find_nm(const uint8_t* t, uint32_t from, uint32_t end, const char* pat) {
    for (uint32_t i = from; i + 4 <= end; i++)
        if (t[i] == (uint8_t)pat[0] && t[i + 1] == (uint8_t)pat[1] && t[i + 2] == (uint8_t)pat[2] && t[i + 3] == (uint8_t)pat[3]) return (int)i;
    return -1;
}

static void dump_ctrl_aml(const char* path) {
    int sl = (int)strlen(path);
    const char* nm = path + (sl >= 4 ? sl - 4 : 0);
    uint8_t* tabs[24];
    uint32_t lens[24];
    int nt = acpi_aml_tables(tabs, lens, 24);
    int nop = 0;
    for (int ti = 0; ti < nt; ti++) {
        const uint8_t* t = tabs[ti];
        uint32_t len = lens[ti];
        for (uint32_t i = 36; i + 12 < len; i++) {
            if (t[i] != 0x5B) continue;
            if (t[i + 1] == 0x80 && nop < 14 && i + 14 < len) {                 // OperationRegion(name, space, offset, length)
                const uint8_t* q = t + i + 2;
                uint8_t space = q[4];
                if (space == 0 || space == 2) {
                    uint32_t off = 0, o = 0;
                    if (q[5] == 0x0C) { off = (uint32_t)q[6] | ((uint32_t)q[7] << 8) | ((uint32_t)q[8] << 16) | ((uint32_t)q[9] << 24); o = 1; }
                    else if (q[5] == 0x0B) { off = (uint32_t)q[6] | ((uint32_t)q[7] << 8); o = 1; }
                    else if (q[5] == 0x0A) { off = q[6]; o = 1; }
                    if (o) { alog("OpRegion %c%c%c%c %s offset %x", q[0], q[1], q[2], q[3], space == 0 ? "mem" : "pcicfg", off); nop++; }
                }
                continue;
            }
            if (t[i + 1] != 0x82) continue;                                     // DeviceOp
            uint32_t p = i + 2, lead = t[p], cnt = (lead >> 6) & 3;
            uint32_t pk = cnt == 0 ? (lead & 0x3F) : (lead & 0x0F);
            for (uint32_t k = 0; k < cnt; k++) pk |= (uint32_t)t[p + 1 + k] << (4 + 8 * k);
            uint32_t nstart = p + 1 + cnt, end = p + pk;
            if (end > len || memcmp(t + nstart, nm, 4)) continue;
            alog("device %s: %u bytes of AML", nm, pk);
            int c = find_nm(t, nstart + 4, end, "_CRS");
            if (c >= 0) hexlog("_CRS ", t + c + 4, end - (uint32_t)(c + 4) > 60 ? 60 : end - (uint32_t)(c + 4));
            int ps = find_nm(t, nstart + 4, end, "_PS0");
            if (ps >= 0) hexlog("_PS0 ", t + ps + 4, end - (uint32_t)(ps + 4) > 60 ? 60 : end - (uint32_t)(ps + 4));
            int st = find_nm(t, nstart + 4, end, "_STA");
            if (st >= 0) hexlog("_STA ", t + st + 4, end - (uint32_t)(st + 4) > 24 ? 24 : end - (uint32_t)(st + 4));
        }
        kfree(tabs[ti]);
    }
}

// ---- entry points -------------------------------------------------------------------------------
void i2chid_init(uint32_t mboot_phys) {
    if (cmdline_has(mboot_phys, "noi2c")) { tlog("disabled (noi2c)"); return; }
    crumb("touchpad: reading the ACPI tables");
    uint8_t* tabs[24];
    uint32_t lens[24];
    int nt = acpi_aml_tables(tabs, lens, 24);
    T.ndev = 0;
    for (int i = 0; i < nt; i++)
        if (T.ndev < MAXDEV) T.ndev += i2caml_find(tabs[i], lens[i], T.dev + T.ndev, MAXDEV - T.ndev);
    // touchpads first (a touchscreen on the same controller must not be tried before it)
    int no = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < T.ndev; i++) {
            const char* h = T.dev[i].hid;
            int pad = !strncmp(h, "SYNA", 4) || !strncmp(h, "ELAN", 4) || !strncmp(h, "ALPS", 4) || !strncmp(h, "CYAP", 4) || !strncmp(h, "FTE", 3) || !strncmp(h, "HPQ", 3);
            if (pad == (pass == 0)) T.order[no++] = i;
        }
    // where the AML says each controller's registers are
    for (int i = 0; i < T.ndev; i++) {
        struct i2c_ctrl_bars b;
        if (i2caml_ctrl_bars((const uint8_t* const*)tabs, lens, nt, T.dev[i].ctrl, aml_rd, &b)) {
            T.dbar[i] = b.bar0; T.dbar1[i] = b.psat_known ? b.bar1 : 0;
            tlog("AML: %s registers at %x (len %x), config mirror %x", T.dev[i].ctrl, b.bar0, b.len0, b.bar1);
        } else tlog("AML: no register address found for %s", T.dev[i].ctrl);
    }
    if (T.ndev) {                                                       // the touchpad's interrupt pin (an INT33FF community: pad register = base + 0x4400 + 0x400 * (pin / 15) + 8 * (pin % 15))
        uint32_t pin, gb;
        char src[5];
        int chv;
        const struct i2c_hid_dev* d0 = &T.dev[T.order[0]];
        if (i2caml_gpioint((const uint8_t* const*)tabs, lens, nt, d0->name, &pin, src) && i2caml_mem32((const uint8_t* const*)tabs, lens, nt, src, &gb, &chv)) {
            tlog("%s interrupt: pin %u of %s (registers %x, %s)", d0->hid, pin, src, gb, chv ? "Cherry Trail pin controller" : "unknown controller type");
            if (chv && gb >= 0xFEC00000u && gb < 0xFF000000u) {
                T.attn = (volatile uint32_t*)(gb + 0x4400 + 0x400 * (pin / 15) + 8 * (pin % 15));      // inside the identity-mapped uncached 0xFEC00000 window
                tlog("  pad %x: control0 %x control1 %x", (uint32_t)T.attn, T.attn[0], T.attn[1]);
                T.attn_last = (T.attn[0] >> 1) & 1;
            }
        } else tlog("%s: no interrupt pin found in the AML", d0->hid);
    }
    for (int i = 0; i < nt; i++) kfree(tabs[i]);
    if (!T.ndev) return;                                                // nothing to look for: stay out of the way
    for (int i = 0; i < T.ndev; i++) {
        const struct i2c_hid_dev* d = &T.dev[i];
        tlog("ACPI: %s at I2C address %x, %u Hz, controller %s", d->hid, d->addr, d->speed, d->ctrl);
        char rs[48]; int k = 0;
        for (int j = 0; j < d->nreg && k < 40; j++) { rs[k++] = "0123456789abcdef"[(d->reg[j] >> 4) & 15]; rs[k++] = "0123456789abcdef"[d->reg[j] & 15]; rs[k++] = ' '; }
        rs[k] = 0;
        tlog("  _DSM register candidates: %s", k ? rs : "none");
    }
    crumb("touchpad: memory map and PCI ranges");
    collect_ranges(mboot_phys);
    dump_ctrl_aml(T.dev[T.order[0]].ctrl);        // what the AML says about the controller (also in the on-stick log with the raw tables)
    int have = 0;
    for (int i = 0; i < T.ndev; i++) if (T.dbar[i]) have = 1;
    if (have && !cmdline_has(mboot_phys, "i2cscan")) {
        T.state = 5;                              // the firmware told us where the controller is: bring it up from the main loop (no scan)
        tlog("controller addresses known from the AML: bring-up starts 1.5 s after boot");
    } else if (cmdline_has(mboot_phys, "i2cscan")) {
        scan_restart();                           // the hunt itself runs from i2chid_poll, a little per tick
        tlog("controller hunt starts %u s after boot (a little per tick)", SCAN_START_TICK / 100);
    } else {
        T.state = 4;                              // the first real run froze inside the blind memory hunt: it only runs when asked for
        tlog("controller not located: the blind hunt is off (`touchpad scan` starts it)");
    }
}

void i2chid_retry(void) {
    if (!T.ndev) { tlog("no I2C-HID device in ACPI"); return; }
    if (T.state == 2) { tlog("already running"); return; }
    if (!T.nblk) { scan_restart(); tlog("no controller block known yet: hunting again"); return; }
    T.state = 1;
    try_all_blocks();
    if (T.state != 2) T.state = 3;
}

static void process(const uint8_t* b, int n, uint32_t now) {
    if (T.have_touch) {
        struct hid_contact c[HID_MAXFINGER];
        int nc, count, button;
        if (hid_touch_decode(&T.touch, T.ids, b, n, c, &nc, &count, &button)) {
            struct ptp_out o;
            T.button = button;
            ptp_report(&T.ptp, now, c, nc, count, button, &o);
            if (o.changed) mouse_inject(o.dx, o.dy, o.wheel, o.buttons);
            return;
        }
    }
    for (int i = 0; i < T.nptr; i++) {
        int rx, ry, w, bt;
        if (!hid_decode(&T.lay[i], T.ids, b, n, &rx, &ry, &w, &bt)) continue;
        if (T.lay[i].absolute) {
            int cx, cy;
            mouse_get_position(&cx, &cy);
            rx = (int)(((uint32_t)rx * (uint32_t)(SCREEN_W - 1)) / 65535u) - cx;
            ry = (int)(((uint32_t)ry * (uint32_t)(SCREEN_H - 1)) / 65535u) - cy;
        }
        mouse_inject(rx, ry, -w, bt);
        return;
    }
    T.unmatched++;
}

void i2chid_enable(int on) { T.paused = !on; tlog(on ? "polling on" : "polling off"); }

void i2chid_rescan(void) {
    if (!T.ndev) { tlog("no I2C-HID device in ACPI"); return; }
    if (T.state == 2) { tlog("already running"); return; }
    scan_restart();
    tlog("hunting again from the start");
}

void i2chid_poll(void) {
    if (T.state == 1) { scan_step(); return; }
    if (T.state == 5) { aml_bringup(); return; }
    if (T.state != 2 || T.paused) return;
    uint32_t t = tick_count;
    if (t == T.last_tick) return;
    T.last_tick = t;
    static uint8_t buf[HIDI2C_MAXREPORT];
    int lvl = 1;
    if (T.attn) {                                                  // observe the interrupt line (diagnostics: the read below is not gated by it)
        lvl = (int)((T.attn[0] >> 1) & 1);
        if (!lvl) T.attn_low++;
        if (lvl != T.attn_last) {
            T.attn_last = lvl;
            if (++T.attn_edges <= 30) tlog("interrupt line %s", lvl ? "high" : "low");
        }
    }
    int n = hidi2c_input(&T.h, buf, sizeof buf);
    if (n > 0 && !lvl) T.reports_low++;
    uint32_t now = t * 10;
    if (n == -5) {                                                 // a burst of non-reports (not stray ones minutes apart) restarts the device
        if (t - T.garbage_tick > 300) T.garbage_run = 0;
        T.garbage_tick = t;
        if (++T.garbage_run >= 30 && t >= T.next_recover) recover_device(t);
    } else if (n > 0) {
        T.garbage_run = 0;
        if (T.recover_logged) { tlog("reports are coming after restart %u", T.recovers); T.recover_logged = 0; }
    }
    if (n > 0) {
        T.reports++;
        T.fails = 0;
        if (T.logged < 3) { T.logged++; hexline("report:", buf, n); }
        process(buf, n, now);
    } else if (n == 0) {
        T.empty++;
        if (T.have_touch) {
            struct ptp_out o;
            ptp_idle(&T.ptp, now, T.button, &o);
            if (o.changed) mouse_inject(o.dx, o.dy, o.wheel, o.buttons);
        }
    } else {
        T.errors++;
        if (T.errors <= 3 || (n == -5 && ((T.errors <= 40 && (T.errors & 7) == 0) || (T.recovers && T.recovers <= 20 && T.garbage_run == 1)))) {
            tlog("input read error %d (abort %x), max_in %u", n, T.dw.last_abort, T.h.max_in);
            if (n == -5) hexline("  raw:", T.h.bad, 16);
        }
        if (n == -1 && ++T.fails >= 20) { tlog("bus stuck: touchpad polling stopped"); T.state = 3; }
    }
}

static int sfmt(char* buf, int cap, int at, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[100];
    int n = devlog_fmt(line, sizeof line, fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && at < cap - 2; i++) buf[at++] = line[i];
    if (at < cap - 1) buf[at++] = '\n';
    buf[at] = 0;
    return at;
}

void i2chid_aml(char* buf, int cap) {
    buf[0] = 0;
    devlog_tail('a', buf, cap, 0, 24);
}

void i2chid_status(char* buf, int cap) {
    buf[0] = 0;
    const char* st = T.state == 2 ? (T.paused ? "paused (touchpad on)" : "running") : T.state == 1 ? "searching" : T.state == 3 ? "NOT WORKING" :
                     T.state == 4 ? "controller not located (`touchpad scan` hunts for it)" : T.state == 5 ? "starting (address from the AML)" : "no I2C-HID device in ACPI";
    int at = sfmt(buf, cap, 0, "touchpad: %s, reports %u, empty polls %u, errors %u, unmatched %u", st, T.reports, T.empty, T.errors, T.unmatched);
    if (T.state == 2)
        at = sfmt(buf, cap, at, "input reads: %u NACK, %u length 0, %u length ffff, %u not a report; interrupt line low %u polls, %u edges, %u reports while low",
                  T.h.nack, T.h.zeros, T.h.ones, T.h.stale, T.attn_low, T.attn_edges, T.reports_low);
    if (T.state == 1)
        at = sfmt(buf, cap, at, "hunt: %s at %x, %u pages read, %d controller block(s) found%s", tick_count < SCAN_START_TICK ? "starts soon" : "now", S.w, S.pages, T.nblk,
                  T.paused ? " (paused)" : "");
    devlog_tail('t', buf, cap, at, 22);
}

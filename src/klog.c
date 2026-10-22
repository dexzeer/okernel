// The on-stick log (see klog.h).
#include "klog.h"
#include "acpi.h"
#include "i2chid.h"
#include "memory.h"
#include "string.h"
#include "xhci.h"
#include "rtw/rtw.h"
#include "devlog.h"
#include "serial.h"
#include "cpufreq.h"
#include "ata.h"
#include "part.h"
#include "crash.h"

extern uint32_t tick_count;

typedef char klog_hdr_is_512[(sizeof(struct klog_hdr) == 512) ? 1 : -1];

static char L[KLOG_LOG_SECTORS * 512];       // this boot's log (also collects what was logged before the stick showed up)
static uint32_t len, flushed;
static int truncated;
static int state;                            // 0 waiting for the stick, 1 logging, 2 off
static uint32_t S0;                          // first sector of the region
static uint32_t log_sect;                    // first sector of this boot's log slot
static struct klog_hdr H;
static uint32_t last_flush, last_snap;
static int in_flush;
static char snap[KLOG_SNAP_SECTORS * 512];
static uint8_t chunk[64 * 512];
static int on_ata;                           // the region is the log partition of the installed disk, not the stick

static void put(const char* s) {
    while (*s && len < sizeof L - 1024) L[len++] = *s++;
}

static void put_num(uint32_t v) {
    char t[12];
    int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    char o[12];
    int n = 0;
    while (k) o[n++] = t[--k];
    o[n] = 0;
    put(o);
}

void klog_write(char tag, const char* line) {
    if (len >= sizeof L - 1024) {
        if (!truncated) { truncated = 1; put("[log full]\n"); }
        return;
    }
    put("t="); put_num(tick_count); put(" [");
    char t[2] = { tag, 0 };
    put(t); put("] ");
    put(line);
    put("\n");
}

static int wr(uint32_t sect, const void* data, uint32_t n) {
    if (!on_ata) return xhci_storage_rw(1, sect, (void*)data, n);
    for (uint32_t i = 0; i < n; i++)
        if (ata_write(sect + i, (const uint8_t*)data + i * 512)) return 0;
    return 1;
}

static uint32_t ser_flushed;                                 // serial_total up to which the stick has it

// Copy the part of the serial ring the stick has not seen yet (whole sectors, in runs that do not wrap) and tell the header.
static void ser_flush(void) {
    uint32_t end = serial_total;
    if (end == ser_flushed) return;
    uint32_t from = ser_flushed;
    if (end - from > SERIAL_RING) from = end - SERIAL_RING;
    uint32_t s = from / 512, last = (end - 1) / 512;            // absolute sector numbers; the ring slot is s % KLOG_SER_SECTORS
    uint32_t base = S0 + KLOG_SER_FIRST + (H.boot & 1) * KLOG_SER_SECTORS;
    while (s <= last) {
        uint32_t slot = s % KLOG_SER_SECTORS, n = last - s + 1;
        if (n > 64) n = 64;
        if (slot + n > KLOG_SER_SECTORS) n = KLOG_SER_SECTORS - slot;
        if (!wr(base + slot, serial_ring + slot * 512, n)) { state = 2; return; }
        s += n;
    }
    ser_flushed = end;
    H.ser_total[H.boot & 1] = end;
    H.ser_boot[H.boot & 1] = H.boot;
    wr(S0, &H, 1);
}

void klog_flush(void) {
    if (state != 1 || in_flush) return;
    uint32_t end = len;
    if (end == flushed) return;
    in_flush = 1;
    uint32_t first = flushed / 512, last = (end + 511) / 512;
    if (last < KLOG_LOG_SECTORS) last++;                       // one zero sector behind the text: the end marker (older boots left data there)
    for (uint32_t s = first; s < last; ) {
        uint32_t n = last - s > 64 ? 64 : last - s;
        if (!wr(log_sect + s, L + s * 512, n)) { state = 2; in_flush = 0; return; }
        s += n;
    }
    flushed = end;
    in_flush = 0;
}

int klog_active(void) { return state == 1; }

static void write_header(void) { wr(S0, &H, 1); }

void klog_set_scan_window(uint32_t w) {
    if (state != 1) return;
    H.scan_window = w;
    write_header();
}

static void dump_tables(void) {
    uint8_t* tabs[24];
    uint32_t lens[24];
    int nt = acpi_aml_tables(tabs, lens, 24);
    uint32_t sect = KLOG_TAB_FIRST;
    H.ntab = 0;
    for (int i = 0; i < nt; i++) {
        uint32_t nsect = (lens[i] + 511) / 512;
        if (H.ntab < 16 && sect + nsect <= KLOG_SER_FIRST) {
            struct klog_tab* t = &H.tab[H.ntab];
            memcpy(t->sig, tabs[i], 4); t->sect = sect; t->len = lens[i];
            int ok = 1;
            for (uint32_t s = 0; s < nsect && ok; s += 64) {
                uint32_t n = nsect - s > 64 ? 64 : nsect - s;
                memset(chunk, 0, sizeof chunk);
                uint32_t off = s * 512, cnt = lens[i] - off > n * 512 ? n * 512 : lens[i] - off;
                memcpy(chunk, tabs[i] + off, cnt);
                ok = wr(S0 + sect + s, chunk, n);
            }
            if (!ok) { kfree(tabs[i]); for (int j = i + 1; j < nt; j++) kfree(tabs[j]); return; }
            H.ntab++;
            sect += nsect;
        }
        kfree(tabs[i]);
    }
}

static void init_region(void) {
    H.boot++;
    log_sect = S0 + 1 + (H.boot & 1) * KLOG_LOG_SECTORS;
    uint32_t prev_scan = H.scan_window;
    state = 1;
    if (on_ata) devlog('k', "log on the disk's log partition: boot %u", H.boot);
    if (prev_scan) devlog('k', "the last boot died while reading window %x", prev_scan);
    write_header();
    dump_tables();
    write_header();
    klog_flush();
}

static void init_stick(uint32_t sectors) {
    if (sectors < KLOG_REGION_SECTORS + 4096) { state = 2; devlog('k', "stick too small for the log"); return; }
    S0 = sectors - KLOG_REGION_SECTORS;
    if (!xhci_storage_rw(0, S0, &H, 1)) { state = 2; return; }
    if (memcmp(H.magic, KLOG_MAGIC, 16)) {
        state = 2;
        devlog('k', "USB stick without the log marker: not touched");
        return;
    }
    devlog('k', "log on the USB stick, stick %u MB", sectors >> 11);
    init_region();
}

// The log partition of an installed disk (part.h) carries the same region at its start (the image builder marks it).
static int init_disk(void) {
    const struct part* p = part_role(PART_LOG);
    if (!p || p->count < CRASH_PART_SECTORS || ata_read(p->start, (uint8_t*)&H) || memcmp(H.magic, KLOG_MAGIC, 16)) return 0;
    on_ata = 1;
    S0 = p->start;
    init_region();
    return 1;
}

uint32_t klog_boot(void) { return state == 1 ? H.boot : 0; }

// From the panic screen: everything logged so far goes to the disk now (interrupts are off; nothing else runs).
void klog_panic_flush(void) {
    if (state != 1 || !on_ata) return;
    in_flush = 0;
    klog_flush();
    if (state == 1) ser_flush();
}

void klog_poll(void) {
    if (state == 2) return;
    if (state == 0) {
        static int disk_tried;                                 // once pfs_init has read the partition table
        if (!disk_tried && part_count() > 0) {
            disk_tried = 1;
            if (init_disk()) { last_flush = last_snap = tick_count; return; }
        }
        static uint32_t next;
        if ((int)(tick_count - next) < 0) return;
        next = tick_count + 50;
        uint32_t sectors;
        if (!xhci_storage_ready(&sectors)) return;
        init_stick(sectors);
        last_flush = last_snap = tick_count;
        return;
    }
    uint32_t sectors;
    if (!on_ata && !xhci_storage_ready(&sectors)) { state = 2; return; }
    if ((int)(tick_count - last_flush) >= 100) { last_flush = tick_count; klog_flush(); if (state == 1) ser_flush(); }
    if (state == 1 && (int)(tick_count - last_snap) >= 500) {
        last_snap = tick_count;
        memset(snap, 0, sizeof snap);
        xhci_status(snap, 9000);
        int n = (int)strlen(snap);
        snap[n++] = '\n';
        i2chid_status(snap + n, 6000);
        n = (int)strlen(snap);
        snap[n++] = 10;
        rtw_command("status", snap + n, 2500);                  // Wi-Fi adapter state
        n = (int)strlen(snap);
        snap[n++] = 10;
        cpufreq_report(snap + n, 1000);                         // clock measurements
        n = (int)strlen(snap);
        snap[n++] = 10;
        {
            uint32_t tot, used, peak, lf;
            heap_stats(&tot, &used, &peak, &lf);
            char t[160];
            int k = 0;
            const char* p = "heap KB total/used/peak/largest-free: ";
            while (*p) t[k++] = *p++;
            uint32_t v[4] = { tot >> 10, used >> 10, peak >> 10, lf >> 10 };
            for (int i = 0; i < 4; i++) {
                char d[12]; int m = 0; uint32_t x = v[i];
                if (!x) d[m++] = '0';
                while (x) { d[m++] = (char)('0' + x % 10); x /= 10; }
                while (m) t[k++] = d[--m];
                t[k++] = i < 3 ? '/' : '\n';
            }
            t[k] = 0;
            memcpy(snap + n, t, (size_t)k + 1);
        }
        wr(S0 + 513, snap, KLOG_SNAP_SECTORS / 2);             // 16 KB are plenty
    }
}

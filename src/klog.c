// The on-stick log (see klog.h).
#include "klog.h"
#include "acpi.h"
#include "i2chid.h"
#include "memory.h"
#include "string.h"
#include "xhci.h"
#include "rtw/rtw.h"
#include "devlog.h"

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

static int wr(uint32_t sect, const void* data, uint32_t n) { return xhci_storage_rw(1, sect, (void*)data, n); }

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
        if (H.ntab < 16 && sect + nsect <= KLOG_REGION_SECTORS) {
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

static void init_stick(uint32_t sectors) {
    if (sectors < KLOG_REGION_SECTORS + 4096) { state = 2; devlog('k', "stick too small for the log"); return; }
    S0 = sectors - KLOG_REGION_SECTORS;
    if (!xhci_storage_rw(0, S0, &H, 1)) { state = 2; return; }
    if (memcmp(H.magic, KLOG_MAGIC, 16)) {
        state = 2;
        devlog('k', "USB stick without the log marker: not touched");
        return;
    }
    H.boot++;
    log_sect = S0 + 1 + (H.boot & 1) * KLOG_LOG_SECTORS;
    uint32_t prev_scan = H.scan_window;
    state = 1;
    devlog('k', "log on the USB stick: boot %u, stick %u MB", H.boot, sectors >> 11);
    if (prev_scan) devlog('k', "the last boot died while reading window %x", prev_scan);
    write_header();
    dump_tables();
    write_header();
    klog_flush();
}

void klog_poll(void) {
    if (state == 2) return;
    if (state == 0) {
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
    if (!xhci_storage_ready(&sectors)) { state = 2; return; }
    if ((int)(tick_count - last_flush) >= 100) { last_flush = tick_count; klog_flush(); }
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
        wr(S0 + 513, snap, KLOG_SNAP_SECTORS / 2);             // 16 KB are plenty
    }
}

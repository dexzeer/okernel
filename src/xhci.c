// xHCI host controller + USB pointing devices (see xhci.h). Written from the xHCI 1.2, USB 2.0 and HID 1.11
// specifications. Polled; every wait is bounded so a dead controller can never hang the boot.
//
// The driver is meant to take whatever pointer the user plugs in: boot-protocol mice, mice with report ids and 16 bit
// axes, wireless receivers (composite devices with several HID interfaces), tablets (absolute), and any of them behind
// USB 2 hubs. Every device is identified in an on-screen log (`usb` shell command), because the machine this runs on
// has no serial port to read.
#include "xhci.h"
#include <stdarg.h>
#include "cursor.h"
#include "devlog.h"
#include "graphics.h"
#include "hid.h"
#include "lapic.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "pcix.h"
#include "serial.h"
#include "string.h"
#include "rtw/rtw.h"

extern uint32_t tick_count;          // 100 Hz since boot (desktop.c); not running while xhci_init() executes

// ---- register layout ---------------------------------------------------------------------------
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18

#define OP_USBCMD  0x00
#define OP_USBSTS  0x04
#define OP_PAGESZ  0x08
#define OP_CRCR    0x18
#define OP_DCBAAP  0x30
#define OP_CONFIG  0x38
#define OP_PORTSC  0x400            // + 0x10 * (port - 1)

#define CMD_RUN   (1u << 0)
#define CMD_RESET (1u << 1)
#define STS_HCH   (1u << 0)
#define STS_CNR   (1u << 11)

// PORTSC: RW1C change bits 17..23; PED (bit 1) is RW1C too (writing 1 disables the port)
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR  (1u << 4)
#define PORT_PP  (1u << 9)
#define PORT_CSC (1u << 17)
#define PORT_WRC (1u << 19)
#define PORT_PRC (1u << 21)
#define PORT_WPR (1u << 31)
#define PORT_CHANGES (0x7Fu << 17)
#define PORT_PRESERVE(v) ((v) & ~(PORT_CHANGES | PORT_PED | PORT_WPR))
#define PORT_PLS(v) (((v) >> 5) & 15)
#define PORT_SPEED(v) (((v) >> 10) & 15)

// interrupter 0 (runtime registers + 0x20)
#define IR_IMAN   0x20
#define IR_ERSTSZ 0x28
#define IR_ERSTBA 0x30
#define IR_ERDP   0x38

// TRB types
#define TRB_NORMAL 1
#define TRB_SETUP  2
#define TRB_DATA   3
#define TRB_STATUS 4
#define TRB_LINK   6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS 11
#define TRB_CONFIGURE 12
#define TRB_EVALUATE 13
#define TRB_RESET_EP 14
#define TRB_STOP_EP 15
#define TRB_SET_DEQ 16
#define EV_TRANSFER 32
#define EV_COMMAND 33
#define EV_PORT 34

struct trb { uint32_t lo, hi, status, control; };

struct ring {
    volatile struct trb* trb;
    uint32_t phys;
    int n;                       // TRBs including the link at the end
    int enq;
    int cycle;
};

// ---- state --------------------------------------------------------------------------------------
#define EVN 256
#define MAXSLOTS 16
#define NBUF 128                 // transfers kept in flight per pointer endpoint (ring of NBUF + 1 TRBs)
#define MAXEP 6                  // pointer endpoints served at once
#define MAXHUBPORT 15

// a pointer endpoint: one interrupt IN pipe delivering input reports
struct hidep {
    int used, slot, dci, mps, tlen, slotsz, ifnum;
    int boot;                    // fixed boot-protocol layout instead of a parsed descriptor
    int nl, uses_ids;
    struct hid_ptr lay[HID_MAXRPT];
    struct ring ring;
    uint8_t* buf;                // NBUF * slotsz
    int bufsz;
    uint32_t reports, errors, unmatched;
    int fail, recoveries, logged;
    uint16_t vid, pid;
    uint32_t last_tick, restarts, watchdog, bad, next_try;   // halted-endpoint restarts, restarts found by the state check, failed restarts
    int lastcode;                // completion code of the last transfer event
    uint32_t burst, idle_restarts, stall_logs;   // reports in the current burst; restarts after a pause; stall dumps written
    int armed;                   // a report came since the last pause check
    uint32_t since, planned;     // reports since the ring was last (re)started; restarts made just before the ring wraps
    int replan;                  // restart before the ring's link TRB is reached
    int dead;                    // gave up after repeated failed restarts
};

// a device that owns a slot
struct dev {
    int used, root, parent, pport, speed, depth;
    uint32_t route;
    int tt_slot, tt_port;
    int hub, nports;
    uint16_t vid, pid;
    uint8_t child[MAXHUBPORT + 1];     // slot of the device on hub port n
    uint8_t known[MAXHUBPORT + 1], tries[MAXHUBPORT + 1];
    uint32_t next[MAXHUBPORT + 1];
};

struct rport { uint8_t known, tries, slot; uint32_t next; };

static struct {
    int ready;
    struct pci_loc loc;
    uint16_t did;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* rt;
    volatile uint8_t* db;
    int maxslots, maxports, ctxsz;
    uint64_t* dcbaa;
    struct ring cmd;
    volatile struct trb* ev;
    uint32_t ev_phys;
    int ev_deq, ev_cycle;
    uint8_t* dma;                // 4KB: descriptors and the setup scratch
    volatile int cmd_done, cmd_code, cmd_slot;
    volatile int ctl_done, ctl_code, ctl_len, ctl_want;
    uint32_t port_pending;       // bit n = port n changed
    uint32_t next_scan;
    int rescan_req;
    uint32_t reports;
    uint32_t slow, slowmax;      // polls that took >= 50 ms, and the longest in 10 ms ticks
    struct ring ep0[MAXSLOTS + 1];
    uint8_t* devctx[MAXSLOTS + 1];
    uint8_t* inctx;
} X;

// the one USB mass storage device (bulk-only transport) the on-stick log uses; see klog.c
static struct {
    int used, failed, slot, in_dci, out_dci, in_ep, out_ep;
    struct ring rin, rout;
    uint8_t* data;               // 64 KB, 64 KB aligned: one TRB never crosses a 64 KB boundary
    uint8_t* cb;                 // CBW at 0, CSW at 64
    uint32_t tag;
    uint32_t sectors;            // capacity (blocks of 512; larger devices are cut at 2^32 blocks)
    volatile int done, code;
} B;

#define WL_NRX 6
#define WL_RXSZ 32768
#define WL_MAXOUT 4
static struct {
    int used, slot, speed, nout, started, dead;
    int in_ep, in_dci, out_ep[WL_MAXOUT], out_dci[WL_MAXOUT], out_mps[WL_MAXOUT];
    struct ring rin, rout[WL_MAXOUT];
    uint8_t* rx;                 // WL_NRX buffers of WL_RXSZ, aligned to their size (a TRB never crosses a 64 KB boundary)
    uint8_t* tx;                 // 64 KB, 64 KB aligned
    int rx_head, rx_len[WL_NRX];
    volatile int rx_state[WL_NRX];   // 0 queued, 1 completed, 2 failed
    volatile int out_done, out_code;
    uint16_t vid, pid;
    int ctl_fails;
    uint32_t rx_errors, rx_recoveries;
} WL;


static struct hidep E[MAXEP];
static struct dev D[MAXSLOTS + 1];
static struct rport RP[32];

static inline uint32_t rd32(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static inline void wr32(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
static inline void wr64(volatile uint8_t* b, uint32_t off, uint32_t phys) { wr32(b, off, phys); wr32(b, off + 4, 0); }   // low first

static void delay_us(uint32_t us) { pit_delay_us(us); }
static void delay_ms(uint32_t ms) { while (ms--) pit_delay_us(1000); }

#define xlog(...) devlog('u', __VA_ARGS__)
#define fmt_to devlog_fmt

// ---- memory ---------------------------------------------------------------------------------------
// physically contiguous, zeroed memory with an alignment (the heap lives in the high alias, so phys = V2P)
static void* dma_alloc(uint32_t size, uint32_t align) {
    uint8_t* p = (uint8_t*)kcalloc(1, size + align);
    if (!p) return 0;
    return (void*)(((uint32_t)p + align - 1) & ~(align - 1));
}
static uint32_t phys_of(const void* p) { return V2P((uint32_t)p); }

// ---- rings --------------------------------------------------------------------------------------
static void ring_reset(struct ring* r) {
    memset((void*)r->trb, 0, (uint32_t)(r->n - 1) * 16);
    volatile struct trb* l = &r->trb[r->n - 1];            // link back to the start, toggling the cycle bit
    l->lo = r->phys; l->hi = 0; l->status = 0;
    l->control = (TRB_LINK << 10) | (1u << 1);
    r->enq = 0; r->cycle = 1;
}

static int ring_init(struct ring* r, int n) {
    r->trb = (volatile struct trb*)dma_alloc((uint32_t)n * 16, 4096);
    if (!r->trb) return 0;
    r->phys = phys_of((const void*)r->trb);
    r->n = n;
    ring_reset(r);
    return 1;
}

static void ring_push(struct ring* r, uint32_t lo, uint32_t hi, uint32_t status, uint32_t control) {
    volatile struct trb* t = &r->trb[r->enq];
    t->lo = lo; t->hi = hi; t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = control | (uint32_t)r->cycle;
    r->enq++;
    if (r->enq == r->n - 1) {                           // hand the link TRB to the controller and wrap
        volatile struct trb* l = &r->trb[r->enq];
        l->control = (l->control & ~1u) | (uint32_t)r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
}

// ---- pointer reports ------------------------------------------------------------------------------
static int ep_recover(struct hidep* e);
static struct hidep* ep_find(int slot, int dci) {
    for (int i = 0; i < MAXEP; i++)
        if (E[i].used && E[i].slot == slot && E[i].dci == dci) return &E[i];
    return 0;
}

static void ep_requeue(struct hidep* e) {
    uint32_t idx = (uint32_t)e->ring.enq;
    ring_push(&e->ring, phys_of(e->buf + idx * (uint32_t)e->slotsz), 0, (uint32_t)e->tlen, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));   // IOC | ISP
    wr32(X.db, (uint32_t)e->slot * 4, (uint32_t)e->dci);
}

static void ptr_report(struct hidep* e, const uint8_t* b, int n) {
    if (n <= 0) return;
    if ((int)(tick_count - e->last_tick) > 25) e->burst = 0;
    e->burst++; e->armed = 1;
    e->reports++; X.reports++; e->last_tick = tick_count;
    if (e->logged < 2) {
        e->logged++;
        char hex[40]; int k = 0;
        for (int i = 0; i < n && i < 10; i++) { hex[k++] = "0123456789abcdef"[b[i] >> 4]; hex[k++] = "0123456789abcdef"[b[i] & 15]; hex[k++] = ' '; }
        hex[k] = 0;
        xlog("slot %d report (%d bytes): %s", e->slot, n, hex);
    }
    int rx, ry, w, bt;
    if (e->boot) {
        if (n < 3) return;
        mouse_inject((int8_t)b[1], (int8_t)b[2], n >= 4 ? -(int8_t)b[3] : 0, b[0] & 7);
        return;
    }
    for (int i = 0; i < e->nl; i++) {
        if (!hid_decode(&e->lay[i], e->uses_ids, b, n, &rx, &ry, &w, &bt)) continue;
        if (e->lay[i].absolute) {
            int cx, cy;
            mouse_get_position(&cx, &cy);
            rx = (int)(((uint32_t)rx * (uint32_t)(SCREEN_W - 1)) / 65535u) - cx;
            ry = (int)(((uint32_t)ry * (uint32_t)(SCREEN_H - 1)) / 65535u) - cy;
        }
        mouse_inject(rx, ry, -w, bt);
        return;
    }
    e->unmatched++;
}

// ---- event ring ---------------------------------------------------------------------------------
static void pump(void) {
    int moved = 0;
    for (;;) {
        volatile struct trb* t = &X.ev[X.ev_deq];
        uint32_t ctl = t->control;
        if ((int)(ctl & 1) != X.ev_cycle) break;
        uint32_t lo = t->lo, status = t->status;
        uint32_t type = (ctl >> 10) & 63;
        if (type == EV_COMMAND) {
            X.cmd_code = (int)(status >> 24); X.cmd_slot = (int)(ctl >> 24); X.cmd_done = 1;
        } else if (type == EV_TRANSFER) {
            int slot = (int)(ctl >> 24), ep = (int)((ctl >> 16) & 31), code = (int)(status >> 24);
            struct hidep* e = ep_find(slot, ep);
            if (e) {
                uint32_t idx = (lo - e->ring.phys) / 16;
                e->lastcode = code;
                if (idx < NBUF && (code == 1 || code == 13)) {
                    int n = e->tlen - (int)(status & 0xFFFFFF);
                    e->recoveries = 0; e->bad = 0;
                    ptr_report(e, e->buf + idx * (uint32_t)e->slotsz, n);
                    ep_requeue(e);
                    if (++e->since >= NBUF - 8) e->replan = 1;      // the controller stalled at the ring's link TRB on the real laptop: never get there
                } else if (code == 26 || code == 27) {
                    // Stopped: the leftovers of a Stop Endpoint we issued ourselves; the restart queues everything again
                } else {
                    e->errors++;
                    e->fail = code ? code : 99;
                }
            } else if (B.used && slot == B.slot && (ep == B.in_dci || ep == B.out_dci)) {
                B.code = code; B.done = 1;
            } else if (WL.used && slot == WL.slot && ep != 1) {
                if (ep == WL.in_dci) {
                    uint32_t idx = (lo - WL.rin.phys) / 16;
                    if (idx < WL_NRX) {
                        if (code == 1 || code == 13) { WL.rx_len[idx] = WL_RXSZ - (int)(status & 0xFFFFFF); WL.rx_state[idx] = 1; }
                        else if (code != 26 && code != 27) { WL.rx_len[idx] = 0; WL.rx_state[idx] = 2; }
                    }
                } else {
                    WL.out_code = code; WL.out_done = 1;
                }
            } else {
                X.ctl_code = code; X.ctl_len = X.ctl_want - (int)(status & 0xFFFFFF); X.ctl_done = 1;
            }
        } else if (type == EV_PORT) {
            uint32_t port = lo >> 24;
            if (port >= 1 && port <= 31) X.port_pending |= 1u << port;
        }
        X.ev_deq++;
        if (X.ev_deq == EVN) { X.ev_deq = 0; X.ev_cycle ^= 1; }
        moved = 1;
    }
    if (moved) {
        uint32_t pa = X.ev_phys + (uint32_t)X.ev_deq * 16;
        wr32(X.rt, IR_ERDP, pa | 8);                  // EHB: clear the event handler busy flag
        wr32(X.rt, IR_ERDP + 4, 0);
        wr32(X.rt, IR_IMAN, rd32(X.rt, IR_IMAN) | 1); // clear IP (write 1)
    }
}

// ---- commands and control transfers -------------------------------------------------------------
static int command(uint32_t lo, uint32_t hi, uint32_t status, uint32_t control, int* slot_out) {
    X.cmd_done = 0;
    ring_push(&X.cmd, lo, hi, status, control);
    wr32(X.db, 0, 0);
    for (int i = 0; i < 20000; i++) {                  // 1 s
        pump();
        if (X.cmd_done) { if (slot_out) *slot_out = X.cmd_slot; return X.cmd_code; }
        delay_us(50);
    }
    return -1;
}

// a control transfer on endpoint 0 of `slot`; data (len bytes) goes to/from X.dma. Returns the completion code;
// X.ctl_len = bytes actually transferred.
static int control(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, int in) {
    struct ring* r = &X.ep0[slot];
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(r, (uint32_t)req_type | ((uint32_t)req << 8) | ((uint32_t)value << 16), (uint32_t)index | ((uint32_t)len << 16), 8,
              (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (len) ring_push(r, phys_of(X.dma), 0, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    ring_push(r, 0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));
    X.ctl_done = 0; X.ctl_want = len; X.ctl_len = 0;
    wr32(X.db, (uint32_t)slot * 4, 1);
    for (int i = 0; i < 20000; i++) {                  // 1 s (a working device answers in milliseconds)
        pump();
        if (X.ctl_done) return X.ctl_code;
        delay_us(50);
    }
    return -1;
}

static int is_ok(int c) { return c == 1 || c == 13; }

// bring a halted or wedged endpoint back to a defined state with its ring dequeue pointer at `deq_phys | cycle`
static int ep_restart(int slot, int dci, uint32_t deq_phys, int cycle, int wedged) {
    uint32_t base = ((uint32_t)slot << 24) | ((uint32_t)dci << 16);
    int c = command(0, 0, 0, (TRB_RESET_EP << 10) | base, 0);
    if (c != 1 && wedged) c = command(0, 0, 0, (TRB_STOP_EP << 10) | base, 0);
    (void)c;
    return command(deq_phys | (uint32_t)cycle, 0, 0, (TRB_SET_DEQ << 10) | base, 0) == 1;
}

// endpoint 0 stalled or timed out: reset it and skip whatever is left of the failed transfer
static void ep0_recover(int slot, int code) {
    struct ring* r = &X.ep0[slot];
    ep_restart(slot, 1, r->phys + (uint32_t)r->enq * 16, r->cycle, code == -1);
}

// control transfer with recovery of endpoint 0 on failure
static int ctl(int slot, uint8_t rt, uint8_t rq, uint16_t v, uint16_t i, uint16_t len, int in) {
    int code = control(slot, rt, rq, v, i, len, in);
    if (!is_ok(code)) ep0_recover(slot, code);
    return code;
}

// descriptor read with up to 3 tries; the data is in X.dma, the length in X.ctl_len
static int get_desc(int slot, uint8_t rt, uint16_t value, uint16_t index, uint16_t len) {
    int code = -1;
    for (int t = 0; t < 3; t++) {
        code = ctl(slot, rt, 6, value, index, len, 1);
        if (is_ok(code) || code == -1) return code;                   // a timeout is not worth repeating
        delay_ms(10);
    }
    return code;
}

// a halted (or stopped) pointer endpoint: reset it, empty the ring and queue the transfers again
static int ep_recover(struct hidep* e) {
    e->since = 0; e->replan = 0;
    ring_reset(&e->ring);
    if (!ep_restart(e->slot, e->dci, e->ring.phys, 1, 1)) return 0;
    for (int k = 0; k < NBUF; k++) ep_requeue(e);
    return 1;
}

// ---- contexts -----------------------------------------------------------------------------------
static uint32_t* ctx(uint8_t* base, int index) { return (uint32_t*)(base + (uint32_t)index * (uint32_t)X.ctxsz); }

static void ep0_context(uint32_t* e, int mps, uint32_t deq_phys) {
    e[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);       // CErr 3, control endpoint
    e[2] = deq_phys | 1;                                         // dequeue pointer, DCS = 1
    e[4] = 8;                                                    // average TRB length
}

static int interval_field(int speed, int binterval) {
    if (speed >= 3) {                                             // high / super speed: 2^(bInterval-1) microframes
        int e = binterval - 1;
        return e < 0 ? 0 : e > 15 ? 15 : e;
    }
    int ms8 = (binterval ? binterval : 1) * 8;                    // low / full speed: milliseconds -> 125 us units
    int e = 0;
    while ((2 << e) <= ms8 && e < 10) e++;
    return e < 3 ? 3 : e;
}

// ---- devices and slots ----------------------------------------------------------------------------
static void free_slot(int slot) {
    command(0, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)slot << 24), 0);
    X.dcbaa[slot] = 0;
}

// forget a device and everything behind it
static void dev_drop(int slot) {
    if (slot < 1 || slot > MAXSLOTS || !D[slot].used) return;
    for (int i = 0; i < MAXEP; i++) if (E[i].used && E[i].slot == slot) E[i].used = 0;
    if (B.used && B.slot == slot) B.used = 0;
    if (WL.used && WL.slot == slot) { WL.used = 0; WL.started = 0; }
    for (int s = 1; s <= MAXSLOTS; s++)
        if (D[s].used && D[s].parent == slot) dev_drop(s);
    if (D[slot].parent && D[D[slot].parent].used && D[slot].pport <= MAXHUBPORT) D[D[slot].parent].child[D[slot].pport] = 0;
    D[slot].used = 0;
    free_slot(slot);
}

static struct hidep* ep_alloc(void) {
    for (int i = 0; i < MAXEP; i++) if (!E[i].used) {
        struct ring r = E[i].ring; uint8_t* buf = E[i].buf; int bufsz = E[i].bufsz;     // keep the DMA memory for reuse
        memset(&E[i], 0, sizeof E[i]);
        E[i].ring = r; E[i].buf = buf; E[i].bufsz = bufsz;
        E[i].used = 1;
        return &E[i];
    }
    return 0;
}

static const char* speed_name(int s) {
    return s == 1 ? "full" : s == 2 ? "low" : s == 3 ? "high" : s == 4 ? "super" : "?";
}

static void describe_layout(int slot, const struct hid_ptr* p, int ids) {
    xlog("slot %d ptr: id %d %s x%d@%d y%d@%d w%d btn%d%d%d len %d", slot, ids ? p->id : 0, p->absolute ? "abs" : "rel",
         p->x.bits, p->x.off, p->y.bits, p->y.off, p->wheel.bits, p->btn[0].bits ? 1 : 0, p->btn[1].bits ? 1 : 0, p->btn[2].bits ? 1 : 0, p->bytes);
}

struct ifc { int num, cls, sub, proto, rptlen, ep, epmps, epint, bin, bout, binmps, boutmps, nout, outs[4], outmps[4]; };

// ---- USB mass storage, bulk-only transport (SCSI transparent command set) -----------------------------------
// Only for the on-stick log (klog.c): synchronous, one command at a time, a failure switches it off for good.
static int bulk_xfer(int in, uint32_t phys, uint32_t len) {
    struct ring* r = in ? &B.rin : &B.rout;
    B.done = 0;
    ring_push(r, phys, 0, len, (TRB_NORMAL << 10) | (1u << 5) | (in ? (1u << 2) : 0));      // IOC (+ ISP for IN)
    wr32(X.db, (uint32_t)B.slot * 4, (uint32_t)(in ? B.in_dci : B.out_dci));
    for (int i = 0; i < 100000; i++) {                                                       // 5 s
        pump();
        if (B.done) return B.code;
        delay_us(50);
    }
    return -1;
}

// clear a halted bulk endpoint: skip what is left on the ring, then CLEAR_FEATURE(ENDPOINT_HALT)
static void bulk_unhalt(int in) {
    struct ring* r = in ? &B.rin : &B.rout;
    ep_restart(B.slot, in ? B.in_dci : B.out_dci, r->phys + (uint32_t)r->enq * 16, r->cycle, 0);
    ctl(B.slot, 0x02, 1, 0, (uint16_t)(in ? B.in_ep : B.out_ep), 0, 0);
}

// one SCSI command; data (len bytes) in B.data. Returns 0 on success, 1 = command failed (CSW status), -1 = transport failure
static int bot_cmd(const uint8_t* cdb, int cdblen, int in, uint32_t len) {
    if (!B.used || B.failed) return -1;
    uint8_t* cbw = B.cb;
    memset(cbw, 0, 64);
    uint32_t tag = ++B.tag;
    *(uint32_t*)(cbw + 0) = 0x43425355u;                       // 'USBC'
    *(uint32_t*)(cbw + 4) = tag;
    *(uint32_t*)(cbw + 8) = len;
    cbw[12] = in ? 0x80 : 0;
    cbw[13] = 0; cbw[14] = (uint8_t)cdblen;
    memcpy(cbw + 15, cdb, (uint32_t)cdblen);
    int c = bulk_xfer(0, phys_of(cbw), 31);
    if (!is_ok(c)) { if (c == 6) bulk_unhalt(0); return -1; }
    if (len) {
        c = bulk_xfer(in, phys_of(B.data), len);
        if (c == 6) bulk_unhalt(in);
        else if (!is_ok(c)) return -1;
    }
    uint8_t* csw = B.cb + 64;
    for (int tries = 0; tries < 2; tries++) {
        memset(csw, 0, 16);
        c = bulk_xfer(1, phys_of(csw), 13);
        if (c == 6) { bulk_unhalt(1); continue; }
        if (!is_ok(c)) return -1;
        if (*(uint32_t*)csw != 0x53425355u || *(uint32_t*)(csw + 4) != tag) return -1;
        return csw[12] == 0 ? 0 : 1;
    }
    return -1;
}

static int scsi_simple(uint8_t op, uint32_t lba, uint32_t count, int in, uint32_t len) {
    uint8_t cdb[10] = { op, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(count >> 8), (uint8_t)count, 0 };
    return bot_cmd(cdb, 10, in, len);
}

// set the interface up as storage. Returns 1 when ready.
static int storage_setup(int slot, int speed, const struct ifc* f, int cfgval) {
    if (!is_ok(ctl(slot, 0x00, 9, (uint16_t)cfgval, 0, 0, 0))) { xlog("  storage: set configuration failed"); return 0; }
    if (!B.rin.trb && (!ring_init(&B.rin, 17) || !ring_init(&B.rout, 17))) { xlog("  storage: out of memory"); return 0; }
    if (!B.data) { B.data = (uint8_t*)dma_alloc(65536, 65536); B.cb = (uint8_t*)dma_alloc(4096, 64); }
    if (!B.data || !B.cb) { xlog("  storage: out of memory"); return 0; }
    ring_reset(&B.rin); ring_reset(&B.rout);
    B.slot = slot; B.in_ep = f->bin; B.out_ep = f->bout;
    B.in_dci = (f->bin & 15) * 2 + 1; B.out_dci = (f->bout & 15) * 2;
    B.failed = 0; B.sectors = 0;
    memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
    memcpy(ctx(X.inctx, 1), ctx(X.devctx[slot], 0), (uint32_t)X.ctxsz);
    int maxdci = B.in_dci > B.out_dci ? B.in_dci : B.out_dci;
    ctx(X.inctx, 0)[1] = 1u | (1u << B.in_dci) | (1u << B.out_dci);
    uint32_t* ci = ctx(X.inctx, B.in_dci + 1);
    uint32_t* co = ctx(X.inctx, B.out_dci + 1);
    ci[1] = (3u << 1) | (6u << 3) | ((uint32_t)f->binmps << 16);  ci[2] = B.rin.phys | 1;  ci[4] = 4096;
    co[1] = (3u << 1) | (2u << 3) | ((uint32_t)f->boutmps << 16); co[2] = B.rout.phys | 1; co[4] = 4096;
    uint32_t* sc2 = ctx(X.inctx, 1);
    if (((sc2[0] >> 27) & 31) < (uint32_t)maxdci) sc2[0] = (sc2[0] & 0x07FFFFFFu) | ((uint32_t)maxdci << 27);
    int code = command(phys_of(X.inctx), 0, 0, (TRB_CONFIGURE << 10) | ((uint32_t)slot << 24), 0);
    if (code != 1) { xlog("  storage: configure endpoint failed (code %d)", code); return 0; }
    B.used = 1;
    (void)speed;
    uint8_t sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    int ready = 0;
    for (int t = 0; t < 6 && !ready; t++) {                       // TEST UNIT READY, clearing unit-attention states with REQUEST SENSE
        uint8_t tur[6] = { 0 };
        int r = bot_cmd(tur, 6, 0, 0);
        if (r == 0) ready = 1;
        else if (r < 0) break;
        else { bot_cmd(sense, 6, 1, 18); delay_ms(50); }
    }
    if (!ready) { xlog("  storage: not ready"); B.used = 0; return 0; }
    if (scsi_simple(0x25, 0, 0, 1, 8) != 0) { xlog("  storage: read capacity failed"); B.used = 0; return 0; }
    uint32_t last = ((uint32_t)B.data[0] << 24) | ((uint32_t)B.data[1] << 16) | ((uint32_t)B.data[2] << 8) | B.data[3];
    uint32_t bs = ((uint32_t)B.data[4] << 24) | ((uint32_t)B.data[5] << 16) | ((uint32_t)B.data[6] << 8) | B.data[7];
    if (bs != 512 || last == 0xFFFFFFFFu) { xlog("  storage: block size %u / %u blocks: not used", bs, last); B.used = 0; return 0; }
    B.sectors = last + 1;
    xlog("  storage: %u blocks of 512 (%u MB)", B.sectors, B.sectors >> 11);
    return 1;
}

int xhci_storage_ready(uint32_t* sectors) {
    if (!B.used || B.failed) return 0;
    *sectors = B.sectors;
    return 1;
}

// read or write `n` (<= 128) sectors; the data goes through a DMA buffer. Returns 1 on success; a failure disables the storage.
int xhci_storage_rw(int write, uint32_t lba, void* data, uint32_t n) {
    if (!B.used || B.failed || n == 0 || n > 128) return 0;
    if (n > 128 || lba + n > B.sectors || lba + n < lba) return 0;
    if (write) memcpy(B.data, data, n * 512);
    int r = scsi_simple(write ? 0x2A : 0x28, lba, n, !write, n * 512);
    if (r != 0) { B.failed = 1; xlog("storage %s lba %u failed (%d): log disabled", write ? "write" : "read", lba, r); return 0; }
    if (!write) memcpy(data, B.data, n * 512);
    return 1;
}
// ---- USB Wi-Fi adapter (vendor class, see src/rtw) ----------------------------------------------------------
// Register access is a vendor control transfer, firmware and frames go out on bulk OUT endpoints and come in on one bulk
// IN endpoint that keeps WL_NRX buffers in flight. Everything is polled from the caller; the receive buffers are
// handed out in ring order.
static void wl_rx_queue(int i) {
    WL.rx_state[i] = 0;
    ring_push(&WL.rin, phys_of(WL.rx + (uint32_t)i * WL_RXSZ), 0, WL_RXSZ, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));   // IOC | ISP
    wr32(X.db, (uint32_t)WL.slot * 4, (uint32_t)WL.in_dci);
}

static int wl_setup(int slot, int speed, const struct ifc* f, int cfgval, uint16_t vid, uint16_t pid) {
    if (!f->bin || f->nout < 1) { xlog("  wifi: bulk endpoints missing"); return 0; }
    if (!is_ok(ctl(slot, 0x00, 9, (uint16_t)cfgval, 0, 0, 0))) { xlog("  wifi: set configuration failed"); return 0; }
    if (!WL.rin.trb && !ring_init(&WL.rin, WL_NRX + 1)) { xlog("  wifi: out of memory"); return 0; }
    for (int i = 0; i < f->nout; i++)
        if (!WL.rout[i].trb && !ring_init(&WL.rout[i], 9)) { xlog("  wifi: out of memory"); return 0; }
    if (!WL.rx) WL.rx = (uint8_t*)dma_alloc(WL_NRX * WL_RXSZ, WL_RXSZ);
    if (!WL.tx) WL.tx = (uint8_t*)dma_alloc(65536, 65536);
    if (!WL.rx || !WL.tx) { xlog("  wifi: out of memory"); return 0; }
    ring_reset(&WL.rin);
    WL.slot = slot; WL.speed = speed; WL.nout = f->nout; WL.started = 0; WL.dead = 0; WL.ctl_fails = 0;
    WL.vid = vid; WL.pid = pid;
    WL.in_ep = f->bin; WL.in_dci = (f->bin & 15) * 2 + 1;
    int maxdci = WL.in_dci;
    for (int i = 0; i < f->nout; i++) {
        ring_reset(&WL.rout[i]);
        WL.out_ep[i] = f->outs[i]; WL.out_mps[i] = f->outmps[i] ? f->outmps[i] : 512;
        WL.out_dci[i] = (f->outs[i] & 15) * 2;
        if (WL.out_dci[i] > maxdci) maxdci = WL.out_dci[i];
    }
    memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
    memcpy(ctx(X.inctx, 1), ctx(X.devctx[slot], 0), (uint32_t)X.ctxsz);
    uint32_t add = 1u | (1u << WL.in_dci);
    uint32_t* ci = ctx(X.inctx, WL.in_dci + 1);
    ci[1] = (3u << 1) | (6u << 3) | ((uint32_t)f->binmps << 16);  ci[2] = WL.rin.phys | 1;  ci[4] = 4096;
    for (int i = 0; i < f->nout; i++) {
        add |= 1u << WL.out_dci[i];
        uint32_t* co = ctx(X.inctx, WL.out_dci[i] + 1);
        co[1] = (3u << 1) | (2u << 3) | ((uint32_t)WL.out_mps[i] << 16); co[2] = WL.rout[i].phys | 1; co[4] = 4096;
    }
    ctx(X.inctx, 0)[1] = add;
    uint32_t* sc2 = ctx(X.inctx, 1);
    if (((sc2[0] >> 27) & 31) < (uint32_t)maxdci) sc2[0] = (sc2[0] & 0x07FFFFFFu) | ((uint32_t)maxdci << 27);
    int code = command(phys_of(X.inctx), 0, 0, (TRB_CONFIGURE << 10) | ((uint32_t)slot << 24), 0);
    if (code != 1) { xlog("  wifi: configure endpoint failed (code %d)", code); return 0; }
    WL.used = 1;
    xlog("  wifi adapter %x:%x ready: %d bulk OUT, in ep %x", vid, pid, WL.nout, WL.in_ep);
    return 1;
}

int xhci_wl_present(struct xhci_wl_info* info) {
    if (!WL.used) return 0;
    if (info) { info->vid = WL.vid; info->pid = WL.pid; info->speed = WL.speed; info->nout = WL.nout; }
    return 1;
}

void xhci_wl_pump(void) { if (X.ready) pump(); }

int xhci_wl_ctrl(int in, uint8_t req, uint16_t value, uint16_t index, void* buf, uint16_t len) {
    if (!WL.used || WL.dead || len > 512) return -1;
    if (!in && len) memcpy(X.dma, buf, len);
    int code = ctl(WL.slot, in ? 0xC0 : 0x40, req, value, index, len, in);
    if (!is_ok(code)) {
        if (++WL.ctl_fails >= 8) { WL.dead = 1; xlog("wifi: control transfers keep failing (code %d): adapter given up", code); }
        return -2;
    }
    WL.ctl_fails = 0;
    if (in && len) memcpy(buf, X.dma, len);
    return len;
}

int xhci_wl_bulk_out(int ep, const void* data, uint32_t len) {
    if (!WL.used || WL.dead || ep < 0 || ep >= WL.nout || !len || len > 65536) return -1;
    memcpy(WL.tx, data, len);
    struct ring* r = &WL.rout[ep];
    int zlp = (len % (uint32_t)WL.out_mps[ep]) == 0;
    WL.out_done = 0;
    ring_push(r, phys_of(WL.tx), 0, len, (TRB_NORMAL << 10) | (zlp ? (1u << 4) : (1u << 5)));      // CH or IOC
    if (zlp) ring_push(r, phys_of(WL.tx), 0, 0, (TRB_NORMAL << 10) | (1u << 5));
    wr32(X.db, (uint32_t)WL.slot * 4, (uint32_t)WL.out_dci[ep]);
    for (int i = 0; i < 40000; i++) {                                                               // 2 s
        pump();
        if (WL.out_done) {
            if (is_ok(WL.out_code)) return 0;
            xlog("wifi: bulk OUT %d failed (code %d)", ep, WL.out_code);
            ep_restart(WL.slot, WL.out_dci[ep], r->phys + (uint32_t)r->enq * 16, r->cycle, 0);
            ctl(WL.slot, 0x02, 1, 0, (uint16_t)WL.out_ep[ep], 0, 0);
            return -2;
        }
        delay_us(50);
    }
    xlog("wifi: bulk OUT %d timed out", ep);
    ep_restart(WL.slot, WL.out_dci[ep], r->phys + (uint32_t)r->enq * 16, r->cycle, 1);
    return -3;
}

void xhci_wl_rx_start(void) {
    if (!WL.used || WL.started) return;
    WL.started = 1; WL.rx_head = 0;
    ring_reset(&WL.rin);
    for (int i = 0; i < WL_NRX; i++) wl_rx_queue(i);
}

static void wl_rx_recover(void) {
    WL.rx_recoveries++;
    ring_reset(&WL.rin);
    ep_restart(WL.slot, WL.in_dci, WL.rin.phys, 1, 1);
    ctl(WL.slot, 0x02, 1, 0, (uint16_t)WL.in_ep, 0, 0);
    WL.rx_head = 0;
    for (int i = 0; i < WL_NRX; i++) wl_rx_queue(i);
}

int xhci_wl_rx_get(const uint8_t** data) {
    if (!WL.used || !WL.started) return 0;
    pump();
    for (int guard = 0; guard < WL_NRX; guard++) {
        int i = WL.rx_head;
        if (!WL.rx_state[i]) return 0;
        if (WL.rx_state[i] == 2) { WL.rx_errors++; wl_rx_recover(); return 0; }
        if (WL.rx_len[i] > 0) { *data = WL.rx + (uint32_t)i * WL_RXSZ; return WL.rx_len[i]; }
        xhci_wl_rx_done();                                                                        // an empty transfer: drop it
    }
    return 0;
}

void xhci_wl_rx_done(void) {
    if (!WL.used || !WL.started) return;
    int i = WL.rx_head;
    if (!WL.rx_state[i]) return;
    WL.rx_head = (i + 1) % WL_NRX;
    wl_rx_queue(i);
}

struct eparam { int root, parent, pport, speed, depth; uint32_t route; int tt_slot, tt_port; };

// Address the device, read its descriptors; a hub gets its ports powered, a device with pointer interfaces gets them
// started. Returns the slot kept (> 0) or 0; *known = the device was identified (do not retry it).
static int enum_device(const struct eparam* a, int* known) {
    *known = 0;
    int slot = 0;
    int code = command(0, 0, 0, TRB_ENABLE_SLOT << 10, &slot);
    if (code != 1 || slot < 1 || slot > MAXSLOTS) {
        xlog("enable slot failed (code %d)", code);
        if (code == 1 && slot > 0) free_slot(slot);
        return 0;
    }
    memset(&D[slot], 0, sizeof D[slot]);
    D[slot].used = 1; D[slot].root = a->root; D[slot].parent = a->parent; D[slot].pport = a->pport;
    D[slot].speed = a->speed; D[slot].depth = a->depth; D[slot].route = a->route;
    D[slot].tt_slot = a->tt_slot; D[slot].tt_port = a->tt_port;
    int speed = a->speed;
    int mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    if (!X.devctx[slot]) X.devctx[slot] = (uint8_t*)dma_alloc(32u * (uint32_t)X.ctxsz, 4096);
    if (!X.devctx[slot] || (!X.ep0[slot].trb && !ring_init(&X.ep0[slot], 33))) { xlog("out of memory"); D[slot].used = 0; free_slot(slot); return 0; }
    ring_reset(&X.ep0[slot]);
    memset(X.devctx[slot], 0, 32u * (uint32_t)X.ctxsz);
    X.dcbaa[slot] = phys_of(X.devctx[slot]);
    memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
    ctx(X.inctx, 0)[1] = 3;                                       // add: slot context + endpoint 0
    uint32_t* sc = ctx(X.inctx, 1);
    sc[0] = a->route | ((uint32_t)speed << 20) | (1u << 27);      // context entries = 1
    sc[1] = (uint32_t)a->root << 16;
    sc[2] = (uint32_t)a->tt_slot | ((uint32_t)a->tt_port << 8);
    ep0_context(ctx(X.inctx, 2), mps, X.ep0[slot].phys);
    code = command(phys_of(X.inctx), 0, 0, (TRB_ADDRESS << 10) | ((uint32_t)slot << 24), 0);
    if (code != 1) { xlog("address device failed (code %d)", code); dev_drop(slot); return 0; }
    delay_ms(5);                                                  // SetAddress recovery time

    // the real maximum packet size of endpoint 0 comes with the first 8 bytes of the device descriptor
    code = get_desc(slot, 0x80, 0x0100, 0, 8);
    if (!is_ok(code)) { xlog("device descriptor failed (code %d)", code); dev_drop(slot); return 0; }
    int dmps = X.dma[7];
    int real = speed == 4 ? (1 << dmps) : dmps;
    if (real != mps && real >= 8 && real <= 512) {
        memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
        ctx(X.inctx, 0)[1] = 2;                                   // evaluate endpoint 0 only
        ctx(X.inctx, 2)[1] = (uint32_t)real << 16;
        if (command(phys_of(X.inctx), 0, 0, (TRB_EVALUATE << 10) | ((uint32_t)slot << 24), 0) != 1) { xlog("evaluate context failed"); dev_drop(slot); return 0; }
    }
    uint8_t dd[18];
    memset(dd, 0, sizeof dd);
    code = get_desc(slot, 0x80, 0x0100, 0, 18);
    if (is_ok(code)) memcpy(dd, X.dma, 18);
    uint16_t vid = (uint16_t)(dd[8] | (dd[9] << 8)), pid = (uint16_t)(dd[10] | (dd[11] << 8));
    D[slot].vid = vid; D[slot].pid = pid;
    // configuration descriptor: header first for the total length, then everything
    code = get_desc(slot, 0x80, 0x0200, 0, 9);
    if (!is_ok(code)) { xlog("config descriptor failed (code %d)", code); dev_drop(slot); return 0; }
    uint32_t total = (uint32_t)(X.dma[2] | (X.dma[3] << 8));
    if (total > 512) total = 512;
    if (total < 9) { xlog("bad config descriptor"); dev_drop(slot); return 0; }
    code = get_desc(slot, 0x80, 0x0200, 0, (uint16_t)total);
    if (!is_ok(code)) { xlog("config descriptor failed (code %d)", code); dev_drop(slot); return 0; }
    uint8_t cfg[512];
    memcpy(cfg, X.dma, total);
    int cfgval = cfg[5];

    struct ifc ifs[8];
    int nif = 0;
    struct ifc* cur = 0;
    for (uint32_t p = 0; p + 2 <= total && cfg[p] >= 2; p += cfg[p]) {
        if (cfg[p + 1] == 4 && p + 9 <= total) {                  // interface (alternate setting 0 only)
            if (cfg[p + 3] == 0 && nif < 8) {
                cur = &ifs[nif++];
                memset(cur, 0, sizeof *cur);
                cur->num = cfg[p + 2]; cur->cls = cfg[p + 5]; cur->sub = cfg[p + 6]; cur->proto = cfg[p + 7];
            } else cur = 0;
        } else if (cfg[p + 1] == 0x21 && cur && p + 9 <= total && cfg[p + 5] >= 1 && cfg[p + 6] == 0x22) {   // HID descriptor
            cur->rptlen = cfg[p + 7] | (cfg[p + 8] << 8);
        } else if (cfg[p + 1] == 5 && p + 7 <= total && cur && !cur->ep && (cfg[p + 3] & 3) == 3 && (cfg[p + 2] & 0x80)) {
            cur->ep = cfg[p + 2]; cur->epmps = (cfg[p + 4] | (cfg[p + 5] << 8)) & 0x7FF; cur->epint = cfg[p + 6];
        } else if (cfg[p + 1] == 5 && p + 7 <= total && cur && (cfg[p + 3] & 3) == 2) {          // bulk endpoint
            int mp = (cfg[p + 4] | (cfg[p + 5] << 8)) & 0x7FF;
            if ((cfg[p + 2] & 0x80) && !cur->bin) { cur->bin = cfg[p + 2]; cur->binmps = mp; }
            else if (!(cfg[p + 2] & 0x80) && !cur->bout) { cur->bout = cfg[p + 2]; cur->boutmps = mp; }
            if (!(cfg[p + 2] & 0x80) && cur->nout < 4) { cur->outs[cur->nout] = cfg[p + 2]; cur->outmps[cur->nout] = mp; cur->nout++; }
        }
    }
    int is_hub = dd[4] == 9 || (nif >= 1 && ifs[0].cls == 9 && dd[4] == 0);
    xlog("%s speed %s slot %d %x:%x class %d cfg %d, %d interface(s)", D[slot].parent ? "hub dev" : "root dev", speed_name(speed), slot,
         vid, pid, dd[4], cfgval, nif);
    for (int i = 0; i < nif; i++)
        xlog("  if%d class %d/%d/%d ep %x mps %d int %d rd %d", ifs[i].num, ifs[i].cls, ifs[i].sub, ifs[i].proto, ifs[i].ep, ifs[i].epmps,
             ifs[i].epint, ifs[i].rptlen);

    // ---- a hub: power its ports, polled from scan_hubs() ----
    if (is_hub) {
        if (speed == 4) { xlog("  SuperSpeed hubs are not supported"); *known = 1; dev_drop(slot); return 0; }
        if (a->depth >= 5) { xlog("  hubs nested too deep"); *known = 1; dev_drop(slot); return 0; }
        if (!is_ok(ctl(slot, 0x00, 9, (uint16_t)cfgval, 0, 0, 0))) { xlog("  set configuration failed"); dev_drop(slot); return 0; }
        code = get_desc(slot, 0xA0, 0x2900, 0, 9);
        if (!is_ok(code) || X.ctl_len < 7) { xlog("  hub descriptor failed (code %d)", code); dev_drop(slot); return 0; }
        int np = X.dma[2];
        int chars = X.dma[3] | (X.dma[4] << 8);
        int pwrgood = X.dma[5] * 2;
        if (np < 1) { xlog("  hub without ports"); *known = 1; dev_drop(slot); return 0; }
        if (np > MAXHUBPORT) np = MAXHUBPORT;
        memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
        ctx(X.inctx, 0)[1] = 1;                                   // update the slot context only
        memcpy(ctx(X.inctx, 1), ctx(X.devctx[slot], 0), (uint32_t)X.ctxsz);
        uint32_t* hc = ctx(X.inctx, 1);
        hc[0] |= 1u << 26;                                        // Hub
        if (speed == 3 && dd[6] == 2) hc[0] |= 1u << 25;          // multi-TT
        hc[1] = (hc[1] & 0x00FFFFFFu) | ((uint32_t)np << 24);
        hc[2] = (hc[2] & ~(3u << 16)) | ((uint32_t)((chars >> 5) & 3) << 16);
        code = command(phys_of(X.inctx), 0, 0, (TRB_CONFIGURE << 10) | ((uint32_t)slot << 24), 0);
        if (code != 1) { xlog("  hub configure failed (code %d)", code); dev_drop(slot); return 0; }
        D[slot].hub = 1; D[slot].nports = np;
        for (int p = 1; p <= np; p++) ctl(slot, 0x23, 3, 8, (uint16_t)p, 0, 0);       // SET_FEATURE PORT_POWER
        delay_ms((uint32_t)(pwrgood < 100 ? 100 : pwrgood > 500 ? 500 : pwrgood));
        xlog("  hub with %d port(s)%s", np, (speed == 3 && dd[6] == 2) ? ", multi-TT" : "");
        *known = 1;
        return slot;
    }

    // ---- mass storage: the first one is kept for the on-stick log (klog.c) ----
    for (int i = 0; i < nif && !B.used; i++) {
        const struct ifc* f = &ifs[i];
        if (f->cls != 8 || f->sub != 6 || f->proto != 0x50 || !f->bin || !f->bout) continue;
        if (storage_setup(slot, speed, f, cfgval)) { B.slot = slot; *known = 1; return slot; }
        *known = 1; dev_drop(slot); return 0;
    }

    // ---- a USB Wi-Fi adapter the Wi-Fi driver knows (src/rtw) ----
    if (rtw_usb_id_match(vid, pid)) {
        for (int i = 0; i < nif; i++) {
            if (ifs[i].cls != 0xFF) continue;
            *known = 1;
            if (wl_setup(slot, speed, &ifs[i], cfgval, vid, pid)) return slot;
            dev_drop(slot);
            return 0;
        }
    }

    // ---- pointer interfaces ----
    int chosen = 0, ep_slots[MAXEP];
    for (int i = 0; i < nif; i++) {
        struct ifc* f = &ifs[i];
        if (f->cls != 3 || !f->ep) continue;
        struct hidep* e = ep_alloc();
        if (!e) { xlog("  too many pointer endpoints"); break; }
        e->slot = slot; e->dci = (f->ep & 15) * 2 + 1; e->mps = f->epmps ? f->epmps : 8; e->ifnum = f->num; e->vid = vid; e->pid = pid;
        int parsed = 0;
        if (f->rptlen > 0) {
            int want = f->rptlen > 2048 ? 2048 : f->rptlen;
            code = get_desc(slot, 0x81, 0x2200, (uint16_t)f->num, (uint16_t)want);
            if (is_ok(code)) {
                parsed = hid_parse_pointer(X.dma, X.ctl_len > 0 && X.ctl_len <= want ? X.ctl_len : want, e->lay, HID_MAXRPT, &e->uses_ids);
                if (!parsed) xlog("  if%d: report descriptor (%d bytes) has no pointer", f->num, X.ctl_len);
            } else xlog("  if%d: report descriptor failed (code %d)", f->num, code);
        }
        int tl = 0;
        if (parsed > 0) {
            e->nl = parsed;
            for (int k = 0; k < parsed; k++) { describe_layout(slot, &e->lay[k], e->uses_ids); if (e->lay[k].bytes > tl) tl = e->lay[k].bytes; }
        } else if (f->sub == 1 && f->proto == 2) {
            e->boot = 1; tl = 8;
            xlog("  if%d: boot protocol mouse layout", f->num);
        } else { e->used = 0; continue; }
        e->tlen = e->mps > tl ? e->mps : tl;
        if (e->tlen > 1024) e->tlen = 1024;
        e->slotsz = (e->tlen + 15) & ~15;
        ep_slots[chosen++] = (int)(e - E);
    }
    if (!chosen) {
        xlog("  no pointer device (keyboard, storage or other); not started");
        *known = 1;
        dev_drop(slot);
        return 0;
    }

    if (!is_ok(ctl(slot, 0x00, 9, (uint16_t)cfgval, 0, 0, 0))) { xlog("  set configuration failed"); goto fail; }
    for (int k = 0; k < chosen; k++) {
        struct hidep* e = &E[ep_slots[k]];
        if (e->boot && !is_ok(ctl(slot, 0x21, 0x0B, 0, (uint16_t)e->ifnum, 0, 0))) xlog("  set protocol(boot) failed (continuing)");
    }
    memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
    memcpy(ctx(X.inctx, 1), ctx(X.devctx[slot], 0), (uint32_t)X.ctxsz);           // current slot context
    int maxdci = 1;
    uint32_t addflags = 1;
    for (int k = 0; k < chosen; k++) {
        struct hidep* e = &E[ep_slots[k]];
        if (!e->ring.trb && !ring_init(&e->ring, NBUF + 1)) { xlog("  out of memory"); goto fail; }
        if (e->bufsz < NBUF * e->slotsz) {
            e->buf = (uint8_t*)dma_alloc((uint32_t)(NBUF * e->slotsz), 64);
            if (!e->buf) { e->bufsz = 0; xlog("  out of memory"); goto fail; }
            e->bufsz = NBUF * e->slotsz;
        }
        ring_reset(&e->ring);
        if (e->dci > maxdci) maxdci = e->dci;
        addflags |= 1u << e->dci;
        int epint = 10;
        for (int i = 0; i < nif; i++) if (ifs[i].num == e->ifnum) epint = ifs[i].epint;
        uint32_t* ec = ctx(X.inctx, e->dci + 1);
        ec[0] = (uint32_t)interval_field(speed, epint) << 16;
        ec[1] = (3u << 1) | (7u << 3) | ((uint32_t)e->mps << 16);                // interrupt IN
        ec[2] = e->ring.phys | 1;
        ec[4] = (uint32_t)(e->tlen > 1024 ? 1024 : e->tlen) | ((uint32_t)e->mps << 16);
    }
    uint32_t* sc2 = ctx(X.inctx, 1);
    if (((sc2[0] >> 27) & 31) < (uint32_t)maxdci) sc2[0] = (sc2[0] & 0x07FFFFFFu) | ((uint32_t)maxdci << 27);
    ctx(X.inctx, 0)[1] = addflags;
    code = command(phys_of(X.inctx), 0, 0, (TRB_CONFIGURE << 10) | ((uint32_t)slot << 24), 0);
    if (code != 1) { xlog("  configure endpoint failed (code %d)", code); goto fail; }

    for (int k = 0; k < chosen; k++) {
        struct hidep* e = &E[ep_slots[k]];
        for (int i = 0; i < NBUF; i++) ep_requeue(e);
        xlog("  pointer running: slot %d dci %d mps %d len %d%s", slot, e->dci, e->mps, e->tlen, e->boot ? " (boot)" : "");
    }
    *known = 1;
    return slot;
fail:
    for (int k = 0; k < chosen; k++) E[ep_slots[k]].used = 0;
    dev_drop(slot);
    return 0;
}

// ---- root hub ports -----------------------------------------------------------------------------
static int port_reset(int port) {
    uint32_t off = OP_PORTSC + 0x10u * (uint32_t)(port - 1);
    uint32_t v = rd32(X.op, off);
    if (!(v & PORT_CCS)) return 0;
    wr32(X.op, off, PORT_PRESERVE(v) | PORT_CHANGES);            // acknowledge old changes
    v = rd32(X.op, off);
    int pls = (int)PORT_PLS(v);
    if (pls == 10 || pls == 6) {                                 // USB 3 link in compliance mode / inactive: warm reset
        wr32(X.op, off, PORT_PRESERVE(v) | PORT_WPR);
        for (int i = 0; i < 5000; i++) { v = rd32(X.op, off); if (v & PORT_WRC) break; delay_us(100); }
        wr32(X.op, off, PORT_PRESERVE(v) | PORT_CHANGES);
        delay_ms(20);
        v = rd32(X.op, off);
    } else if (!(v & PORT_PED) || PORT_SPEED(v) != 4) {          // USB 3 ports come up enabled; USB 2 ports need a reset
        wr32(X.op, off, PORT_PRESERVE(v) | PORT_PR);
        int ok = 0;
        for (int i = 0; i < 5000; i++) {                         // 500 ms
            v = rd32(X.op, off);
            if (v & PORT_PRC) { ok = 1; break; }
            delay_us(100);
        }
        if (!ok) { xlog("port %d reset timed out (PORTSC %08x)", port, v); return 0; }
        wr32(X.op, off, PORT_PRESERVE(v) | PORT_PRC);
        delay_ms(10);
        v = rd32(X.op, off);
    }
    wr32(X.op, off, PORT_PRESERVE(v) | PORT_CHANGES);
    return (v & PORT_CCS) && (v & PORT_PED);
}

static void root_enum(int p) {
    struct rport* r = &RP[p];
    r->tries++;
    r->next = tick_count + (r->tries < 4 ? 30 : 500);   // 300 ms between the first attempts, then 5 s (a flaky port is retried for a while)
    if (!port_reset(p)) { xlog("port %d: no usable device (PORTSC %08x)", p, rd32(X.op, OP_PORTSC + 0x10u * (uint32_t)(p - 1))); return; }
    uint32_t v = rd32(X.op, OP_PORTSC + 0x10u * (uint32_t)(p - 1));
    struct eparam a;
    memset(&a, 0, sizeof a);
    a.root = p; a.speed = (int)PORT_SPEED(v);
    int known = 0;
    int slot = enum_device(&a, &known);
    r->slot = (uint8_t)slot;
    r->known = (uint8_t)known;
}

static void scan_roots(void) {
    for (int p = 1; p <= X.maxports && p < 32; p++) {
        uint32_t off = OP_PORTSC + 0x10u * (uint32_t)(p - 1);
        uint32_t v = rd32(X.op, off);
        struct rport* r = &RP[p];
        if (v & PORT_CHANGES) {
            static int plogged;
            if ((r->known || r->slot) && plogged < 24) { plogged++; xlog("t=%u port %d: status change, PORTSC %08x", tick_count, p, v); }
            wr32(X.op, off, PORT_PRESERVE(v) | (v & PORT_CHANGES));                        // acknowledge
        }
        if (!(v & PORT_CCS) || ((v & PORT_CSC) && (r->known || r->slot))) {                 // gone (or replaced between scans)
            if (r->slot || r->known) xlog("t=%u port %d: device removed", tick_count, p);
            if (r->slot) dev_drop(r->slot);
            memset(r, 0, sizeof *r);
            if (!(v & PORT_CCS)) continue;
        }
        if (!r->known && r->tries < 10 && (int)(tick_count - r->next) >= 0) root_enum(p);
    }
}

// ---- hubs ---------------------------------------------------------------------------------------
static int hub_status(int slot, int port, uint32_t* st, uint32_t* ch) {
    int code = ctl(slot, 0xA3, 0, 0, (uint16_t)port, 4, 1);
    if (!is_ok(code) || X.ctl_len < 4) return 0;
    *st = (uint32_t)(X.dma[0] | (X.dma[1] << 8)); *ch = (uint32_t)(X.dma[2] | (X.dma[3] << 8));
    return 1;
}

static void hub_clear(int slot, int port, int feature) { ctl(slot, 0x23, 1, (uint16_t)feature, (uint16_t)port, 0, 0); }

// reset a hub port; returns the port status after the reset (0 = failed)
static uint32_t hub_port_reset(int slot, int port) {
    ctl(slot, 0x23, 3, 4, (uint16_t)port, 0, 0);                 // SET_FEATURE PORT_RESET
    uint32_t st = 0, ch = 0;
    for (int i = 0; i < 60; i++) {
        delay_ms(10);
        if (!hub_status(slot, port, &st, &ch)) return 0;
        if ((ch & 0x10) && !(st & 0x10)) break;
    }
    hub_clear(slot, port, 20);                                   // C_PORT_RESET
    delay_ms(10);
    if (!hub_status(slot, port, &st, &ch)) return 0;
    return (st & 1) && (st & 2) ? (st | 0x10000u) : 0;
}

static void hub_enum(int hub, int port) {
    struct dev* h = &D[hub];
    h->tries[port]++;
    h->next[port] = tick_count + 30;
    uint32_t st = hub_port_reset(hub, port);
    if (!st) { xlog("hub %d port %d: reset failed", hub, port); return; }
    struct eparam a;
    memset(&a, 0, sizeof a);
    a.root = h->root; a.parent = hub; a.pport = port;
    a.speed = (st & 0x200) ? 2 : (st & 0x400) ? 3 : 1;
    a.depth = h->depth + 1;
    a.route = h->route | ((uint32_t)port << (4 * h->depth));
    if (a.speed != 3 && h->speed == 3) { a.tt_slot = hub; a.tt_port = port; }           // low/full speed device behind a high speed hub
    else if (a.speed != 3) { a.tt_slot = h->tt_slot; a.tt_port = h->tt_port; }          // behind a full speed hub: the TT above us
    int known = 0;
    int slot = enum_device(&a, &known);
    if (slot > 0 && D[hub].used) D[hub].child[port] = (uint8_t)slot;
    if (D[hub].used) D[hub].known[port] = (uint8_t)known;
}

static void scan_hubs(void) {
    for (int s = 1; s <= MAXSLOTS; s++) {
        if (!D[s].used || !D[s].hub) continue;
        for (int p = 1; p <= D[s].nports && D[s].used; p++) {
            uint32_t st, ch;
            if (!hub_status(s, p, &st, &ch)) continue;
            if (ch & 0x01) hub_clear(s, p, 16);                  // C_PORT_CONNECTION
            if (ch & 0x02) hub_clear(s, p, 17);                  // C_PORT_ENABLE
            if (ch & 0x10) hub_clear(s, p, 20);                  // C_PORT_RESET
            if (ch & 0x04) hub_clear(s, p, 18);                  // C_PORT_SUSPEND
            if (ch & 0x08) hub_clear(s, p, 19);                  // C_PORT_OVER_CURRENT
            int gone = !(st & 1) || ((ch & 1) && (D[s].child[p] || D[s].known[p]));
            if (gone) {
                if (D[s].child[p] || D[s].known[p]) xlog("hub %d port %d: device removed", s, p);
                if (D[s].child[p]) dev_drop(D[s].child[p]);
                D[s].child[p] = 0; D[s].known[p] = 0; D[s].tries[p] = 0;
                if (!(st & 1)) continue;
            }
            if (!D[s].known[p] && D[s].tries[p] < 4 && (int)(tick_count - D[s].next[p]) >= 0) hub_enum(s, p);
        }
    }
}

// ---- bring-up -----------------------------------------------------------------------------------
static int wait_status(uint32_t mask, uint32_t want, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if ((rd32(X.op, OP_USBSTS) & mask) == want) return 1;
        delay_us(100);
    }
    return 0;
}

static int cmdline_has(uint32_t mboot, const char* word) {
    uint32_t flags = *(volatile uint32_t*)(mboot + 0);
    if (!(flags & (1u << 2))) return 0;
    const char* c = (const char*)P2V_U32(*(volatile uint32_t*)(mboot + 16));
    uint32_t wl = (uint32_t)strlen(word);
    for (int i = 0; c[i] && i < 512; i++)
        if (!strncmp(c + i, word, wl) && (i == 0 || c[i - 1] == ' ') && (c[i + wl] == 0 || c[i + wl] == ' ')) return 1;
    return 0;
}

void xhci_init(uint32_t mboot_phys) {
    if (cmdline_has(mboot_phys, "nousb")) { serial_puts("[xhci] disabled (nousb)\n"); return; }
    struct pci_loc l;
    int found = 0;
    for (int n = 0; pcix_find_class(0x0C, 0x03, n, &l); n++)
        if (pcix_read8(l, 0x09) == 0x30) { found = 1; break; }
    if (!found) { xlog("no xHCI controller"); return; }
    X.loc = l;
    X.did = (uint16_t)(pcix_read32(l, 0) >> 16);
    // power state D0 if the controller was parked
    uint8_t pm = pcix_find_cap(l, 1, 0);
    if (pm) { uint16_t cs = pcix_read16(l, (uint8_t)(pm + 4)); if (cs & 3) { pcix_write16(l, (uint8_t)(pm + 4), cs & ~3u); delay_us(20000); } }
    pcix_enable(l, 1);
    int is_mem, above;
    uint32_t bar = pcix_bar(l, 0, &is_mem, &above);
    uint32_t size = pcix_bar_size(l, 0);
    if (!bar || !is_mem || above || size < 0x1000) { xlog("BAR0 unusable: base %x size %x mem %d above4g %d", bar, size, is_mem, above); return; }
    if (size > 0x400000) size = 0x400000;
    X.cap = (volatile uint8_t*)paging_map_mmio(bar, size);
    if (!X.cap) { xlog("cannot map the registers"); return; }
    uint32_t caplen = *(volatile uint8_t*)X.cap;
    uint32_t hcs1 = rd32(X.cap, CAP_HCSPARAMS1), hcs2 = rd32(X.cap, CAP_HCSPARAMS2), hcc = rd32(X.cap, CAP_HCCPARAMS1);
    X.op = X.cap + caplen;
    X.rt = X.cap + (rd32(X.cap, CAP_RTSOFF) & ~0x1Fu);
    X.db = X.cap + (rd32(X.cap, CAP_DBOFF) & ~3u);
    X.maxslots = (int)(hcs1 & 0xFF);
    X.maxports = (int)(hcs1 >> 24);
    X.ctxsz = (hcc & 4) ? 64 : 32;
    xlog("%x:%x at %d:%d.%d, version %x, %d slots, %d ports, %d-byte ctx", 0x8086, X.did, l.bus, l.dev, l.fn, rd32(X.cap, 0) >> 16,
         X.maxslots, X.maxports, X.ctxsz);

    // take the controller from the firmware (USB Legacy Support extended capability)
    uint32_t xecp = (hcc >> 16) * 4;
    for (int guard = 0; xecp && guard < 32; guard++) {
        uint32_t cv = rd32(X.cap, xecp);
        if ((cv & 0xFF) == 1) {
            wr32(X.cap, xecp, cv | (1u << 24));                 // OS owned
            for (int i = 0; i < 10000 && (rd32(X.cap, xecp) & (1u << 16)); i++) delay_us(100);
            if (rd32(X.cap, xecp) & (1u << 16)) xlog("firmware did not release the controller (continuing)");
            uint32_t ctl = rd32(X.cap, xecp + 4);
            ctl &= ~((0x3u << 1) | (0xFFu << 5) | (0x7u << 17));  // no SMIs
            ctl |= 0x7u << 29;                                   // clear pending SMI status
            wr32(X.cap, xecp + 4, ctl);
        }
        uint32_t next = (cv >> 8) & 0xFF;
        if (!next) break;
        xecp += next * 4;
    }

    // stop, reset
    wr32(X.op, OP_USBCMD, rd32(X.op, OP_USBCMD) & ~CMD_RUN);
    if (!wait_status(STS_HCH, STS_HCH, 100)) xlog("did not halt (continuing)");
    wr32(X.op, OP_USBCMD, CMD_RESET);
    for (int i = 0; i < 10000 && ((rd32(X.op, OP_USBCMD) & CMD_RESET) || (rd32(X.op, OP_USBSTS) & STS_CNR)); i++) delay_us(100);
    if ((rd32(X.op, OP_USBCMD) & CMD_RESET) || (rd32(X.op, OP_USBSTS) & STS_CNR)) { xlog("reset did not finish: giving up"); return; }

    if (X.maxslots > MAXSLOTS) X.maxslots = MAXSLOTS;
    wr32(X.op, OP_CONFIG, (uint32_t)X.maxslots);
    X.dcbaa = (uint64_t*)dma_alloc(8u * 256, 4096);
    X.dma = (uint8_t*)dma_alloc(4096, 4096);
    X.inctx = (uint8_t*)dma_alloc(33u * (uint32_t)X.ctxsz, 4096);
    X.ev = (volatile struct trb*)dma_alloc(EVN * 16, 4096);
    uint32_t* erst = (uint32_t*)dma_alloc(16, 4096);
    if (!X.dcbaa || !X.dma || !X.inctx || !X.ev || !erst || !ring_init(&X.cmd, 33)) { xlog("out of memory"); return; }
    // scratchpad buffers the controller asks for
    uint32_t nsp = (((hcs2 >> 21) & 31) << 5) | ((hcs2 >> 27) & 31);
    if (nsp) {
        uint64_t* arr = (uint64_t*)dma_alloc(8 * nsp, 4096);
        if (!arr) return;
        for (uint32_t i = 0; i < nsp; i++) {
            void* pg = dma_alloc(4096, 4096);
            if (!pg) return;
            arr[i] = phys_of(pg);
        }
        X.dcbaa[0] = phys_of(arr);
    }
    wr64(X.op, OP_DCBAAP, phys_of(X.dcbaa));
    wr64(X.op, OP_CRCR, X.cmd.phys | 1);
    X.ev_phys = phys_of((const void*)X.ev);
    erst[0] = X.ev_phys; erst[1] = 0; erst[2] = EVN; erst[3] = 0;
    X.ev_deq = 0; X.ev_cycle = 1;
    wr32(X.rt, IR_ERSTSZ, 1);
    wr64(X.rt, IR_ERDP, X.ev_phys);
    wr64(X.rt, IR_ERSTBA, phys_of(erst));
    wr32(X.op, OP_USBCMD, CMD_RUN);
    if (!wait_status(STS_HCH, 0, 100)) { xlog("did not start"); return; }
    X.ready = 1;

    // ports: power on, give the links time to train, then enumerate what is connected (the rest is found by xhci_poll)
    for (int p = 1; p <= X.maxports && p < 32; p++) {
        uint32_t off = OP_PORTSC + 0x10u * (uint32_t)(p - 1);
        uint32_t v = rd32(X.op, off);
        if (!(v & PORT_PP)) wr32(X.op, off, PORT_PRESERVE(v) | PORT_PP);
    }
    delay_ms(400);
    scan_roots();
    scan_hubs();
    X.port_pending = 0;
    int nptr = 0;
    for (int i = 0; i < MAXEP; i++) nptr += E[i].used;
    xlog("ready: %d pointer endpoint(s)%s", nptr, nptr ? "" : " (devices are watched; try `usb rescan`)");
}

void xhci_rescan(void) { X.rescan_req = 1; }

static void poll_inner(void) {
    pump();
    if (X.rescan_req) {
        X.rescan_req = 0;
        for (int p = 1; p < 32; p++) if (!RP[p].slot) { RP[p].known = 0; RP[p].tries = 0; RP[p].next = 0; }
        for (int s = 1; s <= MAXSLOTS; s++)
            if (D[s].used && D[s].hub)
                for (int p = 1; p <= MAXHUBPORT; p++) if (!D[s].child[p]) { D[s].known[p] = 0; D[s].tries[p] = 0; D[s].next[p] = 0; }
        X.next_scan = 0;
        xlog("rescan");
    }
    for (int i = 0; i < MAXEP; i++) {                           // halted pointer endpoints: reset and restart, as often as needed
        struct hidep* e = &E[i];
        if (!e->used || !e->fail || e->dead) continue;
        if ((int)(tick_count - e->next_try) < 0) continue;      // backing off after the last restart
        int code = e->fail;
        e->fail = 0;
        e->recoveries++; e->restarts++;
        e->next_try = tick_count + (e->recoveries < 8 ? 3 : 30);
        if (e->restarts <= 8 || (e->restarts & 63) == 0) xlog("t=%u slot %d endpoint error (code %d): restart #%u", tick_count, e->slot, code, e->restarts);
        if (!ep_recover(e)) {
            xlog("slot %d: restart failed", e->slot);
            e->next_try = tick_count + 100;
            e->fail = code;
            if (++e->bad >= 10) { e->dead = 1; xlog("slot %d: giving up on this endpoint (unplug and replug)", e->slot); }
        }
    }
    for (int i = 0; i < MAXEP; i++) {                           // about to reach the ring's link TRB: start the ring over (the controller did not follow it)
        struct hidep* e = &E[i];
        if (!e->used || !e->replan || e->fail || e->dead) continue;
        e->planned++;
        if (!ep_recover(e)) xlog("slot %d: planned restart failed", e->slot);
    }
    for (int i = 0; i < MAXEP; i++) {                           // a pointer that went quiet after moving: dump what the controller thinks, then restart it
        struct hidep* e = &E[i];
        if (!e->used || !e->armed || e->fail || e->dead || (int)(tick_count - e->last_tick) < 25) continue;
        e->armed = 0;
        if (e->burst < 4 || !X.devctx[e->slot]) continue;
        if (e->stall_logs < 6) {
            e->stall_logs++;
            volatile uint32_t* oc = (volatile uint32_t*)ctx(X.devctx[e->slot], e->dci);
            uint32_t deq = oc[2] & ~15u;
            xlog("t=%u quiet after %u reports (#%u): ep %08x %08x %08x %08x", tick_count, e->burst, e->reports, oc[0], oc[1], oc[2], oc[3]);
            uint32_t ix = (deq - e->ring.phys) / 16;
            if (ix < (uint32_t)e->ring.n) {
                volatile struct trb* t = &e->ring.trb[ix];
                xlog("  hw deq idx %u trb %08x %08x %08x; ours enq %d cyc %d", ix, t->lo, t->status, t->control, e->ring.enq, e->ring.cycle);
            } else xlog("  hw deq %08x is outside the ring at %08x", deq, e->ring.phys);
            {   // stop the endpoint: that makes the controller write its real dequeue pointer back to the context
                uint32_t sb = ((uint32_t)e->slot << 24) | ((uint32_t)e->dci << 16);
                int sc = command(0, 0, 0, (TRB_STOP_EP << 10) | sb, 0);
                uint32_t dq = oc[2], di = ((dq & ~15u) - e->ring.phys) / 16;
                xlog("  after stop (code %d): hw deq %08x dcs %u idx %u since-restart %u", sc, dq & ~15u, dq & 1, di, e->since);
                if (di < (uint32_t)e->ring.n) {
                    uint32_t a = di > 2 ? di - 2 : 0;
                    xlog("  ctl[%u..] %08x %08x %08x %08x %08x %08x", a, e->ring.trb[a].control, e->ring.trb[a + 1].control, e->ring.trb[a + 2].control,
                         e->ring.trb[a + 3].control, e->ring.trb[a + 4].control, e->ring.trb[a + 5].control);
                }
                xlog("  link trb %08x %08x %08x", e->ring.trb[e->ring.n - 1].lo, e->ring.trb[e->ring.n - 1].status, e->ring.trb[e->ring.n - 1].control);
            }
            struct dev* d = &D[e->slot];
            uint32_t ps = rd32(X.op, OP_PORTSC + 0x10u * (uint32_t)(d->root - 1));
            xlog("  USBSTS %08x ERDP %08x root port %d PORTSC %08x", rd32(X.op, 4), rd32(X.rt, IR_ERDP), d->root, ps);
            uint32_t st, ch;
            if (d->parent && D[d->parent].used && hub_status(d->parent, d->pport, &st, &ch)) xlog("  hub port %d status %04x change %04x", d->pport, st, ch);
        }
        e->idle_restarts++;
        if (!ep_recover(e)) xlog("slot %d: restart after a pause failed", e->slot);
    }
    if (X.port_pending || (int)(tick_count - X.next_scan) >= 0) {
        X.next_scan = tick_count + 20;
        for (int i = 0; i < MAXEP; i++) {                       // the controller's own idea of each endpoint: running (1), or halted / stopped / error
            struct hidep* e = &E[i];
            if (!e->used || e->fail || e->dead || !X.devctx[e->slot]) continue;
            uint32_t st = ((volatile uint32_t*)ctx(X.devctx[e->slot], e->dci))[0] & 7;
            if (st != 1) { e->fail = 100 + (int)st; e->watchdog++; }
        }
        X.port_pending = 0;
        scan_roots();
        scan_hubs();
    }
}

// the main loop polls us: a call that takes long (control transfers with timeouts) freezes the whole desktop, so it is counted
void xhci_poll(void) {
    if (!X.ready) return;
    uint32_t t0 = tick_count;
    poll_inner();
    uint32_t dt = tick_count - t0;
    if (dt >= 5) { X.slow++; if (dt > X.slowmax) X.slowmax = dt; }
}

static int sadd(char* buf, int cap, int at, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[96];
    int n = fmt_to(line, sizeof line, fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && at < cap - 2; i++) buf[at++] = line[i];
    if (at < cap - 1) buf[at++] = '\n';
    buf[at] = 0;
    return at;
}

// `usb selftest`: provokes a STALL on endpoint 0 of every device and checks the recovery path, and stops + restarts every
// pointer endpoint (the pointer must keep moving afterwards: move it and look at the report counters)
void xhci_selftest(char* buf, int cap) {
    int at = 0;
    buf[0] = 0;
    if (!X.ready) { sadd(buf, cap, 0, "usb selftest: xHCI not running"); return; }
    for (int s = 1; s <= MAXSLOTS; s++) {
        if (!D[s].used) continue;
        int code = control(s, 0xC0, 0x7F, 0, 0, 8, 1);                      // a vendor request no device implements: STALL
        int rec = 0;
        if (!is_ok(code)) { ep0_recover(s, code); rec = 1; }
        int c2 = get_desc(s, 0x80, 0x0100, 0, 18);
        int good = is_ok(c2) && X.dma[0] == 18 && X.dma[1] == 1;
        at = sadd(buf, cap, at, "slot %d ep0: bad request -> code %d%s, then descriptor %s", s, code, rec ? ", recovered" : "", good ? "OK" : "FAILED");
    }
    for (int i = 0; i < MAXEP; i++) {
        struct hidep* e = &E[i];
        if (!e->used) continue;
        uint32_t base = ((uint32_t)e->slot << 24) | ((uint32_t)e->dci << 16);
        int c1 = command(0, 0, 0, (TRB_STOP_EP << 10) | base, 0);
        int ok = ep_recover(e);
        at = sadd(buf, cap, at, "slot %d dci %d: stop -> code %d, restart %s", e->slot, e->dci, c1, ok ? "OK" : "FAILED");
    }
    if (!at) sadd(buf, cap, at, "usb selftest: no device to test");
}

// `usb selftest stop`: leaves every pointer endpoint STOPPED; the state check in xhci_poll must find and restart them
void xhci_selftest_stop(char* buf, int cap) {
    int at = 0, n = 0;
    buf[0] = 0;
    for (int i = 0; i < MAXEP; i++) {
        struct hidep* e = &E[i];
        if (!e->used) continue;
        uint32_t base = ((uint32_t)e->slot << 24) | ((uint32_t)e->dci << 16);
        int c = command(0, 0, 0, (TRB_STOP_EP << 10) | base, 0);
        at = sadd(buf, cap, at, "slot %d dci %d: stopped (code %d); the watchdog must bring it back within a second", e->slot, e->dci, c);
        n++;
    }
    if (!n) sadd(buf, cap, at, "usb selftest stop: no pointer endpoint");
}

// ---- status for the shell ---------------------------------------------------------------------------
void xhci_status(char* buf, int cap) {
    int at = 0;
    buf[0] = 0;
    if (!X.ready) {
        at = sadd(buf, cap, at, "usb: xHCI not running");
    } else {
        int nptr = 0;
        for (int i = 0; i < MAXEP; i++) nptr += E[i].used;
        at = sadd(buf, cap, at, "usb: xHCI %x:%x up, %d ports, %d pointer(s), %u reports", 0x8086, X.did, X.maxports, nptr, X.reports);
        for (int p = 1; p <= X.maxports && p < 32; p++) {
            uint32_t v = rd32(X.op, OP_PORTSC + 0x10u * (uint32_t)(p - 1));
            if (!(v & PORT_CCS)) continue;
            const char* what = RP[p].slot ? (D[RP[p].slot].hub ? "hub" : "pointer") : RP[p].known ? "other device" : RP[p].tries >= 10 ? "FAILED" : "pending";
            at = sadd(buf, cap, at, "port %d: %08x %s, %s", p, v, speed_name((int)PORT_SPEED(v)), what);
        }
        for (int i = 0; i < MAXEP; i++)
            if (E[i].used)
            if (E[i].used) {
                uint32_t st = X.devctx[E[i].slot] ? ((volatile uint32_t*)ctx(X.devctx[E[i].slot], E[i].dci))[0] & 7 : 9;
                at = sadd(buf, cap, at, "slot %d %x:%x if%d: %u reports, %u errors, %u unmatched", E[i].slot, E[i].vid, E[i].pid, E[i].ifnum,
                          E[i].reports, E[i].errors, E[i].unmatched);
                at = sadd(buf, cap, at, "  ep state %u, last code %d, restarts %u (watchdog %u, after pauses %u, planned %u), last report %u ms ago%s", st, E[i].lastcode, E[i].restarts, E[i].watchdog, E[i].idle_restarts, E[i].planned, (tick_count - E[i].last_tick) * 10,
                          E[i].dead ? ", GAVE UP" : "");
            }
        uint32_t sts = rd32(X.op, 4);
        at = sadd(buf, cap, at, "USBSTS %08x  event ring at %d  slow polls %u (longest %u ms)", sts, X.ev_deq, X.slow, X.slowmax * 10);
    }
    at = sadd(buf, cap, at, "log:");
    devlog_tail('u', buf, cap, at, 14);
}

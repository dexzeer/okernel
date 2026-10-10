// xHCI host controller + USB HID boot-protocol mouse (see xhci.h). Written from the xHCI 1.2, USB 2.0 and HID 1.11
// specifications. Polled; every wait is bounded so a dead controller can never hang the boot.
#include "xhci.h"
#include "cursor.h"
#include "lapic.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "pcix.h"
#include "serial.h"
#include "string.h"

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
#define PORT_CHANGES (0x7Fu << 17)
#define PORT_PRC (1u << 21)
#define PORT_PRESERVE(v) ((v) & ~(PORT_CHANGES | PORT_PED))

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
#define EVN 64
#define MAXSLOTS 8
#define NBUF 16                  // mouse transfers kept in flight (ring of NBUF + 1 TRBs)

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
    // last command / control transfer results
    volatile int cmd_done, cmd_code, cmd_slot;
    volatile int ctl_done, ctl_code;
    uint32_t port_pending;       // bit n = port n changed
    // the mouse
    int m_present, m_port, m_slot, m_dci, m_mps, m_len;
    struct ring m_ring;
    uint8_t* m_buf;              // NBUF * 16
    uint32_t m_reports, m_errors;
    // per-slot control ring + device context
    struct ring ep0[MAXSLOTS + 1];
    uint8_t* devctx[MAXSLOTS + 1];
    uint8_t* inctx;
} X;

static inline uint32_t rd32(volatile uint8_t* b, uint32_t off) { return *(volatile uint32_t*)(b + off); }
static inline void wr32(volatile uint8_t* b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
static inline void wr64(volatile uint8_t* b, uint32_t off, uint32_t phys) { wr32(b, off, phys); wr32(b, off + 4, 0); }   // low first

static void delay_us(uint32_t us) { pit_delay_us(us); }

// physically contiguous, zeroed memory with an alignment (the heap lives in the high alias, so phys = V2P)
static void* dma_alloc(uint32_t size, uint32_t align) {
    uint8_t* p = (uint8_t*)kcalloc(1, size + align);
    if (!p) return 0;
    return (void*)(((uint32_t)p + align - 1) & ~(align - 1));
}
static uint32_t phys_of(const void* p) { return V2P((uint32_t)p); }

// ---- rings --------------------------------------------------------------------------------------
static int ring_init(struct ring* r, int n) {
    r->trb = (volatile struct trb*)dma_alloc((uint32_t)n * 16, 4096);
    if (!r->trb) return 0;
    r->phys = phys_of((const void*)r->trb);
    r->n = n; r->enq = 0; r->cycle = 1;
    volatile struct trb* l = &r->trb[n - 1];            // link back to the start, toggling the cycle bit
    l->lo = r->phys; l->hi = 0; l->status = 0;
    l->control = (TRB_LINK << 10) | (1u << 1);
    return 1;
}

// returns the physical address of the TRB written
static uint32_t ring_push(struct ring* r, uint32_t lo, uint32_t hi, uint32_t status, uint32_t control) {
    volatile struct trb* t = &r->trb[r->enq];
    uint32_t pa = r->phys + (uint32_t)r->enq * 16;
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
    return pa;
}

// ---- event ring ---------------------------------------------------------------------------------
static void mouse_report(const uint8_t* b, int n);
static void mouse_requeue(void);

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
            if (X.m_present && slot == X.m_slot && ep == X.m_dci) {
                uint32_t idx = (lo - X.m_ring.phys) / 16;
                if (idx < NBUF && (code == 1 || code == 13)) {
                    int n = X.m_len - (int)(status & 0xFFFFFF);
                    mouse_report(X.m_buf + idx * 16, n);
                } else {
                    X.m_errors++;
                }
                mouse_requeue();
            } else {
                X.ctl_code = code; X.ctl_done = 1;
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

// a control transfer on endpoint 0 of `slot`; data (len bytes) goes to/from X.dma. Returns the completion code.
static int control(int slot, uint8_t req_type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, int in) {
    struct ring* r = &X.ep0[slot];
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(r, (uint32_t)req_type | ((uint32_t)req << 8) | ((uint32_t)value << 16), (uint32_t)index | ((uint32_t)len << 16), 8,
              (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (len) ring_push(r, phys_of(X.dma), 0, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    ring_push(r, 0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));
    X.ctl_done = 0;
    wr32(X.db, (uint32_t)slot * 4, 1);
    for (int i = 0; i < 100000; i++) {                 // 5 s
        pump();
        if (X.ctl_done) return X.ctl_code;
        delay_us(50);
    }
    return -1;
}

// ---- contexts -----------------------------------------------------------------------------------
static uint32_t* ctx(uint8_t* base, int index) { return (uint32_t*)(base + (uint32_t)index * (uint32_t)X.ctxsz); }

static void ep0_context(uint32_t* e, int mps, uint32_t deq_phys) {
    e[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);       // CErr 3, control endpoint
    e[2] = deq_phys | 1;                                         // dequeue pointer, DCS = 1
    e[4] = 8;                                                    // average TRB length
}

// ---- enumeration --------------------------------------------------------------------------------
static int port_reset(int port) {
    uint32_t off = OP_PORTSC + 0x10u * (uint32_t)(port - 1);
    uint32_t v = rd32(X.op, off);
    if (!(v & PORT_CCS)) return 0;
    wr32(X.op, off, PORT_PRESERVE(v) | PORT_CHANGES);            // acknowledge old changes
    v = rd32(X.op, off);
    if (!(v & PORT_PED)) {                                       // USB 3 ports come up enabled; USB 2 ports need a reset
        wr32(X.op, off, PORT_PRESERVE(v) | PORT_PR);
        int ok = 0;
        for (int i = 0; i < 5000; i++) {                         // 500 ms
            v = rd32(X.op, off);
            if (v & PORT_PRC) { ok = 1; break; }
            delay_us(100);
        }
        if (!ok) { serial_printf("[xhci] port %d reset timed out (PORTSC %x)\n", port, v); return 0; }
        wr32(X.op, off, PORT_PRESERVE(v) | PORT_PRC);
        delay_us(10000);
        v = rd32(X.op, off);
    }
    wr32(X.op, off, PORT_PRESERVE(v) | PORT_CHANGES);
    return (v & PORT_CCS) && (v & PORT_PED);
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

static void free_slot(int slot) {
    int c = command(0, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)slot << 24), 0);
    (void)c;
    X.dcbaa[slot] = 0;
}

// Address the device on `port`, read its descriptors, and if it has a boot-protocol mouse interface, start it.
static void enumerate(int port) {
    uint32_t v = rd32(X.op, OP_PORTSC + 0x10u * (uint32_t)(port - 1));
    if (!port_reset(port)) return;
    v = rd32(X.op, OP_PORTSC + 0x10u * (uint32_t)(port - 1));
    int speed = (int)((v >> 10) & 15);
    int slot = 0;
    int code = command(0, 0, 0, TRB_ENABLE_SLOT << 10, &slot);
    if (code != 1 || slot < 1 || slot > MAXSLOTS || slot > X.maxslots) { serial_printf("[xhci] enable slot failed (code %d)\n", code); return; }
    int mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    if (!X.devctx[slot]) X.devctx[slot] = (uint8_t*)dma_alloc(32u * (uint32_t)X.ctxsz, 4096);
    if (!X.ep0[slot].trb && !ring_init(&X.ep0[slot], 33)) return;
    X.ep0[slot].enq = 0; X.ep0[slot].cycle = 1;
    memset((void*)X.ep0[slot].trb, 0, 32 * 16);
    {   // restore the link TRB after the wipe
        volatile struct trb* l = &X.ep0[slot].trb[32];
        l->lo = X.ep0[slot].phys; l->hi = 0; l->status = 0; l->control = (TRB_LINK << 10) | (1u << 1);
    }
    memset(X.devctx[slot], 0, 32u * (uint32_t)X.ctxsz);
    X.dcbaa[slot] = phys_of(X.devctx[slot]);
    memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
    ctx(X.inctx, 0)[1] = 3;                                       // add: slot context + endpoint 0
    uint32_t* sc = ctx(X.inctx, 1);
    sc[0] = ((uint32_t)speed << 20) | (1u << 27);                 // context entries = 1
    sc[1] = (uint32_t)port << 16;
    ep0_context(ctx(X.inctx, 2), mps, X.ep0[slot].phys);
    code = command(phys_of(X.inctx), 0, 0, (TRB_ADDRESS << 10) | ((uint32_t)slot << 24), 0);
    if (code != 1) { serial_printf("[xhci] address device failed (code %d)\n", code); free_slot(slot); return; }

    // the real maximum packet size of endpoint 0 comes with the first 8 bytes of the device descriptor
    code = control(slot, 0x80, 6, 0x0100, 0, 8, 1);
    if (code != 1 && code != 13) { serial_printf("[xhci] device descriptor failed (code %d)\n", code); free_slot(slot); return; }
    int dmps = X.dma[7];
    int real = speed == 4 ? (1 << dmps) : dmps;
    if (real != mps && real >= 8) {
        memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
        ctx(X.inctx, 0)[1] = 2;                                   // evaluate endpoint 0 only
        ctx(X.inctx, 2)[1] = (uint32_t)real << 16;
        if (command(phys_of(X.inctx), 0, 0, (TRB_EVALUATE << 10) | ((uint32_t)slot << 24), 0) != 1) { free_slot(slot); return; }
    }
    code = control(slot, 0x80, 6, 0x0100, 0, 18, 1);
    uint16_t vid = 0, pid = 0;
    if (code == 1 || code == 13) { vid = (uint16_t)(X.dma[8] | (X.dma[9] << 8)); pid = (uint16_t)(X.dma[10] | (X.dma[11] << 8)); }
    // configuration descriptor: header first for the total length, then everything
    code = control(slot, 0x80, 6, 0x0200, 0, 9, 1);
    if (code != 1 && code != 13) { free_slot(slot); return; }
    uint32_t total = (uint32_t)(X.dma[2] | (X.dma[3] << 8));
    if (total > 512) total = 512;
    if (total < 9) { free_slot(slot); return; }
    code = control(slot, 0x80, 6, 0x0200, 0, (uint16_t)total, 1);
    if (code != 1 && code != 13) { free_slot(slot); return; }
    uint8_t cfg[512];
    memcpy(cfg, X.dma, total);
    int cfgval = cfg[5];

    int ifnum = -1, epaddr = 0, epmps = 0, epint = 0, in_mouse = 0, ifclass = 0;
    for (uint32_t p = 0; p + 2 <= total && cfg[p] >= 2; p += cfg[p]) {
        if (cfg[p + 1] == 4 && p + 9 <= total) {                  // interface
            in_mouse = cfg[p + 5] == 3 && cfg[p + 6] == 1 && cfg[p + 7] == 2 && cfg[p + 3] == 0;
            if (in_mouse && ifnum < 0) ifnum = cfg[p + 2];
            else in_mouse = in_mouse && ifnum == cfg[p + 2];
            ifclass = cfg[p + 5];
        } else if (cfg[p + 1] == 5 && p + 7 <= total && in_mouse && !epaddr && (cfg[p + 3] & 3) == 3 && (cfg[p + 2] & 0x80)) {
            epaddr = cfg[p + 2]; epmps = (cfg[p + 4] | (cfg[p + 5] << 8)) & 0x7FF; epint = cfg[p + 6];
        }
    }
    (void)ifclass;
    serial_printf("[xhci] port %d speed %d slot %d device %x:%x config %d%s\n", port, speed, slot, vid, pid, cfgval,
                  epaddr ? " - HID mouse" : "");
    if (!epaddr || X.m_present) { free_slot(slot); return; }     // not a mouse (or we have one): leave it unconfigured

    if (control(slot, 0x00, 9, (uint16_t)cfgval, 0, 0, 0) != 1) { serial_puts("[xhci] set configuration failed\n"); free_slot(slot); return; }
    if (control(slot, 0x21, 0x0B, 0, (uint16_t)ifnum, 0, 0) != 1) serial_puts("[xhci] set protocol(boot) failed (continuing)\n");

    int dci = (epaddr & 15) * 2 + 1;
    X.m_len = epmps > 16 ? 16 : epmps < 4 ? 4 : epmps;
    if (!X.m_ring.trb && !ring_init(&X.m_ring, NBUF + 1)) { free_slot(slot); return; }
    if (!X.m_buf) X.m_buf = (uint8_t*)dma_alloc(NBUF * 16, 64);
    if (!X.m_buf) { free_slot(slot); return; }
    X.m_ring.enq = 0; X.m_ring.cycle = 1;
    memset((void*)X.m_ring.trb, 0, NBUF * 16);
    memset(X.inctx, 0, 33u * (uint32_t)X.ctxsz);
    memcpy(ctx(X.inctx, 1), ctx(X.devctx[slot], 0), (uint32_t)X.ctxsz);           // current slot context
    uint32_t* sc2 = ctx(X.inctx, 1);
    if (((sc2[0] >> 27) & 31) < (uint32_t)dci) sc2[0] = (sc2[0] & 0x07FFFFFFu) | ((uint32_t)dci << 27);
    ctx(X.inctx, 0)[1] = 1u | (1u << dci);
    uint32_t* ec = ctx(X.inctx, dci + 1);
    ec[0] = (uint32_t)interval_field(speed, epint) << 16;
    ec[1] = (3u << 1) | (7u << 3) | ((uint32_t)epmps << 16);                         // interrupt IN
    ec[2] = X.m_ring.phys | 1;
    ec[4] = (uint32_t)X.m_len | ((uint32_t)epmps << 16);
    code = command(phys_of(X.inctx), 0, 0, (TRB_CONFIGURE << 10) | ((uint32_t)slot << 24), 0);
    if (code != 1) { serial_printf("[xhci] configure endpoint failed (code %d)\n", code); free_slot(slot); return; }

    X.m_slot = slot; X.m_dci = dci; X.m_port = port; X.m_mps = epmps; X.m_present = 1;
    for (int i = 0; i < NBUF; i++) mouse_requeue();
    wr32(X.db, (uint32_t)slot * 4, (uint32_t)dci);
    serial_printf("[xhci] mouse running: slot %d endpoint %d (dci %d) max packet %d interval %d\n", slot, epaddr, dci, epmps, epint);
}

// ---- the mouse ----------------------------------------------------------------------------------
static void mouse_requeue(void) {
    if (!X.m_present) return;
    uint32_t idx = (uint32_t)X.m_ring.enq;
    ring_push(&X.m_ring, phys_of(X.m_buf + idx * 16), 0, (uint32_t)X.m_len, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));   // IOC | ISP
    wr32(X.db, (uint32_t)X.m_slot * 4, (uint32_t)X.m_dci);
}

static void mouse_report(const uint8_t* b, int n) {
    if (n < 3) return;
    X.m_reports++;
    int buttons = b[0] & 7;
    int dx = (int8_t)b[1], dy = (int8_t)b[2];
    int wheel = n >= 4 ? (int8_t)b[3] : 0;
    mouse_inject(dx, dy, -wheel, buttons);               // HID: wheel forward = positive; the kernel counts "down" as positive
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
    if (!found) { serial_puts("[xhci] no xHCI controller\n"); return; }
    X.loc = l;
    X.did = (uint16_t)(pcix_read32(l, 0) >> 16);
    // power state D0 if the controller was parked
    uint8_t pm = pcix_find_cap(l, 1, 0);
    if (pm) { uint16_t cs = pcix_read16(l, (uint8_t)(pm + 4)); if (cs & 3) { pcix_write16(l, (uint8_t)(pm + 4), cs & ~3u); delay_us(20000); } }
    pcix_enable(l, 1);
    int is_mem, above;
    uint32_t bar = pcix_bar(l, 0, &is_mem, &above);
    uint32_t size = pcix_bar_size(l, 0);
    if (!bar || !is_mem || above || size < 0x1000) { serial_printf("[xhci] BAR0 unusable: base %x size %x mem %d above4g %d\n", bar, size, is_mem, above); return; }
    if (size > 0x400000) size = 0x400000;
    X.cap = (volatile uint8_t*)paging_map_mmio(bar, size);
    if (!X.cap) { serial_puts("[xhci] cannot map the registers\n"); return; }
    uint32_t caplen = *(volatile uint8_t*)X.cap;
    uint32_t hcs1 = rd32(X.cap, CAP_HCSPARAMS1), hcs2 = rd32(X.cap, CAP_HCSPARAMS2), hcc = rd32(X.cap, CAP_HCCPARAMS1);
    X.op = X.cap + caplen;
    X.rt = X.cap + (rd32(X.cap, CAP_RTSOFF) & ~0x1Fu);
    X.db = X.cap + (rd32(X.cap, CAP_DBOFF) & ~3u);
    X.maxslots = (int)(hcs1 & 0xFF);
    X.maxports = (int)(hcs1 >> 24);
    X.ctxsz = (hcc & 4) ? 64 : 32;
    serial_printf("[xhci] %x:%x at %d:%d.%d BAR0 %x (%u KB): version %x, %d slots, %d ports, %d-byte contexts\n",
                  0x8086, X.did, l.bus, l.dev, l.fn, bar, size >> 10, rd32(X.cap, 0) >> 16, X.maxslots, X.maxports, X.ctxsz);

    // take the controller from the firmware (USB Legacy Support extended capability)
    uint32_t xecp = (hcc >> 16) * 4;
    for (int guard = 0; xecp && guard < 32; guard++) {
        uint32_t cv = rd32(X.cap, xecp);
        if ((cv & 0xFF) == 1) {
            wr32(X.cap, xecp, cv | (1u << 24));                 // OS owned
            for (int i = 0; i < 10000 && (rd32(X.cap, xecp) & (1u << 16)); i++) delay_us(100);
            if (rd32(X.cap, xecp) & (1u << 16)) serial_puts("[xhci] firmware did not release the controller (continuing)\n");
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
    if (!wait_status(STS_HCH, STS_HCH, 100)) serial_puts("[xhci] did not halt (continuing)\n");
    wr32(X.op, OP_USBCMD, CMD_RESET);
    for (int i = 0; i < 10000 && ((rd32(X.op, OP_USBCMD) & CMD_RESET) || (rd32(X.op, OP_USBSTS) & STS_CNR)); i++) delay_us(100);
    if ((rd32(X.op, OP_USBCMD) & CMD_RESET) || (rd32(X.op, OP_USBSTS) & STS_CNR)) { serial_puts("[xhci] reset did not finish: giving up\n"); return; }

    if (X.maxslots > MAXSLOTS) X.maxslots = MAXSLOTS;
    wr32(X.op, OP_CONFIG, (uint32_t)X.maxslots);
    X.dcbaa = (uint64_t*)dma_alloc(8u * 256, 4096);
    X.dma = (uint8_t*)dma_alloc(4096, 4096);
    X.inctx = (uint8_t*)dma_alloc(33u * (uint32_t)X.ctxsz, 4096);
    X.ev = (volatile struct trb*)dma_alloc(EVN * 16, 4096);
    uint32_t* erst = (uint32_t*)dma_alloc(16, 4096);
    if (!X.dcbaa || !X.dma || !X.inctx || !X.ev || !erst || !ring_init(&X.cmd, 33)) { serial_puts("[xhci] out of memory\n"); return; }
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
    if (!wait_status(STS_HCH, 0, 100)) { serial_puts("[xhci] did not start\n"); return; }
    X.ready = 1;

    // ports: power on, then enumerate what is connected
    delay_us(100000);
    for (int p = 1; p <= X.maxports && p < 32; p++) {
        uint32_t off = OP_PORTSC + 0x10u * (uint32_t)(p - 1);
        uint32_t v = rd32(X.op, off);
        if (!(v & PORT_PP)) { wr32(X.op, off, PORT_PRESERVE(v) | PORT_PP); }
    }
    delay_us(200000);
    for (int p = 1; p <= X.maxports && p < 32; p++) {
        uint32_t v = rd32(X.op, OP_PORTSC + 0x10u * (uint32_t)(p - 1));
        if (v & PORT_CCS) enumerate(p);
    }
    X.port_pending = 0;
    serial_printf("[xhci] ready%s\n", X.m_present ? ", mouse attached" : ", no mouse yet (hot-plug is watched)");
}

void xhci_poll(void) {
    if (!X.ready) return;
    pump();
    if (X.port_pending) {
        uint32_t pend = X.port_pending;
        X.port_pending = 0;
        for (int p = 1; p < 32; p++) {
            if (!(pend & (1u << p))) continue;
            uint32_t off = OP_PORTSC + 0x10u * (uint32_t)(p - 1);
            uint32_t v = rd32(X.op, off);
            wr32(X.op, off, PORT_PRESERVE(v) | (v & PORT_CHANGES));       // acknowledge
            if (!(v & PORT_CCS)) {
                if (X.m_present && X.m_port == p) {
                    serial_printf("[xhci] mouse unplugged (port %d)\n", p);
                    int slot = X.m_slot;
                    X.m_present = 0;
                    free_slot(slot);
                }
            } else if (!X.m_present && (v & (1u << 17))) {                  // connect change with something there
                enumerate(p);
            }
        }
    }
}

void xhci_status(char* buf, int cap) {
    const char* s = "usb: no xHCI controller";
    char t[96];
    if (X.ready) {
        char* p = t;
        const char* a = X.m_present ? "usb: xHCI up, mouse on port " : "usb: xHCI up, no mouse";
        while (*a) *p++ = *a++;
        if (X.m_present) {
            *p++ = (char)('0' + (X.m_port / 10 % 10)); *p++ = (char)('0' + X.m_port % 10);
            const char* b = ", reports "; while (*b) *p++ = *b++;
            char r[12]; int k = 0; uint32_t n = X.m_reports;
            if (!n) r[k++] = '0';
            while (n) { r[k++] = (char)('0' + n % 10); n /= 10; }
            while (k) *p++ = r[--k];
        }
        *p = 0;
        s = t;
    }
    int i = 0;
    while (s[i] && i < cap - 1) { buf[i] = s[i]; i++; }
    buf[i] = 0;
}

// virtio-gpu 2D display backend (virtio 1.x GPU device, section 5.7).
//
// The backbuffer itself is the guest resource's backing store: the device reads
// it directly, so presenting a frame is "transfer these rectangles + flush" and
// no pixel is copied by the CPU. The pointer is a hardware cursor (cursorq), so
// moving the mouse never touches the scene.
#include "display.h"
#include "virtio.h"
#include "memlayout.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_GPU_DEV 0x1050            // modern virtio-gpu (0x1040 + 16)

enum {
    CMD_GET_DISPLAY_INFO = 0x0100, CMD_RESOURCE_CREATE_2D = 0x0101, CMD_RESOURCE_UNREF = 0x0102,
    CMD_SET_SCANOUT = 0x0103, CMD_RESOURCE_FLUSH = 0x0104, CMD_TRANSFER_TO_HOST_2D = 0x0105,
    CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    CMD_UPDATE_CURSOR = 0x0300, CMD_MOVE_CURSOR = 0x0301,
    RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO = 0x1101,
};
#define FMT_B8G8R8A8 1
#define FMT_B8G8R8X8 2

struct hdr { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx, pad[3]; } __attribute__((packed));
struct rect { uint32_t x, y, w, h; } __attribute__((packed));
struct res_create { struct hdr h; uint32_t id, format, w, hgt; } __attribute__((packed));
struct set_scanout { struct hdr h; struct rect r; uint32_t scanout, id; } __attribute__((packed));
struct res_flush { struct hdr h; struct rect r; uint32_t id, pad; } __attribute__((packed));
struct xfer { struct hdr h; struct rect r; uint64_t offset; uint32_t id, pad; } __attribute__((packed));
struct attach { struct hdr h; uint32_t id, nr; } __attribute__((packed));
struct mem_entry { uint64_t addr; uint32_t len, pad; } __attribute__((packed));
struct cursor_cmd { struct hdr h; uint32_t scanout, x, y, pad; uint32_t id, hot_x, hot_y, pad2; } __attribute__((packed));
struct display_info { struct hdr h; struct { struct rect r; uint32_t enabled, flags; } pm[16]; } __attribute__((packed));

#define MAXR 24                           // rectangles per present batch
#define CUR_SLOTS 16
#define RES_SCREEN 1
#define RES_CURSOR 2

struct slot { struct xfer x; struct hdr resp; };

struct dma {
    struct slot slots[MAXR];
    struct res_flush fl; struct hdr flresp;
    uint8_t scratch[1024]; struct display_info info;   // init-time commands
    struct hdr scratch_resp;
    struct cursor_cmd cur[CUR_SLOTS];
};

static struct {
    struct virtio_dev dev;
    struct vq ctrl, curq;
    struct dma* dma;
    uint32_t* bb; int w, h;
    int cur_ok, cur_shown, cur_next;
    uint32_t* cur_img;                     // 64x64 ARGB backing of the cursor resource
    int cur_hot_x, cur_hot_y;
    int ready;
} G;

static uint32_t phys_of(const void* p) { return V2P((uint32_t)p); }

// Synchronous control command: cmd (+ optional extra payload) in, resp out.
static int ctl(const void* cmd, uint32_t clen, const void* extra, uint32_t elen, void* resp, uint32_t rlen) {
    struct vq_seg s[3];
    int n = 0;
    s[n].phys = phys_of(cmd); s[n].len = clen; s[n].write = 0; n++;
    if (extra) { s[n].phys = phys_of(extra); s[n].len = elen; s[n].write = 0; n++; }
    s[n].phys = phys_of(resp); s[n].len = rlen; s[n].write = 1; n++;
    int head = vq_add(&G.ctrl, s, n);
    if (head < 0) return -1;
    vq_kick(&G.ctrl);
    return vq_wait(&G.ctrl, head) < 0 ? -1 : 0;
}

static int ctl_ok(const void* cmd, uint32_t clen, const void* extra, uint32_t elen) {
    struct hdr* r = &G.dma->scratch_resp;
    memset(r, 0, sizeof *r);
    if (ctl(cmd, clen, extra, elen, r, sizeof *r) < 0) return 0;
    if (r->type != RESP_OK_NODATA) {
        serial_printf("[vgpu] command %x failed: response %x\n", ((const struct hdr*)cmd)->type, r->type);
        return 0;
    }
    return 1;
}

static int create_resource(uint32_t id, uint32_t fmt, uint32_t w, uint32_t h) {
    struct res_create* c = (struct res_create*)G.dma->scratch;
    memset(c, 0, sizeof *c);
    c->h.type = CMD_RESOURCE_CREATE_2D; c->id = id; c->format = fmt; c->w = w; c->hgt = h;
    return ctl_ok(c, sizeof *c, 0, 0);
}

static int attach_backing(uint32_t id, const void* mem, uint32_t len) {
    struct attach* a = (struct attach*)G.dma->scratch;
    struct mem_entry* e = (struct mem_entry*)(G.dma->scratch + sizeof *a);
    memset(G.dma->scratch, 0, sizeof *a + sizeof *e);
    a->h.type = CMD_RESOURCE_ATTACH_BACKING; a->id = id; a->nr = 1;
    e->addr = phys_of(mem); e->len = len;
    return ctl_ok(a, sizeof *a + sizeof *e, 0, 0);
}

// ---- display ops ------------------------------------------------------------

static void vgpu_present(const struct display_rect* rects, int n) {
    if (!G.ready || n <= 0) return;
    struct display_rect merged;
    if (n > MAXR) {                                   // too many: one bounding box
        int x0 = rects[0].x, y0 = rects[0].y, x1 = x0 + rects[0].w, y1 = y0 + rects[0].h;
        for (int i = 1; i < n; i++) {
            if (rects[i].x < x0) x0 = rects[i].x;
            if (rects[i].y < y0) y0 = rects[i].y;
            if (rects[i].x + rects[i].w > x1) x1 = rects[i].x + rects[i].w;
            if (rects[i].y + rects[i].h > y1) y1 = rects[i].y + rects[i].h;
        }
        merged.x = x0; merged.y = y0; merged.w = x1 - x0; merged.h = y1 - y0;
        rects = &merged; n = 1;
    }
    int x0 = G.w, y0 = G.h, x1 = 0, y1 = 0, heads[MAXR], nq = 0;
    for (int i = 0; i < n; i++) {
        struct slot* s = &G.dma->slots[nq];
        memset(&s->x, 0, sizeof s->x);
        memset(&s->resp, 0, sizeof s->resp);
        s->x.h.type = CMD_TRANSFER_TO_HOST_2D;
        s->x.r.x = rects[i].x; s->x.r.y = rects[i].y; s->x.r.w = rects[i].w; s->x.r.h = rects[i].h;
        s->x.offset = ((uint64_t)rects[i].y * G.w + rects[i].x) * 4;     // byte offset into the backing store
        s->x.id = RES_SCREEN;
        struct vq_seg sg[2] = { { phys_of(&s->x), sizeof s->x, 0 }, { phys_of(&s->resp), sizeof s->resp, 1 } };
        int hd = vq_add(&G.ctrl, sg, 2);
        if (hd < 0) { vq_kick(&G.ctrl); for (int k = 0; k < nq; k++) vq_wait(&G.ctrl, heads[k]); nq = 0; i--; continue; }
        heads[nq++] = hd;
        if (rects[i].x < x0) x0 = rects[i].x;
        if (rects[i].y < y0) y0 = rects[i].y;
        if (rects[i].x + rects[i].w > x1) x1 = rects[i].x + rects[i].w;
        if (rects[i].y + rects[i].h > y1) y1 = rects[i].y + rects[i].h;
    }
    struct res_flush* f = &G.dma->fl;
    memset(f, 0, sizeof *f); memset(&G.dma->flresp, 0, sizeof G.dma->flresp);
    f->h.type = CMD_RESOURCE_FLUSH; f->id = RES_SCREEN;
    f->r.x = x0; f->r.y = y0; f->r.w = x1 - x0; f->r.h = y1 - y0;
    struct vq_seg sg[2] = { { phys_of(f), sizeof *f, 0 }, { phys_of(&G.dma->flresp), sizeof G.dma->flresp, 1 } };
    int fh = vq_add(&G.ctrl, sg, 2);
    vq_kick(&G.ctrl);
    static int logged;
    for (int k = 0; k < nq; k++) {
        if (vq_wait(&G.ctrl, heads[k]) < 0) { serial_puts("[vgpu] transfer timed out\n"); G.ready = 0; return; }
        if (G.dma->slots[k].resp.type != RESP_OK_NODATA && logged++ < 4)
            serial_printf("[vgpu] transfer rejected: response %x\n", G.dma->slots[k].resp.type);
    }
    if (fh >= 0 && vq_wait(&G.ctrl, fh) < 0) { serial_puts("[vgpu] flush timed out\n"); G.ready = 0; return; }
    if (G.dma->flresp.type != RESP_OK_NODATA && logged++ < 4)
        serial_printf("[vgpu] flush rejected: response %x\n", G.dma->flresp.type);
}

// Cursor commands go through the cursor queue without waiting for the device;
// their buffers rotate through a small ring and are reaped lazily.
static void cursor_cmd(uint32_t type, int x, int y, uint32_t res) {
    struct cursor_cmd* c = &G.dma->cur[G.cur_next];
    memset(c, 0, sizeof *c);
    c->h.type = type; c->scanout = 0;
    c->x = (uint32_t)(x < 0 ? 0 : x); c->y = (uint32_t)(y < 0 ? 0 : y);
    c->id = res; c->hot_x = (uint32_t)G.cur_hot_x; c->hot_y = (uint32_t)G.cur_hot_y;
    struct vq_seg sg = { phys_of(c), sizeof *c, 0 };
    if (vq_add(&G.curq, &sg, 1) < 0) return;           // queue full: drop, the next move catches up
    G.cur_next = (G.cur_next + 1) % CUR_SLOTS;
    vq_kick(&G.curq);
}

static void vgpu_cursor_move(int x, int y) {
    if (!G.ready || !G.cur_ok || !G.cur_shown) return;
    cursor_cmd(CMD_MOVE_CURSOR, x, y, RES_CURSOR);
}

static void vgpu_cursor_show(int on) {
    if (!G.ready || !G.cur_ok) return;
    G.cur_shown = on;
    cursor_cmd(CMD_UPDATE_CURSOR, 0, 0, on ? RES_CURSOR : 0);
}

static int vgpu_cursor_define(const uint32_t* argb, int w, int h, int hot_x, int hot_y) {
    if (!G.ready || w > 64 || h > 64) return -1;
    if (!G.cur_img) {
        G.cur_img = (uint32_t*)kcalloc(64 * 64, 4);
        if (!G.cur_img) return -1;
        if (!create_resource(RES_CURSOR, FMT_B8G8R8A8, 64, 64)) return -1;
        if (!attach_backing(RES_CURSOR, G.cur_img, 64 * 64 * 4)) return -1;
    }
    memset(G.cur_img, 0, 64 * 64 * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) G.cur_img[y * 64 + x] = argb[y * w + x];
    struct xfer* t = (struct xfer*)G.dma->scratch;
    memset(t, 0, sizeof *t);
    t->h.type = CMD_TRANSFER_TO_HOST_2D; t->r.w = 64; t->r.h = 64; t->id = RES_CURSOR;
    if (!ctl_ok(t, sizeof *t, 0, 0)) return -1;
    G.cur_hot_x = hot_x; G.cur_hot_y = hot_y;
    G.cur_ok = 1;
    return 0;
}

static const struct display_ops ops = {
    "virtio-gpu", vgpu_present, vgpu_cursor_define, vgpu_cursor_move, vgpu_cursor_show,
};

const struct display_ops* vgpu_probe(uint32_t* backbuffer, int w, int h) {
    struct pci_loc loc;
    if (!pcix_find_id(VIRTIO_VENDOR, VIRTIO_GPU_DEV, 0, &loc)) return 0;
    serial_printf("[vgpu] virtio-gpu at %d:%d.%d\n", loc.bus, loc.dev, loc.fn);
    if (virtio_pci_probe(&G.dev, loc) != 0) return 0;
    uint64_t feat = 0;
    if (virtio_negotiate(&G.dev, 0, &feat) != 0) return 0;
    if (virtio_queue_setup(&G.dev, 0, 64, &G.ctrl) != 0 || virtio_queue_setup(&G.dev, 1, 16, &G.curq) != 0) {
        serial_puts("[vgpu] queue setup failed\n");
        virtio_fail(&G.dev);
        return 0;
    }
    virtio_driver_ok(&G.dev);
    G.dma = (struct dma*)kcalloc(1, sizeof(struct dma));
    if (!G.dma) return 0;
    G.bb = backbuffer; G.w = w; G.h = h;

    // what the host shows (informational)
    {
        struct hdr* q = (struct hdr*)G.dma->scratch;
        memset(q, 0, sizeof *q);
        q->type = CMD_GET_DISPLAY_INFO;
        memset(&G.dma->info, 0, sizeof G.dma->info);
        if (ctl(q, sizeof *q, 0, 0, &G.dma->info, sizeof G.dma->info) == 0 && G.dma->info.h.type == RESP_OK_DISPLAY_INFO)
            serial_printf("[vgpu] scanout 0: %s %dx%d\n", G.dma->info.pm[0].enabled ? "enabled" : "disabled",
                          G.dma->info.pm[0].r.w, G.dma->info.pm[0].r.h);
    }
    if (!create_resource(RES_SCREEN, FMT_B8G8R8X8, (uint32_t)w, (uint32_t)h) ||
        !attach_backing(RES_SCREEN, backbuffer, (uint32_t)w * (uint32_t)h * 4)) {
        virtio_fail(&G.dev);
        return 0;
    }
    struct set_scanout* so = (struct set_scanout*)G.dma->scratch;
    memset(so, 0, sizeof *so);
    so->h.type = CMD_SET_SCANOUT; so->r.w = (uint32_t)w; so->r.h = (uint32_t)h; so->scanout = 0; so->id = RES_SCREEN;
    if (!ctl_ok(so, sizeof *so, 0, 0)) { virtio_fail(&G.dev); return 0; }
    G.ready = 1;
    struct display_rect all = { 0, 0, w, h };
    vgpu_present(&all, 1);
    serial_printf("[vgpu] scanout 0 = %dx%d from the backbuffer, hardware cursor %s\n", w, h, "ready");
    return &ops;
}

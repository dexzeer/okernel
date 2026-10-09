#include "virtio.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"

// Common configuration structure offsets (virtio 1.x, section 4.1.4.3)
#define C_DEV_FEAT_SEL   0
#define C_DEV_FEAT       4
#define C_DRV_FEAT_SEL   8
#define C_DRV_FEAT       12
#define C_NUM_QUEUES     18
#define C_STATUS         20
#define C_QUEUE_SELECT   22
#define C_QUEUE_SIZE     24
#define C_QUEUE_ENABLE   28
#define C_QUEUE_NOTIFY   30
#define C_QUEUE_DESC     32
#define C_QUEUE_DRIVER   40
#define C_QUEUE_DEVICE   48

static inline uint8_t  rd8(volatile uint8_t* b, uint32_t o) { return *(volatile uint8_t*)(b + o); }
static inline uint16_t rd16(volatile uint8_t* b, uint32_t o) { return *(volatile uint16_t*)(b + o); }
static inline uint32_t rd32(volatile uint8_t* b, uint32_t o) { return *(volatile uint32_t*)(b + o); }
static inline void wr8(volatile uint8_t* b, uint32_t o, uint8_t v) { *(volatile uint8_t*)(b + o) = v; }
static inline void wr16(volatile uint8_t* b, uint32_t o, uint16_t v) { *(volatile uint16_t*)(b + o) = v; }
static inline void wr32(volatile uint8_t* b, uint32_t o, uint32_t v) { *(volatile uint32_t*)(b + o) = v; }

int virtio_pci_probe(struct virtio_dev* d, struct pci_loc loc) {
    d->loc = loc;
    d->common = d->notify_base = d->isr = d->devcfg = 0;
    d->notify_mult = 0; d->devcfg_len = 0;
    pcix_enable(loc, 1);
    uint8_t cap = 0;
    while ((cap = pcix_find_cap(loc, 0x09, cap)) != 0) {          // vendor-specific capability
        uint8_t type = pcix_read8(loc, cap + 3), bar = pcix_read8(loc, cap + 4);
        uint32_t off = pcix_read32(loc, cap + 8), len = pcix_read32(loc, cap + 12);
        if (type < 1 || type > 4 || bar > 5) continue;
        int is_mem, above;
        uint32_t base = pcix_bar(loc, bar, &is_mem, &above);
        if (!base || !is_mem) {
            serial_printf("[virtio] cap type %d: BAR%d %s\n", type, bar, above ? "is above 4GB (unsupported)" : "unassigned");
            continue;
        }
        volatile uint8_t* m = (volatile uint8_t*)paging_map_mmio(base + off, len ? len : 4096);
        if (!m) { serial_puts("[virtio] out of MMIO windows\n"); return -1; }
        switch (type) {
        case 1: d->common = m; break;
        case 2: d->notify_base = m; d->notify_mult = pcix_read32(loc, cap + 16); break;
        case 3: d->isr = m; break;
        case 4: d->devcfg = m; d->devcfg_len = len; break;
        }
    }
    if (!d->common || !d->notify_base) { serial_puts("[virtio] missing common/notify capability\n"); return -1; }
    return 0;
}

void virtio_fail(struct virtio_dev* d) {
    wr8(d->common, C_STATUS, rd8(d->common, C_STATUS) | VIRTIO_STATUS_FAILED);
}

int virtio_negotiate(struct virtio_dev* d, uint64_t want, uint64_t* got) {
    volatile uint8_t* c = d->common;
    wr8(c, C_STATUS, 0);
    for (int i = 0; i < 1000000 && rd8(c, C_STATUS) != 0; i++) __asm__ volatile("pause");
    wr8(c, C_STATUS, VIRTIO_STATUS_ACK);
    wr8(c, C_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    wr32(c, C_DEV_FEAT_SEL, 0);
    uint32_t lo = rd32(c, C_DEV_FEAT);
    wr32(c, C_DEV_FEAT_SEL, 1);
    uint32_t hi = rd32(c, C_DEV_FEAT);
    uint64_t dev = ((uint64_t)hi << 32) | lo;
    if (!(dev & VIRTIO_F_VERSION_1)) {
        serial_puts("[virtio] device is legacy-only (no VERSION_1)\n");
        virtio_fail(d);
        return -1;
    }
    uint64_t use = (want & dev) | VIRTIO_F_VERSION_1;
    wr32(c, C_DRV_FEAT_SEL, 0); wr32(c, C_DRV_FEAT, (uint32_t)use);
    wr32(c, C_DRV_FEAT_SEL, 1); wr32(c, C_DRV_FEAT, (uint32_t)(use >> 32));
    wr8(c, C_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if (!(rd8(c, C_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio] device rejected the feature set\n");
        virtio_fail(d);
        return -1;
    }
    if (got) *got = use;
    return 0;
}

int virtio_queue_setup(struct virtio_dev* d, int qidx, int max_size, struct vq* q) {
    volatile uint8_t* c = d->common;
    wr16(c, C_QUEUE_SELECT, (uint16_t)qidx);
    uint16_t dsz = rd16(c, C_QUEUE_SIZE);
    if (dsz == 0) return -1;                         // queue not available
    uint16_t n = dsz < max_size ? dsz : (uint16_t)max_size;
    while (n & (n - 1)) n &= n - 1;                  // power of two
    wr16(c, C_QUEUE_SIZE, n);

    uint32_t desc_sz = 16u * n;
    uint32_t avail_off = desc_sz;
    uint32_t avail_sz = 2u * (3u + n);
    uint32_t used_off = (avail_off + avail_sz + 3u) & ~3u;
    uint32_t used_sz = 4u + 8u * n + 2u;
    uint32_t total = used_off + used_sz + 16;
    uint8_t* mem = (uint8_t*)kcalloc(1, total);
    uint8_t* done = (uint8_t*)kcalloc(1, n);
    uint32_t* dlen = (uint32_t*)kcalloc(n, 4);
    if (!mem || !done || !dlen) { serial_puts("[virtio] no memory for a queue\n"); return -1; }

    q->index = (uint16_t)qidx; q->size = n; q->last_used = 0;
    q->desc = (volatile struct vring_desc*)mem;
    q->avail = (volatile uint16_t*)(mem + avail_off);
    q->used = mem + used_off;
    q->done = done; q->done_len = dlen; q->mem = mem;
    for (uint16_t i = 0; i < n; i++) q->desc[i].next = (uint16_t)(i + 1);
    q->free_head = 0; q->nfree = n;
    q->avail[0] = 1;                                 // VRING_AVAIL_F_NO_INTERRUPT: we poll

    uint32_t pd = V2P((uint32_t)mem), pa = pd + avail_off, pu = pd + used_off;
    wr32(c, C_QUEUE_DESC, pd);   wr32(c, C_QUEUE_DESC + 4, 0);
    wr32(c, C_QUEUE_DRIVER, pa); wr32(c, C_QUEUE_DRIVER + 4, 0);
    wr32(c, C_QUEUE_DEVICE, pu); wr32(c, C_QUEUE_DEVICE + 4, 0);
    uint16_t noff = rd16(c, C_QUEUE_NOTIFY);
    q->notify = (volatile uint16_t*)(d->notify_base + (uint32_t)noff * d->notify_mult);
    wr16(c, C_QUEUE_ENABLE, 1);
    return 0;
}

void virtio_driver_ok(struct virtio_dev* d) {
    wr8(d->common, C_STATUS, rd8(d->common, C_STATUS) | VIRTIO_STATUS_DRIVER_OK);
}

int vq_add(struct vq* q, const struct vq_seg* segs, int nseg) {
    if (nseg <= 0) return -1;
    if (q->nfree < nseg) { vq_reap(q); if (q->nfree < nseg) return -1; }
    uint16_t head = q->free_head, idx = head;
    for (int i = 0; i < nseg; i++) {
        volatile struct vring_desc* d = &q->desc[idx];
        uint16_t nxt = d->next;                      // free-list link
        d->addr = segs[i].phys;
        d->len = segs[i].len;
        d->flags = (uint16_t)((i < nseg - 1 ? VRING_DESC_F_NEXT : 0) | (segs[i].write ? VRING_DESC_F_WRITE : 0));
        if (i < nseg - 1) d->next = nxt;             // chain follows the free list
        idx = nxt;
    }
    q->free_head = idx;
    q->nfree = (uint16_t)(q->nfree - nseg);
    q->done[head] = 0;
    uint16_t ai = q->avail[1];
    q->avail[2 + (ai & (q->size - 1))] = head;
    __asm__ volatile("" ::: "memory");
    q->avail[1] = (uint16_t)(ai + 1);
    return head;
}

void vq_kick(struct vq* q) {
    __sync_synchronize();                            // ring writes visible before the doorbell
    *q->notify = q->index;
}

int vq_reap(struct vq* q) {
    int n = 0;
    for (;;) {
        __asm__ volatile("" ::: "memory");
        uint16_t uidx = *(volatile uint16_t*)(q->used + 2);
        if (q->last_used == uidx) break;
        volatile uint32_t* e = (volatile uint32_t*)(q->used + 4 + 8u * (q->last_used & (q->size - 1)));
        uint32_t id = e[0], len = e[1];
        if (id < q->size) {
            q->done[id] = 1;
            q->done_len[id] = len;
            // give the whole chain back to the free list
            uint16_t i = (uint16_t)id, cnt = 1;
            while (q->desc[i].flags & VRING_DESC_F_NEXT) { i = q->desc[i].next; cnt++; }
            q->desc[i].next = q->free_head;
            q->free_head = (uint16_t)id;
            q->nfree = (uint16_t)(q->nfree + cnt);
        }
        q->last_used++;
        n++;
    }
    return n;
}

int vq_wait(struct vq* q, int head) {
    for (uint32_t spin = 0; spin < 40000000u; spin++) {
        vq_reap(q);
        if (q->done[head]) { q->done[head] = 0; return (int)q->done_len[head]; }
        __asm__ volatile("pause" ::: "memory");
    }
    return -1;
}

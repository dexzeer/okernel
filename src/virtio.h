#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include "pcix.h"

// Virtio 1.x over PCI (modern, memory-mapped capabilities) with split
// virtqueues. Polled: no interrupts are used (INTx is masked in the PCI
// command register). Written from the virtio specification.

#define VIRTIO_F_VERSION_1 (1ull << 32)

#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

struct vq_seg {
    uint32_t phys;
    uint32_t len;
    int write;             // device writes into this segment
};

struct vq {
    uint16_t index;        // queue number
    uint16_t size;
    uint16_t last_used;
    uint16_t free_head;
    uint16_t nfree;
    volatile struct vring_desc* desc;
    volatile uint16_t* avail;      // flags, idx, ring[size]
    volatile uint8_t* used;        // flags(2), idx(2), ring[size]{id(4), len(4)}
    volatile uint16_t* notify;
    uint8_t* done;                 // per head: completion seen (0/1)
    uint32_t* done_len;
    void* mem;
};

struct virtio_dev {
    struct pci_loc loc;
    volatile uint8_t* common;
    volatile uint8_t* notify_base;
    uint32_t notify_mult;
    volatile uint8_t* isr;
    volatile uint8_t* devcfg;
    uint32_t devcfg_len;
};

// Map the capability regions of the device at `loc` (enables memory space and
// bus mastering, masks INTx). Returns 0 on success.
int virtio_pci_probe(struct virtio_dev* d, struct pci_loc loc);
// Reset, acknowledge, negotiate features (always VERSION_1 plus the bits in
// `want` the device offers). *got = accepted set. Returns 0 on success.
int virtio_negotiate(struct virtio_dev* d, uint64_t want, uint64_t* got);
// Allocate and enable queue `qidx` with at most `max_size` entries.
int virtio_queue_setup(struct virtio_dev* d, int qidx, int max_size, struct vq* q);
void virtio_driver_ok(struct virtio_dev* d);
void virtio_fail(struct virtio_dev* d);

// Add a descriptor chain (does not notify). Returns the head id or -1 if the
// queue is full.
int vq_add(struct vq* q, const struct vq_seg* segs, int nseg);
void vq_kick(struct vq* q);
// Collect finished chains; returns how many.
int vq_reap(struct vq* q);
// Spin until chain `head` finished; returns bytes the device wrote, or -1 on
// timeout.
int vq_wait(struct vq* q, int head);

#endif

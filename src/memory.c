#include "memory.h"
#include "memlayout.h"
#include "serial.h"
#include "spinlock.h"
#include <stdint.h>

// Multiboot header flags
#define MULTIBOOT_MAGIC 0x2BADB002
#define MULTIBOOT_FLAG_MEM (1 << 0)   // mem_lower/mem_upper valid (bit 1 is the boot device, which GRUB on UEFI does not set)

// Multiboot info structure (partial)
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    // ... more fields we don't need yet
};

// Bitmap for physical memory manager
// Each bit represents one 4KB page
// 0 = free, 1 = used
#define PAGE_SIZE 4096
// Up to 512 GB: one PML4 entry of direct map. One bit per page, in memory taken from the RAM itself on a machine with more than 4 GB.
#define MAX_RAM_PAGES (1u << 27)
#define LOW_PAGES (1u << 20)                     // the pages below 4 GB
static uint8_t bitmap_low[LOW_PAGES / 8];
static uint8_t* bitmap = bitmap_low;
#define MAX_REGIONS 32
static struct { uint64_t base, end; } regions[MAX_REGIONS];
static int nregions;
static uint32_t hint_low, hint_high;             // no free page below these

static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

// Locks for SMP: the other CPUs run jobs that may call kmalloc/kfree. IRQs go
// off first so an interrupt handler on the same CPU cannot spin on a lock its
// own interrupted code holds.
static spinlock_t pmm_lock = SPINLOCK_INIT;
static spinlock_t heap_lock = SPINLOCK_INIT;

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

static void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

static uint32_t boot_info_phys;

// Boot module `idx` (multiboot `module` lines): its bytes through the high alias. 0 when there is no such module.
const uint8_t* memory_boot_module(int idx, uint32_t* len, const char** cmdline) {
    if (!boot_info_phys) return 0;
    uint32_t flags = *(volatile uint32_t*)(uintptr_t)(boot_info_phys + 0);
    if (!(flags & (1u << 3))) return 0;
    uint32_t mcount = *(volatile uint32_t*)(uintptr_t)(boot_info_phys + 20), maddr = *(volatile uint32_t*)(uintptr_t)(boot_info_phys + 24);
    if (idx < 0 || (uint32_t)idx >= mcount) return 0;
    volatile uint32_t* me = (volatile uint32_t*)P2V_ADDR(maddr + (uint32_t)idx * 16);
    if (len) *len = me[1] - me[0];
    if (cmdline) *cmdline = me[2] ? (const char*)P2V_ADDR(me[2]) : "";
    return (const uint8_t*)P2V_ADDR(me[0]);
}

void memory_init(uint32_t mboot_phys) {
    boot_info_phys = mboot_phys;
    // mboot_phys is PHYS (GRUB structs live low). Plain phys derefs through
    // the boot PD's 0-4M LOW identity window. (An earlier pushed-args mixup
    // in start.asm once delivered the magic number in this slot — diagnosed
    // via serial mboot print + GDB; the push order is fixed, this stays.)
    uint32_t mp = mboot_phys;
    serial_printf("[mem] mboot phys=%x\n", mp);
    uint32_t mboot_flags = *(volatile uint32_t*)(uintptr_t)(mp + 0);
    uint32_t mboot_upper = *(volatile uint32_t*)(uintptr_t)(mp + 8);

    serial_printf("[mem] mboot phys=%x flags=%x\n", mboot_phys, mboot_flags);

    // Where the kernel image ends (the _kernel_* symbols are HIGH linked: convert to phys before page math, or the
    // 0xC01xxxxx vaddr overflows the bitmap).
    extern uint32_t _kernel_end;
    uint32_t kernel_end = (V2P(&_kernel_end) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t kernel_end_page = kernel_end / PAGE_SIZE;

    // The bootloader's memory map (multiboot flag bit 6) is the truth: UEFI firmware leaves ACPI/reserved holes right
    // above 1MB, so "mem_upper" (the one contiguous run at 1MB) can be a few MB on a machine with gigabytes. Only
    // usable (type 1) pages above the kernel image are freed; everything else stays marked used. Without a map, fall
    // back to mem_upper. Memory above MAX_RAM_PAGES is left alone.
    uint32_t top_pages = LOW_PAGES;
    uint32_t map_len = (mboot_flags & (1u << 6)) ? *(volatile uint32_t*)(uintptr_t)(mp + 44) : 0;
    uint32_t map_addr = (mboot_flags & (1u << 6)) ? *(volatile uint32_t*)(uintptr_t)(mp + 48) : 0;
    if (map_len) {
        // First pass: the regions, the top of RAM and where the bitmap goes. It must be below 4 GB (only that is mapped so far) and above everything
        // the bootloader left in memory.
        uint64_t top = 0, busy = kernel_end;
        if (mboot_flags & (1u << 3)) {
            uint32_t mc = *(volatile uint32_t*)(uintptr_t)(mp + 20), ma = *(volatile uint32_t*)(uintptr_t)(mp + 24);
            for (uint32_t m = 0; m < mc && m < 8; m++) {
                volatile uint32_t* me = (volatile uint32_t*)(uintptr_t)(ma + m * 16);
                if (me[1] > busy) busy = me[1];
            }
        }
        for (uint32_t off = 0; off + 24 <= map_len; ) {
            volatile uint8_t* e = (volatile uint8_t*)(uintptr_t)(map_addr + off);
            uint32_t esz = *(volatile uint32_t*)e;
            uint64_t base = *(volatile uint64_t*)(e + 4), len = *(volatile uint64_t*)(e + 12);
            uint32_t type = *(volatile uint32_t*)(e + 20);
            if (esz < 20) break;
            off += esz + 4;
            if (type != 1 || !len) continue;
            uint64_t end = base + len;
            if (end > (uint64_t)MAX_RAM_PAGES * PAGE_SIZE) end = (uint64_t)MAX_RAM_PAGES * PAGE_SIZE;
            if (end <= base) continue;
            if (nregions < MAX_REGIONS) { regions[nregions].base = base; regions[nregions].end = end; nregions++; }
            if (end > top) top = end;
        }
        top_pages = (uint32_t)(top / PAGE_SIZE);
        uint64_t bm_phys = 0, bm_bytes = ((uint64_t)top_pages / 8 + PAGE_SIZE) & ~(uint64_t)(PAGE_SIZE - 1);
        if (top_pages > LOW_PAGES) {
            busy = (busy + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            for (int i = 0; i < nregions && !bm_phys; i++) {
                uint64_t lo = regions[i].base < busy ? busy : regions[i].base;
                lo = (lo + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                if (lo + bm_bytes <= regions[i].end && lo + bm_bytes <= 0x100000000ull) bm_phys = lo;
            }
            if (bm_phys) bitmap = (uint8_t*)P2V_ADDR(bm_phys);
            else {
                serial_puts("[mem] no room for a page bitmap above 4 GB: using the RAM below it\n");
                top_pages = LOW_PAGES;
            }
        }
        { uint64_t fill = bitmap == bitmap_low ? (uint64_t)LOW_PAGES / 8 : bm_bytes; for (uint64_t i = 0; i < fill; i++) bitmap[i] = 0xFF; }
        total_pages = 0;
        uint32_t nreg = 0;
        for (uint32_t off = 0; off + 24 <= map_len; ) {
            volatile uint8_t* e = (volatile uint8_t*)(uintptr_t)(map_addr + off);
            uint32_t esz = *(volatile uint32_t*)e;
            uint64_t base = *(volatile uint64_t*)(e + 4);
            uint64_t len = *(volatile uint64_t*)(e + 12);
            uint32_t type = *(volatile uint32_t*)(e + 20);
            if (esz < 20) break;
            off += esz + 4;
            if (type != 1 || len == 0) continue;
            uint64_t end = base + len;
            uint64_t cap = (uint64_t)top_pages * PAGE_SIZE;
            if (end > cap) end = cap;
            uint64_t lo = base < kernel_end ? kernel_end : base;
            if (lo < 0x100000) lo = 0x100000;
            lo = (lo + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            end &= ~(uint64_t)(PAGE_SIZE - 1);
            if (end <= lo) continue;
            for (uint32_t pg = (uint32_t)(lo / PAGE_SIZE); pg < (uint32_t)(end / PAGE_SIZE); pg++) bitmap_clear(pg);
            if (end / PAGE_SIZE > total_pages) total_pages = (uint32_t)(end / PAGE_SIZE);
            nreg++;
        }
        serial_printf("[mem] memory map: %u usable region(s), top %u MB\n", nreg, (uint32_t)(((uint64_t)total_pages * PAGE_SIZE) >> 20));
        if (bitmap != bitmap_low) {
            uint64_t bp = V2P(bitmap);
            for (uint64_t a = bp; a < bp + bm_bytes; a += PAGE_SIZE) bitmap_set((uint32_t)(a / PAGE_SIZE));
        }
        // keep the bootloader's own structures (info block, map) out of the allocator
        for (uint32_t a = mp & ~(PAGE_SIZE - 1); a < mp + 128; a += PAGE_SIZE) if (a / PAGE_SIZE < total_pages) bitmap_set(a / PAGE_SIZE);
        for (uint32_t a = map_addr & ~(PAGE_SIZE - 1); a < map_addr + map_len; a += PAGE_SIZE) if (a / PAGE_SIZE < total_pages) bitmap_set(a / PAGE_SIZE);
        // boot modules (multiboot flag bit 3): the table, the strings and the module images stay out of the allocator
        if (mboot_flags & (1u << 3)) {
            uint32_t mcount = *(volatile uint32_t*)(uintptr_t)(mp + 20), maddr = *(volatile uint32_t*)(uintptr_t)(mp + 24);
            for (uint32_t a = maddr & ~(PAGE_SIZE - 1); a < maddr + mcount * 16 + 16; a += PAGE_SIZE) if (a / PAGE_SIZE < total_pages) bitmap_set(a / PAGE_SIZE);
            for (uint32_t m = 0; m < mcount && m < 8; m++) {
                volatile uint32_t* me = (volatile uint32_t*)(uintptr_t)(maddr + m * 16);
                uint32_t ms = me[0], mend = me[1], mstr = me[2];
                for (uint32_t a = ms & ~(PAGE_SIZE - 1); a < mend; a += PAGE_SIZE) if (a / PAGE_SIZE < total_pages) bitmap_set(a / PAGE_SIZE);
                if (mstr) { uint32_t pg = mstr / PAGE_SIZE; if (pg < total_pages) bitmap_set(pg); }
                serial_printf("[mem] module %u: %x-%x\n", m, ms, mend);
            }
        }
    } else {
        uint32_t mem_upper_kb = mboot_upper;
        if (!(mboot_flags & MULTIBOOT_FLAG_MEM)) {
            serial_puts("[mem] no memory info from multiboot\n");
            mem_upper_kb = 15 * 1024;                       // assume 1MB-16MB
        }
        for (uint32_t i = 0; i < LOW_PAGES / 8; i++) bitmap[i] = 0xFF;
        total_pages = (mem_upper_kb * 1024) / PAGE_SIZE;
        if (total_pages > LOW_PAGES) total_pages = LOW_PAGES;
        regions[0].base = (uint64_t)kernel_end_page * PAGE_SIZE; regions[0].end = (uint64_t)total_pages * PAGE_SIZE; nregions = 1;
        for (uint32_t i = kernel_end_page; i < total_pages; i++) bitmap_clear(i);
        serial_printf("[mem] upper memory: %u MB\n", (total_pages * PAGE_SIZE) >> 20);
    }

    (void)top_pages;
    used_pages = 0;
    for (uint32_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) used_pages++;
    }

    serial_puts("[mem] total pages: ");
    // Print total_pages in decimal
    char buf[12];
    int idx = 0;
    uint32_t tmp = total_pages;
    if (tmp == 0) { buf[idx++] = '0'; }
    else {
        char rev[12];
        int ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
        while (ri > 0) { buf[idx++] = rev[--ri]; }
    }
    buf[idx] = 0;
    serial_puts(buf);
    serial_puts(", used: ");
    tmp = used_pages; idx = 0;
    if (tmp == 0) { buf[idx++] = '0'; }
    else {
        char rev[12];
        int ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
        while (ri > 0) { buf[idx++] = rev[--ri]; }
    }
    buf[idx] = 0;
    serial_puts(buf);
    serial_puts("\n");
}

int pmm_region(int i, uint64_t* base, uint64_t* end) {
    if (i < 0 || i >= nregions) return 0;
    *base = regions[i].base;
    *end = regions[i].end;
    return 1;
}

static uint32_t scan_free(uint32_t from, uint32_t to) {            // first free page in [from, to), or `to`
    uint32_t i = from;
    while (i < to) {
        if (!(i & 7) && bitmap[i >> 3] == 0xFF) { i += 8; continue; }
        if (!bitmap_test(i)) return i;
        i++;
    }
    return to;
}

phys_t pmm_alloc_page_low(void) {
    uint64_t fl = irq_save();
    spin_lock(&pmm_lock);
    uint32_t lim = total_pages < LOW_PAGES ? total_pages : LOW_PAGES;
    uint32_t p = scan_free(hint_low, lim);
    phys_t r = 0;
    if (p < lim) {
        bitmap_set(p);
        used_pages++;
        hint_low = p + 1;
        r = (phys_t)p * PAGE_SIZE;
    } else hint_low = lim;
    spin_unlock(&pmm_lock);
    irq_restore(fl);
    return r;
}

phys_t pmm_alloc_page(void) {
    if (total_pages > LOW_PAGES) {
        uint64_t fl = irq_save();
        spin_lock(&pmm_lock);
        uint32_t from = hint_high < LOW_PAGES ? LOW_PAGES : hint_high;
        uint32_t p = scan_free(from, total_pages);
        phys_t r = 0;
        if (p < total_pages) {
            bitmap_set(p);
            used_pages++;
            r = (phys_t)p * PAGE_SIZE;
            p++;
        }
        hint_high = p;
        spin_unlock(&pmm_lock);
        irq_restore(fl);
        if (r) return r;
    }
    return pmm_alloc_page_low();
}

void pmm_free_page(phys_t addr) {
    uint64_t pg = addr / PAGE_SIZE;
    uint64_t fl = irq_save();
    spin_lock(&pmm_lock);
    if (pg < total_pages && bitmap_test((uint32_t)pg)) {
        uint32_t page = (uint32_t)pg;
        bitmap_clear(page);
        used_pages--;
        if (page < LOW_PAGES) { if (page < hint_low) hint_low = page; }
        else if (page < hint_high) hint_high = page;
    }
    spin_unlock(&pmm_lock);
    irq_restore(fl);
}

uint32_t pmm_get_total_pages(void) { return total_pages; }
uint32_t pmm_get_used_pages(void) { return used_pages; }
uint32_t pmm_get_free_pages(void) { return total_pages - used_pages; }

// ---- Kernel heap: segregated-fit allocator with boundary tags ----
//
// One contiguous HIGH-virtual region right after _kernel_end, sized from the
// RAM GRUB reports (paging_init maps all of RAM into the high alias). Blocks
// carry a 16-byte header {size|flags, prev_size, magic, pad}; free blocks
// thread a doubly-linked list through their payload and are binned by size
// (64 classes: 4 sub-bins per power of two). kfree validates the magic, so a
// bad/double free is logged and ignored instead of corrupting the heap, and
// coalesces with both neighbours. Allocations are 16-byte aligned. All entry
// points run with interrupts masked (IRQ paths may allocate).
//
// The heap used to be a 48MB bump allocator whose kfree was a no-op; the web
// engine allocates/frees per page load, so it needs a real one.

#define HB_HDR      16u
#define HB_ALIGN    16u
#define HB_MIN      32u           // smallest block (header + 2 list pointers)
#define HB_USED     1u            // size field flag: this block allocated
#define HB_PFREE    2u            // size field flag: previous block is free
#define HB_MAGIC_U  0xA110C8EDu
#define HB_MAGIC_F  0xF4EEB10Cu
#define HB_NBINS    64

struct hblock {
    uint32_t size;      // whole block bytes (multiple of 16) | flags
    uint32_t prev_size; // valid when HB_PFREE: size of the free block before
    uint32_t magic;
    uint32_t pad;
    struct hblock* next; // free blocks only
    struct hblock* prev;
};

static uintptr_t heap_start = 0, heap_end = 0;
static struct hblock* bins[HB_NBINS];
static uint64_t bin_map = 0;
static uint32_t heap_used = 0, heap_peak = 0;

#define HB_SIZE(b) ((b)->size & ~15u)
#define HB_NEXT_BLK(b) ((struct hblock*)((uint8_t*)(b) + HB_SIZE(b)))

static int hb_bin(uint32_t sz) {
    // sz >= 32. Classes: exact 16-byte steps up to 256, then 4 per power of 2.
    if (sz < 256) return (int)(sz >> 4);                // 2..15
    int lg = 31 - __builtin_clz(sz);                    // >= 8
    int sub = (int)((sz >> (lg - 2)) & 3);
    int b = 16 + (lg - 8) * 4 + sub;
    return b < HB_NBINS ? b : HB_NBINS - 1;
}

static void hb_unlink(struct hblock* b) {
    int i = hb_bin(HB_SIZE(b));
    if (b->prev) b->prev->next = b->next; else bins[i] = b->next;
    if (b->next) b->next->prev = b->prev;
    if (!bins[i]) bin_map &= ~(1ull << i);
}

static void hb_insert(struct hblock* b) {
    int i = hb_bin(HB_SIZE(b));
    b->magic = HB_MAGIC_F;
    b->prev = 0;
    b->next = bins[i];
    if (bins[i]) bins[i]->prev = b;
    bins[i] = b;
    bin_map |= 1ull << i;
    // footer protocol: the following block learns our size + free state
    struct hblock* n = HB_NEXT_BLK(b);
    if ((uintptr_t)n < heap_end) { n->prev_size = HB_SIZE(b); n->size |= HB_PFREE; }
}

void heap_init(void) {
    // Heap lives in HIGH virtual (CPU uses virt); the bitmap tracks PHYS
    // pages, so convert with V2P before marking. _kernel_end is high-linked.
    extern uint32_t _kernel_end;
    uint32_t kend_phys = V2P(&_kernel_end);
    uint32_t start_page = (kend_phys + PAGE_SIZE - 1) / PAGE_SIZE;

    // Size from RAM: everything above the kernel minus a reserve for the
    // PMM (user pages, page tables, stacks). 128MB RAM keeps ~1/6 back.
    // With memory-map holes the region right after the kernel may be small: use the largest free run above it.
    uint32_t low_top = total_pages < LOW_PAGES ? total_pages : LOW_PAGES;
    uint32_t run_start = 0, run_len = 0, free_pages = 0;
    for (uint32_t i = start_page; i < low_top; ) {
        if (bitmap_test(i)) { i++; continue; }
        uint32_t j2 = i;
        while (j2 < low_top && !bitmap_test(j2)) j2++;
        free_pages += j2 - i;
        if (j2 - i > run_len) { run_len = j2 - i; run_start = i; }
        i = j2;
    }
    if (run_len) start_page = run_start;
    uint32_t avail = run_len;
    uint32_t reserve = total_pages > LOW_PAGES + 4096 ? 0 : free_pages / 6;   // with RAM above 4 GB the page allocator lives there
    if (reserve < 4096) reserve = 4096;               // >= 16MB for the PMM
    uint32_t pages = avail > reserve ? avail - reserve : avail / 2;
    if (pages > (3072u << 8)) pages = 3072u << 8;     // block sizes are 32-bit
    if (pages < (32u << 8)) pages = (avail * 3) / 4;  // tiny-RAM fallback

    heap_start = P2V_ADDR((phys_t)start_page * PAGE_SIZE);
    heap_end = heap_start + (uint64_t)pages * PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) bitmap_set(start_page + i);
    used_pages += pages;

    for (int i = 0; i < HB_NBINS; i++) bins[i] = 0;
    bin_map = 0;
    // One big free block, plus a 16-byte used sentinel at the very end so
    // coalescing never walks off the region.
    struct hblock* sentinel = (struct hblock*)(heap_end - HB_HDR);
    sentinel->size = HB_HDR | HB_USED;
    sentinel->magic = HB_MAGIC_U;
    struct hblock* b = (struct hblock*)heap_start;
    b->size = (uint32_t)(heap_end - heap_start - HB_HDR);
    b->prev_size = 0;
    hb_insert(b);
    serial_printf("[mem] heap %u MB at %lx\n", (uint32_t)(((uint64_t)pages * PAGE_SIZE) >> 20), heap_start);
}

static struct hblock* hb_find(uint32_t need) {
    int i = hb_bin(need);
    // Search the exact bin first (blocks there may be smaller than need)
    for (struct hblock* b = bins[i]; b; b = b->next)
        if (HB_SIZE(b) >= need) return b;
    uint64_t m = (i + 1 < 64) ? (bin_map & ~((2ull << i) - 1)) : 0;
    if (!m) return 0;
    int j = __builtin_ctzll(m);
    return bins[j]; // every block in a higher bin is large enough
}

#define HB_MAX_ALLOC 0xFFFF0000u

void* kmalloc(size_t size64) {
    if (size64 > HB_MAX_ALLOC) {
        serial_printf("[diag] kmalloc FAIL size=%lx\n", (uint64_t)size64);
        return 0;
    }
    uint32_t size = (uint32_t)size64;
    if (heap_start == 0) heap_init();
    if (size == 0 || size > heap_end - heap_start) return 0;
    uint32_t need = (size + HB_HDR + HB_ALIGN - 1) & ~(HB_ALIGN - 1);
    if (need < HB_MIN) need = HB_MIN;

    uint64_t fl = irq_save(); spin_lock(&heap_lock);
    struct hblock* b = hb_find(need);
    if (!b) {
        spin_unlock(&heap_lock); irq_restore(fl);
        serial_printf("[diag] kmalloc FAIL size=%u used=%u\n", size, heap_used);
        return 0;
    }
    hb_unlink(b);
    uint32_t bsz = HB_SIZE(b);
    uint32_t pflag = b->size & HB_PFREE;
    if (bsz - need >= HB_MIN) {
        struct hblock* r = (struct hblock*)((uint8_t*)b + need);
        r->size = bsz - need;
        r->prev_size = 0;
        b->size = need | HB_USED | pflag;
        hb_insert(r);
    } else {
        b->size = bsz | HB_USED | pflag;
        struct hblock* n = HB_NEXT_BLK(b);
        if ((uintptr_t)n < heap_end) n->size &= ~HB_PFREE;
    }
    b->magic = HB_MAGIC_U;
    heap_used += HB_SIZE(b);
    if (heap_used > heap_peak) heap_peak = heap_used;
    spin_unlock(&heap_lock); irq_restore(fl);
    return (uint8_t*)b + HB_HDR;
}

static int hb_valid(void* ptr) {
    uintptr_t p = (uintptr_t)ptr;
    if (p < heap_start + HB_HDR || p >= heap_end || (p & (HB_ALIGN - 1))) return 0;
    struct hblock* b = (struct hblock*)(p - HB_HDR);
    return b->magic == HB_MAGIC_U && (b->size & HB_USED);
}

// Usable bytes of a live block; 0 if invalid.
uint32_t ksize(void* ptr) {
    if (!ptr || !hb_valid(ptr)) return 0;
    struct hblock* b = (struct hblock*)((uint8_t*)ptr - HB_HDR);
    return HB_SIZE(b) - HB_HDR;
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint64_t fl = irq_save(); spin_lock(&heap_lock);
    if (!hb_valid(ptr)) {
        spin_unlock(&heap_lock); irq_restore(fl);
        serial_printf("[diag] kfree: bad or double free %p\n", ptr);
        return;
    }
    struct hblock* b = (struct hblock*)((uint8_t*)ptr - HB_HDR);
    uint32_t sz = HB_SIZE(b);
    heap_used -= sz;
    uint32_t pflag = b->size & HB_PFREE;
    b->size = sz | pflag;
    // merge forward
    struct hblock* n = HB_NEXT_BLK(b);
    if ((uintptr_t)n < heap_end && !(n->size & HB_USED)) {
        hb_unlink(n);
        sz += HB_SIZE(n);
        n->magic = 0;
        b->size = sz | pflag;
    }
    // merge backward
    if (pflag) {
        struct hblock* p = (struct hblock*)((uint8_t*)b - b->prev_size);
        hb_unlink(p);
        b->magic = 0;
        sz += HB_SIZE(p);
        p->size = sz | (p->size & HB_PFREE);
        b = p;
    }
    hb_insert(b);
    spin_unlock(&heap_lock); irq_restore(fl);
}

void* kcalloc(size_t n, size_t size) {
    if (size && n > HB_MAX_ALLOC / size) return 0;
    size_t t = n * size;
    uint8_t* p = (uint8_t*)kmalloc(t);
    if (p) for (size_t i = 0; i < t; i++) p[i] = 0;
    return p;
}

void* krealloc(void* ptr, size_t size) {
    if (size > HB_MAX_ALLOC) return 0;
    if (!ptr) return kmalloc(size);
    if (!size) { kfree(ptr); return 0; }
    if (!hb_valid(ptr)) {
        serial_printf("[diag] krealloc: bad pointer %p\n", ptr);
        return 0;
    }
    struct hblock* b = (struct hblock*)((uint8_t*)ptr - HB_HDR);
    uint32_t have = HB_SIZE(b) - HB_HDR;
    if (size <= have) return ptr;
    uint8_t* q = (uint8_t*)kmalloc(size);
    if (!q) return 0;
    uint32_t* s = (uint32_t*)ptr; uint32_t* d = (uint32_t*)q;
    for (uint32_t i = 0; i < have / 4; i++) d[i] = s[i];
    kfree(ptr);
    return q;
}

// Under a hypervisor guest RAM is backed lazily, one trap per 4 KB page (WHPX: ~70,000 cycles = 19 us for the first write to a page,
// 46 cycles for the next; `memtouch`). A page load that grows the JS heap by 170 MB stalled the main loop for seconds in memset/gc_alloc,
// freezing the video and starving the sound. The idle loop therefore writes a zero to every heap page ahead of the allocator
// (kmalloc carves blocks from the LOW end of the free space) with a lock-prefixed add of 0: atomic, value unchanged, so it is safe on
// memory that is in use or being freed on another CPU. Returns 1 while pages remain.
static uintptr_t pretouch_next;
static uint32_t pretouch_t0;
int heap_pretouch(uint32_t budget_cycles) {
    if (!heap_start || heap_end - heap_start < 2 * PAGE_SIZE) return 0;
    if (!pretouch_next) { pretouch_next = heap_start & ~(uintptr_t)(PAGE_SIZE - 1); pretouch_t0 = 1; }
    if (pretouch_next >= heap_end - PAGE_SIZE) return 0;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t0 = ((uint64_t)hi << 32) | lo, tn = t0;
    do {
        __asm__ volatile("lock addl $0, (%0)" : : "r"(pretouch_next) : "memory", "cc");
        pretouch_next += PAGE_SIZE;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        tn = ((uint64_t)hi << 32) | lo;
    } while (pretouch_next < heap_end - PAGE_SIZE && tn - t0 < budget_cycles);
    if (pretouch_next >= heap_end - PAGE_SIZE) { serial_printf("[mem] heap pre-touched (%u MB)\n", (uint32_t)((heap_end - heap_start) >> 20)); return 0; }
    return 1;
}

void heap_stats(uint32_t* total, uint32_t* used, uint32_t* peak, uint32_t* largest_free) {
    if (heap_start == 0) heap_init();
    uint64_t fl = irq_save(); spin_lock(&heap_lock);
    if (total) *total = (uint32_t)(heap_end - heap_start);
    if (used) *used = heap_used;
    if (peak) *peak = heap_peak;
    if (largest_free) {
        uint32_t big = 0;
        for (int i = HB_NBINS - 1; i >= 0 && !big; i--)
            for (struct hblock* b = bins[i]; b; b = b->next)
                if (HB_SIZE(b) > big) big = HB_SIZE(b);
        *largest_free = big;
    }
    spin_unlock(&heap_lock); irq_restore(fl);
}

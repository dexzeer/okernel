#include "memory.h"
#include "memlayout.h"
#include "serial.h"
#include <stdint.h>

// Multiboot header flags
#define MULTIBOOT_MAGIC 0x2BADB002
#define MULTIBOOT_FLAG_MEM (1 << 1)

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
// Up to 896MB: the high alias (paging_init) spans 0xC0000000-0xF8000000;
// RAM beyond that has no kernel virtual address and is left unmanaged.
#define MAX_RAM_PAGES (896u * 256u)
#define BITMAP_SIZE (MAX_RAM_PAGES / 8)
static uint8_t bitmap[BITMAP_SIZE];

static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

static void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

void memory_init(uint32_t mboot_phys) {
    // mboot_phys is PHYS (GRUB structs live low). Plain phys derefs through
    // the boot PD's 0-4M LOW identity window. (An earlier pushed-args mixup
    // in start.asm once delivered the magic number in this slot — diagnosed
    // via serial mboot print + GDB; the push order is fixed, this stays.)
    uint32_t mp = mboot_phys;
    serial_printf("[mem] mboot phys=%x\n", mp);
    uint32_t mboot_flags = *(volatile uint32_t*)(mp + 0);
    uint32_t mboot_upper = *(volatile uint32_t*)(mp + 8);

    // Clear bitmap (all pages marked as used by default)
    for (int i = 0; i < BITMAP_SIZE; i++) {
        bitmap[i] = 0xFF;
    }

    serial_printf("[mem] mboot phys=%x flags=%x\n", mboot_phys, mboot_flags);

    uint32_t mem_upper_kb = mboot_upper;
    if (!(mboot_flags & MULTIBOOT_FLAG_MEM)) {
        serial_puts("[mem] no memory info from multiboot\n");
        // Assume 1MB-16MB if no info
        mem_upper_kb = 15 * 1024; // KB above 1MB
    }

    // Total memory = lower (< 1MB) + upper (> 1MB)
    // We only manage upper memory (> 1MB) for simplicity
    uint32_t mem_upper_bytes = mem_upper_kb * 1024;

    total_pages = mem_upper_bytes / PAGE_SIZE;

    // Cap to bitmap size
    if (total_pages > BITMAP_SIZE * 8) {
        total_pages = BITMAP_SIZE * 8;
    }

    serial_puts("[mem] upper memory: ");
    // Print in decimal
    uint32_t mb = mem_upper_bytes / (1024 * 1024);
    serial_putchar('0' + (mb / 100));
    serial_putchar('0' + ((mb / 10) % 10));
    serial_putchar('0' + (mb % 10));
    serial_puts(" MB\n");

    // Mark pages below 1MB as used (BIOS, IVT, BDA, etc.)
    uint32_t reserved_pages = 1024 * 1024 / PAGE_SIZE; // 256 pages
    for (uint32_t i = 0; i < reserved_pages && i < total_pages; i++) {
        bitmap_set(i);
    }

    // Mark pages used by kernel (from 1MB to end of kernel).
    // _kernel_* are HIGH linked: convert to phys before page math, or the
    // 0xC01xxxxx vaddr overflows the 32K-entry bitmap (786K pages).
    extern uint32_t _kernel_start;
    extern uint32_t _kernel_end;
    uint32_t kernel_start = V2P((uint32_t)&_kernel_start);
    uint32_t kernel_end = V2P((uint32_t)&_kernel_end);

    // Align to page boundaries
    kernel_start = kernel_start & ~(PAGE_SIZE - 1);
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint32_t kernel_start_page = kernel_start / PAGE_SIZE;
    uint32_t kernel_end_page = kernel_end / PAGE_SIZE;

    for (uint32_t i = kernel_start_page; i < kernel_end_page && i < total_pages; i++) {
        bitmap_set(i);
    }

    // Mark all pages ABOVE the kernel as FREE
    for (uint32_t i = kernel_end_page; i < total_pages; i++) {
        bitmap_clear(i);
    }

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

uint32_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            return i * PAGE_SIZE;
        }
    }
    return 0; // Out of memory
}

void pmm_free_page(uint32_t addr) {
    uint32_t page = addr / PAGE_SIZE;
    if (page < total_pages && bitmap_test(page)) {
        bitmap_clear(page);
        used_pages--;
    }
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

static uint32_t heap_start = 0, heap_end = 0;
static struct hblock* bins[HB_NBINS];
static uint64_t bin_map = 0;
static uint32_t heap_used = 0, heap_peak = 0;

#define HB_SIZE(b) ((b)->size & ~15u)
#define HB_NEXT_BLK(b) ((struct hblock*)((uint8_t*)(b) + HB_SIZE(b)))

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    __asm__ volatile("pushl %0; popfl" :: "r"(f) : "memory", "cc");
}

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
    if ((uint32_t)n < heap_end) { n->prev_size = HB_SIZE(b); n->size |= HB_PFREE; }
}

void heap_init(void) {
    // Heap lives in HIGH virtual (CPU uses virt); the bitmap tracks PHYS
    // pages, so convert with V2P before marking. _kernel_end is high-linked.
    extern uint32_t _kernel_end;
    uint32_t kend_phys = V2P((uint32_t)&_kernel_end);
    uint32_t start_page = (kend_phys + PAGE_SIZE - 1) / PAGE_SIZE;

    // Size from RAM: everything above the kernel minus a reserve for the
    // PMM (user pages, page tables, stacks). 128MB RAM keeps ~1/6 back; the
    // high alias ends at 0xF8000000 (see paging_init), which caps us too.
    uint32_t avail = total_pages > start_page ? total_pages - start_page : 0;
    uint32_t reserve = total_pages / 6;
    if (reserve < 4096) reserve = 4096;               // >= 16MB for the PMM
    uint32_t pages = avail > reserve ? avail - reserve : avail / 2;
    if (pages > (768u << 8)) pages = 768u << 8;       // <= 768MB
    if (pages < (32u << 8)) pages = (avail * 3) / 4;  // tiny-RAM fallback

    heap_start = (start_page * PAGE_SIZE) + KERNEL_VBASE;
    heap_end = heap_start + pages * PAGE_SIZE;
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
    b->size = (heap_end - heap_start - HB_HDR);
    b->prev_size = 0;
    hb_insert(b);
    serial_printf("[mem] heap %u MB at %x\n", (pages * PAGE_SIZE) >> 20, heap_start);
}

static struct hblock* hb_find(uint32_t need) {
    int i = hb_bin(need);
    // Search the exact bin first (blocks there may be smaller than need)
    for (struct hblock* b = bins[i]; b; b = b->next)
        if (HB_SIZE(b) >= need) return b;
    uint64_t m = (i + 1 < 64) ? (bin_map & ~((2ull << i) - 1)) : 0;
    if (!m) return 0;
    uint32_t lo = (uint32_t)m; // no libgcc: split the 64-bit ctz
    int j = lo ? __builtin_ctz(lo) : 32 + __builtin_ctz((uint32_t)(m >> 32));
    return bins[j]; // every block in a higher bin is large enough
}

void* kmalloc(uint32_t size) {
    if (heap_start == 0) heap_init();
    if (size == 0 || size > heap_end - heap_start) return 0;
    uint32_t need = (size + HB_HDR + HB_ALIGN - 1) & ~(HB_ALIGN - 1);
    if (need < HB_MIN) need = HB_MIN;

    uint32_t fl = irq_save();
    struct hblock* b = hb_find(need);
    if (!b) {
        irq_restore(fl);
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
        if ((uint32_t)n < heap_end) n->size &= ~HB_PFREE;
    }
    b->magic = HB_MAGIC_U;
    heap_used += HB_SIZE(b);
    if (heap_used > heap_peak) heap_peak = heap_used;
    irq_restore(fl);
    return (uint8_t*)b + HB_HDR;
}

static int hb_valid(void* ptr) {
    uint32_t p = (uint32_t)ptr;
    if (p < heap_start + HB_HDR || p >= heap_end || (p & (HB_ALIGN - 1))) return 0;
    struct hblock* b = (struct hblock*)(p - HB_HDR);
    return b->magic == HB_MAGIC_U && (b->size & HB_USED);
}

// Usable bytes of a live block (QuickJS memory accounting); 0 if invalid.
uint32_t ksize(void* ptr) {
    if (!ptr || !hb_valid(ptr)) return 0;
    struct hblock* b = (struct hblock*)((uint8_t*)ptr - HB_HDR);
    return HB_SIZE(b) - HB_HDR;
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint32_t fl = irq_save();
    if (!hb_valid(ptr)) {
        irq_restore(fl);
        serial_printf("[diag] kfree: bad or double free %x\n", (uint32_t)ptr);
        return;
    }
    struct hblock* b = (struct hblock*)((uint8_t*)ptr - HB_HDR);
    uint32_t sz = HB_SIZE(b);
    heap_used -= sz;
    uint32_t pflag = b->size & HB_PFREE;
    b->size = sz | pflag;
    // merge forward
    struct hblock* n = HB_NEXT_BLK(b);
    if ((uint32_t)n < heap_end && !(n->size & HB_USED)) {
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
    irq_restore(fl);
}

void* kcalloc(uint32_t n, uint32_t size) {
    if (size && n > 0xFFFFFFFFu / size) return 0;
    uint32_t t = n * size;
    uint8_t* p = (uint8_t*)kmalloc(t);
    if (p) for (uint32_t i = 0; i < t; i++) p[i] = 0;
    return p;
}

void* krealloc(void* ptr, uint32_t size) {
    if (!ptr) return kmalloc(size);
    if (!size) { kfree(ptr); return 0; }
    if (!hb_valid(ptr)) {
        serial_printf("[diag] krealloc: bad pointer %x\n", (uint32_t)ptr);
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

void heap_stats(uint32_t* total, uint32_t* used, uint32_t* peak, uint32_t* largest_free) {
    if (heap_start == 0) heap_init();
    uint32_t fl = irq_save();
    if (total) *total = heap_end - heap_start;
    if (used) *used = heap_used;
    if (peak) *peak = heap_peak;
    if (largest_free) {
        uint32_t big = 0;
        for (int i = HB_NBINS - 1; i >= 0 && !big; i--)
            for (struct hblock* b = bins[i]; b; b = b->next)
                if (HB_SIZE(b) > big) big = HB_SIZE(b);
        *largest_free = big;
    }
    irq_restore(fl);
}

#include "paging.h"
#include "memlayout.h"
#include "memory.h"
#include "serial.h"
#include "lapic.h"
#include <stdint.h>

// x86-64 page tables (4 levels, 64-bit entries); the address space is described in memlayout.h. The tree:
//   PML4[0..255] -> user space, private to each address space. The kernel's own tree has the identity-mapped first 128 MB there.
//   PML4[256] -> the direct map of RAM, PML4[384] -> device registers, PML4[511] -> the kernel image: one PML4 entry each, copied into every address
//   space when it is created. Their tables exist before the first space does, so a kernel mapping made later is seen everywhere.
// Entries hold PHYSICAL addresses, tables are reached through the direct map.
typedef uint64_t pte_t;

#define P_PRESENT 0x01u
#define P_RW      0x02u
#define P_US      0x04u
#define P_PWT     0x08u
#define P_PCD     0x10u
#define P_PS      0x80u
#define P_TABLE   (P_PRESENT | P_RW | P_US)
#define P_KTABLE  (P_PRESENT | P_RW)
#define PG_2M     0x00200000ull

#define PML4_DIRECT 256
#define PML4_MMIO   384
#define PML4_IMAGE  511

static inline void pte_set(volatile pte_t* p, uint64_t v) { *p = v; }       // one aligned 64-bit store: atomic for the other CPUs
static inline pte_t* tbl(phys_t phys) { return (pte_t*)P2V_ADDR(phys); }
static inline void invlpg(uintptr_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }

// The kernel's own tree (pid 0, boot, APs): the first 128 MB identity (supervisor).
static pte_t kpml4[512] __attribute__((aligned(4096)));
static pte_t kpdpt[512] __attribute__((aligned(4096)));
static pte_t kpd_low[512] __attribute__((aligned(4096)));
// The direct map. The first 4 GB have their tables here (usable before the allocator hands out anything); RAM above gets tables at paging_init.
static pte_t kpdpt_dm[512] __attribute__((aligned(4096)));
static pte_t kpd_dm[4][512] __attribute__((aligned(4096)));
// The kernel image: 0xFFFFFFFF80000000 (PDPT index 510) = the first GB of RAM.
static pte_t kpdpt_img[512] __attribute__((aligned(4096)));
static pte_t kpd_img[512] __attribute__((aligned(4096)));
// Device registers: 2 GB of 2 MB pages.
#define MMIO_PDS 2
#define MMIO_SLOTS (MMIO_PDS * 512)
static pte_t kpdpt_mmio[512] __attribute__((aligned(4096)));
static pte_t kpd_mmio[MMIO_PDS][512] __attribute__((aligned(4096)));

static phys_t alloc_table(void) {
    phys_t ph = pmm_alloc_page();
    if (!ph) return 0;
    uint64_t* z = (uint64_t*)P2V_ADDR(ph);
    for (int i = 0; i < 512; i++) z[i] = 0;
    return ph;
}

// Next level below table `t` at `idx`; creates it when `alloc`. A table on the way to a user page gets U/S set (the CPU
// checks every level). Returns 0 when absent, when it cannot be allocated, or when a large page is in the way.
static pte_t* walk_next(pte_t* t, unsigned idx, int alloc, uint32_t flags) {
    pte_t e = t[idx];
    if (!(e & P_PRESENT)) {
        if (!alloc) return 0;
        phys_t ph = alloc_table();
        if (!ph) return 0;
        pte_set(&t[idx], ph | flags);
        return tbl(ph);
    }
    if (e & P_PS) return 0;
    if (alloc && (flags & P_US) && !(e & P_US)) pte_set(&t[idx], e | P_US);
    return tbl(PTE_ADDR(e));
}

// The 4 KB entry for user address `va` in the address space whose PML4 is at phys `cr3`.
pte_t* aspace_pte(phys_t cr3, uint64_t va, int alloc) {
    if (va >= USER_SPACE_TOP) return 0;
    pte_t* pdpt = walk_next(tbl(cr3), (va >> 39) & 511, alloc, P_TABLE);
    if (!pdpt) return 0;
    pte_t* pd = walk_next(pdpt, (va >> 30) & 511, alloc, P_TABLE);
    if (!pd) return 0;
    pte_t* pt = walk_next(pd, (va >> 21) & 511, alloc, P_TABLE);
    if (!pt) return 0;
    return &pt[(va >> 12) & 511];
}

// New address space: an empty user half and the shared kernel half. Returns the PML4's phys address (the CR3 value).
phys_t aspace_create(void) {
    phys_t pml4 = alloc_table();
    if (!pml4) return 0;
    pte_set(&tbl(pml4)[PML4_DIRECT], kpml4[PML4_DIRECT]);
    pte_set(&tbl(pml4)[PML4_MMIO], kpml4[PML4_MMIO]);
    pte_set(&tbl(pml4)[PML4_IMAGE], kpml4[PML4_IMAGE]);
    return pml4;
}

// Map a 4 KB user page; `writable` 0 makes it read-only. Returns 0 on success, -1 when a table could not be allocated.
int aspace_map(phys_t cr3, uint64_t va, phys_t phys, int writable) {
    pte_t* e = aspace_pte(cr3, va, 1);
    if (!e) return -1;
    pte_set(e, PTE_ADDR(phys) | P_PRESENT | P_US | (writable ? P_RW : 0));
    return 0;
}

// Change the permission of a mapped page (flushes it when this space is loaded); 0 = was not mapped.
int aspace_set_writable(phys_t cr3, uint64_t va, int writable) {
    pte_t* e = aspace_pte(cr3, va, 0);
    if (!e || !(*e & P_PRESENT)) return 0;
    pte_t v = *e;
    pte_set(e, writable ? (v | P_RW) : (v & ~(pte_t)P_RW));
    invlpg(va);
    return 1;
}

int aspace_present(phys_t cr3, uint64_t va) {
    pte_t* e = aspace_pte(cr3, va, 0);
    return e && (*e & P_PRESENT);
}

// Unmap one user page; returns its phys address (0 if it was not mapped). The caller frees it.
phys_t aspace_unmap(phys_t cr3, uint64_t va) {
    pte_t* e = aspace_pte(cr3, va, 0);
    if (!e || !(*e & P_PRESENT)) return 0;
    phys_t ph = PTE_ADDR(*e);
    pte_set(e, 0);
    return ph;
}

// Calls fn(va, phys) for every present 4 KB user page, in address order; stops at the first non-zero answer.
int aspace_walk_user(phys_t cr3, int (*fn)(uint64_t va, phys_t phys, void* ctx), void* ctx) {
    pte_t* pml4 = tbl(cr3);
    for (uint64_t l4 = 0; l4 < PML4_DIRECT; l4++) {
        if (!(pml4[l4] & P_PRESENT)) continue;
        pte_t* pdpt = tbl(PTE_ADDR(pml4[l4]));
        for (uint64_t g = 0; g < 512; g++) {
            if (!(pdpt[g] & P_PRESENT)) continue;
            pte_t* pd = tbl(PTE_ADDR(pdpt[g]));
            for (uint64_t d = 0; d < 512; d++) {
                pte_t de = pd[d];
                if (!(de & P_PRESENT) || (de & P_PS)) continue;
                pte_t* pt = tbl(PTE_ADDR(de));
                for (uint64_t k = 0; k < 512; k++) {
                    pte_t e = pt[k];
                    if (!(e & P_PRESENT)) continue;
                    int r = fn((l4 << 39) | (g << 30) | (d << 21) | (k << 12), PTE_ADDR(e), ctx);
                    if (r) return r;
                }
            }
        }
    }
    return 0;
}

// Free every user page and every user-side table of the address space; the PML4 stays (empty user half).
// The caller reloads CR3 if this is the running space.
void aspace_free_user(phys_t cr3) {
    pte_t* pml4 = tbl(cr3);
    for (unsigned l4 = 0; l4 < PML4_DIRECT; l4++) {
        if (!(pml4[l4] & P_PRESENT)) continue;
        phys_t pdpt_ph = PTE_ADDR(pml4[l4]);
        pte_t* pdpt = tbl(pdpt_ph);
        for (unsigned g = 0; g < 512; g++) {
            if (!(pdpt[g] & P_PRESENT)) continue;
            phys_t pd_ph = PTE_ADDR(pdpt[g]);
            pte_t* pd = tbl(pd_ph);
            for (unsigned d = 0; d < 512; d++) {
                pte_t de = pd[d];
                if (!(de & P_PRESENT) || (de & P_PS)) continue;
                phys_t pt_ph = PTE_ADDR(de);
                pte_t* pt = tbl(pt_ph);
                for (unsigned k = 0; k < 512; k++)
                    if (pt[k] & P_PRESENT) pmm_free_page(PTE_ADDR(pt[k]));
                pmm_free_page(pt_ph);
            }
            pmm_free_page(pd_ph);
        }
        pmm_free_page(pdpt_ph);
        pte_set(&pml4[l4], 0);
    }
}

// Release a whole address space: user pages, user tables and the PML4.
void aspace_destroy(phys_t cr3) {
    aspace_free_user(cr3);
    pmm_free_page(cr3);
}

// Page attribute table: entries 0-7 are indexed by (PAT<<2 | PCD<<1 | PWT).
// Power-on default is WB, WT, UC-, UC, WB, WT, UC-, UC; entry 1 (PWT alone,
// otherwise write-through, which the kernel never asks for) becomes WC.
// Every CPU must run with the same PAT, so APs call this too.
static int pat_ok;
int pat_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!((d >> 16) & 1)) return 0;
    wrmsr(0x277, 0x0007040600070106ULL);
    pat_ok = 1;
    return 1;
}
int pat_enabled(void) { return pat_ok; }

// ---- device mappings: 2 MB pages out of the pool. The last two slots are the probe window.
#define PROBE_SLOT (MMIO_SLOTS - 2)
#define MMIO_MAPS 64
static uint32_t mmio_next;
static struct { phys_t base; uint32_t n, slot, flags; } mmio_map[MMIO_MAPS];
static int nmmio_map;

static inline pte_t* mmio_entry(uint32_t slot) { return &kpd_mmio[slot >> 9][slot & 511]; }

// A virtual mapping of [phys, phys + len) with these attributes; a block already mapped with the same attributes is shared.
static void* map_range(phys_t phys, uint64_t len, uint32_t flags) {
    phys_t base = phys & ~(PG_2M - 1);
    uint64_t off = phys - base;
    if (!len) len = 1;
    uint64_t n64 = (off + len + PG_2M - 1) >> 21;
    if (n64 > PROBE_SLOT) return 0;
    uint32_t n = (uint32_t)n64;
    for (int i = 0; i < nmmio_map; i++)
        if (mmio_map[i].flags == flags && mmio_map[i].base <= base && base + ((uint64_t)n << 21) <= mmio_map[i].base + ((uint64_t)mmio_map[i].n << 21))
            return (void*)(MMIO_BASE + ((uint64_t)mmio_map[i].slot << 21) + (base - mmio_map[i].base) + off);
    if (nmmio_map == MMIO_MAPS || mmio_next + n > PROBE_SLOT) return 0;
    uint32_t slot = mmio_next;
    for (uint32_t k = 0; k < n; k++) pte_set(mmio_entry(slot + k), (base + ((uint64_t)k << 21)) | flags | P_PRESENT | P_RW | P_PS);
    mmio_map[nmmio_map].base = base; mmio_map[nmmio_map].n = n; mmio_map[nmmio_map].slot = slot; mmio_map[nmmio_map].flags = flags;
    nmmio_map++;
    mmio_next += n;
    return (void*)(MMIO_BASE + ((uint64_t)slot << 21) + off);
}

void* paging_map(phys_t phys_addr) {
    phys_t b = phys_addr & ~0x3FFFFFull;
    uint8_t* v = (uint8_t*)map_range(b, 0x400000, 0);
    return v ? v + (phys_addr - b) : 0;
}

void* paging_map_mmio(phys_t phys_addr, uint64_t len) { return map_range(phys_addr, len, P_PWT | P_PCD); }

// A scratch 4MB window that can be re-pointed at any 4MB-aligned physical block, to read device registers in address space nobody has mapped
// (hunting for an ACPI-mode controller). paging_probe_window(0) unmaps it again.
void* paging_probe_window(phys_t phys4mb) {
    uintptr_t va = MMIO_BASE + ((uint64_t)PROBE_SLOT << 21);
    phys_t base = phys4mb & ~0x3FFFFFull;
    pte_set(mmio_entry(PROBE_SLOT), phys4mb ? (base | P_PRESENT | P_RW | P_PWT | P_PCD | P_PS) : 0);
    pte_set(mmio_entry(PROBE_SLOT + 1), phys4mb ? ((base + PG_2M) | P_PRESENT | P_RW | P_PWT | P_PCD | P_PS) : 0);
    invlpg(va);
    invlpg(va + PG_2M);
    return phys4mb ? (void*)va : 0;
}

static uint8_t* fb_ptr;
static phys_t fb_phys;
uint8_t* paging_fb_ptr(void) { return fb_ptr; }
phys_t paging_fb_phys(void) { return fb_phys; }

static void map_direct_2m(uint64_t addr) {
    pte_t* pd;
    unsigned g = (unsigned)(addr >> 30);
    if (g < 4) pd = kpd_dm[g];
    else {
        if (!(kpdpt_dm[g] & P_PRESENT)) {
            phys_t ph = pmm_alloc_page_low();         // reachable through the boot tables, which are still loaded
            if (!ph) return;
            uint64_t* z = (uint64_t*)P2V_ADDR(ph);
            for (int i = 0; i < 512; i++) z[i] = 0;
            pte_set(&kpdpt_dm[g], ph | P_KTABLE);
        }
        pd = tbl(PTE_ADDR(kpdpt_dm[g]));
    }
    pte_set(&pd[(addr >> 21) & 511], addr | P_PRESENT | P_RW | P_PS);
}

void paging_init(phys_t framebuffer_addr, uint64_t fb_bytes) {
    for (int i = 0; i < 512; i++) {
        kpml4[i] = 0; kpdpt[i] = 0; kpd_low[i] = 0; kpdpt_dm[i] = 0; kpdpt_img[i] = 0; kpd_img[i] = 0; kpdpt_mmio[i] = 0;
        for (int g = 0; g < 4; g++) kpd_dm[g][i] = 0;
        for (int g = 0; g < MMIO_PDS; g++) kpd_mmio[g][i] = 0;
    }
    pte_set(&kpml4[0], V2P(kpdpt) | P_TABLE);
    pte_set(&kpdpt[0], V2P(kpd_low) | P_TABLE);
    pte_set(&kpml4[PML4_DIRECT], V2P(kpdpt_dm) | P_KTABLE);
    for (int g = 0; g < 4; g++) pte_set(&kpdpt_dm[g], V2P(kpd_dm[g]) | P_KTABLE);
    pte_set(&kpml4[PML4_MMIO], V2P(kpdpt_mmio) | P_KTABLE);
    for (int g = 0; g < MMIO_PDS; g++) pte_set(&kpdpt_mmio[g], V2P(kpd_mmio[g]) | P_KTABLE);
    pte_set(&kpml4[PML4_IMAGE], V2P(kpdpt_img) | P_KTABLE);
    pte_set(&kpdpt_img[510], V2P(kpd_img) | P_KTABLE);

    // Low identity 0-128M (supervisor): the AP trampoline runs through it, and legacy low-memory uses (VGA). User processes never see it.
    for (uint64_t addr = 0; addr < 0x08000000; addr += PG_2M)
        pte_set(&kpd_low[addr >> 21], addr | P_PRESENT | P_RW | P_PS);

    // The direct map: below 4 GB everything up to the end of usable RAM (ACPI tables and the like sit in the holes), above it the usable
    // regions only, rounded out to 2 MB: there can be device memory between them.
    {
        uint64_t top = 0;
        for (int i = 0; ; i++) {
            uint64_t b, e;
            if (!pmm_region(i, &b, &e)) break;
            if (e <= 0x100000000ull) { if (e > top) top = e; }
            else if (b < 0x100000000ull) top = 0x100000000ull;
        }
        top = (top + PG_2M - 1) & ~(PG_2M - 1);
        for (uint64_t addr = 0; addr < top; addr += PG_2M) {
            map_direct_2m(addr);
            if (addr < 0x40000000u) pte_set(&kpd_img[addr >> 21], addr | P_PRESENT | P_RW | P_PS);
        }
        uint64_t high = 0;
        for (int i = 0; ; i++) {
            uint64_t b, e;
            if (!pmm_region(i, &b, &e)) break;
            if (e <= 0x100000000ull) continue;
            if (b < 0x100000000ull) b = 0x100000000ull;
            for (uint64_t addr = b & ~(PG_2M - 1); addr < e; addr += PG_2M) map_direct_2m(addr);
            high += e - b;
        }
        serial_printf("[paging] direct map covers %u MB below 4 GB, %u MB above\n", (uint32_t)(top >> 20), (uint32_t)(high >> 20));
    }

    // Write-combining (PAT entry 1 = PWT) when the CPU has a PAT. On real hardware the video aperture lives in the PCI hole, which firmware leaves
    // uncached: a write-back mapping then crawls (a full 1080p frame takes seconds), and combined writes are what the aperture is built for.
    // PAT=WC overrides an MTRR of UC. At least 12 MB: the mode can differ from what the bootloader first reported.
    if (framebuffer_addr) {
        uint32_t fbf = pat_init() ? P_PWT : 0;
        fb_ptr = (uint8_t*)map_range(framebuffer_addr, fb_bytes < (12u << 20) ? (12u << 20) : fb_bytes, fbf);
        fb_phys = framebuffer_addr;
    }

    // Switch from the boot tables to the full map
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)V2P(kpml4)) : "memory");
    serial_printf("[paging] 64-bit map on: cr3=%lx\n", V2P(kpml4));
}

// Map the low 0-1M DMA/VGA window's virtual address for a phys < 1M.
// Identity supervisor: virt == phys, valid after paging_init.
void* paging_map_low(uint32_t phys_addr) {
    return (void*)(uintptr_t)phys_addr;
}

// The kernel's PML4 (CR3 value): pid 0, boot, application processors
phys_t paging_kernel_pd_phys(void) {
    return V2P(kpml4);
}

// Map one user page into the address space at `cr3` (PHYS PML4 address). No TLB flush: a page that was not mapped
// before has no TLB entry, and a space that is not running is flushed by the CR3 switch.
void paging_map_user_pd(phys_t pd_phys, uint64_t virt_addr, phys_t phys_addr) {
    if (virt_addr >= USER_SPACE_TOP) return;
    if (!pd_phys) return;
    pte_t* e = aspace_pte(pd_phys, virt_addr, 1);
    if (e) pte_set(e, PTE_ADDR(phys_addr) | P_TABLE);
}

// Map a page as user-accessible (ring 3 can read/write it) in the kernel's own tree, with a TLB flush.
void paging_map_user(uint64_t virt_addr, phys_t phys_addr) {
    if (virt_addr >= USER_SPACE_TOP) return;
    paging_map_user_pd(V2P(kpml4), virt_addr, phys_addr);
    invlpg(virt_addr);
}

// Check a user buffer range is fully mapped user-accessible in the CURRENT
// address space (reads CR3). Ring-3 syscall pointers must pass this before
// the kernel dereferences them: a bad pointer must print an error, never
// take the kernel down with a page fault.
int paging_user_range_valid(uint64_t virt_addr, uint64_t len) {
    if (len == 0) return 0;
    if (virt_addr + len < virt_addr) return 0; // wraparound
    if (virt_addr + len > USER_SPACE_TOP) return 0; // the kernel's addresses are off-limits
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    phys_t cr3_phys = PTE_ADDR(cr3_val);
    uint64_t start = virt_addr & ~0xFFFull;
    uint64_t end = (virt_addr + len - 1) & ~0xFFFull;
    for (uint64_t page = start; ; page += 0x1000) {
        // Present + user-accessible at every level (U/S=1 is bit 2)
        pte_t* e = aspace_pte(cr3_phys, page, 0);
        if (!e || (*e & (P_PRESENT | P_US)) != (P_PRESENT | P_US)) return 0;
        if (page == end) break;
    }
    return 1;
}

// Whether an address can be read without faulting, in the address space that is loaded now (the panic screen scans the stack with it: no
// allocation, no locks).
int paging_va_mapped(uintptr_t va) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t e = cr3 & ~0xFFFull;
    static const int shift[4] = { 39, 30, 21, 12 };
    for (int lvl = 0; lvl < 4; lvl++) {
        const pte_t* t = (const pte_t*)P2V_ADDR(PTE_ADDR(e));
        e = t[(va >> shift[lvl]) & 511];
        if (!(e & P_PRESENT)) return 0;
        if (lvl && lvl < 3 && (e & P_PS)) return 1;
    }
    return 1;
}

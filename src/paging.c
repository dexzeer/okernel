#include "paging.h"
#include "memlayout.h"
#include "memory.h"
#include "serial.h"
#include "lapic.h"
#include <stdint.h>

// x86-64 page tables (4 levels, 64-bit entries). The kernel is still 32-bit code: it runs in compatibility mode with
// the CPU in long mode (boot/start.asm), so every address it handles is below 4 GB. That makes the tree small:
//   PML4[0] -> one PDPT; PDPT[0..2] -> private page directories (user space 0..3 GB), PDPT[3] -> `kpd[3]`, the
//   kernel half 0xC0000000..0xFFFFFFFF built from 2 MB pages and SHARED by every address space (no copy at creation,
//   so a kernel mapping made later is seen everywhere).
// Entries hold PHYSICAL addresses (< 4 GB), tables are reached through the high alias (P2V). Because the high
// dword of every entry is always zero, an entry is written with one 32-bit store (atomic for the other CPUs).
typedef uint64_t pte_t;

#define P_PRESENT 0x01u
#define P_RW      0x02u
#define P_US      0x04u
#define P_PS      0x80u
#define P_TABLE   (P_PRESENT | P_RW | P_US)

static inline void pte_set(volatile pte_t* p, uint32_t lo) { ((volatile uint32_t*)p)[0] = lo; }
static inline pte_t* tbl(uint32_t phys) { return (pte_t*)P2V_U32(phys); }
static inline void invlpg(uint32_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }
static inline void tlb_flush(void) { uint32_t t; __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(t) :: "memory"); }

// The kernel's own tree (pid 0, boot, APs): kpd[0] maps 0-128 MB identity (supervisor), kpd[1..2] are empty until a
// device above 1 GB asks for a window, kpd[3] is the kernel half.
static pte_t kpml4[512] __attribute__((aligned(4096)));
static pte_t kpdpt[512] __attribute__((aligned(4096)));
static pte_t kpd[4][512] __attribute__((aligned(4096)));
// The kernel half is also reachable at 0xFFFFFFFFC0000000 (PML4[511] -> PDPT[511] -> kpd[3]): 64-bit code addresses
// kernel data with a sign-extended 32-bit displacement, and 0xC0xxxxxx sign-extends to exactly this alias.
static pte_t kpdpt_hi[512] __attribute__((aligned(4096)));

static uint32_t alloc_table(void) {
    uint32_t ph = pmm_alloc_page();
    if (!ph) return 0;
    uint32_t* z = (uint32_t*)P2V_U32(ph);
    for (int i = 0; i < 1024; i++) z[i] = 0;
    return ph;
}

// Next level below table `t` at `idx`; creates it when `alloc`. A table on the way to a user page gets U/S set (the CPU
// checks every level). Returns 0 when absent, when it cannot be allocated, or when a 2 MB page is in the way.
static pte_t* walk_next(pte_t* t, unsigned idx, int alloc, uint32_t flags) {
    uint32_t e = (uint32_t)t[idx];
    if (!(e & P_PRESENT)) {
        if (!alloc) return 0;
        uint32_t ph = alloc_table();
        if (!ph) return 0;
        pte_set(&t[idx], ph | flags);
        return tbl(ph);
    }
    if (e & P_PS) return 0;
    if (alloc && (flags & P_US) && !(e & P_US)) pte_set(&t[idx], e | P_US);
    return tbl(e & 0xFFFFF000u);
}

// The 4 KB entry for `va` (< 4 GB) in the address space whose PML4 is at phys `cr3`.
pte_t* aspace_pte(uint32_t cr3, uint32_t va, int alloc) {
    if (va >= KERNEL_VBASE) return 0;                  // the shared kernel half is not edited through here
    pte_t* pdpt = walk_next(tbl(cr3), 0, alloc, P_TABLE);
    if (!pdpt) return 0;
    pte_t* pd = walk_next(pdpt, va >> 30, alloc, P_TABLE);
    if (!pd) return 0;
    pte_t* pt = walk_next(pd, (va >> 21) & 511, alloc, P_TABLE);
    if (!pt) return 0;
    return &pt[(va >> 12) & 511];
}

// New address space: an empty user half and the shared kernel half. Returns the PML4's phys address (the CR3 value).
uint32_t aspace_create(void) {
    uint32_t pml4 = alloc_table();
    if (!pml4) return 0;
    uint32_t pdpt = alloc_table();
    if (!pdpt) { pmm_free_page(pml4); return 0; }
    pte_set(&tbl(pml4)[0], pdpt | P_TABLE);
    pte_set(&tbl(pdpt)[3], (uint32_t)kpdpt[3]);        // the kernel half, the same entry for everyone
    pte_set(&tbl(pml4)[511], (uint32_t)kpml4[511]);    // and its high alias (see kpdpt_hi)
    return pml4;
}

// Map a 4 KB user page; `writable` 0 makes it read-only. Returns 0 on success, -1 when a table could not be allocated.
int aspace_map(uint32_t cr3, uint32_t va, uint32_t phys, int writable) {
    pte_t* e = aspace_pte(cr3, va, 1);
    if (!e) return -1;
    pte_set(e, (phys & 0xFFFFF000u) | P_PRESENT | P_US | (writable ? P_RW : 0));
    return 0;
}

// Change the permission of a mapped page (flushes it when this space is loaded); 0 = was not mapped.
int aspace_set_writable(uint32_t cr3, uint32_t va, int writable) {
    pte_t* e = aspace_pte(cr3, va, 0);
    if (!e || !(*e & P_PRESENT)) return 0;
    uint32_t v = (uint32_t)*e;
    pte_set(e, writable ? (v | P_RW) : (v & ~(uint32_t)P_RW));
    invlpg(va);
    return 1;
}

int aspace_present(uint32_t cr3, uint32_t va) {
    pte_t* e = aspace_pte(cr3, va, 0);
    return e && (*e & P_PRESENT);
}

// Unmap one user page; returns its phys address (0 if it was not mapped). The caller frees it.
uint32_t aspace_unmap(uint32_t cr3, uint32_t va) {
    pte_t* e = aspace_pte(cr3, va, 0);
    if (!e || !(*e & P_PRESENT)) return 0;
    uint32_t ph = (uint32_t)*e & 0xFFFFF000u;
    pte_set(e, 0);
    return ph;
}

// Calls fn(va, phys) for every present 4 KB user page, in address order; stops at the first non-zero answer.
int aspace_walk_user(uint32_t cr3, int (*fn)(uint32_t va, uint32_t phys, void* ctx), void* ctx) {
    pte_t* pml4 = tbl(cr3);
    if (!(pml4[0] & P_PRESENT)) return 0;
    pte_t* pdpt = tbl((uint32_t)pml4[0] & 0xFFFFF000u);
    for (unsigned g = 0; g < 3; g++) {
        if (!(pdpt[g] & P_PRESENT)) continue;
        pte_t* pd = tbl((uint32_t)pdpt[g] & 0xFFFFF000u);
        for (unsigned d = 0; d < 512; d++) {
            uint32_t de = (uint32_t)pd[d];
            if (!(de & P_PRESENT) || (de & P_PS)) continue;
            pte_t* pt = tbl(de & 0xFFFFF000u);
            for (unsigned k = 0; k < 512; k++) {
                uint32_t e = (uint32_t)pt[k];
                if (!(e & P_PRESENT)) continue;
                int r = fn((g << 30) | (d << 21) | (k << 12), e & 0xFFFFF000u, ctx);
                if (r) return r;
            }
        }
    }
    return 0;
}

// Free every user page and every user-side table of the address space; the PML4 and the PDPT stay (empty user half).
// The caller reloads CR3 if this is the running space.
void aspace_free_user(uint32_t cr3) {
    pte_t* pml4 = tbl(cr3);
    if (!(pml4[0] & P_PRESENT)) return;
    pte_t* pdpt = tbl((uint32_t)pml4[0] & 0xFFFFF000u);
    for (unsigned g = 0; g < 3; g++) {
        if (!(pdpt[g] & P_PRESENT)) continue;
        uint32_t pd_ph = (uint32_t)pdpt[g] & 0xFFFFF000u;
        pte_t* pd = tbl(pd_ph);
        for (unsigned d = 0; d < 512; d++) {
            uint32_t de = (uint32_t)pd[d];
            if (!(de & P_PRESENT) || (de & P_PS)) continue;
            pte_t* pt = tbl(de & 0xFFFFF000u);
            for (unsigned k = 0; k < 512; k++)
                if (pt[k] & P_PRESENT) pmm_free_page((uint32_t)pt[k] & 0xFFFFF000u);
            pmm_free_page(de & 0xFFFFF000u);
            pte_set(&pd[d], 0);
        }
        pte_set(&pdpt[g], 0);
        pmm_free_page(pd_ph);
    }
}

// Release a whole address space: user pages, user tables, the PDPT and the PML4.
void aspace_destroy(uint32_t cr3) {
    aspace_free_user(cr3);
    pte_t* pml4 = tbl(cr3);
    if (pml4[0] & P_PRESENT) pmm_free_page((uint32_t)pml4[0] & 0xFFFFF000u);
    pmm_free_page(cr3);
}

// Map one 4 MB window as two 2 MB pages at kernel virtual `va` (4 MB aligned, in the kernel half or the low 3 GB of
// the kernel's own tree). Returns 0 if something is already mapped there.
static int map_4mb(uint32_t va, uint32_t phys, uint32_t flags) {
    pte_t* pd = kpd[va >> 30];
    unsigned i = (va >> 21) & 511;
    if ((pd[i] & P_PRESENT) || (pd[i + 1] & P_PRESENT)) return 0;
    pte_set(&pd[i], (phys & 0xFFC00000u) | flags | P_PS);
    pte_set(&pd[i + 1], ((phys & 0xFFC00000u) + 0x200000u) | flags | P_PS);
    return 1;
}

// Kernel MMIO window (paging_map): 16 x 4MB above the RAM alias.
#define MMIO_VBASE_HI 0xF8000000u

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

void paging_init(uint32_t framebuffer_addr, uint32_t fb_bytes) {
    for (int i = 0; i < 512; i++) { kpml4[i] = 0; kpdpt[i] = 0; for (int g = 0; g < 4; g++) kpd[g][i] = 0; }
    pte_set(&kpml4[0], V2P((uint32_t)kpdpt) | P_TABLE);
    for (int g = 0; g < 3; g++) pte_set(&kpdpt[g], V2P((uint32_t)kpd[g]) | P_TABLE);
    pte_set(&kpdpt[3], V2P((uint32_t)kpd[3]) | P_PRESENT | P_RW);          // supervisor only
    for (int i = 0; i < 512; i++) kpdpt_hi[i] = 0;
    pte_set(&kpdpt_hi[511], V2P((uint32_t)kpd[3]) | P_PRESENT | P_RW);
    pte_set(&kpml4[511], V2P((uint32_t)kpdpt_hi) | P_PRESENT | P_RW);

    // Low identity 0-128M (supervisor). The low half is transitional: it keeps every pre-high-half phys deref working
    // (DMA, VGA, PMM) while call sites convert to V2P/P2V; user processes never see it.
    for (uint32_t addr = 0; addr < 0x08000000; addr += 0x00200000)
        pte_set(&kpd[0][addr >> 21], addr | P_PRESENT | P_RW | P_PS);

    // The kernel half: RAM as a high alias of phys 0.., ending below the MMIO window (0xF8000000) and below an
    // identity-mapped framebuffer that might sit in the high range.
    {
        uint32_t ram = pmm_get_total_pages() * 4096u;
        uint32_t lim = MMIO_VBASE_HI - KERNEL_VBASE;
        if (framebuffer_addr >= KERNEL_VBASE &&
            (framebuffer_addr & 0xFFC00000) - KERNEL_VBASE < lim)
            lim = (framebuffer_addr & 0xFFC00000) - KERNEL_VBASE;
        if (ram > lim) ram = lim;
        for (uint32_t addr = 0; addr + 0x00200000 <= ram; addr += 0x00200000)
            pte_set(&kpd[3][addr >> 21], addr | P_PRESENT | P_RW | P_PS);
        serial_printf("[paging] high alias covers %u MB\n", ram >> 20);
    }

    // Map framebuffer (typically 0xFD000000, ~8.3MB for 1920x1080x32).
    // Identity (virt == phys), supervisor.
    if (framebuffer_addr) {
        // Write-combining (PAT entry 1 = PWT) when the CPU has a PAT. On real
        // hardware the video aperture lives in the PCI hole, which firmware
        // leaves uncached: a write-back mapping then crawls (a full 1080p
        // frame takes seconds), and combined writes are what the aperture is
        // built for. PAT=WC overrides an MTRR of UC.
        uint32_t fbf = pat_init() ? 0x0B : 0x03;
        uint32_t fb_base = framebuffer_addr & 0xFFC00000;
        // 4MB windows covering the whole frame buffer (at least 3 = 12MB, the old fixed size)
        uint32_t nwin = ((framebuffer_addr & 0x003FFFFF) + fb_bytes + 0x003FFFFF) >> 22;
        if (nwin < 3) nwin = 3;
        if (nwin > 12) nwin = 12;
        for (uint32_t i = 0; i < nwin; i++) {
            uint32_t base = fb_base + i * 0x00400000;
            map_4mb(base, base, fbf);
        }
    }

    // Map e1000 MMIO (typically 0xFEB80000), supervisor
    map_4mb(0xFEB80000 & 0xFFC00000, 0xFEB80000 & 0xFFC00000, 0x03);

    // Local APIC + I/O APIC + HPET live in 0xFEC00000-0xFEFFFFFF. Uncached
    // (PCD|PWT) and identity-mapped. A relocated APIC base gets its own window.
    {
        uint32_t wins[2] = { 0xFEC00000u, lapic_phys_base() };
        for (int i = 0; i < 2; i++) {
            if (!wins[i]) continue;
            uint32_t base = wins[i] & 0xFFC00000;
            map_4mb(base, base, 0x1B);
        }
    }

    // Switch from the boot tables (0-64M low+high) to the full map
    __asm__ volatile("mov %0, %%cr3" : : "r"(V2P((uint32_t)kpml4)) : "memory");
    serial_printf("[paging] high-half map on: cr3=%x kernel_end=%x\n",
                  V2P((uint32_t)kpml4), 0u);
}

// Kernel MMIO windows (0xF8000000+, 16 x 4MB): the pool paging_map and
// paging_map_mmio draw from. The kernel half is shared by all address spaces, so a window made at any time is seen
// everywhere.
#define MMIO_VBASE MMIO_VBASE_HI
#define MMIO_WINDOWS 16          // the last one is the probe window (paging_probe_window), not part of the pool
#define MMIO_POOL (MMIO_WINDOWS - 1)
static uint32_t mmio_next = MMIO_VBASE;
static struct { uint32_t base; uint32_t flags; } mmio_win[MMIO_WINDOWS];

static void* map_windows(uint32_t phys_addr, uint32_t len, uint32_t flags) {
    uint32_t base = phys_addr & 0xFFC00000;
    uint32_t off = phys_addr & 0x003FFFFF;
    uint32_t n = (off + len + 0x003FFFFF) >> 22;
    if (n == 0) n = 1;
    if (n == 1) {                       // reuse an existing window of the same kind
        for (uint32_t v = MMIO_VBASE, i = 0; v < mmio_next; v += 0x00400000, i++)
            if (mmio_win[i].base == base && mmio_win[i].flags == flags)
                return (void*)(v + off);
    }
    if (mmio_next + n * 0x00400000 > MMIO_VBASE + MMIO_POOL * 0x00400000) return 0;
    uint32_t vbase = mmio_next;
    for (uint32_t k = 0; k < n; k++) {
        if (!map_4mb(vbase + k * 0x00400000, base + k * 0x00400000, flags)) return 0;
        mmio_win[((vbase - MMIO_VBASE) >> 22) + k].base = base + k * 0x00400000;
        mmio_win[((vbase - MMIO_VBASE) >> 22) + k].flags = flags;
    }
    mmio_next += n * 0x00400000;
    tlb_flush();
    return (void*)(vbase + off);
}

// Cached mapping of one 4MB phys region (RAM-like: ACPI tables). Returns a HIGH
// virtual address, never phys.
void* paging_map(uint32_t phys_addr) { return map_windows(phys_addr, 1, 0x03); }

// A scratch 4MB window (the last MMIO slot) that can be re-pointed at any 4MB-aligned physical block, to read device
// registers in address space nobody has mapped (hunting for an ACPI-mode controller). The entries live in the shared
// kernel half, so every address space sees a change at once.
// paging_probe_window(0) unmaps it again.
void* paging_probe_window(uint32_t phys4mb) {
    uint32_t va = MMIO_VBASE + (MMIO_WINDOWS - 1) * 0x00400000u;
    unsigned idx = (va - KERNEL_VBASE) >> 21;
    static int ours;                                       // 1 once we have put our own entry there
    if (!ours) {
        if (kpd[3][idx] & P_PRESENT) return 0;             // something else (an identity-mapped framebuffer) lives there
        ours = 1;
    }
    uint32_t base = phys4mb & 0xFFC00000;
    pte_set(&kpd[3][idx], phys4mb ? (base | 0x9B) : 0);                       // P | RW | PWT | PCD | PS
    pte_set(&kpd[3][idx + 1], phys4mb ? ((base + 0x200000u) | 0x9B) : 0);
    invlpg(va);
    invlpg(va + 0x200000u);
    return phys4mb ? (void*)va : 0;
}

// Uncached (PCD|PWT) mapping of `len` bytes of device registers at phys_addr,
// contiguous in virtual memory. Returns 0 when the pool is exhausted.
void* paging_map_mmio(uint32_t phys_addr, uint32_t len) { return map_windows(phys_addr, len, 0x1B); }

// Map the low 0-1M DMA/VGA window's virtual address for a phys < 1M.
// Identity supervisor: virt == phys, valid after paging_init.
void* paging_map_low(uint32_t phys_addr) {
    return (void*)phys_addr;
}

// The kernel's PML4 (CR3 value): pid 0, boot, application processors
uint32_t paging_kernel_pd_phys(void) {
    return V2P((uint32_t)kpml4);
}

// Map one user page into the address space at `cr3` (PHYS PML4 address). No TLB flush: a page that was not mapped
// before has no TLB entry, and a space that is not running is flushed by the CR3 switch.
void paging_map_user_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t phys_addr) {
    if (virt_addr >= KERNEL_VBASE) return;
    if (!pd_phys) return;
    pte_t* e = aspace_pte(pd_phys, virt_addr, 1);
    if (e) pte_set(e, (phys_addr & 0xFFFFF000u) | P_TABLE);
}

// Map a page as user-accessible (ring 3 can read/write it) in the kernel's own tree, with a TLB flush.
// phys_addr is PHYSICAL; virt_addr is the user-low virtual address.
void paging_map_user(uint32_t virt_addr, uint32_t phys_addr) {
    if (virt_addr >= KERNEL_VBASE) return;
    paging_map_user_pd(V2P((uint32_t)kpml4), virt_addr, phys_addr);
    invlpg(virt_addr);
}

// Check a user buffer range is fully mapped user-accessible in the CURRENT
// address space (reads CR3). Ring-3 syscall pointers must pass this before
// the kernel dereferences them — a bad pointer must print an error, never
// take the kernel down with a page fault.
int paging_user_range_valid(uint32_t virt_addr, uint32_t len) {
    if (len == 0) return 0;
    if (virt_addr + len < virt_addr) return 0; // wraparound
    if (virt_addr + len > KERNEL_VBASE) return 0; // kernel half off-limits
    uint32_t cr3_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
    cr3_phys &= 0xFFFFF000u;
    uint32_t start = virt_addr & 0xFFFFF000;
    uint32_t end = (virt_addr + len - 1) & 0xFFFFF000;
    for (uint32_t page = start; ; page += 0x1000) {
        // Present + user-accessible at every level (U/S=1 is bit 2)
        pte_t* e = aspace_pte(cr3_phys, page, 0);
        if (!e || (*e & (P_PRESENT | P_US)) != (P_PRESENT | P_US)) return 0;
        if (page == end) break;
    }
    return 1;
}

// Whether a kernel/user address below 4 GB can be read without faulting, in the address space that is loaded now
// (the panic screen scans the stack with it: no allocation, no locks).
int paging_va_mapped(uint32_t va) {
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= ~0xFFFu;
    if (cr3 >= 0x30000000u) return 0;                          // outside the kernel's direct map: cannot look
    const pte_t* pml4 = (const pte_t*)P2V_U32(cr3);
    uint32_t e = (uint32_t)pml4[0];
    if (!(e & P_PRESENT)) return 0;
    if ((e & ~0xFFFu) >= 0x30000000u) return 0;
    const pte_t* t = (const pte_t*)P2V_U32(e & ~0xFFFu);       // PDPT
    e = (uint32_t)t[va >> 30];
    if (!(e & P_PRESENT)) return 0;
    if ((e & ~0xFFFu) >= 0x30000000u) return 0;
    t = (const pte_t*)P2V_U32(e & ~0xFFFu);                    // PD
    e = (uint32_t)t[(va >> 21) & 511];
    if (!(e & P_PRESENT)) return 0;
    if (e & P_PS) return 1;
    if ((e & ~0xFFFu) >= 0x30000000u) return 0;
    t = (const pte_t*)P2V_U32(e & ~0xFFFu);                    // PT
    return (uint32_t)t[(va >> 12) & 511] & P_PRESENT;
}

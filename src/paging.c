#include "paging.h"
#include "memlayout.h"
#include "memory.h"
#include "serial.h"
#include <stdint.h>

// High-half kernel page tables. The kernel links at 0xC0100000 (phys 1MB),
// so every stored PD/PT value is PHYS (V2P) and every deref goes through P2V.
// PD 768..799 mirror phys 0-128M as the kernel high map; PD 0 keeps a 0-1M
// supervisor window for e1000 DMA (0x80000-0x9FFFF) + VGA (0xB8000); FB/MMIO
// live high and supervisor-only. User space 0..767 stays private per process.
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
// 32 tables for high kernel map (0–128M phys), 1 for low 0-1M window,
// up to 4 for framebuffer+MMIO, spares for paging_map/paging_map_user
static uint32_t page_tables[80][1024] __attribute__((aligned(4096)));
static int pt_count = 0;

extern uint32_t boot_pd[1024];

// Map one 4MB phys window at an explicit PD index (high kernel map)
static void map_4mb_at(uint32_t phys_base, uint32_t pd_index, uint32_t flags) {
    if (page_directory[pd_index] & 0x01) return;
    if (pt_count >= 80) return;

    uint32_t* pt = page_tables[pt_count];
    uint32_t pt_phys = V2P((uint32_t)pt);
    for (int i = 0; i < 1024; i++) {
        pt[i] = ((phys_base & 0xFFC00000) + i * 0x1000) | flags;
    }
    page_directory[pd_index] = pt_phys | flags;
    pt_count++;
}

static void map_4mb_high(uint32_t phys_base) {
    map_4mb_at(phys_base, PD_KERNEL_BASE + ((phys_base >> 22) & 0x3F), 0x03);
}

void paging_init(uint32_t framebuffer_addr) {
    for (int i = 0; i < 1024; i++) page_directory[i] = 0;

    // Full low identity 0-128M (supervisor) + high kernel alias of the same.
    // The low half is transitional: it keeps every pre-high-half phys
    // deref working (DMA, VGA, PMM) while call sites convert to V2P/P2V.
    // Per-process user isolation (clearing low PD 1-767) is Priority 3.
    for (uint32_t addr = 0; addr < 0x08000000; addr += 0x00400000) {
        uint32_t pd = addr >> 22;
        if (pt_count < 80) {
            uint32_t* pt = page_tables[pt_count];
            uint32_t pt_phys = V2P((uint32_t)pt);
            for (int i = 0; i < 1024; i++)
                pt[i] = ((addr & 0xFFC00000) + i * 0x1000) | 0x03;
            page_directory[pd] = pt_phys | 0x03;
            pt_count++;
        }
        map_4mb_high(addr);
    }

    // Map framebuffer (typically 0xFD000000, ~8.3MB for 1920x1080x32).
    // Identity PD index (phys>>22): FB stays at its phys address, supervisor.
    if (framebuffer_addr) {
        uint32_t fb_base = framebuffer_addr & 0xFFC00000;
        for (int i = 0; i < 3; i++) {
            uint32_t base = fb_base + i * 0x00400000;
            if (!(page_directory[base >> 22] & 0x01))
                map_4mb_at(base, base >> 22, 0x03);
        }
    }

    // Map e1000 MMIO (typically 0xFEB80000 -> PD 1018), supervisor
    {
        uint32_t base = 0xFEB80000 & 0xFFC00000;
        if (!(page_directory[base >> 22] & 0x01))
            map_4mb_at(base, base >> 22, 0x03);
    }

    // Switch from the boot PD (0-4M low+high) to the full map
    __asm__ volatile("mov %0, %%cr3" : : "r"(V2P((uint32_t)page_directory)));
    serial_printf("[paging] high-half map on: cr3=%x kernel_end=%x\n",
                  V2P((uint32_t)page_directory), 0u);
}

// Map a 4MB phys region into the kernel high MMIO window (0xC8000000+) and
// return its HIGH virtual address. Never returns phys.
#define MMIO_VBASE 0xC8000000u
static uint32_t mmio_next = MMIO_VBASE;

void* paging_map(uint32_t phys_addr) {
    uint32_t base = phys_addr & 0xFFC00000;
    uint32_t off = phys_addr & 0x003FFFFF;
    // Reuse an existing window for this base if present
    for (uint32_t v = MMIO_VBASE; v < mmio_next; v += 0x00400000) {
        uint32_t pd = v >> 22;
        uint32_t* pt = (uint32_t*)P2V_U32(page_directory[pd] & 0xFFFFF000);
        if ((pt[0] & 0xFFC00000) == base)
            return (void*)(v + off);
    }
    if (mmio_next + 0x00400000 > 0xC8000000u + 16 * 0x00400000) return 0;
    uint32_t vbase = mmio_next;
    map_4mb_at(base, vbase >> 22, 0x03);
    mmio_next += 0x00400000;
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
    return (void*)(vbase + off);
}

// Map the low 0-1M DMA/VGA window's virtual address for a phys < 1M.
// Identity supervisor: virt == phys, valid after paging_init.
void* paging_map_low(uint32_t phys_addr) {
    return (void*)phys_addr;
}

// Kernel PD phys, for process.c to share the high map (PD 768-1023)
uint32_t paging_kernel_pd_phys(void) {
    return V2P((uint32_t)page_directory);
}

// Map one user page into the PD at pd_phys (PHYS address), with TLB flush.
// Used when the target is the RUNNING address space (current CR3).
static void paging_map_user_internal(uint32_t pd_phys, uint32_t virt_addr,
                                     uint32_t phys_addr) {
    uint32_t *pd = (uint32_t*)P2V_U32(pd_phys);
    uint32_t pd_index = virt_addr >> 22;
    uint32_t pt_index = (virt_addr >> 12) & 0x3FF;

    // Ensure page directory entry exists (page-granular PT via PMM:
    // kmalloc is only 4B-aligned, and masking its low 12 bits corrupts).
    // PMM pages are PHYS: deref through the HIGH alias (P2V) — the full map
    // covers all phys, and the stored PD value stays phys for CR3 walks.
    if (!(pd[pd_index] & 0x01)) {
        uint32_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;
        uint32_t *pt = (uint32_t*)P2V_U32(pt_phys);
        for (int i = 0; i < 1024; i++) pt[i] = 0;
        pd[pd_index] = pt_phys | 0x07; // present + rw + user
    } else {
        // Pre-existing table: flip the PD entry to user so ring 3 can walk
        // through it. (Kernel-high PDEs never reach this branch: guarded above.)
        pd[pd_index] |= 0x04;
    }

    uint32_t *pt = (uint32_t*)P2V_U32(pd[pd_index] & 0xFFFFF000);
    pt[pt_index] = (phys_addr & 0xFFFFF000) | 0x07; // present + rw + user

    // The mapping changed underfoot — flush the TLB entry so the CPU walks
    // the new tables instead of the cached supervisor translation.
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

// Same, but no TLB flush: the target address space is NOT running (the CR3
// switch flushes the whole TLB anyway). Shares the body via the flush flag.
static void paging_map_user_internal_noflush(uint32_t pd_phys, uint32_t virt_addr,
                                             uint32_t phys_addr) {
    uint32_t *pd = (uint32_t*)P2V_U32(pd_phys);
    uint32_t pd_index = virt_addr >> 22;
    uint32_t pt_index = (virt_addr >> 12) & 0x3FF;

    if (!(pd[pd_index] & 0x01)) {
        uint32_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;
        uint32_t *pt = (uint32_t*)P2V_U32(pt_phys);
        for (int i = 0; i < 1024; i++) pt[i] = 0;
        pd[pd_index] = pt_phys | 0x07; // present + rw + user
    } else {
        pd[pd_index] |= 0x04;
    }

    uint32_t *pt = (uint32_t*)P2V_U32(pd[pd_index] & 0xFFFFF000);
    pt[pt_index] = (phys_addr & 0xFFFFF000) | 0x07; // present + rw + user
}

// Map a page as user-accessible (ring 3 can read/write it)
// Used for user-mode stacks and code. Marks BOTH the page-table entry AND
// the page-directory entry User (U/S=1): the CPU checks every level, so a
// supervisor PD entry faults ring 3 even when the PT entry says user.
// phys_addr is PHYSICAL; virt_addr is the user-low virtual address.
void paging_map_user(uint32_t virt_addr, uint32_t phys_addr) {
    // Low user space only: never allow remapping the kernel high half
    if (virt_addr >= KERNEL_VBASE) return;
    paging_map_user_internal(V2P((uint32_t)page_directory),
                             virt_addr, phys_addr);
}

// Same mapping into an explicit page directory (PHYS PD address).
// No TLB flush: the target address space is not running (flush on switch).
void paging_map_user_pd(uint32_t pd_phys, uint32_t virt_addr, uint32_t phys_addr) {
    if (virt_addr >= KERNEL_VBASE) return;
    if (!pd_phys) return;
    paging_map_user_internal_noflush(pd_phys, virt_addr, phys_addr);
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
    uint32_t *pd = (uint32_t*)P2V_U32(cr3_phys);
    uint32_t start = virt_addr & 0xFFFFF000;
    uint32_t end = (virt_addr + len - 1) & 0xFFFFF000;
    for (uint32_t page = start; ; page += 0x1000) {
        uint32_t pde = pd[page >> 22];
        // Present + user-accessible at BOTH levels (U/S=1 is bit 2 = 0x04)
        if (!(pde & 0x01) || !(pde & 0x04)) return 0;
        uint32_t *pt = (uint32_t*)P2V_U32(pde & 0xFFFFF000);
        uint32_t pte = pt[(page >> 12) & 0x3FF];
        if (!(pte & 0x01) || !(pte & 0x04)) return 0;
        if (page == end) break;
    }
    return 1;
}

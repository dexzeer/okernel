#include "elf.h"
#include "memlayout.h"
#include "memory.h"
#include <stdint.h>

int elf_validate(const uint8_t *img, uint32_t len) {
    if (!img || len < sizeof(struct elf32_hdr)) return -1;
    const struct elf32_hdr *h = (const struct elf32_hdr*)img;
    if (h->ident[0] != ELF_MAGIC0 || h->ident[1] != ELF_MAGIC1 ||
        h->ident[2] != ELF_MAGIC2 || h->ident[3] != ELF_MAGIC3)
        return -1;
    if (h->ident[4] != ELF_CLASS32 || h->ident[5] != ELF_DATA2LSB)
        return -2;
    if (h->type != ELF_TYPE_EXEC || h->machine != ELF_MACHINE_386)
        return -2;
    if (h->phnum == 0 || h->phentsize < sizeof(struct elf32_phdr))
        return -3;
    if (h->phoff + (uint32_t)h->phnum * h->phentsize > len)
        return -5;
    if (h->entry >= KERNEL_VBASE) return -4;
    // Every LOAD segment must sit fully in user-low and inside the file.
    for (uint16_t i = 0; i < h->phnum; i++) {
        const struct elf32_phdr *p =
            (const struct elf32_phdr*)(img + h->phoff + i * h->phentsize);
        if (p->type != ELF_PH_LOAD) continue;
        if (p->memsz == 0) continue;
        if (p->vaddr >= KERNEL_VBASE) return -4;
        if (p->vaddr + p->memsz < p->vaddr) return -4; // wraparound
        if (p->vaddr + p->memsz > KERNEL_VBASE) return -4;
        if (p->offset + p->filesz < p->offset) return -5;
        if (p->offset + p->filesz > len) return -5;
        if (p->filesz > p->memsz) return -5;
    }
    return 0;
}

int elf_load(const uint8_t *img, uint32_t len, elf_map_fn map_cb,
             uint32_t *entry_out) {
    int rc = elf_validate(img, len);
    if (rc) return rc;
    if (!map_cb || !entry_out) return -1;
    const struct elf32_hdr *h = (const struct elf32_hdr*)img;
    // Mapped pages + their phys side-table (segments are few; linear scan).
    // Overlapping segments share pages: map once, copy each segment's bytes.
    uint32_t pages[32];
    uint32_t phys_for[32];
    int n = 0;
    for (uint16_t i = 0; i < h->phnum; i++) {
        const struct elf32_phdr *p =
            (const struct elf32_phdr*)(img + h->phoff + i * h->phentsize);
        if (p->type != ELF_PH_LOAD || p->memsz == 0) continue;
        uint32_t seg_start = p->vaddr & 0xFFFFF000;
        uint32_t seg_end = (p->vaddr + p->memsz + 0xFFF) & 0xFFFFF000;
        for (uint32_t page = seg_start; page < seg_end; page += 0x1000) {
            int slot = -1;
            for (int m = 0; m < n; m++) {
                if (pages[m] == page) { slot = m; break; }
            }
            if (slot < 0) {
                if (n >= 32) return -5; // absurd segment count
                uint32_t phys = pmm_alloc_page();
                if (!phys) return -5;
                map_cb(page, phys);
                // Zero through the HIGH alias (PMM reuses dirty pages; BSS
                // tails + fresh mappings must read zero).
                uint8_t *z = (uint8_t*)P2V_U32(phys);
                for (int z_i = 0; z_i < 4096; z_i++) z[z_i] = 0;
                pages[n] = page;
                phys_for[n] = phys;
                slot = n;
                n++;
            }
            // Copy this page's slice of the segment's file bytes.
            uint32_t copy_start = p->vaddr > page ? p->vaddr : page;
            uint32_t file_end = p->vaddr + p->filesz;
            uint32_t copy_end = file_end < page + 0x1000 ? file_end : page + 0x1000;
            if (copy_end > copy_start) {
                uint8_t *dst = (uint8_t*)P2V_U32(phys_for[slot]);
                uint32_t dst_off = copy_start - page;
                uint32_t src_off = p->offset + (copy_start - p->vaddr);
                for (uint32_t k = 0; k < copy_end - copy_start; k++)
                    dst[dst_off + k] = img[src_off + k];
            }
        }
    }
    *entry_out = h->entry;
    return 0;
}

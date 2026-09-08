#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// Minimal ELF32 loader (exec-only: ET_EXEC, i386, one-or-more PT_LOAD).
// No dynamic linking, no relocations, no .bss-from-file beyond p_memsz
// zero-fill. Fits the from-scratch mandate (no external loader).

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELF_CLASS32 1
#define ELF_DATA2LSB 1
#define ELF_MACHINE_386 3
#define ELF_TYPE_EXEC 2
#define ELF_PH_LOAD 1
#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_PF_R 4

struct elf32_hdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed));

// Validate headers; returns 0 on success, negative errno-style on failure:
// -1 bad magic, -2 not 32-bit LE exec i386, -3 no program headers,
// -4 segment outside user-low, -5 file truncated.
int elf_validate(const uint8_t *img, uint32_t len);

// Load PT_LOAD segments into a process address space (maps fresh PMM pages
// via the provided map callback, zeroes BSS tails). Returns entry point
// through *entry_out, 0 on success, negative on failure (same codes).
// map_cb(virt_page, phys_page) installs one page (caller provides
// paging_map_user_pd bound to the target PD); zero_cb(page_virt) is not
// needed — pages come zeroed from the PMM... they don't (PMM reuses), so the
// loader memsets through the HIGH alias itself via copy_cb.
typedef void (*elf_map_fn)(uint32_t virt_page, uint32_t phys_page);
int elf_load(const uint8_t *img, uint32_t len, elf_map_fn map_cb,
             uint32_t *entry_out);

#endif

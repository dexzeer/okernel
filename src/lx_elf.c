// ELF64 loading for the Linux subsystem: static executables, PIE, and a dynamic loader (PT_INTERP) mapped next to them.
#include "lx.h"
#include "crypto/rand.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"
#include "string.h"

struct elf64_hdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

static const struct elf64_hdr *check(struct lx_node *n, int *err) {
    if (n->type != LXN_REG) { *err = -LX_EACCES; return 0; }
    if (n->size < sizeof(struct elf64_hdr)) { *err = -LX_ENOEXEC; return 0; }
    const struct elf64_hdr *h = (const struct elf64_hdr*)n->data;
    if (h->ident[0] != 0x7F || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F') { *err = -LX_ENOEXEC; return 0; }
    if (h->ident[4] != 2 || h->ident[5] != 1) { *err = -LX_ENOEXEC; return 0; }       // 64-bit little endian
    if (h->machine != 62 || (h->type != 2 && h->type != 3)) { *err = -LX_ENOEXEC; return 0; }
    if (h->phentsize != sizeof(struct elf64_phdr) || !h->phnum || h->phoff + (uint64_t)h->phnum * h->phentsize > n->size) {
        *err = -LX_ENOEXEC; return 0;
    }
    return h;
}

static uint32_t pgdown(uint64_t v) { return (uint32_t)v & ~0xFFFu; }
static uint32_t pgup(uint64_t v) { return (uint32_t)((v + 0xFFF) & ~0xFFFull); }

// Map the PT_LOAD segments of `n` at `bias`. Returns 0 and the extent [*lo, *hi) of the mapped range.
static int map_segments(struct lx_task *t, struct lx_node *n, const struct elf64_hdr *h, uint32_t bias, uint32_t *lo, uint32_t *hi) {
    *lo = 0xFFFFFFFFu; *hi = 0;
    for (int i = 0; i < h->phnum; i++) {
        const struct elf64_phdr *p = (const struct elf64_phdr*)(n->data + h->phoff + (uint64_t)i * h->phentsize);
        if (p->type != PT_LOAD || !p->memsz) continue;
        if (p->vaddr + bias + p->memsz > LX_USER_TOP || p->vaddr + bias < LX_USER_BASE) return -LX_ENOEXEC;
        if (p->offset + p->filesz > n->size || p->filesz > p->memsz) return -LX_ENOEXEC;
        uint32_t va = (uint32_t)p->vaddr + bias;
        uint32_t start = pgdown(va), end = pgup((uint64_t)va + p->memsz);
        uint32_t prot = ((p->flags & PF_R) ? LX_PROT_READ : 0) | ((p->flags & PF_W) ? LX_PROT_WRITE : 0) | ((p->flags & PF_X) ? LX_PROT_EXEC : 0);
        int r = lx_map_region(t, start, end - start, prot, LX_MAP_PRIVATE, n, p->offset - (va - start));
        if (r) return r;
        struct lx_vma *v = lx_vma_find(t, start);
        if (v) v->fend = va + (uint32_t)p->filesz;
        if (start < *lo) *lo = start;
        if (end > *hi) *hi = end;
    }
    return *hi ? 0 : -LX_ENOEXEC;
}

static void set_comm(struct lx_task *t, const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    int i = 0;
    for (; b[i] && i < 15; i++) t->comm[i] = b[i];
    t->comm[i] = 0;
}

// Push the strings, the auxiliary vector and argv/envp onto the new stack. Returns the initial stack pointer.
static uint64_t build_stack(struct lx_task *t, const char *execfn, const char *const *argv, const char *const *envp,
                            const uint64_t *aux, int naux) {
    uint64_t sp = LX_STACK_TOP;
    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;
    // strings, copied top-down
    uint64_t *argp = (uint64_t*)kmalloc((uint32_t)(sizeof(uint64_t) * (argc + 1)));
    uint64_t *envpp = (uint64_t*)kmalloc((uint32_t)(sizeof(uint64_t) * (envc + 1)));
    if (!argp || !envpp) { if (argp) kfree(argp); if (envpp) kfree(envpp); return 0; }
    uint32_t l = (uint32_t)strlen(execfn) + 1;
    sp -= l; lx_copy_out(t, sp, execfn, l);
    uint64_t execfn_va = sp;
    for (int i = envc - 1; i >= 0; i--) { l = (uint32_t)strlen(envp[i]) + 1; sp -= l; lx_copy_out(t, sp, envp[i], l); envpp[i] = sp; }
    for (int i = argc - 1; i >= 0; i--) { l = (uint32_t)strlen(argv[i]) + 1; sp -= l; lx_copy_out(t, sp, argv[i], l); argp[i] = sp; }
    sp -= 7; lx_copy_out(t, sp, "x86_64", 7);
    uint64_t platform_va = sp;
    uint8_t rnd[16];
    if (!rand_ready() || !rand_bytes(rnd, 16)) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(lo >> ((i & 3) * 8)) ^ (uint8_t)(i * 37 + hi); }
    sp -= 16; lx_copy_out(t, sp, rnd, 16);
    uint64_t random_va = sp;
    sp &= ~15ull;
    // the vector: argc, argv[], 0, envp[], 0, auxv
    int extra = 8;                                    // our own aux entries below
    int nwords = 1 + (argc + 1) + (envc + 1) + 2 * (naux + extra + 1);
    sp -= (uint64_t)nwords * 8;
    if (sp & 15) sp &= ~15ull;                        // keep 16-byte alignment at argc
    uint64_t w = sp;
    uint64_t v = (uint64_t)argc;
    lx_copy_out(t, w, &v, 8); w += 8;
    for (int i = 0; i < argc; i++) { lx_copy_out(t, w, &argp[i], 8); w += 8; }
    v = 0; lx_copy_out(t, w, &v, 8); w += 8;
    for (int i = 0; i < envc; i++) { lx_copy_out(t, w, &envpp[i], 8); w += 8; }
    v = 0; lx_copy_out(t, w, &v, 8); w += 8;
    uint64_t own[16] = {
        AT_PAGESZ, 4096, AT_CLKTCK, 100, AT_PLATFORM, platform_va, AT_RANDOM, random_va,
        AT_EXECFN, execfn_va, AT_HWCAP, 0x178bfbff, AT_SECURE, 0, AT_HWCAP2, 0,
    };
    for (int i = 0; i < 16; i += 2) { lx_copy_out(t, w, &own[i], 16); w += 16; }
    for (int i = 0; i < naux * 2; i += 2) { lx_copy_out(t, w, &aux[i], 16); w += 16; }
    uint64_t end[2] = { AT_NULL, 0 };
    lx_copy_out(t, w, end, 16);
    kfree(argp); kfree(envpp);
    return sp;
}

int lx_exec_load(struct lx_task *t, const char *path, const char *const *argv, const char *const *envp) {
    int err = 0;
    struct lx_node *n = lxfs_resolve(t->cwd, path, 1, &err);
    if (!n) return err;
    const struct elf64_hdr *h = check(n, &err);
    if (!h) return err;
    struct lx_node *interp = 0;
    char interp_path[256];
    for (int i = 0; i < h->phnum; i++) {
        const struct elf64_phdr *p = (const struct elf64_phdr*)(n->data + h->phoff + (uint64_t)i * h->phentsize);
        if (p->type == PT_INTERP && p->filesz < sizeof interp_path && p->offset + p->filesz <= n->size) {
            memcpy(interp_path, n->data + p->offset, (uint32_t)p->filesz);
            interp_path[p->filesz] = 0;
            interp = lxfs_resolve(t->cwd, interp_path, 1, &err);
            if (!interp) { serial_printf("[lx] %s: interpreter %s not found\n", path, interp_path); return -LX_ENOENT; }
        }
    }
    // from here on the old image is gone
    lx_free_user(t);
    t->mm->brk_start = t->mm->brk = 0;
    t->mm->mmap_hint = 0;
    t->fs_base = 0;
    for (int k = 0; k < LX_NSIG; k++) if (t->sh->act[k].handler != 1) memset(&t->sh->act[k], 0, sizeof t->sh->act[k]);   // handlers go, ignored stays
    t->sig_pending = 0;

    uint32_t bias = h->type == 3 ? 0x10000000u : 0;
    uint32_t lo, hi;
    int r = map_segments(t, n, h, bias, &lo, &hi);
    if (r) return r;
    t->mm->brk_start = t->mm->brk = hi;
    uint64_t entry = h->entry + bias;
    uint64_t at_base = 0;
    uint64_t phdr_va = 0;
    for (int i = 0; i < h->phnum; i++) {
        const struct elf64_phdr *p = (const struct elf64_phdr*)(n->data + h->phoff + (uint64_t)i * h->phentsize);
        if (p->type == PT_PHDR) phdr_va = p->vaddr + bias;
    }
    if (!phdr_va) {                                       // the program headers sit inside the first segment
        for (int i = 0; i < h->phnum; i++) {
            const struct elf64_phdr *p = (const struct elf64_phdr*)(n->data + h->phoff + (uint64_t)i * h->phentsize);
            if (p->type == PT_LOAD && h->phoff >= p->offset && h->phoff < p->offset + p->filesz) { phdr_va = p->vaddr + bias + (h->phoff - p->offset); break; }
        }
    }
    if (interp) {
        const struct elf64_hdr *ih = check(interp, &err);
        if (!ih) return err;
        uint32_t span_lo = 0xFFFFFFFFu, span_hi = 0;
        for (int i = 0; i < ih->phnum; i++) {
            const struct elf64_phdr *p = (const struct elf64_phdr*)(interp->data + ih->phoff + (uint64_t)i * ih->phentsize);
            if (p->type != PT_LOAD) continue;
            if (pgdown(p->vaddr) < span_lo) span_lo = pgdown(p->vaddr);
            if (pgup(p->vaddr + p->memsz) > span_hi) span_hi = pgup(p->vaddr + p->memsz);
        }
        uint32_t ibias = 0;
        if (ih->type == 3) {
            ibias = lx_find_free(t, span_hi - span_lo);
            if (!ibias) return -LX_ENOMEM;
            ibias -= span_lo;
            t->mm->mmap_hint = ibias + span_lo;               // later mmaps go below the loader
        }
        uint32_t ilo, ihi;
        r = map_segments(t, interp, ih, ibias, &ilo, &ihi);
        if (r) return r;
        entry = ih->entry + ibias;
        at_base = ibias;
    }
    // stack
    r = lx_map_region(t, LX_STACK_TOP - LX_STACK_SIZE, LX_STACK_SIZE, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANON | LX_MAP_GROWSDOWN, 0, 0);
    if (r) return r;
    uint64_t aux[] = {
        AT_PHDR, phdr_va, AT_PHENT, sizeof(struct elf64_phdr), AT_PHNUM, h->phnum, AT_BASE, at_base,
        AT_FLAGS, 0, AT_ENTRY, h->entry + bias, AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,
    };
    uint64_t sp = build_stack(t, path, argv, envp, aux, (int)(sizeof aux / 16));
    if (!sp) return -LX_ENOMEM;
    memset(&t->regs, 0, sizeof t->regs);
    t->regs.rip = entry;
    t->regs.rsp = sp;
    t->regs.rflags = 0x202;
    t->regs.cs = 0x2B;
    t->regs.ss = 0x23;
    memset(t->fx, 0, 512);
    t->fx[0] = 0x7F; t->fx[1] = 0x03; t->fx[24] = 0x80; t->fx[25] = 0x1F; t->fx[28] = 0xFF; t->fx[29] = 0xFF;
    set_comm(t, path);
    { int i = 0; for (; path[i] && i < 127; i++) t->exe[i] = path[i]; t->exe[i] = 0; }
    return 0;
}

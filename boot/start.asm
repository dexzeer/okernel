; Multiboot header constants
MBALIGN  equ 1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ 1 << 1            ; provide memory map
VIDEO    equ 1 << 2            ; provide video mode info
FLAGS    equ MBALIGN | MEMINFO | VIDEO
MAGIC    equ 0x1BADB002        ; multiboot magic number
CHECKSUM equ -(MAGIC + FLAGS)

; Multiboot header (must be in first 8KB of kernel)
; ELF-STYLE header (NO address fields — original working form): GRUB loads
; by ELF segments (VMA high, LMA phys) and honors VIDEO 1920x1080x32.
; (The bit-16 a.out-kludge variant was tried 2026-09-07: same 640x480 text
; result — the video issue is elsewhere, not the header style. Keep the
; original form so GRUB entry uses ELF e_entry = high _start... see the
; ENTRY-LOW note in linker-high.ld.)
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    ; Video: linear graphics, 1920x1080, 32bpp
    dd 0    ; mode_type = 0 (linear)
    dd 1920 ; width
    dd 1080 ; height
    dd 32   ; bpp

; Minimal boot GDT (phys addresses, loaded pre-paging): null + flat 4G code
; (0x08: exec/read, DPL0) + flat 4G data (0x10: read/write, DPL0). The ljmp
; after CR3 validates against THESE, not GRUB's GDT (whose 0x08 is invalid —
; GDB showed CS=0x10 GRUB-data at the fault). 6-byte pseudo-descriptor.
section .data
align 8
boot_gdt:
    dq 0x0000000000000000        ; null
    dq 0x00CF9A000000FFFF        ; 0x08: code, base 0, limit 4G, gran 4K, 32-bit
    dq 0x00CF92000000FFFF        ; 0x10: data, base 0, limit 4G, gran 4K, 32-bit
boot_gdtr:
    dw boot_gdtr - boot_gdt - 1 ; limit = 24-1
    dd boot_gdt                 ; base (LINKED high; fixed at runtime below)

; Boot page tables: identity-map 0-4M AND high-map 0xC0000000-0xC0400000
; to the same phys 0-4M, so the high-linked kernel can enable paging while
; still running low and then jump high. Phys addrs stored (CPU reads CR3 +
; PD entries as phys). 4K-aligned.
; PHYS LABELS (the NASM wrap-around trap): `mov edx, boot_pt_low-0xC0000000'
; does NOT subtract at build time — with unlinked .o VMAs (0x1000/0x2000)
; NASM emits a RELOCATION (R_386_32 of 0x40001000), and LD resolves the
; subtraction against the FINAL high VMA (0xC0188000-0xC0000000=0x188000).
; So the checked-in disassembly shows 0x188000 (correct phys) while a raw
; `nasm -o /tmp` object shows 0x40001000 (unlinked). Both are correct stages
; of the same arithmetic — do NOT "fix" the expression to a hardcoded phys.
section .data
align 4096
global boot_pd
boot_pd:
    times 1024 dd 0
align 4096
boot_pt_low:
    times 1024 dd 0
align 4096
boot_pt_high:
    times 1024 dd 0

; Stack — 256KB (TLS needs it). HIGH-linked .bss (VMA 0xC01CB000, phys
; 0x1CB000): pre-PG code must NOT touch it (not covered by any mapping
; until paging_init's full map). Post-PG high code uses the VMA name.
section .bss
align 16
stack_bottom:
    resb 262144
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits

; LOW TRAMPOLINE (VMA == LMA == phys): GRUB jumps here with paging OFF, so
; e_entry must be LOW. This section holds ONLY the pre-paging sequence
; (fill boot PD, enable PG, lgdt, ljmp high). The `label - 0xC0000000'
; expressions below resolve against the HIGH-VMA boot tables in .data
; (VMA 0xC0187xxx -> phys 0x187xxx via relocation) — verified objdump -dr.
section .trampoline
global _start
extern kernel_main

_start:
    ; ENTRY: GRUB jumps here at LOW phys with paging OFF. VMA == LMA here
    ; (see .trampoline in linker-high.ld), so labels are already phys — but
    ; the boot TABLES live in HIGH-VMA .data, hence `- 0xC0000000' on those.
    ; Set a LOW scratch ESP first (phys 0x7FF00, free low mem under 1M).
    mov esp, 0x7FF00
    ; ebx = phys multiboot pointer (save first — fill loop clobbers).
    mov edi, ebx                ; edi = phys mboot ptr

    ; Fill both PTs: PTE[i] = (i*0x1000) | 0x03 (present + rw, supervisor)
    mov ecx, 0
.fill_pt:
    mov eax, ecx
    shl eax, 12
    or eax, 0x03
    mov edx, boot_pt_low - 0xC0000000
    mov [edx + ecx*4], eax
    mov edx, boot_pt_high - 0xC0000000
    mov [edx + ecx*4], eax
    inc ecx
    cmp ecx, 1024
    jl .fill_pt

    ; PD[0] -> boot_pt_low (phys), PD[768] -> boot_pt_high (phys).
    ; ABSOLUTE-ADDRESS WARNING: `mov edx, boot_pd - 0xC0000000' looks like a
    ; numeric subtraction, but NASM treats `label - const' as LABEL+(-const):
    ; it emits a RELOCATION (R_386_32 of 0x40000000), NOT a folded immediate.
    ; The .o disassembly shows 0x40000000 (unlinked); LD resolves it against
    ; the HIGH VMA to phys 0x187000. Verified via objdump -dr. Same for every
    ; boot_* expression below — do NOT replace with hardcoded phys.
    mov eax, boot_pt_low - 0xC0000000
    or eax, 0x03
    mov edx, boot_pd - 0xC0000000
    mov [edx + 0*4], eax
    mov eax, boot_pt_high - 0xC0000000
    or eax, 0x03
    mov [edx + 768*4], eax

    ; Enable paging with the boot PD (phys addr)
    mov eax, boot_pd - 0xC0000000
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; Load OUR boot GDT (flat 0x08/0x10) before the far jump: GRUB's 0x08 is
    ; invalid (GDB: CS=0x10 at entry), so `jmp 0x08:.high' GP-faults against
    ; GRUB's tables. boot_gdtr.base is LINKED high — fix to phys first.
    mov eax, boot_gdtr - 0xC0000000
    mov edx, boot_gdt - 0xC0000000
    mov [eax + 2], edx
    lgdt [eax]
    ; Reload data segments to our 0x10 (code follows via the far jump)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to the HIGH kernel (validates against our boot GDT now).
    ; Target is _start_high (HIGH VMA in .text) — LD resolves the relocation.
    jmp 0x08:_start_high

; HIGH ENTRY (VMA high in .text): runs with paging ON via boot_pt_high.
; Fixes ESP to the high stack_top and calls kernel_main(mboot_phys).
section .text
global _start_high
_start_high:
    ; Now running HIGH: fix ESP to the high stack_top, call kernel_main.
    ; NASM resolves stack_top/kernel_main to high linked addrs — correct now.
    ; The phys stack page is the same (boot_pt_low + boot_pt_high alias it).
    mov esp, stack_top
    push eax                ; multiboot magic (1st push = [esp+8], unused)
    push edi                ; phys multiboot pointer (2nd push = [esp+4])
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

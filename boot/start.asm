; Multiboot header constants
MBALIGN  equ 1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ 1 << 1            ; provide memory map
VIDEO    equ 1 << 2            ; provide video mode info
%ifdef TEXTMODE
%ifdef FBTEXT
FLAGS    equ MBALIGN | MEMINFO | VIDEO ; text kernel drawing on a framebuffer (UEFI has no VGA text)
%define HASVIDEO
%else
FLAGS    equ MBALIGN | MEMINFO ; text build: stay in VGA text mode
%endif
%else
FLAGS    equ MBALIGN | MEMINFO | VIDEO
%define HASVIDEO
%endif
%ifndef VID_W
%define VID_W 1920            ; mode requested from the bootloader (GRUB falls back to its own
%define VID_H 1080            ; choice when the display lacks it; the kernel adapts, see graphics_init)
%endif
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
%ifdef HASVIDEO
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    dd 0    ; unused
    ; Video: linear graphics, 1920x1080, 32bpp
    dd 0    ; mode_type = 0 (linear)
    dd VID_W ; width
    dd VID_H ; height
    dd 32   ; bpp
%endif

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
    dd 0                        ; base[63:32]: the CPU is in long mode when this is loaded (10-byte form)

; Boot page tables (x86-64, 4 levels): identity-map 0-64M AND high-map 0xC0000000-0xC4000000
; to the same phys 0-64M (2MB pages), so the high-linked kernel can enable paging while
; still running low and then jump high. The CPU goes into long mode right here (see _start);
; the kernel proper then runs in compatibility mode, i.e. as the 32-bit code it always was. Phys addrs
; stored (CPU reads CR3 + PD entries as phys). 4K-aligned.
; PHYS LABELS (the NASM wrap-around trap): `mov edx, boot_pd-0xC0000000'
; does NOT subtract at build time — with unlinked .o VMAs NASM emits a
; RELOCATION (R_386_32 of 0x40000000+off), and LD resolves the subtraction
; against the FINAL high VMA. So the linked disassembly shows the correct
; phys while a raw `nasm -o /tmp` object shows 0x400xxxxx (unlinked). Both
; are correct stages of the same arithmetic — do NOT "fix" the expression
; to a hardcoded phys.
section .data
align 4096
global boot_pml4
boot_pml4:
    times 1024 dd 0
boot_pdpt:
    times 1024 dd 0
boot_pdlo:
    times 1024 dd 0
boot_pdhi:
    times 1024 dd 0

; Stack — 256KB (TLS needs it). HIGH-linked .bss: pre-PG code must NOT touch
; it (the LOW trampoline runs on a scratch ESP). Post-PG high code uses the
; VMA name; the boot PD's 0-64M high window covers it.
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
    ; ebx = phys multiboot pointer, eax = multiboot magic: kept on the scratch stack (cpuid clobbers ebx).
    push ebx
    push eax

    ; The desktop kernel needs an x86-64 CPU (long mode). Without one, say so on the debug ports and halt.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_lm
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29           ; LM
    jnz .have_lm
.no_lm:
    mov esi, .nolm_msg
    sub esi, 0                  ; (low image: labels are phys here)
.say:
    lodsb
    test al, al
    jz .halt
    mov dx, 0x3F8
    out dx, al
    mov dx, 0xE9
    out dx, al
    jmp .say
.halt:
    cli
    hlt
    jmp .halt
.nolm_msg: db "KAnarchy needs an x86-64 CPU (long mode)", 10, 0
.have_lm:

    ; Boot map: 2MB pages, phys 0-64M both identity (PDPT[0] -> boot_pdlo) and high (PDPT[3] -> boot_pdhi,
    ; 0xC0000000+). The image outgrew one 4K-page table (fonts + the web engine put .bss, and the boot stack in
    ; it, past 4M), and paging_init only runs after memory_init/mboot copy touch .bss too.
    ; ABSOLUTE-ADDRESS WARNING: `mov edx, boot_pml4 - 0xC0000000' looks like a numeric subtraction, but NASM
    ; treats `label - const' as LABEL+(-const): it emits a RELOCATION (R_386_32 of 0x40000000), NOT a folded
    ; immediate. The .o disassembly shows 0x40000000 (unlinked); LD resolves it against the HIGH VMA to phys.
    ; Verified via objdump -dr. Same for every boot_* expression below - do NOT replace with hardcoded phys.
    mov edx, boot_pml4 - 0xC0000000
    mov eax, boot_pdpt - 0xC0000000
    or eax, 3                   ; present + rw
    mov [edx], eax              ; PML4[0] -> PDPT
    mov edx, boot_pdpt - 0xC0000000
    mov eax, boot_pdlo - 0xC0000000
    or eax, 3
    mov [edx], eax              ; PDPT[0] -> identity PD
    mov eax, boot_pdhi - 0xC0000000
    or eax, 3
    mov [edx + 3*8], eax        ; PDPT[3] -> high PD
    mov edx, boot_pdlo - 0xC0000000
    mov esi, boot_pdhi - 0xC0000000
    mov ecx, 0
.fill_pd:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                ; present + rw + PS (2MB)
    mov [edx + ecx*8], eax
    mov [esi + ecx*8], eax
    inc ecx
    cmp ecx, 32
    jl .fill_pd

    ; Long mode: PAE, then EFER.LME, then paging (which makes the CPU active in IA-32e mode).
    mov eax, cr4
    or eax, 0x20                ; CR4.PAE
    mov cr4, eax
    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, 0x100               ; LME
    wrmsr
    mov eax, boot_pml4 - 0xC0000000
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax                ; PG: from here on the CPU is in long mode, running our 32-bit code as compatibility mode

    ; Load OUR boot GDT (flat 0x08/0x10) before the far jump: GRUB's 0x08 is
    ; invalid (GDB: CS=0x10 at entry), so `jmp 0x08:.high' GP-faults against
    ; GRUB's tables. boot_gdtr.base is LINKED high - fix to phys first.
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

    pop eax                     ; multiboot magic
    pop ebx                     ; phys multiboot pointer
    mov edi, ebx

    ; Far jump to the HIGH kernel (validates against our boot GDT now).
    ; Target is _start_high (HIGH VMA in .text) - LD resolves the relocation.
    jmp 0x08:_start_high

; HIGH ENTRY (VMA high in .text): runs with paging ON via the boot PD high window.
; Fixes ESP to the high stack_top and calls kernel_main(mboot_phys).
section .text
global _start_high
_start_high:
    ; Now running HIGH: fix ESP to the high stack_top, call kernel_main.
    ; NASM resolves stack_top/kernel_main to high linked addrs — correct now.
    ; The phys stack page is the same (identity + high windows alias it).
    ; edi = multiboot pointer: the LOW trampoline sets it from ebx before the
    ; far jump, but the TEXT build enters here DIRECTLY from GRUB (ENTRY
    ; _start_high — the trampoline's phys math is high-link-only), so take it
    ; from ebx fresh. Harmless no-op on the desktop path (edi == ebx there).
    mov edi, ebx
    mov esp, stack_top
    push eax                ; multiboot magic (1st push = [esp+8], unused)
    push edi                ; phys multiboot pointer (2nd push = [esp+4])
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

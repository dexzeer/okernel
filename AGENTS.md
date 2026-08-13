# AGENTS.md — okernel

## What this is

From-scratch OS in C + x86 assembly. Two build targets: text mode (VGA 320x200) and desktop mode (640x480 GRUB framebuffer). Runs in QEMU. No standard library, no Linux, no BIOS calls in protected mode.

## Build

```bash
make desktop       # Desktop ISO (okernel-desktop.iso) — primary target
make text          # Text mode ISO (okernel-text.iso)
make clean         # Nukes all .o, .bin, .iso, isodir/
```

Requires: `gcc` (multilib), `nasm`, `ld`, `grub-mkrescue`, `xorriso`, `mtools`.

Desktop QEMU command (must use `-vga std` for framebuffer):
```bash
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std
```

## Gotchas that will bite you

- **BIOS interrupts don't work.** We're in protected mode after GRUB. No `int 0x10`, no `int 0x13`. All hardware access is via port I/O (`inb`/`outb`).
- **Framebuffer is above 4MB** (typically `0xFD000000`). Must set up identity-mapped page tables in `paging.c` before accessing it. Without paging, writing to the framebuffer = instant triple fault.
- **No backbuffer flicker fix is intentional.** We use a backbuffer with dirty-row tracking. `graphics_flush()` copies only changed rows to the framebuffer. If you remove the backbuffer and write directly, you get flicker. If you remove dirty-row tracking, FPS drops from 160+ to ~5.
- **Mouse uses 3-byte PS/2 packets** (no scroll wheel support). The 4-byte IntelliMouse protocol desynchronizes the packet stream and breaks mouse movement entirely. Don't add a 4th byte read.
- **`section .note.GNU-stack`** in .asm files MUST be the last section. If placed at the top, all subsequent code gets swallowed into the non-executable section and the kernel crashes on boot.
- **Compiler flags matter:** `-fno-pic -fno-pie -mno-red-zone` are required. Without them, GCC generates position-independent code with `__x86.get_pc_thunk` calls that crash in a freestanding kernel.
- **BSS is large** (~350KB for backbuffer + window buffers + page tables). The kernel's BSS must fit within identity-mapped memory. If you add large static arrays, verify `_kernel_end` in `linker.ld` stays below available RAM.

## File ownership

| Area | Files | Notes |
|------|-------|-------|
| Boot | `boot/start.asm`, `boot/isr.asm` | Entry point, multiboot header, ISR stubs, GDT flush |
| Display | `src/graphics.c/.h` | Framebuffer, 8x8 font, draw primitives, dirty-row flush |
| Windows | `src/window.c/.h` | Window manager, PS/2 mouse driver, cursor |
| Desktop | `src/desktop.c` | Main loop, shell commands, window creation |
| Text mode | `src/kernel.c`, `src/vga.c/.h`, `src/terminal.c/.h`, `src/shell.c/.h` | Alternative build — VGA text, scrollback |
| Memory | `src/memory.c/.h` | Physical page allocator (bitmap) + bump heap |
| Interrupts | `src/gdt.c/.h`, `src/idt.c/.h`, `src/keyboard.c/.h` | GDT, IDT, PIC, keyboard driver |
| Paging | `src/paging.c/.h` | Identity-mapped page tables for framebuffer |
| Debug | `src/serial.c/.h`, `src/io.h` | COM1 serial output, port I/O |

## Architecture in 30 seconds

1. GRUB loads kernel at 1MB, provides multiboot info (memory map + framebuffer address)
2. `start.asm` sets up stack, calls `kernel_main()`
3. `kernel_main()` does: GDT → IDT → memory → paging → graphics → windows → mouse → keyboard → sti → main loop
4. Main loop: handle mouse clicks/drags → draw wallpaper → draw windows → draw cursor → flush dirty rows
5. Keyboard/mouse work via hardware interrupts (IRQ1/IRQ12), not polling

## Adding new features

- **New shell command:** add to `shell_execute()` in `desktop.c` (desktop) or `shell.c` (text). Follow the `str_eq(cmd_buf, "name")` pattern.
- **New window type:** use `window_create()`, `window_puts()`, `window_set_close_button()`. Windows auto-manage content buffers.
- **New interrupt handler:** add ISR stub in `isr.asm` (use `ISR_NOERRCODE`/`ISR_ERRCODE` macros), register in `idt_init()` in `idt.c`, add handler function.
- **New drawing primitive:** add to `graphics.c`, write to `backbuffer[]`, mark dirty rows with `graphics_mark_dirty(y)`.

## Text vs Desktop mode

The Makefile builds completely separate binaries from different source sets:
- Text: `kernel.c` + `vga.c` + `terminal.c` + `shell.c`
- Desktop: `desktop.c` + `graphics.c` + `window.c` + `paging.c`

Shared: `gdt.c`, `idt.c`, `memory.c`, `serial.c`, `keyboard.c`, `mouse.c`, `io.h`

If you modify shared code, test both builds. If you modify mode-specific code, only that build is affected.

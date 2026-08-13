# okernel — Handoff Document

## What is this?

okernel is a from-scratch operating system built in C and x86 assembly. It has two modes:

- **Text mode** (`make text`) — VGA text terminal with commands, scrolling, terminal multiplexing
- **Desktop mode** (`make desktop`) — 640x480 graphical desktop with windows, mouse, and shell

Current version: **v0.2** (desktop edition)

---

## Project Structure

```
okernel/
├── boot/
│   ├── start.asm          # Kernel entry point, multiboot header, stack
│   └── isr.asm            # Interrupt service routines (ISRs/IRQs), GDT flush
├── src/
│   ├── kernel.c           # Text mode kernel entry (terminal-only build)
│   ├── desktop.c          # Desktop mode kernel entry (graphical build)
│   │
│   ├── graphics.c/.h      # Framebuffer driver, font, drawing primitives
│   ├── window.c/.h        # Window manager, mouse driver, PS/2 handling
│   ├── paging.c/.h        # Identity-mapped page tables for framebuffer access
│   │
│   ├── gdt.c/.h           # Global Descriptor Table setup
│   ├── idt.c/.h           # Interrupt Descriptor Table, PIC remapping, IRQ dispatch
│   ├── keyboard.c/.h      # PS/2 keyboard driver (scancode set 1)
│   ├── mouse.c/.h         # PS/2 mouse driver (3-byte packets, no scroll)
│   │
│   ├── memory.c/.h        # Physical memory manager (bitmap) + kernel heap (bump)
│   ├── serial.c/.h        # Serial port output (COM1, for debugging)
│   ├── io.h               # Port I/O helpers (inb, outb, inw, outw)
│   │
│   ├── vga.c/.h           # VGA text mode driver (text mode build only)
│   ├── terminal.c/.h      # Terminal multiplexer (text mode build only)
│   └── shell.c/.h         # Shell commands (text mode build only)
│
├── linker.ld              # Linker script (kernel at 1MB, symbols for memory bounds)
├── Makefile               # Build system (text, desktop, clean, run targets)
├── service.sh             # Quick launcher: qemu-system-i386 -cdrom okernel.iso -boot d
└── okernel.txt            # ASCII art logo (user-created)
```

---

## Architecture

### Boot Sequence
1. GRUB loads kernel via multiboot spec
2. `start.asm`: sets up stack, pushes multiboot args, calls `kernel_main()`
3. Desktop mode: `start.asm` requests 640x480 linear framebuffer from GRUB
4. `kernel_main()` initializes: GDT → IDT → memory → paging → graphics → windows → mouse → keyboard → main loop

### Memory Layout
- Kernel loaded at 1MB (0x100000)
- BSS contains static variables (backbuffer: 307KB, window buffers, etc.)
- Kernel heap at ~4MB (bump allocator, 4MB)
- Page tables identity-map first 4MB + framebuffer at 0xFD000000

### Display Pipeline
1. All drawing goes to backbuffer (307KB array)
2. Dirty-row tracking — only changed rows are copied to framebuffer
3. `graphics_flush()` copies dirty rows to framebuffer at 0xFD000000
4. Result: 60-160 FPS depending on content

### Input Pipeline
- PS/2 keyboard → IRQ1 → scancode → ASCII → shell/terminal
- PS/2 mouse → IRQ12 → 3-byte packets → smoothed coordinates → cursor
- Mouse smoothing: 4-sample moving average

---

## Build Commands

```bash
make text          # Build text mode ISO (okernel-text.iso)
make desktop       # Build desktop mode ISO (okernel-desktop.iso)
make run           # Run text mode in QEMU
make run-desktop   # Run desktop mode in QEMU
make clean         # Remove all build artifacts
```

### QEMU flags
```bash
# Desktop mode (required for framebuffer display)
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std

# Text mode (standard VGA)
qemu-system-i386 -cdrom okernel-text.iso -boot d

# Debug mode (serial output)
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -nographic -serial stdio
```

---

## Key Technical Details

### VGA Mode 13h (Text Mode Build)
- 320x200, 256 colors, packed pixel
- Set via I/O port manipulation (no BIOS interrupts in protected mode)
- Framebuffer at 0xA0000

### GRUB Framebuffer (Desktop Build)
- 640x480, 8bpp indexed color
- Address provided by GRUB via multiboot info (typically 0xFD000000)
- Requires page tables to access (above 4MB)
- Identity-mapped via `paging_init()`

### Interrupts
- PIC remapped to INT 32-47 (IRQ 0-15)
- IRQ0: timer (~18.2 Hz, used for uptime)
- IRQ1: keyboard (scancode set 1)
- IRQ12: mouse (3-byte PS/2 packets)
- ISR stubs in `isr.asm` save CPU state, call C handlers

### Window System
- Up to 8 windows, each with own content buffer
- Focused window drawn last (z-order)
- Close button (12x12 red X) in title bar
- Title bar drag to move windows
- Per-window font scale (1x or 2x)

### Terminal System (Text Mode)
- 8 terminal slots, each with 80x24 content buffer
- Scrollback: 200 lines history per terminal
- Keyboard scrolling: Page Up/Down, Arrow keys, Home
- Main terminal (ID 0) cannot be closed

---

## Shell Commands

### Desktop Mode
| Command | Description |
|---------|-------------|
| help | Show available commands |
| clear | Clear terminal |
| echo [text] | Print text |
| mem | Memory usage (total/used/free in MB) |
| uptime | System uptime (Hh Mm Ss) |
| about | About okernel |
| neofetch | System info (resolution, shell, memory) |
| sysinfo | Open System Info window |
| terminal | Open new terminal window |
| exit | Close current terminal |
| reboot | Reset CPU |
| shutdown | ACPI power off |

### Text Mode
Same as above plus: `list`, `switch N`

---

## Known Limitations

- **No virtual memory** — identity mapping only, no user-mode processes
- **No filesystem** — everything in memory, no disk I/O
- **No sound** — no audio drivers
- **No networking** — no NIC drivers
- **Bump allocator** — heap doesn't free (kfree is a no-op)
- **Single CPU** — no SMP support
- **No real mouse scroll** — PS/2 3-byte mode only (scroll via keyboard)

---

## Future Ideas (v0.3+)

- VESA/VBE for higher resolutions (1024x768+)
- Basic filesystem (FAT12 or custom)
- Process scheduler with context switching
- User-mode applications
- PCI enumeration + drivers
- Sound (PC speaker or AC97)
- Networking (e1000 or RTL8139)

---

## Files to Know

| File | Why it matters |
|------|---------------|
| `boot/start.asm` | Entry point — where everything begins |
| `boot/isr.asm` | Interrupt handlers — must match IDT setup |
| `src/desktop.c` | Desktop main loop — all user-facing logic |
| `src/graphics.c` | Drawing primitives + framebuffer management |
| `src/window.c` | Window manager + mouse driver |
| `src/paging.c` | Page tables — required for framebuffer access |
| `src/memory.c` | Physical memory manager + heap |
| `linker.ld` | Memory layout — kernel load address, symbols |
| `Makefile` | Build system — text vs desktop targets |

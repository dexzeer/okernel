# okernel — Handoff Document

## What is this?

okernel is a from-scratch operating system built in C and x86 assembly. It has two modes:

- **Text mode** (`make text`) — VGA text terminal with commands, scrolling, terminal multiplexing
- **Desktop mode** (`make desktop`) — 640x480 graphical desktop with windows, mouse, and shell

Current version: **v0.3** (desktop edition with networking)

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
│   │
│   └── net/
│       ├── pci.c/.h       # PCI bus enumeration
│       ├── e1000.c/.h     # e1000 NIC driver (TX + RX working)
│       ├── rtl8139.c/.h   # RTL8139 NIC driver (TX working, RX broken)
│       └── network.c/.h   # ARP, IP, ICMP, UDP, TCP, DNS, HTTP
│
├── linker.ld              # Linker script (kernel at 1MB, symbols for memory bounds)
├── Makefile               # Build system (text, desktop, clean, run targets)
└── okernel.txt            # ASCII art logo (user-created)
```

---

## Architecture

### Boot Sequence
1. GRUB loads kernel via multiboot spec
2. `start.asm`: sets up stack, pushes multiboot args, calls `kernel_main()`
3. Desktop mode: `start.asm` requests 640x480 linear framebuffer from GRUB
4. `kernel_main()` initializes: GDT → IDT → memory → paging → graphics → windows → mouse → keyboard → networking → main loop

### Memory Layout
- Kernel loaded at 1MB (0x100000)
- BSS contains static variables (backbuffer: 307KB, window buffers, etc.)
- Kernel heap at ~4MB (bump allocator, 4MB)
- Page tables identity-map first 4MB + framebuffer at 0xFD000000
- e1000 MMIO mapped via `paging_map()`
- **NIC RX/TX buffers at 0x80000-0x9FFFF** (low memory, below 1MB, for DMA)

### Display Pipeline
1. All drawing goes to backbuffer (307KB array)
2. Dirty-row tracking — only changed rows are copied to framebuffer
3. `graphics_flush()` copies dirty rows to framebuffer at 0xFD000000
4. Result: 60-160 FPS depending on content

### Input Pipeline
- PS/2 keyboard → IRQ1 → scancode → ASCII → shell/terminal
- PS/2 mouse → IRQ12 → 3-byte packets → smoothed coordinates → cursor
- Mouse smoothing: 4-sample moving average

### Networking Stack
1. **PCI**: Bus enumeration, finds e1000 (8086:100E)
2. **e1000 driver**: MMIO-based, TX + RX working. RX buffers in low memory for DMA.
3. **ARP**: Request/reply, single-entry cache
4. **IP**: Header construction, checksum
5. **ICMP**: Echo request/reply (ping)
6. **UDP**: Send/receive
7. **DNS**: Query encoder/decoder, resolves via QEMU SLIRP (10.0.2.3)
8. **TCP**: Minimal stack — SYN/SYN-ACK/ACK/FIN, data transfer
9. **HTTP**: GET requests, response buffering

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
# Desktop mode with networking (e1000)
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0

# Debug mode (serial output + networking)
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0 -nographic -serial stdio

# Packet capture
qemu-system-i386 ... -object filter-dump,id=dump0,netdev=net0,file=/tmp/net.pcap
```

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
| ping | Ping gateway (10.0.2.2) |
| ip | Show IP address |
| resolve [host] | DNS lookup |
| browse [host] [path] | HTTP GET request |
| reboot | Reset CPU |
| shutdown | ACPI power off |

### Text Mode
Same as above plus: `list`, `switch N`

---

## Known Limitations

- **No virtual memory** — identity mapping only, no user-mode processes
- **No filesystem** — everything in memory, no disk I/O
- **No sound** — no audio drivers
- **Bump allocator** — heap doesn't free (kfree is a no-op)
- **Single CPU** — no SMP support
- **No real mouse scroll** — PS/2 3-byte mode only (scroll via keyboard)
- **Minimal TCP** — no retransmission, no windowing, no congestion control
- **HTTP limited** — single GET request, no chunked encoding, no HTTPS

---

## Network Stack Details

### e1000 Driver
- MMIO-based, supports TX and RX
- RX/TX buffers at fixed low memory addresses (0x80000-0x9FFFF)
- Descriptors at 0x80000 (RX) and 0x90000 (TX)
- IRQ handler + polling mode
- MAC address read from RAL/RAH registers after reset

### QEMU Networking
- User-mode SLIRP networking
- Guest IP: 10.0.2.15 (DHCP default)
- Gateway: 10.0.2.2
- DNS server: 10.0.2.3

### Debugging
- Serial port (COM1) output for all network events
- Packet capture via QEMU filter-dump
- Enable with `-nographic -serial stdio` for serial debug

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
| `src/net/e1000.c` | e1000 NIC driver — TX + RX |
| `src/net/network.c` | Full network stack — ARP, IP, ICMP, UDP, TCP, DNS, HTTP |
| `linker.ld` | Memory layout — kernel load address, symbols |
| `Makefile` | Build system — text vs desktop targets |

---

## Critical Bugs Found (and Fixed)

1. **`section .note.GNU-stack` placement**: Must be LAST in .asm files
2. **Compiler flags**: `-fno-pic -fno-pie -mno-red-zone` required for freestanding kernel
3. **Font bit order**: 8x8 font uses LSB-first (bit 0 = leftmost pixel)
4. **Framebuffer access**: Above 4MB, requires identity-mapped page tables
5. **e1000 register offsets**: RDH/RDT/TDH/TDT are at 0x02810/0x02818/0x03810/0x03818 (not 0x0281/0x0282)
6. **e1000 RX buffers**: Must be in low memory (<1MB) for DMA access
7. **16-bit MMIO registers**: Use 16-bit writes for RDH/RDT/TDH/TDT to avoid corrupting adjacent registers

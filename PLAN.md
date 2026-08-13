# okernel — Plan & Status

## Current State (v0.2)

### What's working
- **Boot**: GRUB multiboot, protected mode, identity-mapped paging
- **Display**: 640x480 framebuffer via GRUB, backbuffer with dirty-row tracking
- **Input**: PS/2 keyboard (scancode set 1) + mouse (3-byte packets, smoothed)
- **Desktop**: Window manager with drag, close/minimize buttons, taskbar, wallpaper
- **Terminal**: Multiplexer (8 terminals), blinking cursor, scrollback, keyboard scrolling
- **Shell**: 10 commands (help, clear, echo, mem, uptime, about, neofetch, sysinfo, terminal, exit, reboot, shutdown)
- **Networking**: PCI enumeration works, e1000 TX confirmed, ARP/IP/ICMP/UDP framework written

### What's broken
- **Networking RX**: NIC can't DMA to kernel buffers — both RTL8139 and e1000 have the same issue
- **Root cause**: RX descriptors and buffers are in BSS (above 1MB), NIC needs to DMA to physical addresses that may not be accessible

---

## Next Steps

### Phase 1: Fix Networking RX (Priority: HIGH)
1. **Diagnose DMA issue**: Check if RX buffers are accessible to the NIC's DMA engine
2. **Try allocating from low memory**: Place RX buffers below 1MB (conventional memory area 0x10000-0x9FFFF)
3. **Alternative: Use e1000 with proper page table mapping**: Map the BSS region into the NIC's DMA address space
4. **Fallback: Try virtio-net**: QEMU's paravirtualized NIC, simpler DMA model
5. **Test**: ARP resolution → IP connectivity → ping gateway

### Phase 2: DNS + UDP (Priority: MEDIUM)
1. **DNS resolver**: Query 8.8.8.8 or QEMU's built-in DNS
2. **UDP send/receive**: For DNS queries
3. **Test**: Resolve domain names

### Phase 3: HTTP Client (Priority: MEDIUM)
1. **TCP stack**: SYN/ACK/FIN handshake (or simplified version)
2. **HTTP GET**: Fetch web pages
3. **Test**: Fetch a simple HTML page

### Phase 4: "okai" Browser (Priority: LOW)
1. **HTML parser**: Extract text from basic HTML
2. **Renderer**: Display text in a terminal window
3. **Search integration**: Use DuckDuckGo API or similar
4. **Navigation**: Links, back/forward

### Phase 5: Polish (Priority: LOW)
1. **VGA palette**: Proper 256-color palette for wallpaper
2. **Font**: Larger/better font for terminal
3. **Window resize**: Draggable window borders
4. **Filesystem**: Basic read-only filesystem for config

---

## Technical Notes

### Memory Layout
- Kernel at 1MB (0x100000)
- BSS follows kernel (code + data + uninitialized)
- Page tables identity-map first 4MB
- e1000 MMIO mapped via `paging_map()`
- **Problem**: RX buffers in BSS may not be DMA-accessible

### Build
```bash
make desktop      # Build desktop ISO
make text         # Build text mode ISO
make run-desktop  # Run desktop in QEMU
```

### QEMU Commands
```bash
# Desktop with e1000
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0

# Desktop with RTL8139
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device rtl8139,netdev=net0 -netdev user,id=net0

# Packet capture
-object filter-dump,id=dump0,netdev=net0,file=/tmp/net.pcap
```

### Git Branches
- `kernel` — main development branch
- `networking-backup` — backup before networking work

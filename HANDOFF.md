# okernel — Handoff Document

## What is this?

okernel is a from-scratch operating system built in C and x86 assembly. It has two modes:

- **Text mode** (`make text`) — VGA text terminal with commands, scrolling, terminal multiplexing
- **Desktop mode** (`make desktop`) — 1024x768x32bpp graphical desktop with windows, mouse, browser, editor, and shell

Current version: **v0.4** (desktop edition: 1024x768x32bpp, working browser with resizable windows)

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
│   ├── filesystem.c/.h    # In-memory virtual filesystem (16 files, 4KB max)
│   ├── editor.c/.h        # Text editor (open, edit, save files in windows)
│   ├── browser.c/.h       # Web browser (URL bar, navigation, HTML rendering)
│   ├── html.c/.h          # HTML parser (strips headers, tokenizes tags)
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
│   ├── net/
│   │   ├── pci.c/.h       # PCI bus enumeration
│   │   ├── e1000.c/.h     # e1000 NIC driver (TX + RX working)
│   │   ├── rtl8139.c/.h   # RTL8139 NIC driver (TX working, RX broken)
│   │   └── network.c/.h   # ARP, IP, ICMP, UDP, TCP, DNS, HTTP
│   │
│   └── crypto/            # TLS 1.3 client (host-tested, NOT yet in kernel build)
│       ├── sha256.c/.h       # FIPS 180-4, streaming + one-shot
│       ├── chacha20.c/.h     # RFC 8439 stream cipher
│       ├── poly1305.c/.h     # RFC 8439 MAC (5×26-bit limbs)
│       ├── hmac.c/.h         # HMAC-SHA256 (RFC 2104/4231)
│       ├── hkdf.c/.h         # HKDF-SHA256 (RFC 5869)
│       ├── aead.c/.h         # ChaCha20-Poly1305 AEAD (RFC 8439 §2.8)
│       ├── x25519.c/.h       # RFC 7748 key exchange
│       ├── tls_record.c/.h   # Phase 1: TLS record layer
│       ├── tls_handshake.c/.h# Phase 2: handshake codec + transcript hash
│       ├── tls_keysched.c/.h # Phase 3: key schedule (RFC 8446 §7.1)
│       └── tls_client.c/.h   # Phase 4: full handshake driver (WIP — see below)
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
- Kernel heap right after kernel end (~1.5MB, bump allocator, 16MB — backbuffer + wallpaper cache are 3MB each at 1024x768x32bpp)
- Page tables identity-map first 4MB + framebuffer at 0xFD000000
- e1000 MMIO mapped via `paging_map()`
- **NIC RX/TX buffers at 0x80000-0x9FFFF** (low memory, below 1MB, for DMA)

### Display Pipeline
1. All drawing goes to backbuffer (307KB array)
2. Dirty-row tracking — only changed rows are copied to framebuffer
3. `graphics_flush()` copies dirty rows to framebuffer at 0xFD000000
4. Clip rectangle (`graphics_set_clip`/`graphics_clip_reset`) restricts drawing during scene repair so small repairs only dirty their own rows (enforced in `putpixel`, `rect_fill`, `hline`, `graphics_write_pixel`)
5. Window drag = backbuffer blit + exposed-strip repair (`graphics_blit_rect`); resize = band-only repair; content buffers are slack-allocated with fixed stride (`CONTENT_COLS_MAX`) so resize never reallocates
6. Result (kernel built with `-O2 -fno-strict-aliasing`): ~885 FPS idle, ~726 dragging, ~784 resizing in QEMU

### Cursor Compositor (v0.3.1 — artifact fix)
The cursor is a **stateless sprite** — there is NO saved background patch and no save/restore pair. This is deliberate; the previous save/restore design caused persistent artifacts (arrow ghosts, wallpaper holes in windows) because the saved patch went stale against scene changes and IRQ12 could tear it mid-read.

- Main loop order (in `desktop.c`): `desktop_paint_rect(old cursor rect)` → scene draws → `mouse_paint_cursor()` → `graphics_flush()`. Nothing draws after the sprite.
- **`desktop_paint_rect(x, y, w, h)`** — the single authoritative repair path: wallpaper → icons → windows back-to-front → taskbar. Recomposites any region from the scene model. Also used by cursor erase.
- **`window_paint_region(id, rect)`** — repaints a window from its content model clipped to a rect (frame/title redrawn whole — over-repair is harmless; content cells are clipped)
- **`mouse_get_position()`** — atomic position snapshot (`cli`/`sti` around the two-word read; ISR mutates it on IRQ12)
- Blinking text cursor state is a pure function of `tick_count` (per-window `last_cursor_visible` caches the phase; no global statics)
- Invariant: backbuffer = render(model) + one cursor sprite drawn last. If you reintroduce saved-pixel cursors or draw after `mouse_paint_cursor()`, artifacts WILL return.

### Input Pipeline
- PS/2 keyboard → IRQ1 → scancode → ASCII → shell/terminal
- PS/2 mouse → IRQ12 → 3-byte packets → smoothed coordinates → cursor
- Mouse smoothing: 4-sample moving average (N sustained packets of delta d displace ≈ 8N-12 — matters when scripting tests)

### Networking Stack
1. **PCI**: Bus enumeration, finds e1000 (8086:100E)
2. **e1000 driver**: MMIO-based, TX + RX working. RX buffers in low memory for DMA.
3. **ARP**: Request/reply, single-entry cache
4. **IP**: Header construction, checksum
5. **ICMP**: Echo request/reply (ping)
6. **UDP**: Send/receive
7. **DNS**: Query encoder/decoder, resolves via QEMU SLIRP (10.0.2.3)
8. **TCP**: Minimal stack — SYN/SYN-ACK/ACK/FIN, data transfer, retransmission with exponential backoff (220ms base, 6 attempts, then give-up + timeout message), cumulative ACK processing, RX dedup by sequence number (duplicates re-ACKed, out-of-order dropped), `netdrop N` shell command drops 1-in-N received TCP packets for testing (ARP/DNS/ICMP always pass)
9. **HTTP**: GET requests, response buffering

### Browser (okai)
- **HTML parser** (`html.c`): Strips HTTP headers (`\r\n\r\n`), skips `<head>`, `<script>`, `<style>` blocks, tokenizes h1-h6, p, a, li, pre, br, hr, text
- **Browser window** (`browser.c`): Address bar (g to focus, Esc to exit), toolbar, scrollable content
- **Keyboard**: g=address bar, j/k=scroll, r=refresh, b=back
- **Navigation**: Enter to go, back button with 4-page history
- **Rendering**: Text content with headings, paragraphs, links (cyan), lists, preformatted blocks
- **HTTP integration**: Parses response after connection closes (`http_done` flag), accumulates TCP segments correctly

---

## TLS 1.3 client (Phase 4 complete, Phase 5 + 6 complete)

Phases 1–4 are complete and host-tested. Phase 5 (kernel integration) and
Phase 6 (live in-kernel verification) are complete.

### Phase status
| Phase | Files | Status |
|-------|-------|--------|
| 1 record layer | `tls_record.c/.h` | **PASS** (21/21 host tests) |
| 2 handshake codec + transcript | `tls_handshake.c/.h` | **PASS** (31/31 host tests) |
| 3 key schedule (RFC 8446 §7.1) | `tls_keysched.c/.h` | **PASS** (13/13 host tests, RFC 8448 vector-validated) |
| 4 full handshake driver | `tls_client.c/.h` | **PASS** — full handshake against example.com (Cloudflare), HTTP 200 received |
| 5 kernel integration | `tls_net.c/.h`, `rand.c/.h` | **DONE** — Makefile wired, crypto linked, CPRNG seeded at boot, `https://` fetches work end-to-end in-kernel |
| 6 QEMU verification | (not started) | **PASS** — verified in-kernel against example.com (Cloudflare) via QEMU SLIRP. Full DNS→TCP→TLS 1.3 handshake completes; 865-byte HTTP response decrypted and browser-parsed (7 HTML tokens). Driver: `test_net.py` pattern, command `browser https://example.com/`. **One bug fixed:** TCP segment length was taken from the padded Ethernet frame length instead of the IP `total_length`, so Ethernet min-frame padding (zeros) was fed to TLS as payload and RCV.NXT advanced by the pad count — breaking the handshake. Fixed in `src/net/network.c` (TCP branch of `handle_packet`). |

### Phase 4 — FIXED (4 bugs found and corrected)

Phase 4 now passes against example.com (Cloudflare). Full TLS 1.3 handshake
completes, HTTP request sent, response received and decrypted.

Host test: `tests/test_tls_client.c` connects to example.com:443 over BSD
sockets, performs the full handshake, sends GET /, and verifies HTML response.

**Bugs fixed in this session:**

1. **Transcript body-length mismatch** (`tls_client.c`). `tls_build_client_hello`
   returns TOTAL bytes (4-byte handshake header + body). `tls_transcript_update_msg`
   expects body-only and adds the header internally. Passing the full `ch_len`
   double-counted the header in the transcript hash.
   - Fix: pass `ch + 4, ch_len - 4` in both the `transcript_after_sh` block and
     the `transcript_of()` helper.

2. **Certificate parser wrong format** (`tls_handshake.c`). TLS 1.3 Certificate
   messages have a `cert_request_context<0..2^8-1>` byte before the 3-byte
   `certificate_list` length. The old parser read bytes 0–2 as the list length,
   but byte 0 is actually the context length (0 for server certs).
   - Fix: skip `cert[0]` context bytes, then read list length from offset
     `1 + ctx_len`.

3. **send_aead tag not appended** (`tls_client.c`). `aead_chacha20_poly1305_encrypt`
   writes ciphertext to `out` and tag to a separate `tag[16]` array. The old
   `send_aead` sent `ct_len` bytes from the ciphertext buffer but never appended
   the 16-byte tag — the last 16 bytes were stack garbage.
   - Fix: `memcpy(ct + pt_len + 1, tag, 16)` after encryption.

4. **Finished key derived from wrong secret** (`tls_client.c`). RFC 8446 §4.4.4:
   the client Finished key is derived from the handshake traffic secret (`c_hs`),
   NOT the application traffic secret (`c_ap`). The old code called
   `tls_finished_key(c_ap, c_fin_key)` — the server couldn't verify our MAC.
   - Fix: `tls_finished_key(c_hs, c_fin_key)`.

5. **Separate app traffic sequence counters** (`tls_client.c`). Client and server
   application traffic use independent sequence numbers. The old code shared
   `app_seq` between send and receive — after sending the HTTP request (seq→1),
   the receive tried seq=1 instead of seq=0.
   - Fix: separate `c_ap_seq` and `s_ap_seq` starting at 0.

### Phase 5 (kernel integration) — partially done

**Done:**
- `rand.c/.h` — ChaCha20 CPRNG seeded from caller, rekeys after every 64 bytes
  of output (forward secrecy). `rand_seed()`, `rand_stir()`, `rand_bytes()`,
  `rand_ready()`. Not yet wired into kernel init (needs RDTSC + IRQ jitter
  seeding).
- `tls_net.c/.h` — `https_get(host, path)` wrapper with kernel TCP send/recv
  callbacks, 16KB TLS rx buffer, 16KB response buffer. Currently runs the
  full handshake in a single blocking call (recv callback busy-waits with
  `sti/nop/cli`). Works on host; needs adaptation for kernel IRQ-driven
  model.

**Phase 5 — COMPLETED (all 5 sub-tasks done):**
1. **Makefile** — crypto objects appended to `DESKTOP_OBJ` (sha256, hmac,
   hkdf, aead, chacha20, poly1305, x25519, tls_record, tls_handshake,
   tls_keysched, tls_client, rand, tls_net). `src/string.c` added to
   `COMMON_OBJ` (the freestanding build had no `memcpy`/`memset`/`strlen`).
   `-DKERNEL` added to `CFLAGS` so the shared TLS source switches its logging
   to serial and its RNG to the kernel CPRNG.
2. **`tls_append_data` wired into `tcp_handle_packet`** (`network.c`) — when
   `tls_is_active()` is set, ESTABLISHED payloads are appended to the TLS rx
   buffer (and ACKed) instead of `http_response`. `tls_connection_closed()`
   is called on FIN so the recv callback can EOF promptly. Also exported
   `tcp_is_established()` / `tcp_is_closed()` / `tcp_conn_state()`.
3. **CPRNG seeded at boot** (`desktop.c` `kernel_main()`) — RDTSC mixed with
   the e1000 MAC, `rand_seed()` then 32× `rand_stir()` so it reaches
   `rand_ready()` immediately; the timer ISR keeps stirring for forward
   secrecy. Serial log shows `[rand] CPRNG seeded, ready=1`.
4. **Kernel recv callback** — `kernel_tcp_recv` no longer relies on the old
   `sti;nop;cli` no-op. It drives the NIC itself (`e1000_poll()` +
   `net_poll()`) each spin and returns EOF when the peer closes. The single
   blocking `tls_client_run` is invoked synchronously from `https_get()`, which
   now also performs the DNS resolve + TCP connect (previously missing) before
   the handshake. *Deviation from the original note:* this is a NIC-polling
   blocking design rather than a cooperative `tls_poll()` state machine. It
   keeps `tls_client_run` (host-tested, PASS) unchanged and actually works in
   the kernel — `https_get` runs inside the keyboard ISR (IF=0), where polling
   the RX descriptors (DMA) is sufficient. The cost is the UI freezes for the
   ~1-2s fetch; a cooperative refactor remains a possible future improvement.
5. **Browser HTTPS** — `browser https://host/path` is detected in
   `parse_url` + `browser_open`/`navigate`/`back`; `https_get()` is called
   instead of `http_get()`. The desktop main loop gained a parallel parse path
   that feeds `tls_get_response()` into `html_parse` once `tls_is_done()`.

**Supporting changes (also required to link/build in the kernel):**
- `src/string.c` + `src/string.h` — `memcpy/memset/memcmp/memmove/strlen/
  strncmp/strchr` (the crypto uses these; freestanding had none).
- `src/serial.c` — added `serial_printf()` (minimal `%s %c %d %u %x %X %%`,
  width/precision skipped) used for TLS debug logging.
- `src/crypto/tls_dbg.h` — dual-mode logging: `serial_printf` under `KERNEL`,
  `fprintf(stderr,...)` on host. `tls_client.c` switched from `<stdio.h>` to
  this; its RNG now routes through `rand_bytes()` in kernel mode (host keeps
  the xorshift `host_rng`).
- `boot/start.asm` — kernel stack enlarged 32KB → 256KB (`tls_client_run`
  keeps several ~18KB on-stack record buffers live at once).

**Verification so far:** kernel builds to `okernel-desktop.iso` (no errors,
no undefined symbols); host `test_tls_client` still passes Phase 4 against
example.com; kernel boots clean under QEMU (CPRNG seeded, NIC up, main loop
running). The live in-kernel handshake still needs a networked QEMU (Phase 6).

### Phase 6 (QEMU verification) — VERIFIED
Boot with serial:
```bash
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std \
  -device e1000,netdev=net0 -netdev user,id=net0 \
  -display none -serial file:/tmp/ok/serial.log
```
Then drive the desktop via QEMU monitor `sendkey` (see `test_net.py`): type
`browser https://example.com/` into the focused terminal and press Enter.
Serial log shows: DNS → TCP → ClientHello → ServerHello (`cipher=0x1303
group=0x1d`) → handshake keys derived → app data → close, ending with
`[tls-net] received 865 bytes` and `[br] https parse: count=07`. A headless
driver exists at `/tmp/ok/phase6.py`.

**Bug fixed during Phase 6:** `handle_packet` (TCP branch) derived the TCP
segment length from the padded Ethernet frame length instead of the IP
`total_length` field. Short TLS records are Ethernet-padded to 64 bytes, so the
zero padding was delivered to TLS as payload (and RCV.NXT advanced by the pad
count), which broke the handshake. Fixed by computing `tcp_len` from
`ip_header.total_length` and locating the TCP header at the IP IHL offset.
(See Critical Bugs #19.)

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
| browser [url] | Open web browser (e.g. `browser http://example.com/`) |
| edit [file] | Open text editor with file |
| ls | List files in virtual filesystem |
| open [file] | Open file in text editor |
| reboot | Reset CPU |
| shutdown | ACPI power off |

### Text Mode
Same as above plus: `list`, `switch N`

---

## Known Limitations

- **No virtual memory** — identity mapping only, no user-mode processes
- **In-memory filesystem only** — files lost on reboot, no disk I/O
- **No sound** — no audio drivers
- **Bump allocator** — heap doesn't free (kfree is a no-op)
- **Single CPU** — no SMP support
- **No real mouse scroll** — PS/2 3-byte mode only (scroll via keyboard)
- **Minimal TCP** — no retransmission, no windowing, no congestion control
- **HTTP limited** — single GET request (chunked transfer IS decoded via `http_dechunk`)
- **HTTPS works** via the browser (`browser https://host/path`); it is a
  blocking single-call fetch (UI freezes ~1-2s during the handshake). No
  standalone `https` shell command yet (only the browser path is wired).
- **TLS 1.3 limited to ChaCha20-Poly1305** — SHA-256 only; no AES-GCM, no SHA-384. Phase 4 + 5 complete.
- **TCP minimal** — single connection, no windowing/congestion control, no out-of-order buffering (gaps re-ACKed until the server fills them). Retransmission IS implemented (timer + backoff + give-up). This is the foundation for the planned TLS 1.3 client.

---

## Network Stack Details

### e1000 Driver
- MMIO-based, supports TX and RX
- RX/TX buffers at fixed low memory addresses (0x80000-0x9FFFF)
- Descriptors at 0x80000 (RX) and 0x90000 (TX)
- **RX ring release: software writes `RDT = index of LAST processed descriptor`** (never index+1, never RDH — RDT==RDH reads as "ring full" and hardware silently drops every packet; writing RDH never replenishes the ring and it exhausts after ~15 packets). Both wrong variants caused stalled/blank browser fetches.
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

### Crypto (src/crypto/, host-tested in tests/)
All primitives for the planned TLS 1.3 client. **Every module is host-tested against RFC vectors** (run `cd tests && gcc ... && ./test_*`):
- `sha256.c` — FIPS 180-4, streaming + one-shot
- `chacha20.c` — RFC 8439 stream cipher
- `poly1305.c` — RFC 8439 MAC, 5x26-bit limbs (32-bit friendly)
- `hmac.c` — HMAC-SHA256 (RFC 2104)
- `hkdf.c` — HKDF-SHA256 (RFC 5869), the TLS 1.3 key schedule
- `aead.c` — ChaCha20-Poly1305 AEAD (RFC 8439 §2.8); MAC scratch cap 20KB (TLS records fit)
- `x25519.c` — RFC 7748 key exchange, 16x16-bit limbs, Montgomery ladder; fe_invert = binary square-and-multiply (p-2 exponent, NOT an addition chain — the hand-copied chain computed z^(2^253+3))

Crypto debugging lessons (bit us during bring-up): limb packing must never route >64 bits through a uint64_t; reduction folds need their ×5/×38 factors on BOTH low and high parts of every folded digit, cascades included; `|` vs `+` breaks when limbs carry slack; wrap tests must check the top BIT, not the carry out (p-1 + 19 = exactly 2^255 → bit set, carry zero); count array initializers (a 31-byte exponent array silently zero-padded its top byte).

---

## TLS traps (next agent: read these!)

**Crypto / TLS math (all of these happened in this repo)**
- Limb packing: never route >64 bits through a uint64_t. Shifts ≥64 are UB.
- Limb reduction folds: the ×5/×38 factor applies to BOTH low and high parts of
  every folded digit; cascades included.
- `|` vs `+`: OR silently drops carries when limbs carry slack — always carry
  first, then pack additively.
- Wraparound test for 2^255: check the TOP BIT, not carry-out
  (p-1 + 19 = exactly 2^255 → bit set, carry zero).
- Array initializers: a 31-byte literal in a 32-byte array silently zero-pads.
  Count them or use designated initializers `[0]=.., [1...30]=..`.
- HKDF-Expand-Label prefixes labels with `"tls13 "` — forgetting it produces
  plausible-looking but wrong keys (no error anywhere).
- AEAD nonce = static_iv XOR sequence counter (do not append, do not hash).
  The64-bit counter is BIG-ENDIAN, left-padded to 12 bytes (high byte lands at
  nonce[4], NOT nonce[11]).
- TLS record plaintext ends with real content-type byte then zero padding. When
  scanning back for the type, skip zeros only.
- HKDF-Expand-Label info format is `out_len(2) || label_len(1) || "tls13 " +
  label || context_len(1) || context` — the 1-byte length on the label is the
  `<7..255>` opaque<…> vector encoding.
- Transcript hash is over handshake message header + body: `type(1) || len(3)
  || body`. NOT over the record layer's framing.

**TLS record layer (Phase 1)**
- The header parser should only check 5 bytes of header. The caller reads
  payload separately. An earlier `buf_len - 5 < plen` check turned out to be
  wrong — it's valid for `buf_len == 5` (header-only) with big `plen`.
- `tls_record_parse_header` rejects unknown type bytes and unsupported versions
  but accepts 0x0303 (TLS 1.2 legacy_version) and 0x0304 (TLS 1.3).

**Handshake framing (Phase 2)**
- `list_length` fields in extension bodies are NOT a redundant outer1-byte
  prefix — most extensions use a2-byte length prefix describing the bytes
  inside (inclusive of the list itself, NOT inclusive of the length bytes).
  Don't double-count.
- `uint16_t algs[] = { 0x0403, ... }; memcpy(buf, algs, n)` produces
  LITTLE-ENDIAN bytes (since x86). TLS writes every uint16 as
  BIG-ENDIAN on the wire. **Always use `put_u16()` to write integer
  fields, never `memcpy()` a host-byte-order array.**
- Session ID: TLS 1.3 REQUIRES empty session_id in CH for full TLS 1.3
  compliance, but sending 32 zero bytes works too (some servers prefer it).

**Python ssl compat (Phase 4)**
- Python ssl's `UNEXPECTED_MESSAGE` alert = wrong message ORDER. TLS 1.3 strict
  mode rejects ChangeCipherSpec BETWEEN ClientHello and ServerHello — put it
  AFTER the CH record, not before.
- Python ssl's `NO_SHARED_SIGNATURE_ALGORITHM` = our `signature_algorithms`
  doesn't intersect. Match openssl's exact 13 codes (ECDSA_SECP256R1_SHA256
  through RSA_PKCS1_SHA512).
- Python ssl keeps CH `0x000d` sig algs in the order they appear; offer
  enough algs to cover EC + RSA_PKCS1 + RSA_PSS (some lib combos reject
  PSS-only).
- Some Python ssl builds complain "bad key share" with secp256r1 included
  alongside x25519 — start out offering ONLY x25519 to keep it consistent.
- Python ssl's TLS 1.3 ENFORCES `legacy_compression_methods` to be exactly
  `{ 0x00 }`. Any other value triggers illegal_parameter.
- Supported_group structure: do NOT use the `<2..2^16-1>` outer length prefix
  of an early draft; the final RFC body is `{ uint16 length; NamedGroup[] }`.

**TLS 1.3 transcript hash on the wire (Phase 4 bug — FIXED)**
- `tls_build_client_hello` returns TOTAL bytes (4-byte handshake header + body).
- `tls_transcript_update_msg(t, type, body, body_len)` expects `body_len` to be
  the body bytes ONLY, NOT including the handshake header. The handshake header
  is added internally.
- If the driver passes the full ch_len (header + body) into transcript_update_msg,
  the transcript hash is computed over the right total bytes but the sequence of
  bytes hashed is `type || (bodylen+4) || <body-with-extra-bytes>`, which makes
  the digest mismatch on the server side. **Always pass body-only length.**
- Fix: pass `ch + 4, ch_len - 4` to all `tls_transcript_update_msg` calls.

**Kernel** (these are old but still relevant from prior agents)
- Heap is a bump allocator, 16MB. Nothing frees. Big static arrays go in
  BSS; check `_kernel_end` stays sane.
- e1000 RX release: `RDT = last processed index` — never index+1 (RDT==RDH =
  "ring full" = every packet dropped), never write RDH.
- `tcp_handle_packet` runs in IRQ context. It must not call anything that
  the main loop calls concurrently on the same buffer. Pattern used today:
  IRQ appends to a buffer; main loop only reads after the connection closes.
- `http_get` resets `http_done`/`http_response_len` at entry — a stale
  done-flag racing the parse block caused blank pages for hours.
- Makefile `%.o` depends on headers — never remove (stale objects with mixed
  metrics once garbled the whole screen).
- `section .note.GNU-stack` must be LAST in .asm files.
- Mouse is 3-byte PS/2 only. Scripted QEMU drags undershoot ~18px due to the
  4-sample smoothing — converge iteratively (screendump → locate → correct).

**Process**
- Serial log (`-serial file:`) is ground truth for network bugs. Grep it
  before theorizing. `[sh] exec:` / `[br] parse:` instrumentation lines
  already exist — extend that style.
- Screenshot verification: crop + 3x zoom before asking a vision model;
  count specific pixel colors.

---

## Files to Know

| File | Why it matters |
|------|---------------|
| `boot/start.asm` | Entry point — where everything begins |
| `boot/isr.asm` | Interrupt handlers — must match IDT setup |
| `src/desktop.c` | Desktop main loop — shell commands, icons, editor integration, cursor compositor (`desktop_paint_rect`) |
| `src/graphics.c` | Drawing primitives + framebuffer management + clip rectangle |
| `src/window.c` | Window manager + mouse driver + stateless cursor sprite (`mouse_paint_cursor`, `window_paint_region`) |
| `src/paging.c` | Page tables — required for framebuffer access |
| `src/memory.c` | Physical memory manager + heap |
| `src/filesystem.c` | In-memory virtual filesystem (files persist until reboot) |
| `src/editor.c` | Text editor — open/edit/save files in windows |
| `src/browser.c` | Web browser — URL bar, navigation, HTML rendering |
| `src/html.c` | HTML parser — strips HTTP headers, tokenizes tags |
| `src/net/e1000.c` | e1000 NIC driver — TX + RX |
| `src/net/network.c` | Full network stack — ARP, IP, ICMP, UDP, TCP, DNS, HTTP |
| `src/crypto/tls_client.c` | TLS 1.3 handshake driver (Phase 4 — complete, host-tested) |
| `src/crypto/tls_keysched.c` | RFC 8446 §7.1 — known-good, vector-validated |
| `src/crypto/tls_handshake.c` | ClientHello builder + parsers |
| `src/crypto/tls_record.c` | TLS record layer build/parse |
| `src/crypto/rand.c/.h` | ChaCha20 CPRNG for TLS key generation |
| `src/net/tls_net.c/.h` | HTTPS client wrapper (kernel integration) |
| `src/crypto/aead.c` | ChaCha20-Poly1305 AEAD (Phase 1–3 verified, decrypt path validated in test_tls_crypto) |
| `linker.ld` | Memory layout — kernel load address, symbols |
| `Makefile` | Build system — text vs desktop targets |
| `src/string.c/.h` | libc string funcs (memcpy/memset/strlen) — needed by freestanding crypto |
| `src/serial.c` | `serial_printf()` — minimal formatter for TLS debug logging |
| `src/crypto/tls_dbg.h` | Dual-mode logging: serial (KERNEL) vs fprintf (host) |

---

## Critical Bugs Found (and Fixed)

1. **`section .note.GNU-stack` placement**: Must be LAST in .asm files
2. **Compiler flags**: `-fno-pic -fno-pie -mno-red-zone` required for freestanding kernel
3. **Font bit order**: 8x8 font uses LSB-first (bit 0 = leftmost pixel)
4. **Framebuffer access**: Above 4MB, requires identity-mapped page tables
5. **e1000 register offsets**: RDH/RDT/TDH/TDT are at 0x02810/0x02818/0x03810/0x03818 (not 0x0281/0x0282)
6. **e1000 RX buffers**: Must be in low memory (<1MB) for DMA access
7. **16-bit MMIO registers**: Use 16-bit writes for RDH/RDT/TDH/TDT to avoid corrupting adjacent registers
8. **Cursor save/restore causes artifacts**: saved background patches go stale when the scene changes under them and tear when IRQ12 lands mid-save. Fixed by stateless sprite + scene repair (`desktop_paint_rect`) — see Cursor Compositor above. Do not reintroduce `cursor_bg`.
9. **Unclipped repair painting kills FPS**: repainting a window's whole frame for a 12x16 cursor repair dirties every row the window spans (~75% of screen per frame, FPS 1030→311). Fixed with the graphics clip rectangle — any new repair path must set/reset it around its drawing.
10. **uint16 array memcpy = little-endian on the wire**: TLS writes uint16
    fields BIG-ENDIAN. Always use `put_u16()`. (Phase 4 bug — caught early
    via Python ssl decoding CH with illegal_parameter.)
11. **TLS extension list_length ≠ redundant outer 1-byte prefix**: Most
    extensions use a single 2-byte length prefix; do not double-count.
    (Phase 4 bug — supported_groups was using an obsolete draft format.)
12. **HKDF-Expand-Label needs `"tls13 "` prefix AND a 1-byte label length**:
    The `<7..255>` opaque<…> vector encoding puts `label_len` before the label
    bytes. Without this, every secret deriver computes plausible-looking but
    wrong bytes. (Phase 3 bug.)
13. **TLS record parse_header trunc-check was wrong**: when caller passes
    `buf_len == 5` (header-only), the check `buf_len - 5 < plen` reads
    `0 < plen`, rejecting every valid header. Caller reads payload
    separately. (Phase 1 bug.)
14. **TLS 1.3 transcript hash body-length mismatch** (Phase 4 bug — FIXED):
    `tls_build_client_hello` returns TOTAL bytes (header + body). Pass body
    length minus 4 into `tls_transcript_update_msg` — the function adds the
    4-byte handshake header itself.
15. **TLS Certificate parser missing context_len** (Phase 4 bug — FIXED):
    TLS 1.3 Certificate messages have `cert_request_context<0..2^8-1>` before
    the 3-byte `certificate_list` length. The old parser read bytes 0–2 as the
    list length, but byte 0 is actually the context length (0 for server certs).
    Fix: skip `cert[0]` context bytes, then read list length from offset
    `1 + ctx_len`.
16. **send_aead missing tag append** (Phase 4 bug — FIXED):
    `aead_chacha20_poly1305_encrypt` writes ciphertext to `out` and tag to a
    separate `tag[16]` array. The old `send_aead` sent `ct_len` bytes from
    the ciphertext buffer but never appended the 16-byte tag — the last 16
    bytes were stack garbage. Fix: `memcpy(ct + pt_len + 1, tag, 16)` after
    encryption.
17. **Finished key derived from wrong secret** (Phase 4 bug — FIXED):
    RFC 8446 §4.4.4: client Finished key is derived from the handshake traffic
    secret (`c_hs`), NOT the application traffic secret (`c_ap`). The old code
    called `tls_finished_key(c_ap, c_fin_key)` — the server couldn't verify
    our MAC. Fix: `tls_finished_key(c_hs, c_fin_key)`.
18. **Shared app traffic sequence counter** (Phase 4 bug — FIXED):
    Client and server application traffic use independent sequence numbers.
    The old code shared `app_seq` between send and receive — after sending the
    HTTP request (seq→1), the receive tried seq=1 instead of seq=0. Fix:
    separate `c_ap_seq` and `s_ap_seq` starting at 0.
19. **TCP payload length from padded frame, not IP total_length** (Phase 6 bug — FIXED):
    `handle_packet` (TCP branch, `src/net/network.c`) computed `tcp_len =
    len - eth - ip` from the raw Ethernet frame length. Ethernet pads short
    frames to the 64-byte minimum, so a 6-byte TLS record-header segment
    arrived as a 64-byte frame (40-byte IP datagram + 8 padding) and the stack
    read the 6 trailing zero-pad bytes as payload — feeding zeros to TLS and
    advancing RCV.NXT by the pad count, which garbled the handshake. Fix: derive
    `tcp_len` from `ip_header.total_length` (ntohs) and start the TCP header at
    the IP IHL offset. UDP/DNS were unaffected (they use the UDP length field);
    HTTP had dodged this because servers send MSS-sized segments with no padding.
 20. **TLS buffers too small for real pages (truncated HTTPS)** (FIXED):
    `src/net/tls_net.c` had `TLS_RX_BUF_SIZE` and `tls_response` both at 16KB.
    `e1000_poll()` appends every pending RX descriptor to the ring in one call, so
    a ~28KB page (e.g. `notdexy.ru`, Content-Length 28262) can land in the ring
    before the handshake driver reads it; the 16KB ring filled and `tls_append_data`
    silently dropped bytes, corrupting a record mid-stream (saw `payload read=2026/7133`,
    received only 14243/28879 bytes → blank/partial page). The 16KB `tls_response`
    cap would also have failed past 16KB. Fix: both buffers enlarged to 64KB.
    `example.com` (865B) is unaffected; `notdexy.ru` now fetches the full 28879
    bytes and the browser parses the page. Larger-than-64KB pages still truncate
    (would need a streaming renderer, not a single static buffer).

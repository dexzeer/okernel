# okernel — Handoff Document

## What is this?

okernel is a from-scratch operating system built in C and x86 assembly. It has two modes:

- **Text mode** (`make text`) — VGA text terminal with commands, scrolling, terminal multiplexing
- **Desktop mode** (`make desktop`) — 1024x768x32bpp graphical desktop with windows, mouse, okai, editor, and shell

Current version: **v0.4** (desktop edition: 1024x768x32bpp, working okai with resizable windows)

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
│   ├── okai.c/.h       # Web okai (URL bar, navigation, HTML rendering)
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

### Okai (okai)
- **HTML parser** (`html.c`): Strips HTTP headers (`\r\n\r\n`), skips `<head>`, `<script>`, `<style>` blocks, tokenizes h1-h6, p, a, li, pre, br, hr, text
- **Okai window** (`okai.c`): Address bar (g to focus, Esc to exit), toolbar, scrollable content
- **Scrolling** (`okai.c`): LINE-based via a virtual document — `okai_render_content` lays the WHOLE page out once into a static offscreen grid (`doc_chars`/`doc_attrs`, 1024 lines × 128 cols, shared scratch — only live during render), clamps `scroll_y` to `[0, doc_lines - view_h]`, then blits the visible slice into the window buffer with `window_write_cell`. `content_height` is the true full-page height measured by this layout pass. Do NOT go back to rendering straight into the window with a token-granular `skip`: that re-measured content_height from the scrolled render (bound shrank ~2 lines per 1 line scrolled → j/k stalled) and skipped 2-3 lines per step. `HTML_END_PARA` (`</p>/</div>/</li>` closes) terminates the current line in the layout pass — without it blocks jam together and pages are too short to scroll. Links are recorded in doc rows, then converted to BUFFER rows (`doc_row - scroll_y + 2`; the +2 is toolbar/address bar) after the clamp; off-screen links are invalidated (-1) so they never match clicks.
- **Keyboard**: g=address bar, j/k=scroll, r=refresh, b=back
- **Navigation**: Enter to go, back button with 4-page history
- **Rendering**: CSS-styled text — headings, paragraphs, links, lists, preformatted blocks. A from-scratch CSS engine (`src/css.c`) applies `color`/`background`/`text-align`/`display:none`/`margin` from inline `style=` and `<style>` blocks.
- **Body inheritance**: `<body>`-level styles (`text-align`, `color`, …) are folded into every element via `css_merge_base` so `body{text-align:center}` actually centers content (the flat tokenizer has no DOM hierarchy). The body's `background-color` is the **page background** (painted across the whole content area via `window_set_content_bg`), NOT per-character — and a readable default text color is chosen (black on light pages, white on dark) so text never drowns in white. `render_content` sets the page bg *before* `window_clear` so blank cells carry it.
- **Clickable links**: each `<a href>` is rendered on its own line in blue, recorded as a (row,col,href) region in `struct okai.links[]`, and a desktop mouse-click in that region calls `okai_navigate`. Relative hrefs (`/path`, `//host`, `page.html`) are resolved against the current URL scheme+host by `resolve_href`.
- **HTTP integration**: Parses response after connection closes (`http_done` flag), accumulates TCP segments correctly

---

## Browser visual polish & toolbar navigation (2026-08 sessions)

Goal: make **okai** look and behave like a real browser (visual polish first,
then behavior), verified by screenshotting in QEMU and analyzing with the
`opencode-go/mimo-v2.5` vision model (the base model has no image vision — route
the PNG path into a vision subagent, do NOT eyeball pixels yourself).

**Project path (this session):** all builds ran from
`/media/notdexy/Новый том/projects/okernel` (the non-ASCII path) and worked.
MEMORY.md's claim that the project "moved" to an ASCII path is STALE — verify
with `ls` before trusting either path; they may be the same bind-mount.

### What shipped
- **Phase 0–1 — Firefox-ish blue pixel chrome** (`gradient_fill` primitive +
  `theme.h` palette + `okai_draw_chrome`): tab strip, toolbar, primitive bitmap
  nav-icon buttons (back/forward/reload/home), HTTPS lock, rounded active tab,
  boxed `+` new-tab button, top-right `×` close. Drawn as **pixels** over the
  window's top reserved rows (the window interior is a fixed character-cell grid;
  chrome can't be restyled text — it must be a pixel overlay). `no_titlebar` window
  mode added so the Win95 title bar is suppressed and the browser owns its top.
- **Phase 3 — page CSS**: `OKAI_TEXT_PAD` page margins (content inset, both
  gutters blank), bracketless **BLUE** links (hitbox = text span), H1 extra
  spacing, HR/gutter fixes.
- **Heading SIZE (the #1 visual gap) — DONE, vision-verified:** the window
  interior is a uniform `CHAR_W×CHAR_H` cell grid, so headings can't be bigger
  there. Fix = **pixel-overlay per heading line**: `doc_draw_heading()` places
  each heading char every 2 grid columns and flags the line via `doc_line_kind[]`;
  the blit blanks those grid cells (writes spaces) and
  `okai_draw_heading_pixels()` (called from `okai_draw_chrome_all`, after
  `window_draw_all`) paints the heading text at `scale = 2*font_scale` → exactly
  2× the body glyph (32×64 px), matching page fg/bg. `window.c`'s `vga_to_rgb[16]`
  was made non-static + `extern`-ed in `window.h` so heading colors match the body
  exactly. Links are unaffected (headings aren't links). Vision confirmed ~2×,
  no ghosting/clipping, reasonable spacing. **All of H1–H6 currently render at
  2×** (no hierarchy yet — see Next Steps).
- **Toolbar nav buttons are now CLICKABLE** (were visual-only): `okai_check_nav_click()`
  (geometry mirrors `okai_draw_chrome` exactly — same `cx0/by/btn/gap`) routed in
  `desktop.c`'s mouse handler *before* the title-drag / link branches; actions
  `okai_nav_back/fwd/reload/home` call into the existing history/navigate logic.
  `okai.h` gained the `NAV_*` constants + the four nav function decls.
- **FPS HUD hidden** (`static int g_show_fps = 0` in `desktop.c`).
- Test: `tests/headless/test_nav.py` (uses `OkVM`, mouse gain ≈4.2 → divide
  deltas by 4.2 to converge without overshoot). Verification confirmed the
  hit-test geometry is correct (a click at x≈99 logged `nav action=2` = FWD,
  exactly matching the FWD rect `[74,100]`). The full 4-button sweep PASS was
  being finalized at handoff — **run it to confirm all four light up**.

### Known remaining visual gaps
- Desktop/terminal/editor/sysinfo windows STILL use the Win95 blue title bar
  (Phase 4 only unified the **browser** window). The OS looks like Win95 next to
  a modern browser.
- Nav icons are primitive geometric shapes (no real back-arrow / reload-circle).
- Font is monospace bitmap (inherent from-scratch constraint — no Unicode/TTF).
- Headings are all 2× (no H1>H2>H3 hierarchy).

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
| 6 QEMU verification | (not started) | **PASS** — verified in-kernel against example.com (Cloudflare) via QEMU SLIRP. Full DNS→TCP→TLS 1.3 handshake completes; 865-byte HTTP response decrypted and okai-parsed (7 HTML tokens). Driver: `test_net.py` pattern, command `okai https://example.com/`. **One bug fixed:** TCP segment length was taken from the padded Ethernet frame length instead of the IP `total_length`, so Ethernet min-frame padding (zeros) was fed to TLS as payload and RCV.NXT advanced by the pad count — breaking the handshake. Fixed in `src/net/network.c` (TCP branch of `handle_packet`). |

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
5. **Okai HTTPS** — `okai https://host/path` is detected in
   `parse_url` + `okai_open`/`navigate`/`back`; `https_get()` is called
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
`okai https://example.com/` into the focused terminal and press Enter.
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

## CSS engine (from scratch) — v0.5

A from-scratch CSS implementation — no tinyjs, no external CSS library, preserving
the project's from-scratch identity.

**Files:** `src/css.c/.h` (engine), `src/html.c` (attribute capture + `<style>`
extraction), `src/okai.c` (computed-style application). `src/css.o` added to
`DESKTOP_OBJ`; `strncpy` added to `src/string.c` (freestanding libc lacked it).

**Capabilities:**
- Parses `<style>` blocks (`html_extract_css`) and inline `style=` attributes.
- Selectors: `tag`, `.class`, `#id`, and compounds `tag.class` / `tag#id`.
  (No descendant combinators, no attribute selectors.)
- Cascade by specificity (`id·100 + class·10 + tag`) then source order; inline
  `style=` overrides everything.
- Properties: `color`, `background`/`background-color`, `font-size` (px),
  `font-weight:bold`, `text-align`, `margin`/`-top`/`-bottom` (shorthand →
  top+bottom), `display` (block/inline/none).
- Colors: `#rgb`/`#rrggbb` + a small named set, quantized to the 16-color VGA
  text palette (`window_set_text_color` fg/bg 0-15).
- `html_token` now carries `tag`/`cls`/`id`/`style`; `okai.c` calls
  `css_compute()` per token and applies fg/bg, `display:none` (skip), margins
  (blank lines), `text-align` (center/right padding). Links keep cyan unless CSS
  overrides the color.

**Verification:**
- Host unit test `tests/test_css.c` — 24 assertions, all PASS (parser, cascade,
  inline override, color quantization, `<style>` extraction).
- QEMU smoke (`/tmp/ok/css_smoke.py`): `example.com`'s inline `<style>`
  (`body{background:#eee;...}a:link,a:visited{color:#348...}`) extracted, 5 rules
  parsed, page rendered, no crash. `notdexy.ru` uses external stylesheets → 0
  inline rules (expected; external CSS is deferred).

**Deferred (future work):** external `<link rel=stylesheet>` (2nd TLS GET to the
host); descendant combinators `a b` (needs a parent/ancestor tree; the token
model is flat); full box model (padding/border/width affecting flow);
`font-size`/`font-weight` visual effects (fixed-cell text grid today);
pseudo-classes beyond tag-degradation (`:hover`, `:nth-child`; `a:link`/`:visited`
currently collapse to tag `a`).

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
| okai [url] | Open web okai (e.g. `okai http://example.com/`) |
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
- **HTTPS works** via the okai (`okai https://host/path`); it is a
  blocking single-call fetch (UI freezes ~1-2s during the handshake). No
  standalone `https` shell command yet (only the okai path is wired).
- **TLS 1.3 limited to ChaCha20-Poly1305** — SHA-256 only; no AES-GCM, no SHA-384. Phase 4 + 5 complete.
- **CSS is from-scratch and scoped** — selectors limited to `tag`/`.class`/`#id` (+ compounds); no descendant combinators, external stylesheets, box model, or most pseudo-classes. Colors quantized to the 16-color text palette, so gradients/alpha/`rgb()` etc. are unsupported.
- **TCP minimal** — single connection, no windowing/congestion control, no out-of-order buffering (gaps re-ACKed until the server fills them). Retransmission IS implemented (timer + backoff + give-up). This is the foundation for the planned TLS 1.3 client.

---

## Network Stack Details

### e1000 Driver
- MMIO-based, supports TX and RX
- RX/TX buffers at fixed low memory addresses (0x80000-0x9FFFF)
- Descriptors at 0x80000 (RX) and 0x90000 (TX)
- **RX ring release: software writes `RDT = index of LAST processed descriptor`** (never index+1, never RDH — RDT==RDH reads as "ring full" and hardware silently drops every packet; writing RDH never replenishes the ring and it exhausts after ~15 packets). Both wrong variants caused stalled/blank okai fetches.
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
| `src/okai.c` | Web okai — URL bar, navigation, HTML rendering |
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
    bytes and the okai parses the page. Larger-than-64KB pages still truncate
    (would need a streaming renderer, not a single static buffer).
 21. **Stale-tail host buffer → 2nd okai fetch stalls in HP_DNS** (FIXED — the
    "clicked link opens a black window" bug): every host/path copy used
    `for(...) dst[i]=src[i]; dst[127]=0;` — no NUL at the ACTUAL length. After
    fetching `example.com`, `dns_resolved_host` still held `...com`, so fetching
    `iana.org` left `iana.orgcom`; `dns_host_matches()` then failed forever, the
    TLS fetch stalled in HP_DNS until the 66s timeout, and the new okai window
    rendered pure black (WIN_BG). Fix: `net_copy_str()` (always NUL at actual
    length) at all copy sites in `network.c` (`dns_resolved_host`,
    `dns_pending_host`, `http_pending_host`, `http_pending_path`).
 22. **DNS definitive failure wedged the fetch owner forever** (FIXED): an
    NXDOMAIN (rcode 3) or a response with no A record cleared `dns_pending` but
    left `http_retry_pending=1` forever — the okai HTTP give-up condition
    (`!http_is_retry_pending()`) could never fire, so `okai_fetch_owner` wedged
    and EVERY later okai window stayed black. Fix: `dns_fail_pending_http()`
    aborts the parked HTTP request on definitive DNS failure (tracked via
    `dns_query_host`); `https_get_poll` HP_DNS aborts when `!dns_is_pending()`
    and DNS didn't resolve; `okai_start_fetch` refuses empty-host URLs outright
    (returns -1; desktop claims fetch ownership only when a fetch really fired).
    Failed fetches now render a readable "Unable to load page" error instead of
    a black screen, and the owner is always released.
 23. **google.com garbled mess** (FIXED, three parts): (a) HTTP response buffer
    was 64KB; google serves ~85KB chunked, truncating mid-script so the parser
    emitted only ~21 tokens (header links) — buffer now 128KB (~45 tokens,
    footer links included). (b) `CSS_MAX_RULES` was 48; google's ~200 obfuscated
    `.gb_*` widget rules filled the table before `body{background:#fff;...}`
    was ever reached, so the page rendered default-black — cap now 256 (~730KB
    more BSS; kernel_end ~6.3MB, still inside the 4MB-128MB identity map; the
    heap auto-follows `_kernel_end`, so no overlap). (c) HTML entities were
    never actually decoded anywhere: the plain-text path matched named entities
    with `tag_match()`, whose terminator check requires space/`>`/`/` — an
    entity's `;` never matched. New shared `html_decode_entities()` (numeric +
    18 named entities; non-ASCII codepoints → `?`) applied to h1-h6/li/pre/
    title/`<a>` text AND href; okai render paths also clamp high bytes → `?`
    (the 8x16 font is ASCII-only, so Cyrillic renders as `?` placeholders —
    readable, not entity soup). `&amp;` in hrefs now decodes before navigation.
 24. **Serial log formatters overflowed silently** (FIXED): `css rules` printed
    2 digits (256 → "I6"), `[http] segment, total` printed 4 digits (86KB
    wrapped to "6729" — actively misleading during debugging); both replaced
    with `serial_printf`. The full `css text` serial dump (50KB+ on real pages)
    is trimmed to a 200-char preview. New serial instrumentation: `[okai] link[i]
    row= col0= col1= href=`, `[okai] click row= col= (mx= my=) links=`,
    `[okai] LINK HIT li= -> href` — exact ground truth for headless link tests.
 25. **IntelliMouse ID read drained the byte it was waiting for** (FIXED — the
    "nav test clicks stop registering" bug): after the 200/100/80 magic +
    `0xF2`, `mouse_init_fb` drained the output buffer (consuming ACK and often
    the ID) and then read a not-yet-arrived byte. QEMU's mouse had already
    switched to 4-byte packets while `mouse_has_wheel` stayed 0 → PS/2 stream
    desync shortly into the session: movement died, only lucky button-byte
    resyncs logged. Fix: wait for ACK, then wait for ID (bounded spins).
    Detection now logs `[mse] wheel detect id=0x03 4-byte mode ON`.

---

## Session 2026-08-21 — nav test PASS + OS-wide theming + heading hierarchy

**All four prioritized visual-polish items from the previous handoff are DONE
and vision-verified** (`opencode-go/mimo-v2.5` subagent read the PNGs; overall
verdict PASS).

### 1. Nav test — `tests/headless/test_nav.py` PASSES (4/4 buttons)
Two root causes fixed to get there:

- **Kernel bug — IntelliMouse handshake read the device ID backwards**
  (`window.c` `mouse_init_fb`). After sending the 200/100/80 sample-rate magic
  + `0xF2` (get ID), the old code DRAINED the output buffer (eating the ACK
  and often the ID byte) and then read a byte that hadn't arrived. QEMU's
  mouse had already switched to 4-byte packets while `mouse_has_wheel`
  stayed 0 → permanent PS/2 stream desync a few packets into the session:
  movement died mid-test (4 clicks registered out of ~176 issued). Fixed by
  WAITING for the ACK, then WAITING for the ID (bounded spin loops); the
  detect log `[mse] wheel detect id=0x03 4-byte mode ON` now confirms correct
  detection, and a probe (`burst`+click rounds) shows packets flowing and
  every click registering. NOTE: AGENTS.md's "don't add a 4th byte read"
  warning was about doing the handshake WRONG — with a correct handshake the
  4-byte wheel mode works in QEMU (scroll is wired via `mouse_get_scroll`).
- **Test bug — settle feedback was blind outside browser content.**
  `settle_to` only got position feedback from `[okai] click row=...` lines,
  which the kernel logs ONLY for clicks inside okai content. One overshoot
  past the window edge → no feedback → stale-position corrections pinned the
  cursor at the screen edge forever (x=0, marching down). The test now
  converges on the new `[mse] btn=1 x= y=` lines (logged for EVERY button
  press anywhere). Measured mid-flight mouse gain ≈ 4.8 px/burst-unit
  (quiescent probe said 4.4; old assumption 4.2 undershot).

**New kernel instrumentation (keep):** `[mse] btn=<0-7> x= y= pkts=` on every
button transition, `[mse] wheel detect ...` at boot — exact ground truth for
any headless mouse work; desync shows up as the packet counter stalling.

**Host gotcha that bit this session:** `/tmp` on this machine is aggressively
cleaned — it wiped `/tmp/ok/*` (logs AND scripts) MID-RUN, killing a test.
`okvm.py` `OUTDIR` is now `~/okvm` (durable). `shot_chrome.py` writes
`~/okvm/chrome.png`. Keep test artifacts out of `/tmp`.

### 2. Phase 4 theming — DONE (title bars, buttons, borders, taskbar)
The Win95 chrome is gone; the whole OS now speaks the browser's Firefox-blue
language (`theme.h`):
- **Title bars** (`window_paint_region` in `window.c`): focused = the browser
  toolbar gradient (CHROME_TOOL_TOP→BOT), unfocused = desaturated gray-blue;
  1px TITLE_DIVIDER under the bar; title text via new
  `draw_string_fg()` (graphics.c — fg-only glyphs, no glyph-cell background
  patch on gradients).
- **Buttons**: muted-red rounded close with a 2px pixel-drawn ×, slate
  rounded minimize with a white dash (`round_fill()` helper draws rounded
  rects by skipping corner pixels so the gradient shows through). Hit rects
  unchanged (geometry preserved).
- **Borders**: focused BORDER_ACTIVE (navy) / inactive BORDER_INACTIVE
  (gray) RGB colors; old VGA-index WIN_TITLE_BG/FG, WIN_ACTIVE_BORDER,
  WIN_BORDER_BG defines removed from window.h.
- **Taskbar** (`window_draw_taskbar`): navy gradient + divider, rounded
  buttons — active = CHROME_TAB_ACTIVE with dark text, inactive =
  TASK_BTN_INACT with white text. Desktop.c's taskbar click hit-testing
  unchanged.

### 3. Nav icons — DONE
`okai_icon_back/fwd` are now proper arrows (3px shaft + 2px-thick chevron
head) instead of solid triangles; reload ring is 2px thick with a 2px
arrowhead; home is an outlined house with a filled door. Vision-verified:
all four "clearly recognizable", correct directions, no clipping.

### 4. Heading hierarchy — DONE
H1 = 3× body (3 grid cols per glyph, 2 spacer rows), H2 = 2× (unchanged),
H3–H6 = body size via `doc_draw_block_text` (plain grid text, keeps CSS
color). `doc_line_kind[]` codes: 0=normal, 1=2×, 2=3×; the pixel overlay
(`okai_draw_heading_pixels`) derives rows/scale from the kind
(`glyph_rows = kind+1`, `hscale = (kind+1)*font_scale`). Vision-verified:
"Example Domain" ≈ 2.5–3× body height, clean, no overlap.
CAREFUL: the overlay scans every column (not every hc-th) because centered
headings start at an arbitrary pad offset.

### 5. UX fixes from a user-pasted screenshot (post-vision session)
The user pasted a live screenshot showing two annoyances; both fixed and
verified by `tests/headless/test_addrbar.py` (PASS):

- **Terminal fetch-log flood** (`network.c`): every HTTP fetch echoed a
  180-byte raw response-header dump ("HTTP/1.1 200 OK / Date: / Server: …")
  into the terminal — repeated fetches buried the shell. Now the terminal
  event carries ONLY the status line + " — receiving..." (full headers stay
  in the serial log where debugging belongs).
- **URL bar wiped by stray chrome clicks** (`desktop.c` + new
  `okai_addr_bar_hit()` in `okai.c`): ANY click in the top band used to
  focus + clear the address bar, so a click that missed a nav button
  blanked the URL display (and stray keystrokes like 'g' landed in it).
  Now only a click ON the address bar rect focuses it (same state as 'g',
  non-destructive if already focused); other top-band clicks just drag the
  window. Hit geometry mirrors `okai_draw_chrome`.

**Image-analysis lesson learned:** for "what does this pixel look like"
questions, combine the vision tool with PIXEL FORENSICS — load the PNG with
PIL and compare against the theme's exact RGB constants (e.g.
CHROME_TOOL_TOP 0x6E92C8, TAB_ACTIVE 0xEAF1FB). The vision tool alone
misread tiny 26px-tall title bars as "flat Win95 with 3 buttons" on a
1649×711 capture of the NEW build; the pixel scan proved the gradient and
theme colors were present. Vision = layout/legibility questions; pixels +
serial = ground truth for colors and geometry.

### 6. Browser chrome overhaul (user-reported: "nothing is fixed")
The user's follow-up screenshot flagged four REAL pre-existing chrome bugs
(none touched by items 2–4 above — those were the OS title bar/taskbar):
tab labels half-cut, oversized URL text, terminal overlapping the browser,
bare icon glyphs. All fixed, verified by pixel checks + vision + both
regressions (test_addrbar PASS, test_nav PASS 4/4):

- **Root cause of both text bugs:** chrome text used the 16×32 body font
  inside 18–22px bands; the toolbar gradient painted after covered the
  glyph bottoms. New `draw_char_1x`/`draw_string_1x` (graphics.c — 8×16 px,
  no FONT_SCALE doubling) is the chrome font. Chrome bands grew:
  CHROME_TAB_H 18→22, CHROME_TOOL_H 30→36 (58 total ≤ 96 reserved by
  CHROME_ROWS=3, so no content-grid change). Tab label, "+", and URL are 1×
  and CLAMPED to their bar/tab width (no overflow onto neighbors).
- **Nav buttons are proper chips now:** `round_rect_fill` (public in
  graphics.c; window.c's static helper removed) draws a rounded NAVBTN_BG
  chip + NAVBTN_HI top highlight behind each icon; btn 26→30. Geometry
  mirrored in okai_draw_chrome, okai_check_nav_click, okai_addr_bar_hit —
  keep all three in sync.
- **No more terminal/browser overlap:** the okai window opens at
  (1010, 60, 880, 650), right of the default terminal (80,60,900×650).
- **Test updates:** test_nav TARGETS = 1035/1073/1111/1149 (y band 87..117),
  CONTENT_Y=300; test_addrbar scans y 60..130. NOTE: okvm.py's
  `click_link` content-origin constants (ox,oy=32,62) are STALE for the new
  browser position — update before using test_link_click.py again.

### 7. Terminal mouse-wheel scrollback (like the browser)
Terminals now scroll with the wheel, using the same routing model as the
okai (`desktop.c` main loop: `mouse_get_scroll()` → window under cursor →
browser `okai_handle_mouse_scroll` else terminal `window_scroll_view`).

- **Design** (`window.c`): a per-window scrollback RING (`sb_ring`, 256
  lines × CONTENT_COLS_MAX, ~490KB BSS for 8 windows) archives every line
  pushed off the content grid by `scroll_content`. The wheel moves a VIEW
  offset (`struct window.scroll_off`) — the live grid is never rewritten;
  `window_paint_region` serves the top `scroll_off` rows from the ring and
  shifts live rows down by `scroll_off`. 3 lines per detent, clamped to
  [0, archived]. Blinking cursor is HIDDEN while scrolled back.
- **Auto-follow:** any grid scroll resets `scroll_off = 0` (new output
  snaps to the tail). `window_clear` wipes the ring (the `clear` command
  clears history too); create/destroy/resize reset the state.
- **Wheel sign (CORRECTED after user report):** the real input path
  (QEMU GUI frontend → PS/2) sends Z NEGATIVE for wheel-up; the ISR
  accumulates it raw (`mouse_scroll += z`) and consumers keep the
  "positive = down/toward the tail" convention (browser + terminal
  consistently). **HMP `mouse_move dz` is sign-flipped vs the GUI path:**
  scripts must inject dz=+1 for wheel-UP and dz=-1 for wheel-DOWN.
  (An earlier calibration against HMP only had this backwards.)
- **Verified in QEMU** (`[scr] win= off= hist=` serial ground truth +
  pixel diffs): wheel-up off→17 (clamped at hist=17), view changes;
  wheel-down returns to the exact bottom (pixel diff 0); output after
  scrolling back auto-follows (probe wheel-down emits no new [scr]).
  QEMU 8.2.2 HMP: `mouse_move dx dy [dz]` — dz works.

### 8. okai text-engine upgrade: Cyrillic, structure, tables (v0.6-grade)
Goal: "make okai actually good." This round attacked text quality:

- **Cyrillic + symbol glyphs** (`graphics.c`): `font8x16_ext[0x80][16]`
  generated from the CyrSlav-Terminus16 PSF console font (bitmap data).
  Byte slots: 0x80-0xBF = U+0410..U+044F (А-я), 0xC0+ = Ё ё — – « » • № …
  ° € © ® ™ ─ │. All glyph renderers (draw_char / _scaled / _1x /
  draw_string_fg) go through one `glyph_rows()` helper that handles slots.
  PSF extraction gotcha: kbd PSF1 unicode tables are UCS-2 **little-endian**
  with 0xFFFF separators; keep-first-mapping-per-glyph loses secondary cps —
  parse the FULL table.
- **Charset decoding** (`html.c`): `html_decode_entities` now = charset pass
  THEN entity pass (order matters: entities produce slot bytes that must not
  be re-decoded). UTF-8 (2-3 byte) and windows-1251 (sniffed via
  `charset=...1251` in the first 2KB — matches Content-Type and meta) both
  collapse to slots via `html_map_cp`. Numeric entities are decimal AND hex
  (`&#x27;` — Next.js emits hex!). **map_cp maps control bytes to SPACE** —
  the first version mapped them to '?', putting a stray `?` before every
  block (inter-tag whitespace is real text here).
- **Structure rendering**: `<ul>` li → "• " prefix; `<ol>` li → "N. " (state
  tracked in the tokenizer, prefix inserted AFTER decode); `<hr>` → full
 -width ── rule; `<blockquote>` → separation break; `<img>` → "[image: alt]"
  text token; `<td>/<th>` → HTML_TABLE_CELL tokens, cells flow on one line
  joined by │ and `</tr>` breaks the row (END_PARA). `</ul>/</ol>/</tr>`
  close blocks.
- **Numeric-IP + port URLs**: `http://10.0.2.2:8000/x` works —
  `parse_url` captures `:port`, `http_get_port()` threads it through the
  retry machinery (`http_pending_port`), and `net_parse_ip` + `dns_seed`
  skip DNS for literal addresses. THIS ENABLES HOST-SERVED TEST PAGES:
  `python3 -m http.server 8000` in a dir, then okai
  `http://10.0.2.2:8000/page.html` — deterministic page fixtures for
  rendering tests (see /tmp/www/test.html pattern; re-create as needed).
- **Host test**: `tests/test_text_decode.c` (20 assertions — UTF-8, CP1251,
  decimal+hex entities, markers, img, tables, hr/blockquote) —
  `cd tests && gcc -DKERNEL=0 -I../src -include string.h test_text_decode.c
  ../src/html.c -o t && ./t`. Slot arithmetic for expectations:
  slot = 0x80 + (cp - 0x410) for Cyrillic.
- **Verified**: host test PASS; QEMU end-to-end on a served Russian page —
  H1/H2/H3 hierarchy in Cyrillic, «»—… in paragraphs, • and 1./2. lists, ──
  rule, `Имя │ Значение` table, blockquote with DECODED entities,
  [image: логотип]; vision-confirmed readable; example.com regression OK.
- **Deferred**: async HTTPS (UI still freezes ~1-2s per fetch), external
  CSS, inline images, KOI8-R (rare), CJK (no glyphs — falls to '?').

### 9. Host preview: see okai's rendering WITHOUT booting the kernel
`./okai-preview <url-or-file> [out.png] [cols]` renders any page through the
REAL okai sources on the host in ~50ms: tokenizer + charset/entities, CSS
engine, and document layout are compiled directly from src (html.c, css.c,
okai.c) with `-DHOST_PREVIEW` accessors (end of okai.c: doc grid exposure)
plus kernel-service stubs in `tests/okai_preview.c`. Output is a PNG painted
with the real font (tests/font_data.h, regenerated from graphics.c by
`make -C tests -f Makefile.preview font_data.h`). Identical output to the
kernel minus pixel chrome — THE fast loop for text/CSS/layout work; use
QEMU only for chrome/input/fetch behavior. Build:
`make -C tests -f Makefile.preview okai_preview`.
NOTE: token counts from the preview match the kernel exactly (43/7/92 on
the fixtures) — good quick-fidelity check.

### 10. Mouse-interaction fixes: link clicks, new tab, real Home
All three user-reported, all fixed, gated by `tests/headless/test_links.py`
(PASS: LINK / NEWTAB / HOME):

- **Link clicks were dead**: the desktop click branch computed the content
  row from `y + WIN_BORDER + WIN_TITLE_H` unconditionally — but a
  `no_titlebar` okai's grid starts at `y + WIN_BORDER` (mirrors
  `window_paint_region`'s `title_off`). Every click mapped ~1.25 rows below
  its target, so links never matched. Fixed with `grid_top` (the
  no_titlebar-aware offset). NOTE the serial `[okai] link[i] row=` lines log
  DOC rows; the hit-test uses BUFFER rows (= doc + CHROME_ROWS). okvm.py's
  `click_link` was updated to the current geometry (window 1010,60; +3 rows).
- **'+' new tab works**: `okai_check_nav_click` returns NAV_NEWTAB (5) for
  the tab-strip '+' rect (mirrors the draw, padded 4px); desktop.c routes it
  to `okai_open(OKAI_HOME_URL)`. Successive okai windows now CASCADE
  (`1010 - 30*id, 60 + 45*id`) instead of stacking exactly.
- **Home is a real home**: `okai_nav_home` was `goto_history(0)` — i.e. "go
  back to the first site this session" (the user-visible "home = back"
  bug). Now `okai_navigate(id, OKAI_HOME_URL)` ("http://example.com/" —
  change OKAI_HOME_URL in okai.h), which pushes history like a real browser.

### 11. Tabs, internal homepage, one window, z-order leak fix
The browser became a real tabbed browser (user request). All gated by
`tests/headless/test_links.py` (PASS: HOMEPAGE / LINK IN-PLACE / NEWTAB /
TAB SWITCH / CHROME-BOUNDS) + test_addrbar PASS + test_nav PASS 4/4.

- **Architecture**: `struct okai_tab` (url, title, history, scroll,
  tokens[OKAI_TAB_TOKENS=384], css, links) — one okai WINDOW holds
  `tabs[OKAI_MAX_TABS=4]` + `active_tab`; MAX_OKAIS is now 2. Tab switch
  re-renders the cached page (NO refetch); `+`/`okai <url>` open new tabs
  in the SAME window; the `okai` shell command with no argument opens the
  homepage. Link clicks navigate the current tab in place (no window
  sprawl). Per-tab close × on the active tab; closing the last tab closes
  the window (okai_close compacts the okais[] array — the one-window shell
  path must never see a dead slot).
- **`okai:home`**: OKAI_HOME_HTML is parsed through the normal pipeline
  (html_parse + CSS engine) — headings/styles/links work, zero network.
  `okai_navigate`/`okai_start_fetch` short-circuit it via `okai_is_home`.
  The page: dark navy, centered logo, quick links (example.com, notdexy.ru,
  wikipedia, info.cern.ch). Vision-verified.
- **Z-order leak fixed**: okai chrome used to be painted AFTER all windows
  (`okai_draw_chrome_all`), so a lower browser's chrome painted over a
  higher overlapping window. Chrome + heading overlays now paint right
  after THEIR window in both paint paths (main loop and
  `desktop_paint_rect_skip`), via `okai_paint_overlays(id)`. Chrome-bounds
  pixel probe: no chrome colors outside the chrome band.
- **Bug found on the way — wrapped links recorded broken regions**: a link
  that wrapped mid-text recorded a 1-char click region at the wrap point
  (clicked the wrong thing / dead zones). Fixed: whole-link fit check
  before drawing (fresh line if it doesn't fit, like doc_flow_word).
- **Test-harness lessons** (okvm mouse): the 4-sample smoothing ring keeps
  moving the cursor for several packets after bursts — flush with 5×
  `mouse_move 0 0` before clicking, and verify clicks BY EFFECT (serial
  line appears) with plain retries — move() re-aims from the kernel's
  reported press position, so retries are closed-loop.








### Remaining known gaps (unchanged from before)
- `+` new-tab button / tab clicks / lock icon still visual-only (was
  explicitly optional; untouched).
- All headings use page-level fg/bg in the scaled overlay (CSS heading
  colors apply to H3+ only) — pre-existing behavior.
- Scaled-full-desktop screenshots flatten the gradients visually; verify
  theming on CROPPED+ZOOMED captures.

---

## Next steps (handoff to next agent)

**Do NOT `git commit` unless the user explicitly asks** (prior sessions kept
the tree uncommitted on `kernel`).

1. **Interactive chrome (the last browser-polish item).** The `+` new-tab
   button, tab strip clicks, and the lock icon are visual-only. Wire `+` →
   open a new blank okai tab/window, tab clicks → focus switch, lock →
   HTTPS state indicator. Geometry lives in `okai_draw_chrome`
   (`src/okai.c`); click routing follows the `okai_check_nav_click` +
   desktop.c pattern that just passed its test.

2. **CSS engine deferred items** (see the CSS section above): external
   `<link rel=stylesheet>` fetching (2nd TLS GET), descendant combinators,
   full box model, pseudo-classes.

3. **Cooperative TLS fetch.** `https_get` still blocks the UI ~1-2s inside
   the fetch owner; a `tls_poll()` state machine would un-freeze the desktop
   (original Phase 5 deviation note).

4. **Anything mouse-related:** use the `[mse] btn=1 x= y= pkts=` serial
   ground truth (see session notes above) and `~/okvm` for artifacts. Run
   `python3 tests/headless/test_nav.py` after touching window.c/mouse code —
   it's the regression gate for input + nav routing (currently PASS 4/4).
   QEMU gotcha: bound runs with `timeout`; check `pgrep -c '[q]emu-system-i386'`
   before/after; `pkill -f qemu-system-i386` kills its own shell — bracket trick.



### Hard constraints to respect
- **From-scratch mandate:** no external libs (no TTF/font engines, no GUI
  frameworks). Bitmap font only.
- **No JS** this round (CSS only).
- **Vision verification is mandatory** for any "does it look right?" question —
  the base model cannot see images.
- Project path is the non-ASCII cyrillic path this session; confirm with `ls`.

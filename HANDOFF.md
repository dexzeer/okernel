# okernel — Handoff Document

## What is this?

okernel is a from-scratch operating system built in C and x86 assembly. It has two modes:

- **Text mode** (`make text`) — VGA text terminal with commands, scrolling, terminal multiplexing
- **Desktop mode** (`make desktop`) — 1920x1080x32bpp graphical desktop with windows, mouse, okai, editor, and shell

Current version: **v0.7** (desktop edition: 1920x1080x32bpp, working okai with tabs + resizable windows, from-scratch CSS engine incl. box model)

---

## Project Structure

```
okernel/
├── boot/
│   ├── start.asm          # Kernel entry point, multiboot header, stack
│   ├── isr.asm            # Interrupt service routines (ISRs/IRQs), GDT flush, INT 0x80 stub, enter_user_mode
│   └── user_test.asm     # Ring 3 test program (runs in user mode, calls INT 0x80)
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
│   ├── gdt.c/.h           # Global Descriptor Table + TSS (user segments, ring 3 support)
│   ├── idt.c/.h           # Interrupt Descriptor Table, PIC remapping, IRQ dispatch, INT 0x80 syscall gate
│   ├── keyboard.c/.h      # PS/2 keyboard driver (scancode set 1)
│   ├── mouse.c/.h         # PS/2 mouse driver (IntelliMouse 4-byte wheel mode)
│   ├── process.c/.h       # Process table, per-process page directories, context switching skeleton
│   ├── syscall.c/.h       # INT 0x80 syscall handler (sys_print, sys_exit)
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
│   │
│   ├── js/                # JS engine — "tinyjs ok edition(tm)" (C port of tiny-js, substantially rewritten)
│   │   ├── js_os.h/.c     # Host/kernel shim (kmalloc/kfree, printf, strtod/dtoa; JS_KERNEL flips to freestanding)
│   │   ├── js.h           # Public types: js_var (universal value), js_lex, js_tiny (interp state), tokens, value flags
│   │   ├── js_var.c       # CScriptVar (C port) — refcount GC, string/number/object children
│   │   ├── js_lex.c       # CScriptLex (C port) — tokenizer, owned char* tkStr
│   │   ├── js_parse.c     # CTinyJS (C port) — recursive-descent parser + tree-walking interpreter
│   │   ├── js_funcs.c     # Built-in functions (parseInt, parseFloat, etc.)
│   │   ├── js_math.c      # Math.* built-ins
│   │   ├── js_dom.c/.h    # Kernel glue: js_init(), js_run(), js_dom_run_page() + DOM bridge
│   │   │                    #   getElementById, querySelector, querySelectorAll, createElement,
│   │   │                    #   setText, setStyle, setAttribute, getAttribute, appendChild, body
│   │   └── tests/*.js     # 38 test scripts from upstream tiny-js (all PASS)
│   │
│   └── crypto/            # TLS 1.3 client (host-tested AND integrated into the kernel build — Phase 5/6 complete)
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
│       └── tls_client.c/.h   # Phase 4: full handshake driver (DONE — host-tested PASS; integrated into kernel in Phase 5)
```

---

## Architecture

### Boot Sequence
1. GRUB loads kernel via multiboot spec
2. `start.asm`: sets up stack, pushes multiboot args, calls `kernel_main()`
3. Desktop mode: `start.asm` requests a 1920x1080 linear framebuffer from GRUB (`gfxpayload=1920x1080x32`)
4. `kernel_main()` initializes: GDT → IDT → memory → paging → graphics → windows → mouse → keyboard → networking → main loop

### Memory Layout
- Kernel loaded at 1MB (0x100000)
- BSS contains static variables (window buffers, scrollback rings, etc.). The main backbuffer is dynamically `kmalloc`'d (see below).
- Kernel heap right after kernel end (~1.5MB, bump allocator, 16MB — the backbuffer is `kmalloc`'d at 8MB (1920×1080×32bpp) plus per-window content buffers)
- Page tables identity-map first 4MB + framebuffer at 0xFD000000
- e1000 MMIO mapped via `paging_map()`
- **NIC RX/TX buffers at 0x80000-0x9FFFF** (low memory, below 1MB, for DMA)

### Display Pipeline
1. All drawing goes to a backbuffer (`kmalloc`'d, 8MB = 1920×1080×32bpp)
2. Dirty-row tracking — only changed rows are copied to framebuffer
3. `graphics_flush()` copies dirty rows to framebuffer at 0xFD000000
4. Clip rectangle (`graphics_set_clip`/`graphics_clip_reset`) restricts drawing during scene repair so small repairs only dirty their own rows (enforced in `putpixel`, `rect_fill`, `hline`, `graphics_write_pixel`)
5. Window drag = backbuffer blit + exposed-strip repair (`graphics_blit_rect`); resize = band-only repair; content buffers are slack-allocated with fixed stride (`CONTENT_COLS_MAX`) so resize never reallocates
6. Result (kernel built with `-O2 -fno-strict-aliasing`): hundreds of FPS idle/dragging/resizing in QEMU at 1920×1080 (exact figures scale with resolution; dirty-row tracking keeps repaints cheap)

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
- **HTML parser** (`html.c`): Strips HTTP headers (`\r\n\r\n`), skips `<head>`, `<script>`, `<style>` blocks, tokenizes h1-h6, p, div, a, li, tr, blockquote, ul, ol, pre, br, hr, img, td/th, text. Block tags (`<p>`→`HTML_PARA`, `<div>`→`HTML_DIV`, `<li>`→`HTML_LIST_ITEM`, `<h1>`-`<h6>`→`HTML_Hx`, `<hr>`→`HTML_BLOCK`) carry their `class`/`id`/`style` attributes (captured from both quoted AND unquoted attribute syntax) so the box model can apply; `</p>`/`</div>`/`<li>`/etc. emit `HTML_END_PARA`.
- **Okai window** (`okai.c`): Address bar (g to focus, Esc to exit), toolbar, scrollable content
- **Scrolling** (`okai.c`): LINE-based via a virtual document — `okai_render_content` lays the WHOLE page out once into a static offscreen grid (`doc_chars`/`doc_attrs`, 1024 lines × 128 cols, shared scratch — only live during render), clamps `scroll_y` to `[0, doc_lines - view_h]`, then blits the visible slice into the window buffer with `window_write_cell`. `content_height` is the true full-page height measured by this layout pass. Do NOT go back to rendering straight into the window with a token-granular `skip`: that re-measured content_height from the scrolled render (bound shrank ~2 lines per 1 line scrolled → j/k stalled) and skipped 2-3 lines per step. `HTML_END_PARA` (`</p>/</div>/</li>` closes) terminates the current line in the layout pass — without it blocks jam together and pages are too short to scroll. Links are recorded in doc rows, then converted to BUFFER rows (`doc_row - scroll_y + 2`; the +2 is toolbar/address bar) after the clamp; off-screen links are invalidated (-1) so they never match clicks.
- **Keyboard**: g=address bar, j/k=scroll, r=refresh, b=back
- **Navigation**: Enter to go, back button with 4-page history
- **HTTPS by default**: every navigation — bare host (`okai example.com`), `http://`, or `https://` — is upgraded to `https://` by `okai_normalize_https` (called from `okai_open` / `okai_navigate` / `okai_check_redirect`). The internal `okai:home` scheme is left untouched. If an HTTPS fetch definitively fails (TLS timeout / DNS no-A-record), the desktop response loop retries **once** over plain HTTP via `okai_fallback_http` so http-only hosts still load; the address bar then shows `http://` and the lock reads "Not secure". Verified in QEMU (2026-08-22): `okai http://example.com/` and `okai example.com` both load over HTTPS (`[br] https parse`), no fallback triggered.
- **Rendering**: CSS-styled text — headings, paragraphs, links, lists, preformatted blocks, and a from-scratch **box model** (`margin`/`padding`/`border`/`width`/`height`). A from-scratch CSS engine (`src/css.c`) applies `color`/`background`/`text-align`/`display:none`/`margin(s)`/`padding(s)`/`border`/`width`/`height` from inline `style=` and `<style>` blocks. Colors are full 24-bit RGB.
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
  `okai_draw_heading_pixels()` (now called from `okai_paint_overlays`, after
  `window_draw` in the main loop / `desktop_paint_rect_skip`) paints the heading
  text at `scale = 2*font_scale` → exactly
  2× the body glyph (32×64 px), matching page fg/bg. `window.c`'s `vga_to_rgb[16]`
  was made non-static + `extern`-ed in `window.h` so heading colors match the body
  exactly. Links are unaffected (headings aren't links). Vision confirmed ~2×,
  no ghosting/clipping, reasonable spacing. **Headings now have a hierarchy:** H1 = 3× body, H2 = 2×, H3-H6 = body size (see Session 2026-08-21, item 4).
- **Toolbar nav buttons are now CLICKABLE** (were visual-only): `okai_check_nav_click()`
  (geometry mirrors `okai_draw_chrome` exactly — same `cx0/by/btn/gap`) routed in
  `desktop.c`'s mouse handler *before* the title-drag / link branches; actions
  `okai_nav_back/fwd/reload/home` call into the existing history/navigate logic.
  `okai.h` gained the `NAV_*` constants + the four nav function decls.
- **FPS HUD** (`static int g_show_fps` in `desktop.c` — set to 0 to leave the
  desktop clean; was toggled on (1) to debug the terminal-over-okai FPS
  regression).
- Test: `tests/headless/test_nav.py` (uses `OkVM`, mouse gain ≈4.2 → divide
  deltas by 4.2 to converge without overshoot). Verification confirmed the
  hit-test geometry is correct (a click at x≈99 logged `nav action=2` = FWD,
  exactly matching the FWD rect `[74,100]`). The full 4-button sweep PASS was
  being finalized at handoff — **run it to confirm all four light up**.

### Known remaining visual gaps
- Font is monospace bitmap (inherent from-scratch constraint — no Unicode/TTF; a
  Cyrillic/symbol extension exists via `font8x16_ext`, but no TTF/CJK).
- (Resolved in later 2026-08 sessions: the whole OS — desktop/terminal/editor/
  sysinfo title bars, taskbar, buttons, borders — uses the Firefox-blue theme,
  not Win95; nav icons are proper arrows/reload/home glyphs; headings have a
  H1=3× / H2=2× / H3–H6=body hierarchy.)

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
- `tls_net.c/.h` — `https_get(host, path)` wrapper that QUEUES an async fetch;
  `https_get_poll()` is called once per main loop and advances the fetch one
  step (DNS → TCP:443 → TLS 1.3 record layer) via a non-blocking recv callback
  (`kernel_tcp_recv` returns buffered bytes / -1 on close / 0 if none yet, and
  itself polls the NIC — no busy-wait). `TLS_RX_BUF_SIZE` = 64KB; decrypted
  `tls_response` = 256KB. The UI stays fully responsive while a page loads.

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
   `tls_client_run` (host-tested, PASS) is the resumable driver; `https_get()`
   performs the DNS resolve + TCP connect (previously missing) before the
   handshake. `https_get` QUEUES the fetch (DNS + TCP connect + TLS handshake) and returns
   immediately; `https_get_poll()` is invoked once per main-loop pass and drives
   the resumable `tls_client_run` state machine forward one step at a time via a
   non-blocking recv callback. This keeps `tls_client_run` (host-tested, PASS)
   unchanged while the UI stays fully responsive during the fetch (no freeze).
   The desktop main loop gained a parallel parse path that feeds
   `tls_get_response()` into `html_parse` once `tls_is_done()`.
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

## CSS engine (from scratch) — v0.7 (box model implemented)

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
  `font-weight:bold`, `text-align`, `margin`/`-top`/`-right`/`-bottom`/`-left`,
  `padding` (all four sides), `border` (shorthand → width + `solid`/`dotted`/`dashed`
  style + `#rrggbb` color), `width`, `height`, `display` (block/inline/none). The
  box-model properties apply at doc-build time in the char-grid renderer (margin
  → inset, padding → blank lines, border → a `doc_box_decorate` ring, width →
  content-width clamp) for block-level tokens (P/PRE/BLOCK/LIST_ITEM/DIV/H1-6).
- Colors: `#rgb`/`#rrggbb` + a small named set, resolved to full 24-bit RGB
  (`fg_rgb`/`bg_rgb`/`border_color`), applied via `window_set_text_color_rgb`
  and per-cell `doc_fg_rgb`/`doc_bg_rgb` — NOT quantized to the 16-color VGA
  palette.
- `html_token` now carries `tag`/`cls`/`id`/`style`; `okai.c` calls
  `css_compute()` per token and applies fg/bg, `display:none` (skip), margins
  (blank lines), `text-align` (center/right padding). Links keep cyan unless CSS
  overrides the color.

**Verification:**
- Host unit test `tests/test_css.c` — 24 assertions, all PASS (parser, cascade,
  inline override, color resolution, `<style>` extraction).
- QEMU smoke (`/tmp/ok/css_smoke.py`): `example.com`'s inline `<style>`
  (`body{background:#eee;...}a:link,a:visited{color:#348...}`) extracted, 5 rules
  parsed, page rendered, no crash. `notdexy.ru` uses external stylesheets → 0
  inline rules (expected; external CSS is deferred).

**Deferred (future work):** descendant combinators `a b` (needs a parent/ancestor
tree; the token model is flat); per-side border width/color, border-radius,
box-shadow, `height`/`min-height`; `font-size`/`font-weight` visual effects
(fixed-cell text grid today); pseudo-classes beyond tag-degradation.

---

## External CSS/JS Fetching (COMPLETE)

External `<link rel="stylesheet">` and `<script src="...">` are now fetched
and applied sequentially through the single TCP connection.

### How it works
1. After the main page is fetched and rendered, `okai_queue_sub_resources()`
   scans the raw HTML for `<link rel="stylesheet" href="...">` and
   `<script src="...">` tags, resolves relative URLs, and queues them.
2. CSS links are fetched first (phase 1), then JS scripts (phase 2).
3. Each fetch reuses the single TCP connection (DNS -> TCP -> HTTP).
4. After each CSS is fetched, rules are appended to `T->css_rules` and the
   page is re-rendered. After each JS is fetched, it is executed via `js_run()`.
5. Serial output: `[okai] sub-res: N resources queued`, `[okai] sub-res fetch: CSS/JS url`,
   `[okai] sub-res CSS: X bytes, Y rules`.

### Files involved
- `src/html.c`: `html_extract_link_css()`, `html_extract_script_src()` — URL extraction
- `src/okai.c`: `okai_queue_sub_resources()`, `okai_start_sub_res_fetch()`, `okai_sub_res_done()`
- `src/desktop.c`: Sub-resource fetch handling in main loop

### Verified
- External CSS from DuckDuckGo Lite: 39 rules parsed from `duckduckgo.com/dist/lr.*.css`
- External CSS from local server: background color, text color applied correctly
- DDG Lite search works end-to-end: navigate, search input clickable, results displayed
- Google: 51 tokens, 256 CSS rules, JS-dependent features (search box) unavailable

---


## JS Engine — tinyjs ok edition(tm) (Phase 1–3 complete)

From-scratch C port of the tiny-js engine (gfwilliams/tiny-js, MIT license).
MIT attribution in every ported file header + "tinyjs-derived, substantially
rewritten" note.

**Phase 1:** standalone engine core + serial `print()` test harness — DONE.
**Phase 2:** values, scope, GC, built-in functions — DONE (38/38 tests PASS).
**Phase 3:** DOM bridge to okai — DONE (tested via QEMU, setText/setStyle verified).

### How it works
- `desktop.c` calls `js_dom_run_page(resp, resp_len)` after `html_parse`. This
  extracts `<script>…</script>` tags (case-insensitive, skips `<script src>`) and
  feeds each block to `js_run()`.
- Before running JS, `js_set_current_tab(bi, ok->active_tab)` and `js_set_page_url(T->url)`
  set the context so DOM mutations target the correct tab.
- `js_init()` creates the global engine (`g_js_engine`), registers built-in
  functions and DOM natives (see DOM Bridge below).

### DOM Bridge (Phase 3 — COMPLETE)
The JS engine can now interact with the browser's token-based DOM:

- **`document.getElementById(id)`** — returns a memoized element object with
  native methods attached (setText, setStyle, setAttribute, getAttribute,
  appendChild, addEventListener).
- **`document.querySelector(sel)`** — supports `#id`, `.class`, `tag` selectors.
  Returns first match.
- **`document.querySelectorAll(sel)`** — returns array of matching elements.
- **`document.createElement(tag)`** — creates a new element with generated id.
- **`document.body`** — returns the body element.
- **`el.setText(text)`** — updates the token's text field and triggers re-render.
- **`el.setStyle(prop, value)`** — updates the token's inline style and triggers
  re-render. Appends to existing style string.
- **`el.setAttribute(name, value)`** — updates class/id/style/href on the token.
- **`el.getAttribute(name)`** — reads from the token.
- **`el.innerHTML = "..."`** — strips HTML tags, updates token text.

After any DOM mutation, `js_dom_request_rerender()` is called. The main loop
checks `js_dom_is_rerender_needed()` and re-renders all okai windows.

**Key bug fixed:** `T->token_count` was set AFTER `js_dom_run_page()` ran,
causing DOM lookups to find 0 tokens. Moved assignment before JS execution.

**Known limitation:** Heading overlay uses body-level fg color (not per-token CSS),
so `el.setStyle("color", ...)` on `<h1>` won't change the heading pixel color.

### Port mechanics (C++ → freestanding C)
- `throw` → `setjmp`/`longjmp` via `js_tiny.jb` (freestanding i386 asm in
  `js_os.c` — save/restore ebx,esi,edi,ebp,eip,esp)
- `std::vector<scopes>` → manual dynamic array stack
- `std::string` → owned `char*` (growable `tkStr` in lexer)
- Refcount GC preserved from original
- `addNative("function foo(a,b)", cb, ud)` is the DOM-bridge hook

### Kernel build integration
- `Makefile` `DESKTOP_OBJ` lists `js_os.o`, `js_var.o`, `js_lex.o`, `js_parse.o`,
  `js_funcs.o`, `js_math.o`, `js_dom.o`
- Compiled by `src/js/%.o` rule with `-DJS_KERNEL`
- Host test: compile with `-m32 -DJS_KERNEL` + stub `memory.h`/`serial.h` +
  `kglue.c` → 38/38 PASS

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

## User Mode (Ring 3) — WORKING (2026-09-07)

Ring-3 entry works end to end: `usermode` IRETs to ring 3, `user_mode_test`
runs, `int 0x80` syscalls dispatch, and control returns. Serial proof:

```
[usermode] code=115b80 stack=1ca0f4c
[user] Hello from user mode!
[user] exit syscall — returning to kernel
```

No GP fault, no exception. `test_addrbar` + `test_nav` (4/4) still PASS.

### What's built
- **GDT**: 6 entries — null, kernel code (0x08), kernel data (0x10),
  user code (0x18, DPL=3), user data (0x20, DPL=3), TSS (0x28)
- **TSS**: Task State Segment with SS0=0x10 (kernel data), ESP0 set via
  `tss_set_kernel_stack()` to a real 4KB kernel stack (static in
  `desktop.c`). Loaded via `ltr` at boot.
- **INT 0x80 gate**: DPL=3 (0xEE) so ring 3 can call it. Dispatches to
  `syscall_handler()` in `syscall.c`.
- **Syscall handler**: `sys_print` (serial output), `sys_exit` (stub).
- **`paging_map_user()`**: Maps pages with User bit (0x07) for ring 3 access —
  on BOTH the PT entry and the PD entry, plus `invlpg`.
- **`enter_user_mode()`**: Loads DS/ES/FS/GS=0x23, builds IRET frame
  (EIP, CS=0x1B, EFLAGS=0x3202, ESP, SS=0x23), does IRET.
- **`user_test.asm`**: Ring 3 test program — sets DS/ES/FS/GS=0x23, calls
  int 0x80 (print + exit). Message symbol is `msg_user_hello` so the
  `usermode` command can map its `.rodata` page user-accessible.
- **`usermode` shell command**: Real ESP0 stack, user stack page mapped
  user, code pages + msg page mapped user, then `enter_user_mode()`.
- **`ps` shell command**: Lists processes with PID and state.
- **Process table**: 16 slots, PID tracking, state machine, page directory creation.

### The GP fault (FIXED 2026-09-07 — 4 stacked bugs + 1 bad diagnostic)
The old serial line `error_code=0xd (LDT index 0)` was NEVER the real error
code — the GP handler dereferenced the pushed int_num slot (13 = 0xd) instead
of the CPU error code 44 bytes above the pushed-EDI base (32 pusha + 4 ds +
4 int_num + 4 ret addr). Fixed the decode; the true code was 0x18
(CS selector, RPL=0). Underneath it were four real bugs, all fixed:

1. **IRET frame selectors lacked RPL=3** (`boot/isr.asm`): CS=0x18/SS=0x20 →
   CS=0x1B/SS=0x23. The CPU checks requested privilege in the frame's own
   selectors; bare 0x18 faulted as GP(selector=0x18).
2. **`user_test.asm` used bare 0x20** for DS/ES/FS/GS → 0x23 (same RPL rule;
   `enter_user_mode` also preloads 0x23 before IRET).
3. **`paging_map_user()` left the PD entry supervisor** (`src/paging.c`): the
   CPU checks every level, so a supervisor PD entry faults ring 3 even with a
   user PT entry. Now ORs U/S into pre-existing PD entries + `invlpg`.
4. **ESP0 pointed at BSS, user stack unmapped** (`src/desktop.c`): ESP0 is now
   a real 4KB stack; the kmalloc'd user-stack page and the `.rodata` msg page
   are mapped user-accessible.
5. **Syscall register decode was shifted by 2 slots** (`src/idt.c`): the old
   `frame+8` base pointed at the int_num slot, so eax read the EDX slot
   ("unknown syscall 1137536" = leaked user EIP). Base is now frame+16
   (saved-ebp/ret/int_num/ds skipped), pusha order EDI..EAX.

### Still TODO (Priority 1 remainder)
- `sys_exit` is a stub (returns; user code hangs in `.hang`). No return path
  to the shell prompt yet — `usermode` does not regain control after IRET.
- No per-process page directories wired into the `usermode` path yet
  (`process_create`/`process_switch` exist but unused here); everything still
  shares the kernel page directory with user-bit flips.
- No timer-driven scheduler (IRQ0 switch) — Priority 3.
- GDB-stub recipe retained for the next ring-3 bug (never needed this time —
  serial + objdump sufficed).

### Files involved
- `src/gdt.c/.h`: GDT + TSS setup, user segment descriptors
- `boot/isr.asm`: `enter_user_mode()`, INT 0x80 stub, `user_mode_test`
- `src/idt.c`: INT 0x80 gate (DPL=3), GP fault error code logging
- `src/syscall.c/.h`: Syscall handler
- `src/process.c/.h`: Process table, page directory creation
- `src/paging.c/.h`: `paging_map_user()` for ring-3 page access
- `src/user_test.asm`: Ring 3 test program

---

## Known Limitations

- **Virtual memory** — HIGH-HALF DONE (2026-09-07): desktop kernel links at
  0xC0100000 (phys 1MB, `linker-high.ld`), low trampoline entry (`_start`,
  VMA==LMA, e_entry=0x100030), boot PD maps 0-4M low+high, `paging_init`
  builds full low identity 0-128M + high alias PD 768-799 + FB/MMIO
  supervisor. `V2P`/`P2V_U32` in `src/memlayout.h`. Process table shares
  PD 768-1023, user-low private (copy-high-only). Full low identity is
  transitional (supervisor); clearing low PD 1-767 per process is Priority 3.
- **User mode (Ring 3)** — WORKING (2026-09-07, re-verified on high-half):
  IRET entry at user-low 0x08048000 (position-independent `user_test` copy),
  INT 0x80 syscalls, serial-verified (`[user] Hello from user mode!`).
  `sys_exit` still a stub (no return to shell), no scheduler.
- **In-memory filesystem only** — files lost on reboot, no disk I/O
- **No sound** — no audio drivers
- **Bump allocator** — heap doesn't free (kfree is a no-op)
- **Single CPU** — no SMP support
- **Mouse scroll** — IntelliMouse 4-byte wheel mode IS supported via PS/2 negotiation.
  `mouse_has_wheel` flag, 4th byte read as wheel delta. HMP `mouse_move dz` is
  sign-flipped vs GUI path.
- **Minimal TCP** — single connection, no windowing/congestion control, no out-of-order
  buffering (gaps re-ACKed until the server fills them). Retransmission IS implemented
  (timer + backoff + give-up).
- **HTTP limited** — single GET request (chunked transfer IS decoded via `http_dechunk`)
- **HTTPS works** via the okai (`okai https://host/path`) and is fetched
  ASYNCHRONOUSLY. External CSS/JS from `<link>` and `<script src>` tags are
  fetched sequentially through the same TCP connection after the main page loads.
- **TLS 1.3 limited to ChaCha20-Poly1305** — SHA-256 only; no AES-GCM, no SHA-384.
- **CSS** — from-scratch engine with box model, 24-bit RGB colors, `tag`/`.class`/`#id`
  selectors. External `<link rel="stylesheet">` stylesheets ARE now fetched and applied.
  Descendant combinators, pseudo-classes, gradients/alpha unsupported.
- **JS engine** — tinyjs subset (vars, functions, loops, basic objects). DOM bridge
  with getElementById, querySelector, setText, setStyle. Cannot execute complex
  frameworks (React, etc.) — Google search box requires JS we can't run.

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
| `tests/headless/okvm.py` | Headless QEMU driver library (boot, type, click, screenshot, serial) |
| `tests/headless/TESTING.md` | Complete headless testing playbook |
| `src/js/*.c` | JS engine — tinyjs ok edition (parser, interpreter, DOM glue) |

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

 26. **Forced full-window repaint under a covering window tanked FPS** (FIXED,
     2026-08-24): an earlier terminal-occlusion fix forced `w->dirty = 1` on
     any window overlapping a lower-z okai EVERY main-loop iteration, so a
     terminal sitting over okai repainted its full 900×650 content on every
     mouse-move (≈585k px/frame) — dropping 60 → 13 FPS. Fix: clip okai's
     overlay to the uncovered region (`okai_paint_overlays_rects`) so it never
     paints over the covering window, and removed the forced `w->dirty = 1`.
     The covering window now only repaints when it actually changes (drag /
     focus). (See the z-order/bleed fix, Session 2026-08-24.)

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

### 3. Nav icons — DONE (redesigned 2026-08-24)
`okai_icon_back/fwd` are now **bold filled triangles** drawn via a new
`okai_fill_tri()` scanline fill (replacing the earlier thin chevrons); reload
is a 3px ring + bold arrowhead; home is a filled roof + door. All four sit in
30px rounded chips (`NAVBTN_BG` + `NAVBTN_HI`, `round_rect_fill`).
**From-scratch — NO external icon library.** Vision-verified: all four clearly
recognizable, correct directions, no clipping. Icon geometry is triplicated in
`okai_draw_chrome` / `okai_check_nav_click` / `okai_addr_bar_hit` (keep in
sync).

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
  CHROME_TAB_H 22→38, CHROME_TOOL_H 36→58 (96 total = CHROME_ROWS×FONT_H = 3×32,
  so page content starts exactly at the chrome bottom — the earlier 82px chrome
  left a 14px gap under the nav bar, fixed 2026-08-24). Tab label, "+", and
  URL are 1×
  and CLAMPED to their bar/tab width (no overflow onto neighbors).
- **Nav buttons are proper chips now:** `round_rect_fill` (public in
  graphics.c; window.c's static helper removed) draws a rounded NAVBTN_BG
  chip + NAVBTN_HI top highlight behind each icon; btn 26→30. Geometry
  mirrored in okai_draw_chrome, okai_check_nav_click, okai_addr_bar_hit —
  keep all three in sync.
- **Default window layout:** the okai window opens at
  (1010, 60, 880, 650), right of the default terminal (40,30,900×650). True
  drag-overlap (terminal dragged over okai) is now handled by the z-order +
  clipped-overlay fix (Session 2026-08-24) — this positional offset is just
  the default, non-overlapping spawn.
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

### 8. okai text-engine upgrade: Cyrillic, structure, tables (v0.7-grade, COMPLETE)
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
- **Deferred**: external CSS (`<link>`), inline images, KOI8-R (rare), CJK (no
  glyphs — falls to '?'). (HTTPS is now asynchronous — see Known Limitations.)

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
- **Terminal/okai overlap (bleed) fully fixed — z-order + clipped overlay
  (2026-08-24).** okai's chrome/heading overlay is painted at absolute coords
  every frame, so a higher window that was NOT itself repainting would still
  show okai's nav-bar / big heading bleeding through. Fix has three layers:
  1. **Real z-stack** (`window.h`/`window.c`): `struct window` gained `int z`;
     `window_raise(id)` bumps a window to the top of the stack (monotonic
     `g_z_top`), called from `window_set_focus` so the window you click/drag is
     always topmost. `window_from_point` returns the highest-z window. Both
     desktop draw paths (`window_draw_all` main loop + `desktop_paint_rect_skip`)
     sort visible windows by z ascending and draw back-to-front.
  2. **Overlay clipped to the uncovered region** (`okai_paint_overlays_rects(id,
     rects, nr)` in `okai.c`): the overlay is clipped to the okai window rect
     MINUS every higher-z overlapping window (rectangle subtraction into ≤64
     sub-rects), so it can never paint over a covering window.
  3. **No forced repaint**: the covering window is NOT marked dirty every
     frame (that caused a 60→13 FPS regression — see Critical Bug #26). Because
     the overlay is clipped, the covering window only repaints when it actually
     changes.
  Verify: put a window over okai and the nav bar / big heading stay under it;
  moving the mouse over that overlap is back to ~60 FPS.
- **Skipped-overlay variant rejected**: an earlier attempt skipped the WHOLE
  okai overlay when a higher-z window overlapped — that hid okai's own chrome
  in the partially-uncovered region. Clipping (layer 2) is correct.
- **Bug found on the way — wrapped links recorded broken regions**: a link
  that wrapped mid-text recorded a 1-char click region at the wrap point
  (clicked the wrong thing / dead zones). Fixed: whole-link fit check
  before drawing (fresh line if it doesn't fit, like doc_flow_word).
- **Test-harness lessons** (okvm mouse): the 4-sample smoothing ring keeps
  moving the cursor for several packets after bursts — flush with 5×
  `mouse_move 0 0` before clicking, and verify clicks BY EFFECT (serial
  line appears) with plain retries — move() re-aims from the kernel's
  reported press position, so retries are closed-loop.








### Remaining known gaps
- `+` new-tab button, tab strip clicks, and the HTTPS lock icon are now
  interactive (open tab / switch tab / toggle the security popup respectively)
  — no longer gaps.
- All headings use page-level fg/bg in the scaled overlay (CSS heading
  colors apply to H3+ only) — pre-existing behavior.
- Scaled-full-desktop screenshots flatten the gradients visually; verify
  theming on CROPPED+ZOOMED captures.

---

## Session 2026-08-24 — nav-bar gap, arrow redesign, terminal occlusion, FPS fix

Four user-reported UI issues from a live session, all gated by
`tests/headless/test_tab_x.py` (PASS) + okai-screenshot PIL/pixel verification.
(Headless QEMU **cannot** reliably drag a *titled* window — the PS/2 cursor has
no position-feedback loop for landing in a ~28px title bar — so the
terminal-over-okai drag case is verified by build + logic, not a live drag;
confirm in the GUI.)

### 1. Nav bar cut off the page (14px gap) — FIXED
`theme.h`: `CHROME_TOOL_H` 36→58 so `CHROME_TAB_H(38) + CHROME_TOOL_H(58) =
96 = CHROME_ROWS×FONT_H = 3×32`. The chrome band was previously 82px but page
content starts at row 3 (96px down), leaving a 14px strip of page content
rendering *under* the nav bar. Now the toolbar fills exactly to y=158 and
content starts there — no gap, no overlap. **Invariant:** keep the two chrome
bands summing to `CHROME_ROWS×FONT_H`; never shrink one without the other, and
never below `FONT_H` (32).

### 2. Animation — accepted (no change)
The tick-based ease-out animation (Next-steps item 5) was confirmed "perfect"
by the user; no action taken.

### 3. Broken arrow buttons — FIXED (redesigned)
`okai.c`: `okai_icon_back/fwd/reload/home` rewritten as **bold filled glyphs**
via a new `okai_fill_tri()` scanline fill — left/right filled triangles, a 3px
reload ring + bold arrowhead, a filled roof + door for home. Replaces the
broken thin strokes. **From-scratch — no external icon library.** The 30px chip
geometry is unchanged so the triplicated hit-test / `okai_check_nav_click` /
`okai_addr_bar_hit` stays in sync. PIL confirms white glyph pixels in all four
nav-button boxes.

### 4. Terminal doesn't fully overlap okai — FIXED (z-order + clipped overlay)
Root cause: okai's chrome/heading overlay is painted at absolute coords every
frame; a higher window that wasn't itself repainting would still show okai's
nav bar / big heading bleeding through. Three-layer fix:
- **Real z-stack** (`window.h`/`window.c`): `struct window` gained `int z`;
  `window_raise(id)` bumps a window to the top (monotonic `g_z_top`), called
  from `window_set_focus` so the window you click/drag is always topmost;
  `window_from_point` returns the highest-z window. Both desktop draw paths
  (`window_draw_all` main loop + `desktop_paint_rect_skip`) sort visible
  windows by z ascending and draw back-to-front.
- **Overlay clipped to the uncovered region** (`okai_paint_overlays_rects(id,
  rects, nr)` in `okai.c`): the overlay is clipped to the okai window rect
  MINUS every higher-z overlapping window (rectangle subtraction into ≤64
  sub-rects), so it can never paint over a covering window.
- **Skipped-overlay variant rejected**: an earlier attempt skipped the whole
  okai overlay when a higher-z window overlapped — that hid okai's own chrome
  in the partially-uncovered region. Clipping (above) is correct.

### 5. FPS regression (60→13) when mouse moves over terminal-over-okai — FIXED
The first #4 attempt forced `w->dirty = 1` on any window overlapping a lower-z
okai **every frame**, so a terminal sitting over okai repainted its full
900×650 content on every mouse-move (~585k px/frame) — dropping 60→13 FPS.
**FIX:** clip okai's overlay (above) so it never paints over the covering
window, and **remove the forced `w->dirty = 1`**. The covering window now
repaints only when it actually changes (drag / focus). Moving the mouse over
the overlap is back to ~60 FPS. See Critical Bug #26.

---

## okvm — Headless QEMU Test Harness

**okvm** is the Python library (`tests/headless/okvm.py`) for driving okernel
headlessly in QEMU. It provides keystroke injection, screendump analysis,
empirical mouse movement (bursts that defeat the kernel's 4-sample PS/2
smoothing), clicks, and closed-loop click-convergence driven by the kernel's
own serial instrumentation.

**Output directory:** `~/okvm` (durable — `/tmp` gets wiped on this host).

### Quick start

```python
import sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

vm = OkVM("mytest")             # boots headless QEMU with e1000 networking
time.sleep(14)                  # wait for kernel boot (~8s, 14 is safe)
vm.type_string("okai https://example.com/\n")
ok = vm.wait_for("https parse: count=", timeout=70)
w, h, px = vm.dump()           # screenshot → ~/okvm/mytest.ppm
# ... pixel checks / serial greps ...
vm.kill()
print("PASS" if ok else "FAIL")
```

### Core API

| Method | What it does |
|--------|-------------|
| `OkVM(tag, extra_net=True)` | Boots QEMU headless with serial log (`~/okvm/<tag>_serial.log`), monitor socket, e1000 NIC |
| `vm.type_string(s)` | Types a string via `sendkey` (supports `/`, `.`, `:`, `-`, `\n`, uppercase) |
| `vm.serial()` | Returns full serial log (ground truth for everything) |
| `vm.wait_for(pattern, timeout=70)` | Polls serial until `pattern` appears (or timeout/crash) |
| `vm.dump(path)` | `screendump` → PPM file; returns `(w, h, pixel_bytes)` |
| `vm.burst(dx, dy, n=5)` | Sends `n` relative mouse moves so the smoothing ring fills; net ≈ 3.75× delta |
| `vm.click()` | Left click (mouse_button 1, then 0) |
| `vm.click_link(row, col0, col1)` | Closed-loop link click: click, read kernel `[okai] click row=.. col=..` feedback, correct with burst, repeat |
| `vm.click_lines()` | Parses `[okai] click row=R col=C (mx=X my=Y)` — exact ground truth coords |
| `vm.link_regions()` | Parses `[okai] link[i] row=.. col0=.. col1=.. href=..` — rendered link regions |
| `vm.kill()` | SIGTERM the QEMU process |

### Key ground rules (from TESTING.md)

1. **Serial log is ground truth.** Never guess system state from pixels alone.
   The kernel prints every network event to COM1.
2. **Burst for mouse movement.** QEMU HMP `mouse_move` is RELATIVE; the kernel's
   4-sample smoothing dilutes single moves by ~4×. Use `burst(dx, dy)` (5× repeated
   moves) to fill the ring. Convergence: burst, wait, read serial feedback, correct.
3. **`click_link` is closed-loop.** It reads the kernel's own `[okai] click row=
   col=` line after each click, computes the error, bursts to correct, and repeats.
   This is the single most reliable way to click anything in okai.
4. **Boot wait is 14s.** The kernel boots in ~8s; 14 is a safe margin.
5. **Output in `~/okvm`.** Logs, PPMs, and PNGs go here — `/tmp` is unreliable.

### Mouse automation recipe (PS/2 relative + smoothing)

```
1. Reset to corner:  vm.burst(-300, -300) × 12  (unknown start position)
2. Walk to target:   vm.burst(small_dx, small_dy)  — coarse bursts overshoot
3. Converge:         click → read (mx,my) from serial → burst correction (±12, /~4.5)
4. Verify:           check for [okai] LINK HIT in serial
```

The kernel's 4-sample smoothing ring amplifies sustained bursts by ~4.3× net.
Un-capped convergence bursts overshoot the window and escape permanently.

### Existing test scripts

All in `tests/headless/`:

| Script | What it tests |
|--------|--------------|
| `test_link_click.py` | Link click → new window → fetch → render |
| `test_errors.py` | Empty-host refusal, NXDOMAIN abort, error page, owner release |
| `test_google.py` | Redirect + 85KB chunked + white bg + ≥40 tokens (needs internet) |
| `test_google_search.py` | Google search form input + submit |
| `test_nav.py` | 4-button nav bar sweep (back/fwd/reload/home) |
| `test_addrbar.py` | Address bar focus + URL bar behavior |
| `test_tab_x.py` | Tab open/close animation + `+` button + tab switch |
| `test_links.py` | Link click, new tab, home, tab switch, chrome bounds |
| `test_font_render.py` | Font rendering verification |
| `test_css_box.py` | CSS box model rendering |
| `test_stale_doc.py` | Stale document regression |
| `test_lock.py` | HTTPS lock icon verification |
| `test_https_default.py` | HTTPS-by-default behavior |
| `shot_chrome.py` | Browser chrome screenshot |
| `shot_desktop.py` | Full desktop screenshot |
| `diag_h.py` | Heading diagnostic |
| `diag_nav.py` | Nav diagnostic |

### Vision verification

For "does it look right?" questions, screenshot → convert PPM→PNG → dispatch
to the `opencode-go/mimo-v2.5` vision model (subagent). The base model cannot
see images. Give the model context (window geometry, expected content, known
cursor sprite 12×16 white arrow). Ask for facts, not conclusions.

### Host preview (no QEMU)

`./okai-preview <url-or-file> [out.png] [cols]` renders any page through the
real okai sources on the host in ~50ms. Useful for fast text/CSS/layout iteration
without booting the kernel. Build: `make -C tests -f Makefile.preview okai_preview`.



### Hard constraints to respect
- **From-scratch mandate:** no external libs (no TTF/font engines, no GUI
  frameworks). Bitmap font only. JS engine is self-authored (tinyjs ok edition).
- **JS engine in scope:** tiny-js MIT reference architecture ported to freestanding
  C (≥50% rewrite). MIT attribution kept in every header. Working on DOM bridge.
- **Vision verification is mandatory** for any "does it look right?" question —
  the base model cannot see images.
- Project path is the non-ASCII cyrillic path this session; confirm with `ls`.

## Next Steps for Agent

### Priority 1: User-mode return path — DONE (2026-09-08)
Ring-3 entry AND return both work. `sys_exit` resumes the main loop through
`user_exit_trampoline` (full caller-frame restore: EBP/ESI/EDI/ESP + segments,
register calling convention eip->EAX/esp->EDX so the ESP save is exact);
the shell prints "Back from user mode." and stays interactive. Verified
visually via okvm screenshot (usermode → Back from user mode → help → full
command list, no freeze, no `[ISR] Exception`).
Root-cause chain (bisected with hlt + serial): cdecl passes the saved-ESP arg
on the STACK (not EAX); the trap-stack C frame must be jumped-past (never
called/returned); the landing pad must be branch- and spill-free (-O2 reuses
caller spills across the call). Old screenshots/socks cleaned from
`~/okvm/` + `/tmp` captures removed before verification.

### Priority 3: Scheduler + syscall ABI + isolation — SLICE DONE (2026-09-08)
Shipped (all in-tree, both ISOs build, regressions PASS):
- `src/sched.c/.h` (new, desktop-only): `sched_tick()` slice ACCOUNTING from
  the timer IRQ (never switches CR3/ESP0 itself — no asm context-switch stub
  exists, so preempting a live kernel thread mid-frame would strand it);
  `sched_yield()` safe-point handoff; `sched_spawn_user()` (private PD +
  user-low code/stack mapping + image copy through the high alias);
  `sched_prepare()` / `sched_unprepare()` / `sched_reap()`.
- `src/process.c/.h`: PCB gains `esp0_top` + `ticks_left`; pid 0 owns a real
  4K `idle_stack` set as ESP0 at init (any early ring transition lands valid);
  `process_switch` documented as handoff-point-only (CR3 + ESP0, no mid-frame
  preemption).
- `src/syscall.c` (COMMON, desktop services via hooks): extended ABI 2-5 —
  `SYS_WRITE` (fd 1 → focused terminal + serial mirror), `SYS_GETPID`,
  `SYS_YIELD`, `SYS_MMAP_USER` (page-aligned, user-low only); `SYS_PRINT`
  validates the ring-3 pointer page-by-page AND mirrors to the focused
  terminal (2026-09-08 fix: ring-3 output was serial-only, so the window
  showed Entering/Starting/Back with no program output — looked like nothing
  ran); hooks keep the text build linking (process/paging/window are
  desktop-only; text syscalls fail safe).
- `src/paging.c/.h`: `paging_map_user_pd()` (build a space without switching;
  no TLB flush — CR3 switch flushes), `paging_user_range_valid()` (present +
  U/S at both levels, wraparound + kernel-half rejected).
- `src/desktop.c`: `on_timer` calls `sched_tick()` first (no-op with no user
  process — zero behavior change for existing flows); `usermode` spawns a REAL
  process (private PD via `sched_spawn_user`, pid-tracked pending flag);
  main loop prepares (CR3+ESP0) before IRET, unprepares + reaps after the
  trampoline resumes (user PD never leaks into kernel work; no stale user
  bits in the shared kernel PD anymore). `ps` shows the live process.
- Traps fixed 2026-09-08: spawn rejected the legacy stack top
  (0xBFFFF000+4096 = 0xC0000000 = KERNEL_VBASE exactly — `>=` guard treated
  an exclusive-end TOP as an address; now `>`); same for the alignment check
  (top itself is not mapped — only base = top-4096 must align).
- Still TODO (needs asm stub + entry rewire): true timer preemption of kernel
  threads; `sys_fork`/`sys_exec`/ELF loader.
- Verified: `usermode` → pid 1 spawn → switch → ring-3 Hello (serial AND
  window) → switch home → reap → Back from user mode, no exceptions;
  `test_nav` 4/4 PASS; `test_addrbar` PASS; `test_css` ALL PASS;
  `test_subres` 0 failures.

### Priority 4: System Call Interface — DONE (2026-09-08)
INT 0x80 ABI is now 0-17 (`src/syscall.c` dispatch + `src/sys_proc.c` backend,
desktop hooks; text build links via NULL hooks, fails safe):
- 0 print / 1 exit (legacy pair, now with terminal mirror + return values)
- 2 write (fd 1 terminal fast path + fd-table files/pipes, returns bytes)
- 3 getpid / 4 yield (trap-safe: slice reset only, never switches CR3/ESP0
  under the stub frame) / 5 mmap_user (page-aligned, user-low)
- 6 read (fd 0 = keyboard line queue via `sys_proc_kbd_offer`, else fd table)
- 7 open / 8 close over the VFS (flags 0=ro 1=wo+truncate 2=rw+create)
- 9 fork (full user-low page copy, shared ofd refcounts, parent gets pid)
- 10 exec (ELF from VFS — see Priority 5)
- 11 sbrk (per-process heap 0x08000000→0x40000000, zeroed pages)
- 12 pipe (4KB ring, refcounted ends) / 13 dup (lowest free fd)
- 14 wait (non-blocking zombie reap, -1 = any child) / 15 kill + signals
  (TERM/CHLD/USR1 bitmask, default actions at safe points)
- 16 mmap (fresh zero page, auto-pick scans down from 0xB0000000) /
  17 munmap (free + full TLB flush)
- Return values ride the saved-EAX slot (`syscall_set_ret` /
  `syscall_take_ret` in `idt.c` — popa reloads EAX before iret).
- FD table per PCB (`fds[16]`, 0=kbd 1/2=term, 3+ files/pipes; `struct
  open_file` refcounted across dup/fork); `process_fd_get/alloc/free`.
- Ring-3 test program exercises 0,3,2,4,11,16 live every `usermode` run.
- Still TODO: sys_ioctl, groeiende fd features (non-blocking flags, seek).

### Priority 2: Virtual Memory — High-Half Kernel — DONE (2026-09-07)
Desktop kernel links high (`linker-high.ld`, VMA 0xC0100000 / LMA phys 1MB);
text build keeps `linker.ld`. Boot: LOW trampoline `_start` (VMA==LMA,
e_entry=0x100030, scratch ESP 0x7FF00) fills boot PD (0-4M low PD 0 + high
PD 768), loads private boot GDT (GRUB's 0x08 is invalid — GP sel=0x8 trap),
enables PG, ljumps to `_start_high` (high .text), calls
`kernel_main(mboot_phys)`. Order: memory → mboot field copy (offsets
flags@0/fb@88/pitch@96) → `paging_init` (low 0-128M + high alias 768-799 +
FB/MMIO supervisor, unconditional — boot PD covers only 0-4M) →
`process_init` → graphics (identity FB, NOT `paging_map` window) → rest.
Verified: both ISOs build; ISO boots to 1920x1080 (`[gfx] fb=fd000000
pitch=7680 w=1920 h=1080`); `usermode` ring-3 OK at 0x08048000;
`test_addrbar` PASS; `test_nav` 4/4 PASS; `test_css` ALL PASS; `test_subres`
0 failures (`test_text_decode` 5 pre-existing Cyrillic FAILs, untouched).

Files: `linker-high.ld` (new), `src/memlayout.h` (new: `V2P`/`P2V_U32`),
`boot/start.asm` (trampoline + boot GDT + `_start_high`), `src/paging.c/.h`
(high map + `paging_map` MMIO window + `paging_map_user` PMM-PT),
`src/memory.c` (V2P kernel range, heap-virt), `src/process.c` (copy-high-only,
destroy-low-only, switch+ESP0, boot-PD fallback), `src/desktop.c`
(uncond paging, mboot offsets, PI user copy), `src/user_test.asm`
(position-independent, call/pop msg), `src/graphics.c` (identity FB +
`[gfx]` diag), `src/net/e1000.c` (DMA comment), `Makefile` (dual-link,
start.o-first order, `rm -rf isodir`).

REMAINING TRAPS (do not regress): GRUB entry needs LOW e_entry (high
triple-faults, SeaBIOS text); boot GDT required (GRUB 0x08 invalid);
`paging_init` before `process_init` (.bss beyond 4M); FB via identity, not
MMIO window (black 640x480); `paging_map_user` needs explicit 2nd PMM page
+ FULL 4K source-page copy (59B copy left msg tail unmapped);
multiboot fb@88/pitch@96 (44/52 misread VBE as fb=0x90).

### Priority 3 (old): Context Switching + Round-Robin Scheduler — SUPERSEDED
Replaced by "Priority 3: Scheduler + syscall ABI + isolation — SLICE DONE"
above (2026-09-08): slice accounting + safe-point handoffs + spawn/prepare/
reap + extended syscall ABI + per-PD user mapping + pointer validation.
True timer preemption of kernel threads still needs the asm stub.

### Priority 4 (old): Expand System Call Interface — SUPERSEDED
Replaced by "Priority 4: Expand System Call Interface (remainder)" above.

### Priority 5: ELF Loader — DONE (2026-09-08)
`src/elf.c/.h`: ET_EXEC/i386 validation (magic, class, machine, PH bounds,
user-low-only segments, no wraparound, filesz<=memsz), single-walk loader
(map-once + zero-via-high-alias + copy per page slice, overlapping segments
share pages, 32-page cap). `sys_proc_exec` wipes user-low (frees old pages,
keeps kernel-high shared), maps a fresh stack, loads segments, sets
user_eip/entry + user_esp/top. Shell: `exec <file>` runs ELF from VFS (test:
build a static i386 ET_EXEC with the host cross gcc, `edit`-import or
pre-seed via VFS, then exec).
Trap: the loader takes `elf_map_fn` (bound to the TARGET PD) — never
`paging_map_user` (that maps the RUNNING space).

### Lower Priority
- IPC: pipes + signals + spinlocks/mutexes DONE (see Priority 4 / sys_proc);
  remaining: shared memory (map same phys into two PDs), futexes, named pipes.
- SMP: still single-CPU (APIC, per-CPU, balancing — huge leap, not urgent).
- Better TCP/IP — DONE first slice (2026-09-08, see below).
- Real filesystem + disk — DONE first slice (2026-09-08, see below).

### Persistent filesystem + ATA disk — DONE first slice (2026-09-08)
- `src/ata.c/.h`: polling-PIO LBA28 primary-master driver (IDENTIFY probe,
  0xFF floating-bus + BSY-stuck = absent, ATAPI abort = no ATA disk; all
  fail safe → VFS-only). 512B sectors, ~1s-bounded status waits.
- `src/pfs.c/.h`: from-scratch OKPFS1 layout (NOT FAT/ext — mandate + no
  clock for timestamps): sector 0 superblock (magic/version/nfiles/476B
  bitmap), sectors 1-16 file table (16 entries: name[32]/size/start/flags),
  sector 17+ data (contiguous runs, 64 sectors = 32KB max/file).
  Write-through on every VFS mutation (`fs_install_persist` hook in
  `filesystem.c` — write/append/delete sync; `filesystem.o` stays
  dependency-free); mount-or-format at boot (`pfs_init` after `fs_init`);
  hydrate all entries into VFS; `pfs_status` serial table dump.
- Editor Ctrl+S (0x13) / Ctrl+X (0x18) now WIRED (status bar always
  advertised them — edits died with the window before): save → VFS →
  write-through → disk. Keyboard driver tracks Ctrl (0x1D make/break) and
  maps Ctrl+letter → control codes; terminal ignores control codes.
- Shell: `disk` (presence + mount + serial table), `save <file>` (manual
  sync override), `locktest` (spinlock/mutex selftest → serial PASS/FAIL).
- Verified: diskless boot = VFS-only (no wedge); 20MB `-hda` boot formats +
  mounts (`[ata] disk present: 40960 sectors`, `[pfs] mounted: 0 files
  hydrated`); editor save → `[editor] saved`; REBOOT → `[pfs] mounted: 1
  files hydrated` + `files=1 'p1' size=23 start=17` (persistence PROOF).
- `tests/headless/okvm.py`: `OkVM(tag, disk=path)` appends `-hda` (for PFS
  tests; default runs stay diskless).
- Traps: `fs_delete` copies the name BEFORE clearing (hook needs it);
  `pfs_sync_file` frees the old run BEFORE first-fit alloc (else the file
  can never grow in place); hydrate uses a static 32KB buffer (stack is
  4KB — a 32KB stack buffer would smash it).

### Better TCP/IP — DONE first slice (2026-09-08)
`src/net/network.c` (single-connection stack, still no Reno/CUBIC — flights
are one small GET so cwnd would buy nothing; the wins are latency + honesty):
- Advertised window 60B stub → 32KB (`tcp_send_raw` + SYN). The stub throttled
  fast servers to ~60B per RTT (their silly-window avoidance).
- Peer-window tracking (cached per segment, zero is meaningful) + send gate
  (`tcp_send_data` buffers instead of sending into a shut window) + persist
  timer (1-byte probes on RTO, not counted toward give-up — RFC 793 §3.7).
- Fast retransmit (Tahoe, no cwnd inflation): 3 pure-ACK dups → resend head
  NOW. Discipline fix: ONLY pure ACKs count (plen==0 threaded through
  `tcp_process_ack`) — data-carrying segments with a repeated ack field were
  miscounted, firing 3 bogus fast-rtx mid-download and stalling the NEXT
  connection into SYN-timeout (bisected via `test_errors` recovery FAIL).
- Jacobson/Karels RTO (SRTT/RTTVAR, Karn's rule, clamp 20-600ms) replacing
  the fixed 220ms; per-connection reset (stale LAN SRTT mis-times WAN).
- Reorder buffer (8×1500B): gap segments are STORED (not dropped) and
  drained cumulatively when the hole fills (`tcp_deliver_in_order` +
  `tcp_sink_payload` shared by TLS + HTTP paths); drained ACKs are cumulative.
  Reset per connection (stale gaps would poison the next stream).
- Verified: clean fetch (0 fast-rtx, 0 give-up), 1-in-20 `netdrop` loss fetch
  PASS (RTO-driven, no wedge), `test_nav` 4/4, `test_errors` 5/6 (recovery
  after refusal FIXED by the dup-ACK discipline; recovery after NXDOMAIN
  still FAILs — late-SYNACK + slow-TLS-handshake exceeding the 70s test
  window on a loaded host, NOT a wedge: handshake completes, parses lag;
  was already FAIL on the base commit).

### What NOT to Break
- The external CSS/JS fetching pipeline works end-to-end (DDG search verified)
- The JS DOM bridge works (getElementById, setText, setStyle)
- The browser renders Google (51 tokens, 256 CSS rules)
- All host tests pass (test_css, test_subres)
- Kernel builds clean with no errors


---

## Session 2026-09-08 (evening) — userland cutover: init + exec-from-VFS live, ring-3 sh boots, input path buggy

**Status: Phases 0–4 of `~/.commandcode/plans/userland-cutover.md` DONE and QEMU-verified; Phase 5–6 partial (init spawns, sh runs + prompts, but shell input delivers garbage + fork-child #PF).**

### What works (serial proof, `~/okvm/ulcut*.log`)
- `run /bin/hello` → `[user] hello from userland`, exit code 0, Back from user mode, shell live.
- `run /bin/forktest`, `run /bin/pipetest` → fork/pipe serial proof, no wedge.
- First `run` lazily spawns `/sbin/init` (pid 1) from VFS-seeded ELF (`src/userland_seed.c` + `userland/gen_*.h`): `[spawn_elf] pid=1 '/sbin/init'`, `init: I am pid 1`, forks + execs `/bin/sh`, sh prints `user sh ready` + `u> ` prompt.
- Preemptive scheduler live (`sched_tick` → `process_switch_to` on slice expiry); park/wake for wait/yield/read-empty; zombies + reaping; CLOEXEC-lite; `term_win` stdio binding.

### Bugs fixed this session
1. **Exec #PF exc 14 at new entry** — `sys_proc_exec` wiped user-low PDEs/PTEs in memory but never flushed the TLB, so the iret into the new image faulted on stale translations. Fix: CR3 reload after the wipe in `src/sys_proc.c` (same address space, ring 0 — safe).
2. **fd-0 reads bypassed the keyboard queue** — `sys_proc_read_fd` returned -1 for `PROC_FD_KBD`, so `sys_read(0)` (fd-table path) never drained `kbd_lines`. Fix: route `PROC_FD_KBD` to `sys_proc_kbd_read()` in `src/sys_proc.c`.
3. **#PF handler printed nothing useful** — `src/idt.c` else-branch now logs `err/cr2/eip/esp` for exc 14 (layout: err=pushed[9], EIP=pushed[10], ESP=pushed[13]; same as GP handler).

### OPEN BUG (next step): ring-3 shell input delivers garbage + fork-child #PF
- sh boots and prompts, but its first `sys_read(0)` returns corrupted data: `[exec] not found: <garbage bytes>` with NO typing after sh start (reproduced `~/okvm/ulcut4/ulcut5`).
- Then `[ISR] Exception 14 err=6 cr2=fffffedb eip=804912c esp=bffffdb4` — eip is in sh's `/bin/`-prefix build loop (`mov %dl,-0x126(%ebp,%eax,1)`), fault addr `0xfffffedb` = garbage EBP-relative destination. So either the fork child's stack/EBP is corrupt at resume, or `line[]` contents are garbage and the loop walks off.
- Suspects (in order): (a) stale queue line (`run /bin/sh` offered before sh existed — but that's ASCII, not garbage — so more likely (b) fork-child resume ESP/EIP stash vs actual parent trap state, or (c) park-resume EIP/ESP clobbering the child's stack page.
- Needed instrumentation: log offered-line bytes in `sys_proc_kbd_offer`, log read-return bytes in `sys_proc_kbd_read`, log faulting pid in the #PF handler (weak `process_current`, pid=pcb[0]).

### Docs/process reminders (user directive, standing)
- Keep `to-do.txt` crossed out as items land; update the cutover plan file (`~/.commandcode/plans/userland-cutover.md`) phase checkboxes on the go; HANDOFF gets a session entry each session.
- `usermode` command NOT yet retired (still the known-good ring-3 smoke test until sh input works). Delete it + `user_test.asm` staging only after Phase 6 proves out.
- Kernel shell still consumes every completed line (offers to queue AND executes as a kernel command). Once sh runs foreground, kernel `shell_execute` must skip lines while a userland foreground process owns the terminal — else double-execution (`/bin/hello` exists → kernel would `run` it too).

### Update 2026-09-08 late — input-bug bisect (STILL OPEN, narrowed hard)
- Proven: dbgchild (fork+print, no read/exec) WORKS end to end — parent prints,
  child prints `I AM THE CHILD`, no fault. So fork, child EAX=0, page copy, and
  the entry drain are all correct.
- Proven: the sh fault is SPONTANEOUS — `run /bin/sh`, type nothing after, and
  within ~seconds: stale `run /bin/sh` line (offered at kernel-shell time, BEFORE
  sh existed) is read by sh pid 2, fork child 3 execs GARBAGE, #PF
  `err=6 cr2=fffffedb eip=804913a esp=bffffdb4 pid=3` (child's `/bin/`-prefix loop
  walking a garbage EBP — fault addr is EBP-relative, so the child STACK or its
  page copy is corrupt... OR the parent's `line[]` was already garbage before fork).
- kbd traces added (`[kbd] offer` hex in `sys_proc_kbd_offer`, `[kbd] read` pid+n+hex
  in `sys_proc_kbd_read`, pid in the #PF handler via weak `process_current`): offer
  bytes = clean ASCII `run /bin/sh`; read bytes = SAME clean ASCII. So the queue is
  innocent — corruption happens between sh's `sys_read` return and the child's exec
  (child stack page? fork copy of that page? park-resume clobber?).
- Deferral-guard attempt (hold `switch_busy` across drain dequeue→IRET so the tick
  can't capture the drain's half-built frame) did NOT fix it — and dbgchild working
  suggests the race theory was wrong: if the tick stole drain frames, dbgchild's
  child would fault too. Next suspects: (a) sh's 256B `line[]` + 270B `p[]` + argv
  setup overflowing the single 4KB user stack page (stack page vs heap/args layout
  in `sched_spawn_elf`/`sys_proc_exec`); (b) fork copying a page mid-write; (c) the
  `sys_read` kernel→user copy (`sys_proc_kbd_read` buf through the alias) writing
  to the wrong page.
- Shots not yet fired: per-page dump of child's stack page at fork (parent vs child
  phys), `line[]` address print from sh, stack-top audit (stack page base vs ESP at
  read time — is `line[256]` within the mapped page?).
- Repo-path note: the live repo is `/home/notdexy/projects/okernel`
  (`/media/notdexy/...` is the same file — same dev/inode — but shell cwd defaults
  to the HOME path; always build/test from ONE path or `make` no-ops against stale
  trees).

### Update 2026-09-08 night — ROOT CAUSE FOUND (child EBX garbage) + second crash (e1000_poll #PF) + docs
- ROOT CAUSE (child exec garbage): `[exec-ebx] ebx=8049109 pid=5` — the failing
  child's EBX points at its own fork-resume EIP (the `mov %eax,%ebx` right after
  its fork `int $0x80`), NOT at the `line` buffer. The child is executing the
  PARENT's post-fork path (`mov %eax,%ebx; test; jns/jne...`) instead of the
  `child==0` branch: fork-returned EAX != 0 in the child. So the fork-child
  EAX=0 mechanism fails for sh/forkexec children (but works for dbgchild/dbg2 —
  difference TBD: those fork earlier/first-trap vs after read-park cycles?).
  The `p[270]` build + #PF were downstream of this (child ran parent code with
  parent stack values). Sh `child==0` now execs `line` directly (no child-side
  stack build) — still fails the same way (proves it: EBX never held `line`).
- Why EAX!=0: `enter_user_mode` zeroes EAX iff `user_fork_child` is set
  (`enter_user_mode_fork_child` before enter). Suspects: (a) the flag was consumed
  by a DIFFERENT entry first (stale `user_fork_child=1` from an earlier fork +
  drain ordering: parent entry consumed the child's flag — single global, same
  staleness class as retval/trap-stash/park-state, all now per-slot EXCEPT this
  flag); (b) `sched_fork_take_child` consumed-but-not-entered ordering with park
  resumes. NEXT: make `user_fork_child` per-slot (or pid-tagged) the same way.
- Per-slot conversions landed (all verified building, none yet fixing the child):
  trap stash (EIP/ESP), park arm/consume/resume, syscall retval. Park-drop hygiene
  on fork + destroy also landed (`sched_park_drop`). Exec now builds a clean
  argc=1/argv[0]=path stack (was: empty stack top → garbage argc; forkexec
  hello-after-exec now prints). Exec path guards (`bad path byte/len`) bound the
  damage; `[exec-ebx]` + `[exec-arg]` + `[kbd]` + #PF-pid traces stay until fixed.
- SECOND CRASH (new, kernel-side): `[ISR] Exception 14 err=0 cr2=8001c
  eip=c010cd46 esp=2e6010 pid=4` inside `e1000_poll` (`testb $0x1,0x8000c(%edx)`
  with EDX=0 — descriptor-decode read from phys 0x80000+ → #PF read non-present;
  esp=0x2e5010 is NOT a kernel stack — trap landed on a garbage ESP0). Means: a
  trap/IRQ ran with a stale TSS.ESP0 (freed/reused trap-stack page) OR on a
  half-switched CR3. Appears after sh's child exitsсков... precisely after the
  `exec failed` + init respawn sequence. NEXT: audit ESP0 across
  prepare/unprepare/reap/destroy (esp0_top=0 poison + `tss_set` fallback?) and
  CR3 across the park-trampoline path (park runs on WHOSE PD?).
- Probes added: `/bin/forkexec` (fork→exec hello — isolates read/parse),
  `/bin/dbg2` (child ESP + 512B stack touch — PROVED child stack mapping is
  fine: `child stack OK`), `/bin/dbg3` (child read-after-fork — PROVED queue +
  copy path clean: child got the line). All seeded via userland_seed.c.
- Docs: to-do.txt crossed (done/x, in-progress/~, open bug noted); plan file
  phases marked DONE/PARTIAL + NEXT list; this HANDOFF entry.

### Update 2026-09-09 ~01:00 — isr.asm RESTORED + checkpointed; input bug STILL OPEN (EAX!=0 proven at IRET)
- ACCIDENT + RECOVERY: a `git checkout boot/isr.asm` (meant to drop a comment-only
  edit) reverted the file to the 138-line HEAD version — wiping the entire ring-3
  block (enter_user_mode, fork/park flags, context_switch, both trampolines, .bss
  frame) that was never committed (all untracked/uncommitted session work). Rebuilt
  it line-for-line from the `boot/isr.o` disassembly (`objdump -d`, object survived
  from the 23:32 build) + one fix: the checked-in file never defined `isr80` (the
  INT 0x80 stub idt.c references — pre-existing link gap, previously papered by the
  stale .o). Added the 0x80 stub (err=0 + int_num=0x80 → isr_common_stub) + moved
  `.note.GNU-stack` LAST (was mid-file — code after it lands non-executable).
  Verified: `nasm` clean, globals T (not N), ISO links + boots, ring-3 alive.
- SAFETY (standing): `git commit` early and often on this project — the session
  proved a one-line checkout can nuke a day of asm work. Two checkpoints now exist:
  a09cb63 (cutover WIP) + d1a5548 (isr restore). NEVER `git checkout -- <file>`
  with uncommitted work; use `git diff` + targeted edits instead.
- INPUT BUG, HARD PROOF (forkexec log): `[fork-enter] pid=5 ... EAX=0` logged (drain
  DID flag the child) yet the child's first exec trap shows
  `ebx=8049109` (= its own resume EIP bytes `89 c3...` = `mov %eax,%ebx`) — i.e.
  EAX!=0 at the child's fork-resume instruction DESPITE the flag. So the flag is
  consumed between `enter_user_mode_fork_child()` and the child's IRET — OR the
  IRET lands with a stale EAX path... `enter_user_mode` reads `user_fork_child`
  (single global) at IRET-build time; a park-resume entry for ANOTHER pid between
  flag-set and IRET would NOT consume it (separate flag)... but TWO fork children
  queued WILL (first IRET consumes, second finds clear). Current case has ONE
  child — so suspect: the tick's `process_switch_to` context_switch SAVE/RESTORE
  of... no, IRET EAX is built fresh per entry. STILL OPEN. Next: pid-tag the flag
  (`user_fork_child_pid`, enter takes pid... register convention has no room —
  use a per-slot array + drain-passed slot, or re-set the flag INSIDE the
  cli-held region just before `call enter_user_mode`).
- e1000_poll #PF (err=0 cr2=0x8001c esp=garbage) is a SECOND, kernel-side crash
  (stale ESP0/trap-stack or half-switched CR3 at IRQ time) — after the child
  mess, not instead of it. Audit ESP0 across park paths next.

### Update 2026-09-09 ~01:00 +0500 — MECHANISM FOUND (fork children resume with garbage callee-saved regs)
- The EAX!=0 theory is DEAD. Reaching sh's exec path REQUIRES EAX==0 at `test
  %eax,%eax` (else `jne` diverts to the wait path, never exec) — and the child DID
  reach exec. So EAX was 0; the garbage is ESI: `mov %esi,%ebx` loaded
  0x8049109 into the exec EBX. ESI is derived from EBP (`lea -0x226(%ebp),%esi`
  each loop iteration), so the child's EBP is garbage.
- WHY: fork children enter via a FRESH IRET (`enter_user_mode`) which loads ONLY
  EIP/CS/EFLAGS/ESP/SS (+EAX via the fork flag). EBX/ECX/EDX/ESI/EDI/EBP arrive as
  whatever `enter_user_mode` left — i.e. GARBAGE. The child resumes mid-function
  (`mov %eax,%ebx` after the fork `int $0x80`) with a garbage frame. dbgchild/
  dbg2/forktest children survive because they never touch EBP-relative state
  after fork (call/pop msg = position-independent, ESP-relative touches only);
  sh/forkexec children die the moment they read `line[]` (EBP-0x226).
- FIX (next): user-space resume stub. At fork, push (EBX,EDI,ESI,EBP of the
  trapping parent — idt.c has them in pushed[0..7]) + resume-EIP onto the child's
  stack via the high alias, point the child at a 5-byte stub
  (`pop ebx; pop edi; pop esi; pop ebp; ret` = `5b 5f 5e 5d c3`) placed at the
  stack page base, keep the EAX=0 flag (stub preserves EAX). Child lands at
  resume-EIP with parent's frame intact + ESP = trapped ESP. Needs: idt.c stash
  of EBP/ESI/EDI/EBX (extend per-slot trap stash), PD-walk helper to find the
  child's stack page phys, sanity (tesp offset >= 64 else abort child).
- SECOND CRASH unchanged: e1000_poll #PF err=0 cr2=0x8001c esp=garbage — stale
  ESP0 or half-switched CR3 at IRQ time. Audit AFTER the stub lands.

### Update 2026-09-09 ~01:30 — FORK STUB FIXED (children live); sh reaches exec; stale-queue + e1000 #PF remain
- FORK STUB (sys_proc.c): fork children now enter through a 5-byte stub
  (`pop ebx/edi/esi/ebp; ret`, page base) with frame [EBX][EDI][ESI][EBP]
  [resume-EIP] below the trapped ESP — two bugs fixed along the way: frame was
  laid backwards ([resume][regs] — pops read garbage, ret to garbage, #UD), and
  entry ESP pointed past the frame (+20 — first pop read resume-EIP as EBX, ret
  to EBX = #UD). dbgchild/dbg2/dbg3 still PASS; sh's child now reaches exec with
  VALID ESI/EBP (`ebx=bffffec4` = the real `line` buffer, correct bytes
  `run /bin/sh`... but stale — see below).
- EXEC-ARG MISMATCH (still open, narrowed): sh typed `/bin/hello` but the child
  exec'd the STALE `run /bin/sh` line (offered at kernel-shell time, read by sh
  pid 2 as its first read). So sh's read consumed the stale backlog instead of
  the fresh keystrokes — the keystrokes arrived while sh was parked/execing and
  either (a) went to the kernel shell (which offers+executes them as kernel
  commands — the `Unknown:`/double-execution path), or (b) sat in the queue
  behind... precisely: sh read n=11 stale → fork → child exec stale → fail;
  fresh `/bin/hello` line offered LATER (kernel shell consumed it — check serial
  for `[sh] exec:` after). FIX (next): kernel shell MUST NOT consume lines while
  a userland foreground process owns the terminal (plan NEXT-3 — now the blocker,
  not step 3). Gate: `on_keypress` offers to the queue ALWAYS but calls
  `shell_execute` only when no `run_wait_pid`/foreground userland process is live
  on that window.
- SECOND CRASH (kernel-side, unchanged): e1000_poll #PF err=0 cr2=0x8001c with
  esp=garbage (0x3ff6809 — not a kernel stack). Stale ESP0 or half-switched CR3
  at IRQ time; happens after init respawns sh. Audit ESP0 across park paths +
  CR3 across the park trampoline (whose PD is live when the park jmp runs?).
- Bisect traces stay (`[exec-ebx]`, `[exec-arg]`, `[kbd]`, `[fork]`, #PF pid)
  until sh runs hello end-to-end.

### Update 2026-09-09 ~02:00 — SHELL END-TO-END (hello runs, prompt returns); post-run #PF still open
- MILESTONE: `/bin/sh` reads a typed line, forks, execs `/bin/hello`, hello
  prints (`hello from userland` + pid), exits 0, `Back from user mode`, no wedge.
  Chain that got here: fork stub (callee-saved regs) + exec argv rebuild +
  per-slot trap/park/retval + stale-drop + foreground gate + park-home +
  offer-wake (IRQ arms, main loop enters) + READY-before-prepare + exited-skip.
- STILL OPEN (post-run #PF): `err=0 cr2=0 eip=0 esp=0 pid=4` right after the hello
  exit announce. Shape: the wait-parked sh reaps pid 5 (destroy), retries wait
  (no children → -1), prints... then SOMETHING enters pid 4 with EIP=ESP=0.
  Suspects: (a) sh's park resume staged EIP/ESP=0 (drain consumed the wrong pid's
  park state — self-slot vs pid getters — AUDIT every remaining
  `syscall_park_eip/esp()` self-slot call on drain paths); (b) sh's user_esp got
  clobbered to 0 (exec? no — sh never execs; park_stage with peip/pesp=0 from a
  zeroed slot). NEXT: `[park]` trace already proves stage values — add the same
  for WAKE entries (`[wake] pid eip esp`), find the zero.
- Docs: checkpoints current through 0a14115 (exited-skip). Plan NEXT-1/2 (input
  bug + hello end-to-end) DONE in practice except the post-run crash; NEXT-3
  (kernel shell skip) DONE via the foreground gate; NEXT-4 (builtins) not started.

### Update 2026-09-09 ~02:30 — post-run #PF narrowed: timer switch into reaped waiter slot
- All `[enter]` targets are clean (no zero-addr IRET anywhere — 8 enters, all
  valid). The crash is the TIMER path: `process_switch_to` snapshots
  next_esp/eip BEFORE its cli, then the drain reaps/reuses the slot between
  snapshot and `context_switch` — re-resolve checks UNUSED but ring-3 wait()
  reaps to UNUSED... covered. What it does NOT cover: RING-3 wait reaping the
  CURRENTLY-ENTERED thread's sibling while the tick has it snapshotted, then
  `process_create` REUSING the slot for a fresh spawn (state READY, esp set,
  eip=0/unseeded — passes every guard) while `next_cr3` still holds the OLD
  (freed) PD phys. The switch loads a freed PD → #PF err=0 (read, non-present)
  with garbage ESP. cr2=0/eip=esp=0 in the log is the poisoned PCB (destroy
  zeroes esp/esp0/page_dir), i.e. the tick switched into a slot
  MID-reuse (freed but not yet re-initialized, or re-initialized with a PD
  whose pages were reclaimed).
- NEXT (concrete): (1) add a per-switch serial-free generation guard —
  simplest: `process_destroy` bumps a `generation` on the slot; `switch_to`
  snapshots it + re-checks under cli, aborts on mismatch; (2) stop freeing PD
  pages on destroy when a tick may hold them: defer frees to a reap queue
  drained by the main loop with the tick held (switch_busy) — or simply never
  free PD pages (leak 8KB/exit; fine for a hobby shell — 16 slots max, PMM has
  MBs); (3) re-test sh+hello, then move to builtins (NEXT-4).

## Session 2026-09-09 (later) — TLS 1.3 SERVER AUTHENTICATION (full PKI) — DONE

The crypto stack's diabolical hole is closed: TLS 1.3 now actually
authenticates the server. Before this session, `tls_parse_certificate` only
checked DER framing and `tls_parse_certificate_verify` only checked the sig
length — no X.509, no signatures, no roots, no hostname. Any MITM could
present its own cert and own the session.

### Quick TLS fixes (first commit e547676)
1. **RNG hard-fail**: the `i*0x6D+0x13` fallback for ECDHE keys/CH random is
   GONE — if `rand_bytes` fails the handshake aborts (TLS_FAIL_RNG).
2. **uint8_t → uint32_t `ch_body_len`** (tls_client.c): the transcript
   truncated mod 256 for hostnames ≳46 chars — silently wrong traffic keys.
3. ServerHello **cipher_suite check** (must be 0x1303) + **session-id echo
   check** (session_id is now RANDOM per handshake, not 32 zeros).
4. **Alert discrimination** (tls_decrypt_one returns -2 + desc): close_notify
   ends the fetch cleanly; any OTHER alert/MAC failure is an ERROR — the old
   code delivered a tampered stream's partial bytes as a "successful" page,
   and unauthenticated alerts in RECV_BODY were silently IGNORED (loop until
   timeout).
5. **Flight order enforced**: EE → Certificate → CertificateVerify →
   Finished, each exactly once (hs_next in tls_state).
6. **Sig-alg offer narrowed** to what we can verify (initially {0403,0503,
   0401} — later +{0804,0805} for RSA-PSS, see below).

### Full PKI (commit 4ff3815)
- **`src/crypto/der.c/.h`** — minimal DER walker (low-tag-number only,
  ≤4-byte lengths, no allocation; nodes are views).
- **`src/crypto/x509.c/.h`** — RFC 5280 parser: TBS span, signature +
  algorithm, SPKI (RSA n/e or EC point with named-curve OID), UTCTime/
  GeneralizedTime validity, byte-exact issuer/subject DER (chain matching),
  SAN dNSName (0x82) + BasicConstraints (CA/pathlen), outer==inner sig-alg
  OID check, x509_hostname_match (exact + LEFTMOST-label wildcard, RFC 6125).
- **`src/crypto/sha512.c/.h`** — SHA-512/384 (needed for ecdsa-with-SHA384
  chain certs). TRAP: the length field is 128-bit big-endian at bytes
  112..127 (a 64-bit length at 112 + stale 120..127 = silently wrong digests).
- **`src/crypto/rsa.c/.h`** — RSA public-key verify only: u32-limb
  Montgomery CIOS, R² by doubling, exponentiation with a `t[nl+1]`
  OVERFLOW-WORD-AWARE conditional subtract (the bug that made sig^16 wrong:
  when the CIOS tail's high word is set, the compare must subtract).
  PKCS#1 v1.5 (DigestInfo prefixes for SHA-256/384/512) + **RSASSA-PSS**
  (MGF1, saltLen=hLen) — RFC 8446 §4.4.3 REQUIRES PSS for RSA
  CertificateVerify; a PKCS#1-only offer gets handshake_failure (alert 0x28)
  from RSA-leaf servers.
- **`src/crypto/ec.c/.h`** — ECDSA verify over P-256 AND P-384: Montgomery
  field (u32 limbs), Jacobian dbl/add (a=-3), uniform double-and-add
  ladder. TRAPS FIXED ALONG THE WAY: (a) the M=3(X²−Z⁴) constant was
  4(X²−Z⁴) because add_fe_mod(m,m,m) twice doubles instead of tripling;
  (b) the scalar-mult ladder MUST scan MSB-first — LSB-first while doubling
  R computes the bit-reversed scalar (2G was right, 3G was garbage);
  (c) fe_inv via Fermat in Montgomery form is the proper mont inverse.
- **`tools/gen_roots.py` → `src/crypto/roots.c/.h`** — 14-root store
  (SSL.com ECC/RSA 2022, ISRG X1/X2, GTS R1/R4, DigiCert G2, Amazon 1/3,
  USERTrust RSA/ECC, GlobalSign R3, Sectigo R46/E46) embedded as SPKI DER +
  SHA-256 hashes; matching is hash-to-hash in certverify.c.
- **`src/crypto/certverify.c/.h`** — chain walk: parse ≤5 certs, verify each
  sig with the next cert's key (ECDSA or RSA per the TBS sig alg), issuer/
  subject byte match, CA + pathlen on issuers, dates on every cert, anchor
  at a root by SPKI hash (cross-signed roots work — the cross-cert carries
  the root's KEY), hostname match on the leaf, CV_ERR_* reason codes.
- **`src/rtc.c/.h`** — CMOS RTC (ports 0x70/71, UIP-wait, BCD, read-twice,
  sanity-gated year 2020..2099) → x509_set_now at boot (desktop.c).
- **tls_client.c RECV_HS auth block**: cert_verify → parse leaf →
  CertificateVerify = spaces64 || "TLS 1.3, server CertificateVerify" || 0x00
  || transcript(CH..Certificate) — SIGNED-DATA IS CONTENT‖THASH (hashing
  content alone fails) — verified per alg: 0x0403 ECDSA-P256/SHA256,
  0x0503 ECDSA-P384/SHA384, 0x0804/0x0805 RSA-PSS SHA-256/384, 0x0401
  RSA-PKCS1/SHA256, with the LEAF key. Failure → TLS_FAIL_CERT.
- **No HTTP downgrade on cert failure** (desktop.c): tls_net records
  tls_s.fail_reason; okai's fallback fires ONLY for transport-class
  failures (DNS/timeout/unreachable). Cert/proto/MAC/alert/RNG failures
  render a red SECURITY WARNING page (okai.c, tab cert_failed) and NEVER
  retry over HTTP — a MITM forces that downgrade by killing the handshake.
- **https://host:port/** — https_get_port threads the port (TLS path also
  skips DNS for numeric IPs like the HTTP path does; UNCONDITIONAL
  dns_resolve() would CLEAR the seeded cache and doom numeric hosts).

### Verification
- Host: tests/test_pki.c — 34 PASS: example.com chain (leaf ECDSA-P256/
  SHA256, int P-256→P-384/SHA384, anchor SSL.com ECC Root 2022), tamper +
  outer/inner alg mismatch rejection, hostname (6 cases incl. deep wildcard
  + suffix spoof), validity windows, 5 RSA + 5 ECC root self-verifies,
  full cert_verify + negatives. test_tls_client.c — PHASE 4 + MITM PASS
  (same wire, verify_host=evil.example.attacker.io → rejected, reason=4).
- QEMU: test_pki_qemu.py — `[rtc] wall clock: 2026-9-9`, example.com loads,
  `[tls] certificate chain verified, host matched`. test_certfail.py —
  self-signed RSA server (host :8443, python ssl): handshake completes
  (PSS works), verification FAILS (reason=4), NO http fallback.
- Regressions: test_nav 4/4, test_addrbar, test_subres, text build.
  test_errors 2 known FAILs identical on the pre-session baseline.

### Traps for the next agent
- Advertised sig algs MUST match what the CV dispatcher verifies — offering
  an unverifiable alg lets the server pick it and kills the handshake late.
- The chain's ECDSA alg comes from the ISSUER cert's TBS sig_alg, not the
  leaf's; the CV alg is independent of both.
- Montgomery CIOS conditional subtract must include the overflow word
  (t[nl]|t[nl+1]) — low-limb-only compare silently corrupts big values.
- Scalar mult MSB-first. Always.
- Host test gotchas that cost time: /tmp fixtures die mid-run (use
  tests/fixtures/), python DER walking needs 128-byte SHA-512 blocks, and
  GTS/SSL.com certs' P-384 prime has SEVEN 0xFFFFFFFF groups then
  FFFFFFFE, FFFFFFFF, 0,0, FFFFFFFF — count them.
- RSA-4096 chains fit cert_body[12288] (was 8192 — bumped).
- NEXT crypto steps (optional): P-384 CV for P-384-leaf servers (chain
  verify already has the curve), OCSP stapling, AES-GCM, session tickets.

## Session 2026-09-09 (latest) — ADVERSARIAL SECURITY TESTING of the TLS/PKI stack — DONE

"Test the security of this": built `tests/test_adversarial.c` (host, gcc
-m32 + ASan/UBSan) — a mutation fuzzer over a REAL example.com cert flight
plus a full adversarial mock TLS 1.3 server. 26/26 PASS. The stack survived
every attack class tried; one client bug + one client gap were found and
fixed; the rest of the session's bugs were in the MOCK (the production code
held up). Commits: a63e003 (process.c SEED-SANITY, from earlier in the
session) and 0eb3427 (adversarial suite).

### Section 1 — cert_verify mutation fuzz (4000 rounds, seeded LCG)
- Mutates the REAL example.com flight fixture (from tests/fixtures/) at
  random offsets, 3 modes: bit flip, random byte overwrite (no-op rounds
  SKIPPED — an overwrite that writes the same byte must not count as an
  accepted mutation), truncation.
- Verifies with cert_verify(hostname="example.com"). Requirement: every
  accepted (CV_OK) mutation lands ONLY inside the anchor (root) cert's TBS
  minus its SPKI span — the verifier ignores the anchor's TBS (it is trusted
  by hash) and its signature (self-signed). Anything else verifying = hole.
- Result: 551/3449 accepted, ALL confined to the tolerated anchor regions.
  Zero leaf/intermediate/CV mutations ever verify. No out-of-range CV_ERR
  codes; random garbage never verifies.

### Section 2 — adversarial mock TLS 1.3 server (19 modes)
A from-scratch mock server (x25519 + full HKDF key schedule + chacha20-poly
record layer, openssl signs the test CVs) speaking the real client:
- POSITIVE CONTROLS: ECDSA chain + PSS-on-RSA-leaf chain complete end-to-end
  and deliver the mock response (out_len>0, body matches).
- ATTACKS REJECTED with the right failure class: wrong-key CV, CV bit-flip,
  CV over wrong transcript, hostname mismatch (reason 4/HOSTNAME), expired
  leaf (reason 4), key-share swap (forwarding MITM — SH advertises share B,
  flight still under share A → MAC failure), unoffered cipher (reason 5),
  corrupted session-id echo (5), no key_share (5), missing CertificateVerify
  (PROTO flight order), duplicate EncryptedExtensions (PROTO), flipped server
  Finished (MAC), fatal alert (ALERT), app-data bit-flip (MAC), plaintext
  garbage, oversized record length.
- CLEAN: close_notify ends the fetch with the body delivered (ALERT_CLOSE).

### Found and fixed (production code)
1. **CLIENT: Finished-verify failure had fail_reason=0** (tls_client.c) — a
   bad server Finished now sets TLS_FAIL_MAC (it is an authentication
   failure, not transport noise). Found BY the FIN_FLIPPED test.
2. **CLIENT: flight-parse/order/cert-parse failures got tls_dbg lines** —
   permanent diagnostics (msg type + expected, cert body len).
3. **certverify: `cert_verify_trust_extra(spki, len)`** — single extra
   trusted-SPKI slot (NULL clears) so adversarial tests can install a mock
   root without touching the embedded store.
4. Earlier this session: **process.c SEED-SANITY** — process_switch_to
   refuses to seed a near-zero (<64K) ESP (destroy zeroes esp; the guard
   fires before the zombie-target check) — sp-=5 underflow would corrupt the
   IDT/GDT region. Waiter reaps, tick retries (commit a63e003).

### Hiccups — the debugging journey (all MOCK-side; keep for the next agent)
Chronological, each looked like a crypto bug but was a mock bug:
1. Fuzz false alarm "558 accepted": the whitelist flagged no-op rounds —
   random-byte overwrite writing the SAME byte (1/256 x 4000 ~ 15 hits).
   Fix: skip rounds where mut == msg. Lesson: count only real mutations.
2. Control failed TLS_FAIL_MAC: parse_client_ch read the key_share pub at
   +4 instead of +6 (klen(2) sits between group and key) — the mock ECDH'd
   against a corrupted client key. Parser bug wearing MAC clothing.
3. Flight order violation (got type 0, expected 11): the mock's Certificate
   message went into the flight BODY-ONLY — the flight carries FULL
   handshake messages (4-byte header included). Body-only parses as type 0.
4. Control "certificate not yet valid": openssl stamps notBefore at
   GENERATION time (~15:30Z); the test clock was 14:00. Mock now sets
   23:00 same-day (EXPIRED: 2026-09-12, past its 1-day notAfter). Also the
   KEYSHARE_SWAP branch was LOST in a rewrite (SH always carried share A) —
   restored the share-B swap.
5. DONE with out_len=0: try_queue_body assumed c_buf was contiguous records
   — WRONG twice over: the CH record leads the capture, and the client sends
   the record HEADER and the ciphertext as SEPARATE send() calls. Added a
   record walk; also fixed the inner type check (fin_pt[0] is 20 =
   TLS_HS_FINISHED, the CONTENT type 22 lives at fin_pt[4+32]).
6. AEAD decrypt of the client Finished failed: the mock's nonce XORed the
   LOOP INDEX (i) into bytes 4..11 instead of the SEQ bytes — with seq=0
   the nonce must equal the IV. Diagnosed by recomputing the client-side
   c hs traffic key inside the test (decrypted fine with the same key)
   -> the nonce was the desync. Same class as the 2026-09-08 uint32
   transcript-len bug: off-by-something in a 12-byte field.
7. MAC failure on the BODY record: "s ap traffic" must span
   CH..server-Finished — the mock hashed CH..CV (server Fin missing from
   its own transcript), then briefly CH..SFin..CFin (client Fin added too
   early; it only feeds the resumption master).
8. Tooling: a python splice anchored on str.index matched an EARLIER
   occurrence and quadruplicated a block (mock_recv/mtrans defs) — build
   broke, spliced by line numbers instead. Lesson: never splice test files
   by string search when the string appears in comments.
9. ASan caught a REAL OOB in an early draft: request_len=44 for a 42-byte
   request string — the client faithfully copied 44 bytes past a 43-byte
   global. Fixed with sizeof(req)-1. The client trusting caller-supplied
   lengths is BY DESIGN (like write(fd, buf, len)); callers must be honest.
10. The printf-stripper left orphan args (build broke) and ate one CHECK
    (the ECDSA positive control) — restored, re-verified 26/26.

### Verification
- tests/test_adversarial.c: 26/26 PASS under ASan+UBSan.
- Regressions: test_pki 34 PASS (0 failures), tls_client_test PHASE 4+MITM
  PASS, QEMU test_pki_qemu + test_certfail PASS on the rebuilt kernel
  (client change is desktop-side; text build unaffected).
- ASan catches OOB in host tests: run the suite after ANY crypto/tls change.

### Traps for the next agent
- The mock's transcript must hash CH/SH BODIES (client convention:
  transcript_of hashes st->ch+4/ch_len-4). c_buf holds the CH RECORD — the
  body starts at +9.
- x509_set_now is GLOBAL — set it per-mode inside mock_run, not once in the
  section (EXPIRED needs a different clock than the fresh-certs control).
- tests/adversarial/* are TEST-ONLY keys/certs (generated by
  tests/adversarial/gen_pki.sh, 10y validity, evil.example.com SAN). Never
  reference them from production code paths.
- test_adversarial build (host, ASan):
  gcc -m32 -O1 -g -fsanitize=address,undefined -Isrc/crypto -Isrc
      -o /tmp/opencode/test_adversarial tests/test_adversarial.c
      src/crypto/{tls_client,tls_record,tls_handshake,tls_keysched,sha256,
      sha512,hmac,hkdf,aead,chacha20,poly1305,x25519,der,x509,rsa,ec,
      certverify,roots}.c

### Next steps (priority order)
1. **P-384 CertificateVerify (0x0503) for P-384-leaf servers** — the chain
   verifier already has the curve; only the CV dispatcher needs the case.
   Small, and it unblocks real P-384 sites (Cloudflare-issued leaves).
2. **AES-GCM (0x1302)** — the most common modern suite; needs from-scratch
   AES-128 + GHASH. Only worth it if a target site refuses ChaCha20-Poly.
3. **Extend the fuzzer**: mutate the FULL handshake (SH/EE/record layer,
   not just the cert flight), add truncated-record + split-across-recv
   modes to the mock (the client's rec_have path is untested for partial
   records), and a random-garbage-first-byte-then-valid-SH mode.
4. **Session resumption / tickets** — every sub-resource currently pays a
   full handshake; tickets would make multi-fetch pages much cheaper.
5. **Revocation (OCSP stapling)** — status_request parse + decision; low
   priority (staples are rare) but note it in the SECURITY WARNING page.
6. **Wire the host tests into a make target** (e.g. `make host-tests`:
   test_pki + tls_client_test + test_adversarial) so the next agent runs
   them by default after crypto changes.
7. Deferred UX: cert pinning, security-warning page polish (from the PKI
   session's deferred list).

---

## Session 2026-09-10 (overnight) — host-tests target, P-384 CV proof, split/trunc fuzzer, generation guard, exec EIP/ESP cross ROOT-CAUSED

Worked the adversarial session's Next-steps list top-down. Status: items 1
(P-384 CV), 3 (fuzzer), 6 (host-tests) DONE and verified; item 2 (AES-GCM)
DEFERRED (no site needs it — every target negotiates 0x1303; same call the
PKI session made). Plus two drive-bys found by "test everything": a
pre-existing text-build link break (fixed) and a real kernel bug (fixed,
see Bisect below).

### 1. `make host-tests` — DONE
Makefile target (offline must-pass, informational never gates):
- Must-pass: `t_tls_crypto` (ALL PASS), `t_css` (ALL PASS), `t_subres`
  (0 failures — needs `src/css.c` on the link line: it refs `css_parse`),
  `t_pki` (0 failures), `t_adv` fast `-O2` build + `timeout 300`
  (29/29 PASS after the P-384 + split/trunc additions below).
- Informational (`-` prefix): `t_td` (5 pre-existing Cyrillic FAILs,
  unchanged since 2026-09-07) and the LIVE `t_tls_live` (needs internet;
  passed tonight: 869 bytes + MITM rejected, PHASE 4+MITM PASS).
- Binaries go to `build-host/` (repo-local — /tmp is wiped mid-run on this
  host; `.gitignore`d). Suites run from repo root (fixtures are
  `tests/...`-relative). Verified `make host-tests` RC=0 on the final tree.

### 2. P-384 CertificateVerify positive control — DONE (was "only the CV
dispatcher needs the case"; the dispatcher already had it — the TEST did not)
- `tls_client.c` already handled 0x0503 and the CH offer already included
  it — but NO test ever exercised a P-384 leaf end to end.
- `tests/adversarial/gen_pki.sh` now mints a secp384r1 chain
  (`at_p384_root/int/leaf`, `-sha384` throughout, SAN evil.example.com).
  NOTE: regenerating re-keyed ALL existing fixtures (new at_root/int/leaf
  keys) — test-only PKI, self-contained, committed consistently.
- `test_adversarial.c`: new `MOCK_P384_VALID` mode (P-384 chain, openssl
  `-sha384` CV sign via a new `sha384` flag on `sign_digest`, cv_alg 0x0503)
  with per-mode extra-trust selection (single trust slot re-pointed at the
  P-384 root for that mode). New CHECK: "P-384 leaf + ECDSA-SHA384 CV
  completes".
- Result: 27/27 at that point (later 29/29 with §3).

### 3. Fuzzer extension — DONE (split delivery + truncated flight)
- `MOCK_SPLIT`: `mock_recv` dribbles ≤7B per call through the whole
  handshake — the client's `rec_have` reassembly path (previously untested
  per this doc) now proven byte-identical: positive control, must DONE.
- `MOCK_TRUNCATED`: queue cut to 140B (SH complete + 30B of the encrypted
  flight, then close). Must ERR — never DONE on partial bytes, never spin
  (step loop caps at 500 iters; the CHECK requires ERR).
- Result: ADVERSARIAL TESTS PASS: 29 passed, 0 failed (fast build; ASan
  build is 2-3x slower and timed out at 120s in this env — run it with a
  bigger timeout if you touch crypto).

### 4. Scheduler generation guard — DONE (code), open crash SUPERSEDED by §5
- `struct process` gained `generation` (bumped on create, never cleared by
  destroy); `process_switch_to` snapshots prev/next generations and aborts
  under cli on mismatch (covers OUR-slot-reused, which the pid lookup
  misses — pid lookup only covers target reuse since pids are monotonic).
- The post-run #PF it was meant for (err=0 cr2=eip=esp=0) did NOT reproduce
  in this session's runs; instead the sh_hello runs exposed §5.

### 5. BISECT: exec resumed at EIP=user_esp / ESP=entered_ring3 — ROOT-CAUSED
Symptom (new `tests/headless/test_sh_hello.py`: `run /bin/hello` → Back →
init forks pid 3 → exec /bin/sh): deterministic
`[ISR] Exception 14 err=4 cr2=0 eip=bfffffe0 esp=1 pid=3 cs=1b`.
- Proof chain (all serial): `[fork]` teip=8049035/tesp=bfffffc0 →
  frame+stub correct in memory at BOTH fork time and drain time (temp
  probes, since removed: same phys 0x3df1000, stub 5b5f5e5dc3, f4=8049035)
  → fault `@eip: 01 00 00 00 ec ff ff bf` = the exec-built `[argc=1]
  [argv0]` header at final_esp=0xBFFFFFE0. So the IRET executed DATA as
  code with EAX=0: EIP←0xBFFFFFE0 (= user_esp), ESP←1 (= entered_ring3).
- Root cause: `src/idt.c` exec-redirect read `pcb[8]`/`pcb[7]` by RAW WORD
  INDEX. Inserting `generation` after `state` shifted user_esp 7→8 and
  user_eip 8→9, so new_eip read user_esp and new_esp read entered_ring3.
  The old comment ("_Static_assert-free comment guards drift") described
  exactly this failure mode. Bisect proof: struct-size-only change (new
  .h + old .c) crashed identically; full revert was clean.
- Fix: `generation` moved to END of struct (ABI churn minimized) AND
  idt.c now `#include "process.h"` + reads by
  `offsetof(struct process, user_eip/user_esp)/4` — raw indices are gone,
  mid-struct inserts are safe again. The two local `process_current`
  decls were retyped to the weak `struct process*` form (header-type
  conflict otherwise); weak-NULL behavior in the text build preserved —
  both ISOs link.
- After fix: `test_sh_hello.py` FULL PASS (hello → Back → no #PF → sh
  interactive: typed `help` answered by ring-3 sh). New regression test,
  kept in tree. (Its first draft waited for text-mode "Available
  commands"; desktop prints "Commands:" and sh owns the terminal here —
  the test accepts either shell.)

### 6. Drive-by: text build was BROKEN at base (fixed)
`syscall.c` called `sys_proc_kbd_pending()` (desktop-only) without the
weak attribute → `make text` failed to link on the BASE commit too
(verified via stash). One-line fix: weak decl + NULL check. Both ISOs
build again.

### QEMU battery on the final kernel (all serial-ground-truth)
- test_sh_hello: PASS (hello / Back / clean / sh-interactive)
- test_nav 4/4: PASS · test_addrbar: PASS · test_pki_qemu: PASS (RTC +
  chain verified, no CV-invalid) · test_certfail: PASS (self-signed
  :8443 rejected reason=4, no HTTP fallback; server script is throwaway
  in /tmp — recreate per the recipe: openssl RSA cert + python ssl
  TLS1.3 server)
- test_links: FAIL (HOMEPAGE pass; LINK/NEWTAB/SWITCH/CHROME-BOUNDS fail)
  — PRE-EXISTING, fails IDENTICALLY on the base kernel (same 737
  blue-pixel count): clicks register and links render, but the harness
  converges to the wrong spot (mouse-gain/geometry drift). Needs a
  harness-side look, not a kernel fix.

### Traps for the next agent
- NEVER read PCB fields by raw word index (`pcb[N]`) — use `offsetof`
  (the exec-cross bug). `pid` via `pcb[0]` remains (first field, stable).
- New `struct process` fields go LAST (tail-growth convention, see header).
- /tmp is wiped mid-run on this host: test binaries, logs, and servers go
  in `build-host/` / `~/okvm/`, never /tmp. (The ASan adversarial binary
  died to this once tonight.)
- `make host-tests` after ANY crypto/tls change; `test_sh_hello.py` after
  ANY process/sched/syscall/paging change (it caught a deterministic
  crash in one run).
- Internet here flaps ("immaculous" amounts): QEMU failures on network
  legs need a host `socket.create_connection(('example.com',443))` sanity
  check before any kernel theorizing; prefer `test_certfail` (localhost
  server) for offline TLS-negative proof.

### Remaining next steps (re-prioritized)
1. test_links harness (gain/geometry convergence) — kernel side proven
   innocent (identical base failure); likely GAIN constant or stale origin.
2. AES-GCM (0x1302) — still deferred, same rationale (no target needs it).
3. Session tickets, OCSP, warning-page polish — unchanged.
4. The 2026-09-09 post-run NULL-EIP #PF never reproduced tonight (6+
   sh_hello runs, all clean after the offsetof fix — the exec-cross may
   have BEEN one of its shapes); keep `test_sh_hello.py` in the loop.

---

## Session 2026-09-10 (day) — external review response + resumption + OCSP/pinning (56/56)

An external reviewer read all 20 crypto files (report saved as
`cryptoholes.txt`, in-tree, UNTRACKED — reviewer's working copy, don't
commit). Verdict: math correct, all findings are systems bugs. Worked every
numbered item except AES-GCM (user-deferred); review's #3 (NST) was already
fixed the night before.

### Review fixes (all in-tree, all host-proven)
1. **AEAD oversize = hard error.** `mac_poly1305` had no else-branch
   (unwritten tag / uninit compare). Now returns int; encrypt wipes output
   + returns -1, decrypt refuses before comparing. `send_aead` propagates.
   Test: 25KB vectors in test_tls_crypto (both directions refuse).
2. **Fail-closed clock.** `x509_time_known()` (set only by `x509_set_now`);
   `cert_verify` returns EXPIRED while unknown. Kernel RTC path kept;
   RTC-failure message now says HTTPS is disabled. `test_tls_client.c`
   sets time from `time()/gmtime()` (it relied on the placeholder).
3. **NST tolerance** — pre-existing (night session); reviewer confirmed the
   hole independently (their test servers send tickets; ours didn't).
4. **CV is PSS-only.** 0x0401 dropped from `tls_parse_certificate_verify`
   AND the dispatch (defense in depth); stays in the sig-algs OFFER (chain
   certs legitimately use PKCS#1). Test: MOCK_CV_PKCS1 (valid RSA-PSS-key
   signature under 0x0401 → rejected).
5. **Cleartext alerts rejected post-SH.** RECV_HS + RECV_BODY: outer-type
   ALERT (unauthenticated, `pl == -2`) is now always PROTO — including a
   post-handshake plaintext close_notify, which used to report clean DONE
   with a partial body (truncation attack: 7 injected bytes). Encrypted
   inner close_notify still ends cleanly (CLOSE_NOTIFY control green).
   MAC failures keep TLS_FAIL_MAC (KEYSHARE_SWAP control depends on it).
6. **EKU/KeyUsage/criticality.** Parser captures critical flag + KU bits +
   EKU (serverAuth/anyEKU); unknown-critical → parse fail. certverify
   enforces: leaf digitalSignature + serverAuth-when-EKU-present; issuers
   keyCertSign + serverAuth-when-EKU-present; new CV_ERR_KEYUSE (9).
   Fixtures: at_ku_leaf (no digitalSignature), at_eku_leaf
   (clientAuth-only), at_crit_leaf (unknown critical OID) — all rejected;
   control verifies. New `== 3. key usage` mock section.
7. **HKDF bounds.** expand_label refuses (zeroed output) on label/context
   overflow instead of smashing info[256] + lying length bytes; hkdf_expand
   clamps at 255 blocks (counter-wrap keystream repeat).
8. **Host RNG.** /dev/urandom-seeded (time/pid fallback); fixed seed gone.
   Host binaries stay test-only (production = kernel CPRNG).
9. **Trust hook out of the kernel.** `cert_verify_trust_extra` + slot are
   `#ifndef KERNEL` (no production callers existed — verified by grep).
10. **RSA ≥ 2048.** Leaf + every issuer reject < 256B moduli (KEYUSE).
11. **Quicks:** seq consumes only after successful decrypt; SH must fill
    its record exactly (no silent coalesce-drop); parse_time validates
    every digit; `ec_init()` eager build called at boot (lazy fallback
    kept for host tests); rand XOR-then-rekey documented sound (rekey is
    the ChaCha PRF call — the combiner, not the XOR); ISRG X1/X2 confirmed
    in roots + live (letsencrypt.org chain verified in QEMU).
    Skipped: g_last_fail_reason sync (single-connection, noted).

### Session resumption (RFC 8446 §4.6.1/§4.2.11/§7.1) — DONE, mock-proven
- **Store:** RECV_BODY consumes post-handshake handshake records; NST
  (type 4) parsed + bound to the stashed resumption master (transcript
  through BOTH Finisheds) and cached per-host (4 slots, latest wins,
  lifetime-enforced, ticket ≤ 1024B). Malformed/foreign types → PROTO.
  Modes: MOCK_NST (DONE + cached), MOCK_NST_BAD (ERR, nothing kept).
- **Offer:** SEND_CH upgrades to a PSK-carrying CH (byte-identical base +
  trailing ext) when a fresh small ticket exists: PSK = resumption-expand,
  early/binder-key derived, binder = HMAC over ClientHello1 (truncated
  through the binders-length field) patched in. ECDHE always present
  (forward secrecy either way); offering never weakens (fallback clean).
- **Accept/fallback:** RECV_SH detects selected_identity 0 (accept) /
  absent (zero-early fallback) / foreign (PROTO abort). Accept abbreviates
  the flight (EE→Finished, no cert/CV — auth via Finished MACs under
  PSK-mixed keys); all three transcripts branch on psk_accepted
  (transcript_st helper — cert/CV legs omitted, never hashed-empty).
- **Proofs:** MOCK_PSK_ACCEPT (mock INDEPENDENTLY recomputes the binder
  from saved resumption truth — wrong client math alerts → ERR by
  contradiction; passes), MOCK_PSK_FALLBACK (DONE), MOCK_PSK_FOREIGN
  (ERR). Bisected along the way: a mock off-by-one (`fin_pt+5` fed the
  post-cFin transcript shifted by one — invisible until resumption used
  it) and a mock seq inversion (body encoded seq 0 queued after NST
  seq 1). Client offer needs no clock (age 0 + age_add when unknown);
  kernel passes tick_count (tls_net). **Freestanding trap hit:**
  64-bit `/ 1000` in ticket_fresh needs libgcc `__udivdi3` (absent) —
  compare in ms with a multiply instead.
- **Live: BLOCKED (no ticket issuer reachable).** test_resume.py kept:
  Cloudflare (example.com) and google send NO pre-close NSTs on our
  `Connection: close` fetches (github/wikipedia failed on flaky net).
  Recipe: watch serial for "ticket stored", refetch same host, expect
  "offering PSK" + ("resumption accepted" | clean fallback parse).
  Mock proof (independent binder verification) stands as the correctness
  argument; every live full handshake still passes (fallback-safe design
  + FALLBACK control).

### OCSP stapling — offer + note (validation deferred, honestly)
- CH (both builders) sends RFC 6066 status_request, CORRECT 5-byte empty
  form (type + empty responder list + empty extensions). **Incident:** a
  first 1-byte form was malformed — servers answered decode_error (caught
  by certfail going PROTO-reason-6 instead of CERT-4; also explained a
  QEMU "stall" misdiagnosed as environmental). Fixed + covered by every
  live fetch since.
- `tls_cert_has_staple()` walks CertificateEntry extensions for a
  well-formed status_request; malformed entry framing fails the flight.
  Content unenforced (no OCSP parser/responder PKI — multi-day build for
  a rare extension); the warning page now SAYS revocation is unchecked.
- **Live proof:** `OCSP staple present (noted, unenforced)` observed on
  example.com (Cloudflare staples). Modes: MOCK_STAPLE (DONE + seen),
  MOCK_STAPLE_BAD (ERR).

### TOFU pinning + warning page — DONE
- `tls_pin_check()` (8 slots, memory-only, first-seen wins, mismatch keeps
  old pin): checked after every verified full handshake against the
  verified identity; change → CERT failure (new CV_ERR_PINCHANGED 10,
  CV_ERR_MAX bumped — fuzz range auto-follows). Abbreviated resumptions
  skip (no new key learned). Rotation lockout ends at reboot (documented;
  no override UX exists yet — next UI step, not silently added).
  Unit section `== 4. TOFU pin store` (8 checks). Mock isolation needed
  `tls_pin_clear()` before key-changing modes (VALID/RSA/P-384/SPLIT
  share one mock hostname — the pin correctly fired on each change,
  which initially reddened the suite; clears are test-harness isolation,
  not production behavior).
- `cert_detail` (CV_ERR_* or 0) plumbed tls_client → tls_net →
  `T->cert_detail`; warning page prints the specific reason
  (expired/hostname/key-use/root/chain/pin-change) + "Revocation is not
  checked (no OCSP)." Serial FAILED line gained `detail=N` (certfail
  regex updated — it required `)` right after the digits).

### Verification state
- `make host-tests` RC=0: crypto/css/subres/pki PASS, **adversarial 56/56**
  (fuzz + 30 mock modes + keyuse + pin sections), text_decode 5 known,
  live PHASE 4+MITM PASS.
- QEMU (final ISO): sh_hello PASS, nav 4/4 PASS, addrbar PASS, certfail
  PASS (reason=4, no fallback), pki_qemu PASS (pre-CH-fix ISO; CH bytes
  changed since — re-run on next network window), resume BLOCKED above.
- test_links still red-identical-on-base (harness, untouched).

### Observed (non-crypto, recorded, not pursued)
- Rapid second `okai <same-url>` completes TLS (869B) but never prints
  the parse line — fetch-owner/tab race on back-to-back same-URL fetches?
  Out of crypto scope; needs a desktop look.
- Fixture clocks: gen_pki.sh stamps notBefore at generation; mock clocks
  are now fixture-mtime-relative (adv_clocks) — regens can't red the
  suite again (this bit once tonight).

### Traps for the next agent (process lessons from this session)
- Read-then-edit: three near-misses tonight from oldString boundaries
  eating neighbors (a deleted fn body, a deleted CHECK, a deleted decl).
  Always re-read the region after structural edits; the suite caught
  everything, but slowly. Prefer small unique anchors over big blocks.
- `-Wshadow` would have caught the mock `cert_len` shadow instantly;
  consider adding it to the host-test build lines (not CFLAGS — kernel
  build has its own warnings posture).
- Fuzz reason ranges must follow CV_ERR_MAX, never a fixed code.
- Mock record order == seq order (encode in queue order).
- Mock trust/clock/pin stores are cross-run globals: isolate modes that
  change identity, time, or trust (clear + per-mode clocks).

---

## Session 2026-09-10 (evening) — test_links green, usermode retired, resumption live

### test_links: PASS 5/5 (harness-only fixes; kernel proven innocent twice)
Bisect first: fails IDENTICALLY on base (same 737 blue-pixel count) → never
the kernel. Three harness bugs, all stale-geometry class:
1. **Dead-reckoning convergence.** `click_until` re-aimed from commanded
   (not measured) deltas via `move()` — same gain error every retry, cursor
   pinned (observed x=1199 vs target 1436 across all retries). Rewrote
   closed-loop: click → read kernel `[mse]` press → correct residual →
   repeat. Retries now converge geometrically.
2. **Stale content cells.** Targets used 16x32; the grid is CONTENT_GW/GH =
   12x24 (font_scale 1). Fixed in test_links, test_link_local, and
   `okvm.click_link` (the shared primitive — also fixed its `c_row == row`
   doc-vs-buffer confusion to `row + CHROME_ROWS`, gain 3.0 → 4.0, plus an
   `expect_href` parameter that fails on WRONG-link hits instead of
   compounding clicks in the wrong page).
3. **Stale chrome geometry.** `plus_x` assumed 200px tabs; tabs cap at 300
   (`nbx = 1318`, center 1334,81). Tab width animates on open (eases toward
   target) — stable at 300 for 1–2 tabs, but wait out animations before
   asserting chrome geometry in future tests.
4. **CHROME-BOUNDS rewrote, not relaxed.** Old scan (y=126.., any blue)
   sat INSIDE the 96px chrome and counted blue links + navy logo as bleed
   (737 hits, deterministic). New: light-ramp pixels (gradient top half —
   absent from links/logo, verified by PIL sampling) must be PRESENT in
   the chrome band (positive control, 691 hits) and ABSENT just below it
   (leak=0).
5. **Test-logic bug:** NEWTAB awaited a SECOND "home rendered" that fires
   once per tab (each extra click opened another tab). Now: button proof
   (nav action=5) + "home rendered" already in log.
Online proof: LINK HIT on example.com → `https parse: count=7` over real
HTTPS. New `test_link_local.py`: full link-click loop against localhost
fixture (offline-capable) — caught a REAL kernel bug below.

### Kernel fix: resolve_href dropped :port
Relative hrefs on `http://host:port/` resolved to `http://host/path`
(port parsed then never re-emitted). Local link test exposed it
(`...:8000/page2.html` → `:80`). Fixed (port captured + re-emitted in
both absolute-path and relative branches). Covered by test_link_local.

### usermode retired (Phase 6 exit)
`run /bin/hello` + interactive sh proven by test_sh_hello (multiple green
runs, no post-run crash since the offsetof fix). Removed: `usermode`
command (→ "retired; use run /bin/hello" notice), boot staging block,
legacy single-slot drain branch (queue only now), `sched_spawn_user`,
syscall usertest staging + `user_entry_*` globals/decls, `src/user_test.asm`
+ Makefile obj. Kept: `syscall_can_exit` latch (run path), drain logs.
Verified post-cut: sh_hello PASS, links 5/5 PASS, certfail PASS, both ISOs
link with zero errors.

### Resumption: live OFFER proven (github issues tickets)
With WiFi back: github.com sends NSTs ("ticket stored"), refetch logs
"offering PSK" → server takes the full-handshake fallback → page parses
(RESUME PASS). Abbreviated-accept stays mock-proven (servers here ignore
the offer; no cooperating host found). google sends no pre-close NSTs.
New harness discipline: focus the terminal window (closed-loop click at
(200,400)) before EVERY typed command — after any navigation, keystrokes
land in the browser where 'g' (addr-bar hotkey) eats characters (observed:
github.com → ithub.com → NXDOMAIN, misread as network failure).

### Remaining
- test_resume rapid re-okai parse miss (bytes arrive, no parse line) still
  open — desktop fetch-owner race, out of crypto scope.
- test_links LINK leg needs WiFi (mechanics covered offline by
  test_link_local).
- Next crypto: OCSP validation, pin override UX (both deferred, honestly
  noted on the warning page), AES-GCM (excluded).

---

## Session 2026-09-10 (night) — deep-review response round 2 (78/78 + ASan clean)

Second, much deeper external review (`cryptoholes.txt` rewritten: 48
findings, 1633 lines, committed for provenance). Worked P0 fully + P1
nearly fully in reviewer's priority order. Unchanged by design: full path
building/policies/revocation semantics (documented limits), AES-GCM
(excluded earlier), const-time assembly audit (needs a dedicated session).

### P0 (all done)
1. **SH OOB read (the reviewer's #1).** Oversize SH truncated the copy but
   kept the attacker's length → transcript/parsers read OOB. Fixed:
   reject-before-copy; audited every bounded copy (EE/CERT/CV were already
   check-before-copy; fin exact; rec_buf arithmetic overflow-free).
   Regression: MOCK_SH_BIG (>4KB SH → ERR). ASan would have caught the old
   code (it caught two mock-side sizing slips below instead).
2. **send_aead overflow.** `pt_len + 1 > sizeof` wraps at UINT32_MAX →
   `pt_len >= sizeof` guard; all sums safe by construction after.
3. **out_len wrap.** Subtraction form + explicit invariant check.
4. **RNG re-architecture (Fortuna-lite).** Pools (fast/slow) + SHA-256
   conditioner + source classes (BOOT/TIMER/INPUT/RDRAND); readiness =
   reseed compressing ≥64B from ≥2 classes (NEVER a sample count); MAC is
   personalization only (never counted); RDRAND probed via CPUID and mixed
   as one class among several (absent under QEMU — pools carry it, logged);
   per-call RDTSC nonce fold (snapshot divergence); fail-closed throughout.
   Bug found by the new unit test: per-window mask wipe deadlocked
   readiness without interrupt luck — mask is now lifetime-accumulative.
   `tests/test_rng.c` (9 checks: fresh-closed, single-class flood refused,
   two-class opens, stream advances) in host-tests.
5. **Host RNG fail-closed.** /dev/urandom or abort (TLS_FAIL_RNG) — the
   time/pid fallback is gone.
6. **Concurrency.** cli/sti inside rand_* (timer ISR stirs while main loop
   generates); single-threaded contract documented in tls_client.h/rand.h
   (no IRQ crypto paths — verified: e1000 RX only memcpys).
7. **Record cap 16640** (RFC 8446 §5.1) enforced at recv.
8. **Seq exhaustion guards** on both AEAD paths.
9. **Fragmentation.** RECV_HS reassembles handshake messages across records
   (16KB hs_buf, cap-reject, trailing-after-Finished rejected, compaction).
   MOCK_FRAGMENT (split mid-Certificate → DONE). Bisected: mock burned a
   throwaway encode's seq before the split sends (MAC fail, not logic).
10. **Fuzz infra.** `== 0. parser robustness` section (4000 garbage rounds
    over every parser, range-checked codes) + `make host-tests-asan`.

### P1 (done unless noted)
- verify_host is `#ifndef KERNEL` (kernel never set it — verified).
- CCS must be exactly `01` (was skip-anything).
- HRR magic random explicitly rejected (would have poisoned all keys).
- EE full validation (exact, no-dups, ALPN-if-present == http/1.1 —
  live-verified against Cloudflare) + SH exact + SH/EE dup rejection
  (MOCK_SH_TRAIL/SH_DUP/EE_DUP/EE_ALPN_H2, all ERR).
- ECDSA canonical: strict DER INTEGER + 1<=r,s<n BEFORE reduction +
  0x04 prefix + coordinates < p pre-montgomery (test_crypto_strict.c:
  padded/r+n/zero/negative r, x==p, compressed prefix, RSA e∈{0,1,2,
  even,>32b} — all rejected, controls pass).
- DER: strict positive-INTEGER helper (n/e/serial/pathlen), alg-id full
  consumption + per-alg params (RSA NULL-or-absent, ECDSA absent-only;
  outer==inner incl. params), ext BOOLEAN strict, pathlen malformed→reject
  (was: silent unconstrained!), full month/leap dates, TBS trailing
  reject, cert framing exact (message + entries), certverify list exact.
  (One self-caught pathlen-0-vs-zero-reject ordering bug along the way.)
- AKI/SKI key binding in the walk (both-present → must match;
  sibling-key substitution dead). IP SANs (v4, exact; DNS never matches
  IP hosts and vice versa; leading-zero rejected). Hostnames: 253 cap
  (no truncation), ASCII-only reject. Fixtures: at_ip/at_dnsip (openssl),
  at_aki_ok/at_aki_bad (mint_aki.py + at_int.key). New `== 5.` section
  (13 checks).
- Zeroization: `secure_zero` (header-inline — host builds link libc, not
  string.c) + `tls_state_wipe()` on DONE/ERR in tls_client_run AND tls_net
  (static state must not linger between fetches).
- Ticket clock gate (no offer with now_ms==0; tls_client_run stamps wall
  time), oversized-nonce reject (MOCK_NST_BIGNONCE: dropped but flight
  completes), true oldest-first pin eviction, hkdf_expand/hkdf → int
  (refuse >8160 instead of clamp; expand_label bounds out_len too).
- State-machine rejection table banner over tls_state_step (#28-lite;
  full table refactor deferred — behavior already matches it).
- Deferred with notes: full path policies/constraints (documented as
  limits, not silent), OCSP validation semantics (page already honest),
  TOFU persistence (PFS-shaped future), AEAD static scratch (documented
  under the single-thread contract; streaming Poly1305 is the real fix),
  const-time assembly audit, X25519 extra vectors (reviewer already
  differentially validated the arithmetic: 100/100 vs python crypto).

### Verification
- `make host-tests` RC=0 (crypto/rng/strict/css/subres/pki PASS, 78/78
  adversarial, live PHASE 4+MITM).
- `make host-tests-asan` equivalent: 78/78, zero sanitizer findings (two
  mock-side buffer slips fixed along the way — sh_wire/g_sh_body sizing
  for SH_BIG; production SH cap is what the test targets).
- QEMU final ISO: pki_qemu PASS (live Cloudflare through the entire new
  strictness stack), sh_hello PASS, both ISOs link clean (text needed the
  weak-hook for the keyboard RNG stir — rand.o stays desktop-only).
- test_pki 0 failures (real chains survive serial/params/AKI/RSA-floor).

### Traps (process)
- python one-liners editing C arrays (names/enum) need order verification
  after (caught a positional mismatch by diffing enum vs names).
- Cross-run test globals (pin/ticket stores) need per-mode isolation
  resets wherever leaf identity changes (documented pattern by now).
- Mock seq discipline: encode EXACTLY what is queued, in queue order.
- Fixture clocks stay mtime-relative (adv_clocks) — new fixtures (IP/AKI)
  inherited this for free.

---

## Session 2026-09-10 (late) — P2 round: name constraints, OCSP-staple validation scope, pin persistence, root audit (92/92 + ASan clean)

Worked the review's P2 in priority order. Done: name-constraints
enforcement, honest OCSP scoping (validate staples is a multi-day build —
presence-noted stands, documented), TOFU pin persistence via PFS, root
store audit. Declined with reasons: multi-session architecture (the whole
network stack is single-connection by design — a rewrite, not a hardening
item), const-time assembly audit (needs a dedicated session).

### NameConstraints enforcement (RFC 5280 §4.2.1.10)
- Parser: permitted/excluded dNSName (ASCII-checked) + iPAddress (v4:
  4-byte host or 8-byte addr+mask, stored masked+mask) into capped
  per-type lists; other GeneralName types skipped (documented DNS/v4
  scope); lengths short/long-form capped; unknown-critical still rejects.
- Enforcement (certverify walk, at anchor time): pairwise — every CA's
  constraints apply to every cert below it. Per-type gating (only
  constrained types constrain: CA with DNS-only entries doesn't affect IP
  names and vice versa). DNS: equal-or-subdomain suffix; empty constraint
  matches nothing. IP: masked compare. CN ignored (not a SAN — documented).
  Violations → CV_ERR_CAFLAGS (strerror broadened to "flag/constraint").
- Tests: mint_nc.py (cryptography lib + at_root.key — at_int's pathlen:0
  forbids sub-CAs, so NC intermediates hang off at_root): in-namespace OK,
  permitted-violation CAFLAGS, excluded-namespace CAFLAGS. Fixtures
  at_nc_{int,ok,bad,xint,xlf}.der committed.

### OCSP: enforced scope decision (honest, not theater)
Full OCSP response validation (response parser + responder authorization +
freshness + policy) is a multi-day build for a rarely-observed extension;
only Cloudflare's example.com has ever stapled in our testing, and the
page already discloses non-enforcement. Kept: offer + presence-noted.
NOTED as the explicit next crypto build if revocation becomes a goal
(it needs responder PKI + clock-subtlety work, not an afternoon).

### Pin persistence (PFS-backed TOFU)
- `tls_pin_export/import` (magic OKPIN1/v1/count + host[64]+hash[32]
  entries, bounds-checked, malformed → reject without touching the live
  store) + dirty flag (set on store only).
- desktop: load `/.pins` after userland_seed (malformed → ignore =
  fresh TOFU, never a wedge); save on dirty in the main poll loop.
- Threat model documented at the code: network-only attackers (disk
  writers already own VFS+kernel). Rotation lockout still ends at reboot
  worst-case (file can be deleted); override UX still the follow-up.
- Proven in QEMU with a raw disk image: fetch → "pin store saved" →
  reboot → "mounted: 11 files hydrated" + "restored pin store".
- Unit: export/import round-trip + truncation/magic rejection in the pin
  section (failed imports keep the store).

### Root audit
- `tools/gen_roots.py` re-ran byte-identical (reproducible store).
- 14 anchors cover the field: SSL.com ×2, ISRG ×2 (X1 RSA + X2 P-384),
  GTS ×2, DigiCert G2, Amazon ×2, USERTrust ×2, GlobalSign, Sectigo ×2.
  Anchoring is SPKI-hash (cross-signs work, root-expiry moot for path
  building). Rotation story: re-run generator when the Web PKI shifts.

### Verification
- Adversarial 92/92 (new: SH_TRAIL/SH_DUP/EE_DUP/EE_ALPN_H2/FRAGMENT,
  NST_BIGNONCE, AKI pair, IP×4, hostname×2, NC×3, pin persist×6).
- ASan/UBSan 92/92 clean (fixed 2 mock-side SH_BIG buffer sizes the
  sanitizer caught — production SH cap is what the test targets).
- host-tests RC=0 incl. test_pki (real chains survive ALL new strictness:
  serial/params/AKI/pathlen/EKU/RSA-floor) + live PHASE 4+MITM.
- QEMU final ISO: pki_qemu PASS (Cloudflare through EE-validation, SH
  exact/dup, EKU, AKI, serial strictness), sh_hello PASS, certfail PASS,
  pin save/restore across reboots PASS. Both ISOs link (text needed the
  weak hook for the keyboard RNG stir).

### Traps
- Intermediate-CA fixtures must hang off at_root, not at_int (pathlen:0)
  or they fail the WRONG check (pathlen, not the targeted constraint).
- python edits to C enum/name tables need positional verification
  (caught one duplication + one order mismatch by diffing).
- Mock seq discipline (encode exactly queued, in order) — repeat offender,
  now a code comment at the site.
- Matcher masks must be STORED, not applied-and-forgotten (first NC IP
  draft masked addr in place and lost the mask).

---

## Session 2026-09-11 — P2 OCSP validation + pin persistence (94/94 + ASan clean)

### OCSP response validation (RFC 6960, stapled only)
- New `src/crypto/ocsp.{c,h}`: full BasicOCSPResponse parse with exact
  framing at every level (responseStatus==successful, responseType
  id-pkix-ocsp-basic, version, responderID-byName==issuer-subject
  byte-exact, producedAt hygiene, exactly-one SingleResponse, certID,
  good-status, thisUpdate/nextUpdate, optional single/response extensions
  skipped by length, response signature).
- Trust rule: responder MUST be the issuing CA itself (delegated
  OCSPSigning responders rejected as untrusted — documented, not silent).
  Signature via shared `cert_sig_verify()` (new export; chain walk
  refactored onto it, no behavior change).
- certID: SHA-1 or SHA-256 (new `src/crypto/sha1.c`, NIST vectors in
  test_tls_crypto); issuerNameHash over full issuer Name DER, issuerKeyHash
  over subjectPublicKey bytes (new retained `keybits` view in x509_cert),
  serial compared numerically (padding-insensitive). New `x509_parse_time`
  + `x509_time_add_days` exports; serial view retained on x509_cert.
- Freshness: thisUpdate <= now+1d, nextUpdate (when present) >= now-1d,
  no-nextUpdate bound to 7d. New CV_ERR_OCSP (11, MAX follows).
- Wired after chain+CV+pin in RECV_HS (full handshakes only): staple bytes
  copied bounded (2048 cap, oversize fails the flight), failure → CERT +
  no fallback. Warning page now states the enforced model.
- Mock proofs with REAL openssl-minted responses (runtime `openssl ocsp`,
  responder = at_int, serials read live — regen-safe): good → DONE,
  revoked → CERT, stale (clock +30d) → CERT, malformed → CERT/PROTO.
- Bugs caught by real responses (not theory): ResponseData version is
  OPTIONAL [0] (openssl omits it — my mandatory-INTEGER failed every
  response); nextUpdate is [0] EXPLICIT (not bare); ResponseData may carry
  [1] responseExtensions (echoed Nonce — skipped by length); missed
  ResponseBytes SEQ wrapper (caught instantly by the mock going red).
- Helper `mint_ocsp()` + modes MOCK_STAPLE_REVOKED/STALE in the suite.

### Pin persistence (PFS-backed TOFU)
- `tls_pin_export/import` (magic OKPIN1/v1/count + host[64]+hash[32],
  bounds-checked, malformed → reject without touching the store) +
  dirty flag (set on store only).
- desktop: load `/.pins` after userland_seed (malformed → ignore = fresh
  TOFU, never a wedge); save on dirty in the main poll loop (write-through
  VFS like the editor).
- Threat model documented at the code: network-only attackers (disk
  writers already own VFS+kernel). Rotation lockout ends at reboot
  worst-case; override UX still the follow-up.
- Proven in QEMU with a raw disk: fetch → "pin store saved" → reboot →
  "11 files hydrated" + "restored pin store". Unit round-trip in the pin
  section (truncation/magic rejection keeps the store).

### Root audit
- `tools/gen_roots.py` re-ran byte-identical (reproducible). 14 anchors
  (SSL.com ×2, ISRG ×2, GTS ×2, DigiCert G2, Amazon ×2, USERTrust ×2,
  GlobalSign, Sectigo ×2); SPKI-hash anchoring (cross-signs work, root
  expiry moot for path building). Rotation = re-run generator.

### Silent-TLS-stall episode (undiagnosed flake, honestly recorded)
- Several QEMU runs in a row showed TLS fetches stalling with ZERO bytes
  out (no SEND_CH effect, no FAILED line, eventual desktop-owner timeout
  → HTTP fallback). Host-direct with identical code passed throughout.
- Bisect path walked: port theory (disproven — tcp_connect logged the
  right port), typing theory (disproven — exec log prints first token
  only), malformed-CH theory (ruled out — certfail server got HTTP, and
  the same ISO passed right after). Instrumented HP_TLS/SEND_CH/send
  (all probes then showed the mechanism working), behavior flipped back
  to passing across relinks with ZERO functional changes.
- Leading theory: layout/timing-sensitive silent drop in the
  fire-and-forget send path (kernel_tcp_send returns 0 unconditionally;
  tcp_send_data drops silently when not ESTABLISHED). Recorded hardening
  idea (NOT implemented — undiagnosed flakes don't get shared-path
  surgery): propagate send return codes so SEND_CH fails loud instead of
  stalling into a fallback. All probes removed; tree verified clean.
- Note: coincided with heavy portal DNS interception on this network
  (example.com resolving to 8.47.69.6) — environmental contribution
  likely for the internet legs, but the zero-TX shape is a client-side
  stall regardless of peer.

### Verification
- Adversarial 94/94 (new: OCSP good/revoked/stale/malformed, pin
  persist×6), ASan/UBSan 94/94 clean.
- host-tests RC=0 (crypto incl. SHA-1 vectors, rng, strict, css, subres,
  pki, live PHASE 4+MITM).
- QEMU final ISO: sh_hello PASS, pki_qemu PASS (when the portal allows;
  guest DNS currently intercepted — see episode), certfail PASS ×3
  (reason=4, no fallback), pin save/restore across reboots PASS.
- test_certfail.py hardened with type-verify-retry (HMP shift-punctuation
  flakes); note its "url typed" check matches the exec echo (vacuous for
  args — kept as smoke, the real assertion is the fetch outcome).

### Traps
- OCSP ResponseData version is OPTIONAL ([0] EXPLICIT, usually absent);
  nextUpdate is [0] EXPLICIT; responseExtensions [1] really occur
  (Nonce) — parse all three or die on real responses.
- `openssl ocsp` needs index + rsigner bundle (key+cert cat) + serials
  read live; `-ndays` can't backdate (use clock shifting for stale).
- Fixture clocks stay mtime-relative; OCSP fixtures mint at test runtime
  (never committed) except AKI/NC/IP DERs (mint_*.py committed).

---

## Next steps (2026-09-11 — live session, in progress)

### 0. Land the current tree (uncommitted)
- `M src/crypto/tls_client.c, src/okai.c, src/process.h, src/string.c` +
  `?? tests/test_tls_attacker.c` (test_links/usermode/port-fix session —
  verify green, then commit; message names the three items).

### 1. Independent-stack interop loop (IN PROGRESS, then verify + commit)
- Harness: portable Temurin JREs in `~/tls-att/` (17 present, **21 is the
  one that runs the jars** — class v65), `apps/TLS-Server.jar`,
  `tests/test_tls_attacker.c` (BSD sockets + `tls_client_run` + host-only
  trust hook + wall clock + 25s deadline; unbuffered stdio).
- DONE: full handshake vs CPython/OpenSSL 3.0 serving our test chain —
  chain+hostname+CV verified, NST stored (192B/7200s), HTTP body received,
  close_notify clean. Proves the whole client against a second stack.
- TODO: RSA-leaf + P-384-leaf interop (bundles in `/tmp/opencode/`
  — REBUILD as `~/tls-att/*bundle.pem`, /tmp gets wiped); resumption
  double-run (call `tls_client_run` twice in-process → abbreviated?).
- TLS-Attacker server sends a CANNED 77B ServerHello (no key_share/sv —
  correctly rejected); real handshakes need `-workflow_trace_type` config
  research (time-boxed, else defer — openssl + mock cover the space).
- Housekeeping: kill stray `s_server`/java processes by PID (`echo $!`),
  NEVER `pkill -f <pattern>` (matches your own shell → suicide, observed
  twice); driver must keep the overall deadline (blocking `tls_client_run`
  + timeout-0 recv spins forever otherwise).

### 2. Trust-anchor termination gap (REAL BUG found via interop)
- `certverify` anchors ONLY on in-flight SPKI match. Flights without a
  root-key cert (openssl serving [leaf,int], i.e. most of the real web)
  fail `CV_ERR_ROOT` even with a valid chain to a stored root.
  Cloudflare works only because it ships a cross-cert.
- Fix: store root subject Names in `roots.c` (extend `gen_roots.py`),
  anchor by name match + verify with stored root key. Tests: mock 2-cert
  flight (needs a `trust_extra`-style name hook for the test root).
- Pre-req for calling this a general browser stack.

### 3. Verify + commit
- `make host-tests`, `make host-tests-asan` (slow), both ISOs link clean,
  QEMU: `test_sh_hello.py`, `test_pki_qemu.py` (portal permitting),
  `test_certfail.py`, `test_links.py`, `test_resume.py` as network allows.

### 4. Deferred backlog (unchanged — oldest first)
- OCSP delegated responders; pin override UX; path policies / full
  constraints beyond NC; const-time assembly audit; X25519 extra vectors.
- Rapid re-`okai` same-URL parse miss (desktop fetch-owner race).
- `test_links` LINK leg + `test_resume.py` need real WiFi (offline legs
  covered by `test_link_local.py`).
- AES-GCM (0x1302) — excluded unless a target site refuses ChaCha20.

---

## Interop session results (2026-09-11 late — lands as commit below)

### Proven (independent stacks)
- Full TLS 1.3 handshakes vs CPython/OpenSSL 3.0 AND vs a from-scratch
  pure-Python server (`/tmp/opencode/pyserver.py`, itself validated by
  `openssl s_client` completing handshake+app-data against it):
  P-256/ECDSA, RSA-PSS, P-384/ECDSA leaf chains — chain+hostname+CV live,
  NST stored, HTTP body, close_notify clean. `make tls-interop` builds
  `build-host/t_tlsa [port] [twice] [root.der]` (one root per process —
  the host trust hook holds a single SPKI hash).
- Live ABBREVIATED handshake vs pyserver: `t_tlsa 4447 twice` → PASS x2
  (BINDER OK server-side, EE+Finished only, app data). Resumption logic
  proven correct end-to-end (offer/binder/abbreviated/resumption keys).

### Real PSK-offer bugs found + fixed (all covered by suite now)
1. Missing PskBinderEntry u8 length prefix (binders u16=32 not 33) —
   servers ignored the PSK (clean fallback hid it).
2. psk_key_exchange_modes value 2 (invalid; correct psk_dhe_ke=1) —
   servers decline (mock now checks modes and would have caught it).
3. ClientHello1 truncation off by up to 2B (`binder_off-4`, then `-7`;
   correct `-5` = values_off - 3 (u16+u8) + 2 (u16 kept) - 4 (HS hdr)).
   Mock's parse-driven `body_len-33` was right all along; the 1-test
   failure after fix #1 exposed it.
- Mock `mock_check_psk_offer` now enforces RFC framing (33B/u8=32),
  modes (dhe present), and independent binder recompute.

### Pyserver bugs found (test-harness-only, all fixed there)
- AAD length off-by-one (inner content length forgot the +1 type byte;
  RFC S5.2 AAD uses OUTER header: 0x17 + ct_len incl. tag).
- AAD type byte: outer 0x17, not inner 0x16.
- Must echo client legacy session_id in SH (else `invalid session id`).
- CCS payload is exactly 1 byte (`01`), not a header echo.
- ALPN echo only when solicited (else `unsolicited extension`).
- App-traffic seq RESETS per key change (RFC S5.3) — continuing seq
  breaks every client; ours was right (`s_ap_seq=0` after flight seq 0).
- Our empty-context `tls_derive_secret` for master confirmed correct
  (s_client keylog cross-check); BOTH Derive "derived" steps use "".

### OPEN: OpenSSL declines our PSK (deterministic, unexplained)
- `t_tlsa 444[35] twice` round 2 → `tls_psk_do_binder:binder does not
  verify` (fatal illegal_parameter), s_server AND python-openssl-server,
  age 0 AND age 4s, all chains with matching roots.
- Our offer is byte-perfect per RFC (verified over s_server's own `-msg`
  hex dump: framing strictly valid, modes=01, binders 33/32, age honest,
  ticket+nonce wire-exact) and verifies 4 ways (self-consistent KDF,
  Python recompute, mock independent recompute, pyserver BINDER OK).
- s_client resumes fine against the same servers (control passes).
- Ruled out: framing, modes, binder math, res_master (pyserver-identical
  + proves ours), nonce/ticket (wire-exact), age 0 (4s also rejected),
  ALPN (s_client+ALPN resumes), slots (single-slot overwrite, pairs
  intact), cipher/groups/SNI (same-or-superset), transience (3/3 deterministic).
- Next probes (not yet tried): minimal-CH bisection (drop status_request
  / shrink cipher list — no mechanism, but cheap); read OpenSSL
  `tls_parse_ctos_psk` source when available; test a second independent
  client stack against the same servers (Botan? GnuTLS?) to see if the
  decline is OpenSSL-policy (not our bytes).
- Resumption stays PROVEN (pyserver + mock matrix); OpenSSL-accept is
  the open item, not resumption-correctness.

### Housekeeping (learned hard)
- NEVER `pkill -f <pattern>` (matches own shell → suicide ×3); kill by
  PID from `ss -ltnp` (`grep -oP "pid=\K[0-9]+"`).
- Connection aliasing across shell calls is the #1 time sink: always
  match artifacts by client_random, single-conn logs, atomic commands.
- `openssl s_server` serves ONLY the leaf from `-cert` (no chain!) —
  use `-cert_chain <int+root>` for full flights.
- s_server `-naccept N` EXITS after N accepts (mysterious refusals);
  use large budgets. Single-threaded python servers wedge on half-open
  conns (tee proxies!) — restart freely, never tee a single-threaded
  server without half-close handling.
- CPython does NOT read SSLKEYLOGFILE env — set `ctx.keylog_filename`.
- OpenSSL ticket nonces observed as counters (`00..00`, `00..01`);
  two NSTs per handshake; tickets 192B (openssl) — all server-opaque.

---

## Cryptoholes review disposition (2026-09-12, commit 3035064)

Fresh external review (`cryptoholes.txt`, 7 findings) verified item by
item against the tree, fixed, and re-attacked. Adversarial 103/103
plain + ASan/UBSan, host-tests green, ISOs link, sh_hello PASS, interop
regression green (P-256/RSA/P-384 full HS vs OpenSSL, resumption PASS
x2 vs pyserver, TLS-Attacker canned flight still rejected).

1. **RNG labels (Critical) — FIXED (tiered model).** Old gate (64B from
   any 2 labelled classes) stood. New: declaring reseed needs window
   bytes >= 64B (hw) / >= 128B (no hw) + >= 2 window classes at >= 32B
   each + lifetime >= 2 incl. hardware bit (hw) / >= 3 BOOT+TIMER+INPUT
   (no hw). RDSEED class (0x10, tried first) added; hw tier requires
   actually-stirred hw bytes; NIC RX stirs INPUT timing (keyboard
   already did). Labels bounded to 0x1F (no invented classes).
   Verified live: QEMU guest (hw=0, fail-closed at boot) completes a
   verified TLS handshake after typed input (chain verified in serial).
   Residual risk documented in rand.h (deterministic-VM caveat).
2. **NC overflow fail-open (High) — FIXED.** 5th+ constraint per list
   now fails the parse (was silently dropped).
3. **secure_zero missing (High) — ALREADY FIXED** (volatile wipe in
   src/string.h, review #30). No action.
4. **directoryName/DN + unsupported NC forms (Medium) — FIXED.**
   Any NC GeneralName outside dNSName/IPv4-iPAddress fails the parse
   (covers directoryName, rfc822, URI, otherName, x400, edi, IPv6).
   Narrow-validator contract documented in x509.h; public-web risk
   (constrained intermediates with exotic NC) accepted per review.
5. **SAN/NC truncation (Medium) — FIXED.** Relevant SANs past caps
   (16 DNS / 4 IPv4) fail; whole list still walked (late malformed
   entries still fail). Irrelevant SAN forms + v6 SANs still skipped
   (no match semantics — safe). Fixtures: at_nc_over/dir/ip6 (+leaves),
   at_san_over (17 SANs); mint_nc.py mints hermetically off the frozen
   leaf-mtime base (see below), 8 new CHECKs.
6. **Padding parser (Medium) — FALSE POSITIVE, documented in code.**
   Strip-first is provably correct (type byte last+nonzero stops the
   strip; content never eaten). Type-first BREAKS close_notify
   ([01,00]+21 strips the description byte — demonstrated: flipping
   to type-first fails the CLOSE_NOTIFY suite test). Review's example
   ([41,00]+17) parses correctly under strip-first.
7. **Ticket clockless freshness (Low/Med) — FIXED.**
   `tls_ticket_have(host, now_ms)`; 0/unknown is NOT fresh. Offer path
   unchanged (already gated). Test asserts have(...,0)==0.

Collateral fix (environmental, not the stack): OCSP good-staple test
reddened when fixture mtime-clock drifted >24h behind wall minting
(1d skew tolerance) — STAPLE modes now run on wall clock (chain
windows are 10y wide); mint_nc.py fixtures mint off the frozen base
so re-regens never rot.

---

## Cryptoholes round 2 disposition (2026-09-12, commit 7cfd158)

Second external review (8 findings + RNG note). Verified each against
the tree, fixed, re-attacked. Adversarial 113/113 plain + ASan/UBSan,
host suites green, ISOs link, sh_hello PASS, interop green (P-256/RSA/
P-384 full HS vs OpenSSL, resumption x2 vs pyserver, TLS-Attacker
canned flight still rejected with new SH-reassembly code).

1. **EOF/truncation as success (Critical-ish) — FIXED (layered).**
   TLS: mid-record EOF (rec_have!=0) is now fatal PROTO (no fallback);
   authenticated close_notify sets saw_close (snapshotted past wipe,
   accessor for desktop). New pure helper tls_response_complete():
   COMPLETE (C-L satisfied / chunked terminated) vs SHORT (headers cut,
   short body, unterminated chunks) vs UNKNOWN (close-delimited,
   curl-parity accept). Desktop: SHORT with no close renders a distinct
   TRUNCATED warning (new T->truncated flag, never falls back);
   truncated sub-resources are skipped, never fed to parsers. Tests:
   MOCK_APP_TRUNCATED (mid-record EOF must ERR) + 8 helper unit CHECKs.
2. **Build integrity (High) — ALREADY CLEAN.** No .o tracked; .gitignore
   covers *.o/*.bin/*.iso/isodir/build-host. secure_zero present.
   Nothing to do.
3. **AEAD static scratch (Medium) — FIXED (real streaming).** Poly1305
   init/update/final API (partial-block carry, no statics); one-shot
   reimplemented on top (RFC vectors still pass); AEAD cap kept as a
   pure u64 length check (t_tls_crypto oversize test still passes).
   Fully reentrant now. Streaming equivalence proven across pad-
   alignment classes + byte-at-a-time feeding (new chachapoly vectors,
   wired into host-tests).
4. **Chain order (Medium) — DOCUMENTED limitation.** RFC 8446 §4.4.2
   mandates leaf-first order; we fail closed on anything else
   (availability, never auth). Path building awaits name-bound anchors.
5. **SPKI-pin leaf self-match (Medium) — HARDENED + DOCUMENTED.**
   Hostname/validity/strength/KU/EKU already precede pin-match; added
   self-signature verification on pin-match (forged root-key leaf
   fails without the root key — holds even if CV were skipped) +
   prominent trust-model contract in roots.h/certverify.c.
6. **RSA e<n (Low/Med) — EXPLICIT.** Implied by floors (e<2^32<=n) but
   now enforced directly (future-proof if floors ever change).
7. **SH fragmentation (Medium) — FIXED.** RECV_SH reassembles across
   records (4KB cap, trailing rule kept); hs_buf lifecycle made
   explicit at both transitions. New MOCK_SH_SPLIT test.
8. **KeyUpdate (Low/Med) — DOCUMENTED minimalism.** Short single-fetch
   connections never approach rekey volume; unknown post-HS traffic
   fails closed (safe default, may abort against chatty peers).
9. **RNG uninit ChaCha input (Low) — FIXED.** Both rand.c call sites
   zero the block first (pure keystream, MSan-clean). Added soak test
   (200 reseed-crossing draws advance, ready latches), hw_init smoke,
   u32 counter-exhaustion fail-closed guard.

---

## TLS-Attacker interop (2026-09-12 — full success this time)

Canned workflows are TLS1.2-only (fixed SH random `60b4…`, only
renegotiation_info ext — correctly rejected; debug log proves
server-side cause: HighestClientProtocolVersion detected as TLS12).
Real handshakes need `-workflow_input` with a custom trace
(`tests/tlsattacker-server13.xml`, schema learned from
`-workflow_output` dumps + JAXB error messages which name exact
expected elements: `configuredMessages`, `SupportedVersions`,
`KeyShareExtension`, `Application`).

Results (Java/BouncyCastle crypto — third independent stack):
- Full HS P-256/ECDSA, RSA-PSS, P-384/ECDSA-SHA384: all PASS
  (chain+hostname+CV live, HTTP body, clean close). Recipe in the
  Makefile comment (full chain WITH root via -cert; explicit
  -signature_hash_algo/-signature_algo_cert per key type — the
  default CV selection emits RSA-PSS even for EC keys).
- Mutilated flights (Certificate dropped / CertificateVerify
  dropped): both correctly rejected (PROTO order enforcement vs
  real-stack messages).
- TCP_FRAGMENTATION delivery: PASS (SH + flight reassembly).
- Resumption 3rd opinion still blocked: no session continuity across
  connections in single workflows (canned PSK flows can't complete
  round 1; forging an accepting workflow would skip verification and
  prove nothing). OpenSSL-decline question unchanged; our offer is
  proven-correct vs pyserver + mock + self-consistency.

---

## Caveat fixes (2026-09-12, commit above)

User asked to fix three of the four stated caveats.

1. **Revocation gap — FIXED (two layers).**
   (a) Must-Staple (RFC 7633): TLSFeature status_request(5) parsed in
   x509.c (numeric-5, fail-closed framing); leaf asserting it without a
   valid staple fails the handshake (CV_ERR_OCSP bucket). Fixture
   at_ms_leaf + MOCK_MUSTSTAPLE (refused, right reason logged) +
   MOCK_MUSTSTAPLE_OK (stapled completes). Matches industry direction
   (no live OCSP fetching — privacy/latency/soft-fail reasons as used
   by Chrome/Firefox CRLSet/CRLite models).
   (b) Serial blocklist (CRLSet-nano): (issuer-SP Hash, serial) table
   in certverify.c (8 slots, numeric serial compare, export/import
   OKRVK1 format mirroring pins); checked for leaf+in-flight
   intermediates at anchor time (CV_ERR_REVOKED + UI reason line);
   /.revoked loaded at boot (malformed = ignored, never a wedge).
   11 new CHECKs (add/clear/wrong-issuer/import/export/roundtrip).
2. **First-visit MITM — NARROWED (preloaded pins).** Root pinning
   already forces CA-level capability (network-only attacker gets
   nothing, first visit or not); persistent TOFU alarms key changes.
   Added compiled first-visit pins (exact host, no expiry — expiry
   would silently unprotect; rotation bricks loudly to the warning
   page until updated, same UX as key-change): github.com,
   cloudflare.com, www.ssl.com + test-only entry. New CV_ERR_PRELOAD
   + UI line. Verified: mock PRELOAD_OK/BAD (exact detail code),
   production pins byte-exact vs live certs through our own parser,
   and full live production path — real www.ssl.com chain verifies
   against EMBEDDED roots with no hooks (first time) + preload
   matches. tools/gen_preload.py generates entries (hand-typing
   hashes caused one wrong-pin incident — always generate).
   Full CT/SCT verification deferred with reasons (precert TBS
   reconstruction brittleness; curl/OpenSSL/Go all skip it too).
3. **Deterministic-VM randomness — FIXED (two layers).** Boot jitter
   loop (port-I/O + memory churn RDTSC deltas, BOOT class) + PFS seed
   file (/.rngseed: stirred at boot if present, refreshed with fresh
   output once ready per boot — Linux random-seed pattern; disk-read
   attackers out of scope per existing threat model). Verified live:
   QEMU guest (no RDRAND) completes verified TLS after typed input.
4. **Close-delimited truncation — NOT FIXABLE** (protocol-inherent;
   curl/browsers identical). Covered as far as possible (close_notify
   + framing checks); HTTP/2 would fix it properly (future).

---

## Cryptoholes round 3 disposition (2026-09-12, commit 6ae95cc)

Third review (score 6.5/10). Headline P0 (X25519 timing) fixed and
proven; KAT/fuzz gaps filled; assembly inspected. Adversarial 130/130
plain + ASan/UBSan, host suites green, ISOs link, sh_hello PASS,
interop green (P-256/RSA/P-384 vs OpenSSL, resumption x2 vs pyserver,
TLS-Attacker full-HS x3 key types + mutilation-reject + fragmentation).

P0 — X25519 secret-dependent branch: FIXED. fe_mul121665 fold is now
fixed-16-iteration (was `if (carry)` + `&& t`); fe_tobytes drops two
`if (c)` guards and masks the negativity + revert/keep selects
(SAR/ALU, cmov-friendly, no reliance on cmov). Proven: 500-case
LCG differential vs old code BIT-IDENTICAL + RFC 7748 vectors +
DH-commutativity x8 committed. (One revision dropped the vv>>16
feedback — wrong for negative carry whose SAR saturates at -1;
bisected by the differential, reverted to fixed-count original body.)
Disassembly under real kernel flags (-O2 -fno-pic etc.): every forward
conditional jump is a fixed-trip loop exit; ladder cswap/bit-selects
compiled to cmov/cmovne; fe_invert's public-exponent bit-test needs no
secret handling. volatile secure_zero survives as per-byte movb stores
(no memset folding, no rep stos).
Low-order points: clamp (priv = 0 mod 8) kills the whole small
subgroup to exactly zero — the existing all-zero caller rejection
covers orders 1/2/4/8 completely. Committed u=0/u=1 zero-output tests
+ threat-model note (ephemeral client-only: malicious peer gains
nothing; API stays as-is with documented rationale).

P1 — KAT gaps: FILLED from independent sources (openssl CLI, stdlib
HMAC/HKDF). SHA-256 (3), SHA-384/512 (2+2), HMAC-4231 TC3/TC4/TC6
(big-key path)/TC7, HKDF-5869 TC2 (PRK+OKM)/TC3 (zero salt/info),
plus DH-commutativity. RSA-PSS/ECDSA already differential vs openssl
CLI signing in the mock. (Caught a test bug this way: TC6 length
54 vs 55 — vectors validate the tests too.)

P1 — fuzz gaps: EE + Finished direct garbage rounds added (all listed
surfaces now covered: DER/X.509/Cert/SH/EE/CV/Finished/OCSP/NST/
record/SH-psk/staple). libFuzzer/MSan unavailable (no clang
toolchain) — ASan+UBSan + 4000-round deterministic fuzz stand;
documented, not fixable here.

P2 — bespoke crypto: stays (freestanding kernel, nothing linkable).
Compensated per the review's own prescription: equivalence vs
independent implementations (OpenSSL/pyserver/TLS-Attacker interop +
RFC/NIST vectors) + fuzz corpus. Documented as deliberate.

Also in this round: SH fragmentation reassembly (REQ from this
review's list; MOCK_SH_SPLIT test), truncation completeness section
(8 unit CHECKs), APP_TRUNCATED mock. TLS-Attacker battery re-run
green on the final tree (full HS x3, mutilations rejected,
fragmentation). Resumption 3rd opinion still blocked (no session
continuity in single workflows) — unchanged, documented previously.

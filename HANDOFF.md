# KAnarchy OS — Handoff Document (okernel kernel)

## What is this?

KAnarchy OS is a from-scratch operating system built in C and x86 assembly, powered by the okernel kernel. It has two modes:

- **Text mode** (`make text`) — VGA text terminal with commands, scrolling, terminal multiplexing
- **Desktop mode** (`make desktop`) — 1920x1080x32bpp graphical desktop with windows, mouse, okai (browser), editor, shell, networking, and a TLS 1.3 client

Current version: **v0.8** (desktop edition). Highlights since v0.7:
- Real document rendering: a **DOM tree + from-scratch CSS cascade + display-list layout engine** (replaces the old flat token renderer for pages)
- **External stylesheets are fetched and applied**; external `<script>` is fetched but **not executed** (see below)
- High-half kernel, preemptive scheduler, ring-3 processes, VFS, PFS (persistent FS on ATA), full TLS 1.3 + PKI with adversarial hardening

---

## Project Structure

```
okernel/
├── boot/
│   ├── start.asm          # Low trampoline _start (paging off) + private boot GDT; jumps to _start_high
│   ├── isr.asm            # ISR/IRQ stubs, GDT flush, INT 0x80 gate, enter_user_mode, context_switch
│   └── user_test.asm      # Ring 3 test program (legacy; spawn_user path retired 2026-09-10)
├── src/
│   ├── kernel.c           # Text-mode entry (terminal-only build)
│   ├── desktop.c          # Desktop entry + main loop + shell commands + fetch driver
│   │
│   ├── graphics.c/.h      # Framebuffer, font, primitives, dirty-row flush, clip rect
│   ├── window.c/.h        # Window manager, PS/2 mouse driver, stateless cursor sprite
│   ├── paging.c/.h        # High-half page tables, paging_map_user, user-range validation
│   ├── memlayout.h        # KERNEL_VBASE/V2P/P2V_U32/IS_HIGH
│   │
│   ├── okai.c/.h          # Browser shell: tabs, URL bar state, fetch driver (okai_poll),
│   │                      #   page surface, input routing, forms, error/home pages
│   ├── okai_ui.c          # Browser chrome (Firefox-style): layout, AA icons/text, hit tests
│   ├── web/               # Web engine (desktop-only; see "Browser (okai)" below)
│   │   ├── wdoc.c/.h      # Document controller: load, sub-resource queue, update, paint, hit
│   │   ├── wdom.c/.h      # Heap DOM (int32 links, attrs, text arena, atoms)
│   │   ├── html5.c, charset.c, entities.h   # WHATWG tokenizer + tree builder, decoders
│   │   ├── css_*.c, css.h, css_int.h        # CSS parse/select/cascade/computed values
│   │   ├── lay_*.c, layout.h, lay_int.h     # Box tree, block/inline/float/abs, flex, grid, table, display list
│   │   ├── paint.c, raster.c, surface.h     # Painter + AA rasterizer into a 32bpp surface
│   │   ├── font.c, fontdata.asm             # TrueType engine + embedded Noto subsets (fonts/)
│   │   ├── image.c, svg.c                   # PNG/JPEG/GIF/BMP + inflate/gzip, SVG renderer
│   │   ├── wurl.c                           # RFC 3986 URL resolution
│   │   └── callstack.asm                    # call_on_stack: engine runs on its own 1MB stack
│   ├── textslot.c/.h      # Codepoint -> bitmap-font slot (editor/terminal; split from old html.c)
│   ├── editor.c/.h        # Text editor (open/edit/save)
│   ├── filesystem.c/.h    # In-memory VFS (write-through hooks to PFS)
│   ├── pfs.c/.h           # OKPFS1 persistent FS on ATA (mount-or-format + hydrate)
│   │
│   ├── gdt.c/.h           # GDT + TSS (user segments, ring 3)
│   ├── idt.c/.h           # IDT, PIC remap, IRQ dispatch, INT 0x80 gate, GP error decode
│   ├── keyboard.c/.h      # PS/2 keyboard (scancode set 1)
│   ├── mouse.c/.h         # PS/2 mouse (IntelliMouse 4-byte wheel negotiation)
│   ├── process.c/.h       # Process table (16 slots), private PDs, esp0/ticks, handoff switch
│   ├── sched.c/.h         # Slice accounting, sched_yield, sched_spawn_user, prepare/reap
│   ├── syscall.c/.h       # INT 0x80 ABI 0-17 (print/exit/write/read/open/close/fork/exec/sbrk/
│   │                      #   pipe/dup/wait/kill/mmap/munmap)
│   ├── sys_proc.c/.h      # fd table over VFS + pipe rings, fork/exec, wait/kill/signals
│   ├── elf.c/.h           # ET_EXEC/i386 ELF loader
│   ├── spinlock.c/.h      # xchg spinlock + IRQ-safe variants + mutex (locktest selftest)
│   ├── ata.c/.h           # Polling-PIO LBA28 primary-master disk
│   │
│   ├── memory.c/.h        # Physical page allocator (bitmap) + segregated-fit heap (kfree works)
│   ├── serial.c/.h        # COM1 + serial_printf
│   ├── string.c/.h        # Freestanding mem*/str* (used by crypto + JS)
│   ├── rtc.c/.h           # CMOS real-time clock
│   ├── userland_seed.c    # Embedded seed files (e.g. /bin/hello) for the VFS
│   ├── io.h               # inb/outb/inw/outw
│   │
│   ├── vga.c/.h, terminal.c/.h, shell.c/.h   # Text-mode build only
│   │
│   ├── net/
│   │   ├── pci.c/.h       # PCI enumeration
│   │   ├── e1000.c/.h     # e1000 NIC (TX + RX)
│   │   ├── rtl8139.c/.h   # RTL8139 (TX only; RX broken — unused)
│   │   ├── network.c/.h   # ARP, IP, ICMP, UDP, TCP, DNS, HTTP (dechunk), TLS RX hook
│   │   └── tls_net.c/.h   # Async HTTPS fetch wrapper (DNS+TCP+TLS), kernel_tcp_recv
│   │
│   ├── js/                # "tinyjs ok edition(tm)" — C port of tiny-js (MIT), rewritten
│   │   ├── js_os.h/.c     # Host/kernel shim (kmalloc, printf, strtod/dtoa; -DJS_KERNEL)
│   │   ├── js.h, js_var.c, js_lex.c, js_parse.c, js_funcs.c, js_math.c
│   │   ├── js_dom.c/.h    # js_init/js_run + DOM bridge (getElementById, querySelector,
│   │   │                  #   setText, setStyle, setAttribute, appendChild, body)
│   │   └── tests/*.js     # Upstream tiny-js test scripts
│   │
│   └── crypto/            # TLS 1.3 + PKI (host-tested; integrated in kernel)
│       ├── sha256.c, sha1.c, sha512.c         # hashes
│       ├── chacha20.c, poly1305.c, aead.c     # AEAD (Poly1305 streaming API)
│       ├── hmac.c, hkdf.c                     # MAC + key schedule
│       ├── x25519.c                           # RFC 7748 (constant-time, fail-closed API)
│       ├── der.c                              # DER parser
│       ├── rsa.c, ec.c                        # RSA-PSS/PKCS1 + ECDSA (P-256/P-384)
│       ├── x509.c, certverify.c, roots.c      # X.509 parse, chain verify, embedded roots
│       ├── ocsp.c                             # Stapled-OCSP validation
│       ├── tls_record.c, tls_handshake.c, tls_keysched.c, tls_client.c
│       ├── rand.c/.h                          # ChaCha20 CPRNG (tiered, fail-closed)
│       └── memwipe.h, tls_dbg.h, roots.h
│       
```

Shared between builds: `gdt.c`, `idt.c`, `memory.c`, `serial.c`, `keyboard.c`, `mouse.c`, `string.c`, `syscall.c`, `paging.c`.
Desktop-only: everything else above. **Networking, TLS, the web engine (src/web), JS engine, okai are desktop-only.**
If you touch shared code, build **both** targets.

---

## Architecture

### Boot Sequence
1. GRUB loads the kernel; desktop links high at `0xC0100000` (`linker-high.ld`; text keeps `linker.ld`).
2. LOW `_start` (phys `0x100030`, paging off) builds a private boot GDT + boot PD (4MB PSE pages: phys 0-64M identity at PD 0-15 AND high at PD 768-783 — the image + .bss outgrew a single 4K-page table once fonts/engine landed; the boot stack lives in .bss), then `_start_high` sets the high stack and calls `kernel_main()`.
3. Order in `kernel_main()`: memory → mboot copy (fb@88/pitch@96) → `paging_init` → process table → graphics → windows → mouse → keyboard → networking → CPRNG seed → `sti` → main loop.
4. Desktop asks GRUB for `gfxpayload=1920x1080x32`.

### Memory Layout
- Kernel physical base 0x100000, virtual base 0xC0000000 (`KERNEL_VBASE`). `V2P`/`P2V_U32` in `src/memlayout.h`.
- `paging_init` builds full low identity (0-128M, supervisor) + high alias (PD 768-799) + FB/MMIO supervisor mappings. The framebuffer is above 4MB (typically `0xFD000000`) and is **identity-mapped**, never via `paging_map` (a window map faults).
- Page tables: user-low is per-process (private PD, copy via high alias); kernel-high (PD 768-1023) is shared.
- Image: ~2.8MB text (1.5MB embedded fonts) + ~7.9MB BSS; `_kernel_end` ≈ 11.3MB physical. The old per-tab okai arrays (~37MB) are gone — page data lives on the heap (DOM/styles/layout per `struct wdoc`, page surface per window). Keep big tables on the heap, not in BSS.
- Heap: segregated-fit, coalescing `kfree`, RAM-sized (414MB at `-m 512`, ~54MB at 128MB); `heap_stats()` / shell `mem`.
- NIC RX/TX buffers live in low memory (`0x80000-0x9FFFF`) for DMA.

### Display Pipeline
1. All drawing goes to a `kmalloc`'d backbuffer (8MB).
2. Dirty-row tracking: `graphics_mark_dirty(y)`; `graphics_flush()` copies only changed rows.
3. Clip rectangle (`graphics_set_clip`/`graphics_clip_reset`) restricts repair drawing so small repairs dirty few rows.
4. Window drag = backbuffer blit (`graphics_blit_rect`) + exposed-strip repair; resize = band repair; content buffers are slack-allocated with fixed stride (`CONTENT_COLS_MAX`).
5. Result: hundreds of FPS idle/dragging/resizing at 1920×1080.

### Cursor Compositor — DO NOT reintroduce saved-pixel cursors (v0.3.1 artifact fix)
> **This is the single most important invariant in the window/mouse code. Agents
> have repeatedly "optimized" it back to a saved-background cursor and
> reintroduced the artifacts. Read this before touching cursor drawing.**

The cursor is a **stateless sprite** — there is NO saved background patch and no
save/restore pair. This is deliberate. The previous save/restore design caused
**persistent artifacts** (arrow ghosts left behind on screen, wallpaper-colored
holes punched into windows) because:
1. the saved patch went **stale against scene changes** made underneath it between
   save and restore, and
2. **IRQ12 could tear it mid-read** (the mouse ISR mutates position while the main
   loop was copying the patch).

Rules that keep it correct:
- Main loop order (in `desktop.c`): `desktop_paint_rect(old cursor rect)` → scene
  draws → `mouse_paint_cursor()` → `graphics_flush()`. **Nothing draws after the
  sprite.**
- **`desktop_paint_rect(x, y, w, h)`** — the single authoritative repair path:
  wallpaper → icons → windows back-to-front → taskbar. Recomposites any region
  from the scene model. Also used to erase the cursor.
- **`window_paint_region(id, rect)`** — repaints a window from its content model
  clipped to a rect (frame/title are redrawn whole — over-repair is harmless;
  content cells are clipped).
- **`mouse_get_position()`** — atomic position snapshot (`cli`/`sti` around the
  two-word read; the ISR mutates it on IRQ12).
- Blinking text-cursor state is a **pure function of `tick_count`** (per-window
  `last_cursor_visible` caches the phase; no global statics).
- **Invariant:** backbuffer = render(model) + one cursor sprite drawn LAST. If you
  reintroduce saved-pixel cursors (`cursor_bg`) or draw anything after
  `mouse_paint_cursor()`, artifacts WILL return.

### Window content cells
A window can instead show a **client pixel surface** (`window_set_pixels`, `window_dirty_pixels`): the content area blits those pixels (occlusion-clipped like cells) and fills the rest with `content_bg_rgb`. okai uses this for the page; the cell grid is unused there.

The window interior is otherwise a **fixed character-cell grid** (`CONTENT_GW=12`, `CONTENT_GH=24`, `CONTENT_COLS_MAX` stride). Each cell carries VGA nibbles + per-cell RGB planes (`cell_fg`/`cell_bg`/`cell_attr`). `window_write_cell_rgb` writes RGB; `cell_attr` (BOLD/UL) routes to `draw_char_cell` which draws a styled glyph.
**Gotcha:** `draw_char_cell`/`draw_char_sized` now paint the cell **background even when the glyph is NULL** (control char / uninitialized) — skipping it left stale backbuffer pixels.

### Input Pipeline
- PS/2 keyboard → IRQ1 → scancode → ASCII → focused surface.
- PS/2 mouse → IRQ12 → 3- or 4-byte packets → 4-sample smoothing → cursor.
- IntelliMouse 4-byte wheel mode IS negotiated in `mouse_init_fb`. The 200/100/80 magic + `0xF2` handshake must wait for the ACK **and** the ID byte in order — draining then reading desyncs the PS/2 stream (movement dies mid-session). See Critical Bugs #25.
- Mouse smoothing means scripted moves need ~4× dilation; use `vm.burst()` in headless tests.

### Scheduler & Processes
- Preemptive via `process_switch_to` from the timer IRQ (`sched_tick`), through the `context_switch` stub in `isr.asm`.
- Guards: never switch from ring-3 trap stacks (defers), never into un-entered/BLOCKED/ZOMBIE slots, `switch_busy` closes the drain window, per-slot `generation` aborts reuse-under-cli.
- `sched_yield()` runs on the INT 0x80 trap stack and **never** switches CR3/ESP0 there (slice reset only).
- `process_switch` (lightweight CR3+ESP0) is handoff-point-only (entry/exit drains).
- **PCB offsets: use `offsetof` only; new fields go at the TAIL.** `idt.c`'s exec-redirect reads `user_eip/esp` via `offsetof`; a past hardcoded index crossed EIP/ESP when a field was inserted mid-struct.

### Syscall ABI (INT 0x80)
0=print, 1=exit, 2=write, 3=getpid, 4=yield, 5=mmap_user, … through 17 (see `syscall.h`).
- All user pointers go through `paging_user_range_valid()` (present+U/S at both levels) before dereference — a bad ring-3 pointer prints an error, never faults the kernel.
- `enter_user_mode` uses register convention (eip→EAX, esp→EDX, no stack args). `sys_exit` resumes via `user_exit_trampoline`.
- Return values are written through the saved-EAX slot. Desktop-only services are reached via hooks (text build fails safe).

### Networking Stack
1. **PCI** enumeration finds e1000 (8086:100E).
2. **e1000**: MMIO TX+RX, **polled** from the main loop (interrupts masked — the loop never halts, per-packet IRQs only cost exits). 256 RX / 64 TX descriptors with buffers on the heap (`V2P` of a heap block is its contiguous physical address). TX checks the descriptor's DD before reusing it. Per-packet logging only with `net_trace` (shell `nettrace`).
3. **ARP** (4-entry cache) → **IP** → **ICMP** → **UDP** → **DNS** → **TCP**.
4. **DNS**: 32-entry cache (`dns_lookup`: 1 resolved / 0 in flight / -1 failed), concurrent queries matched by transaction id (the id is kept across resends: SLIRP's host resolver can take >8s for NXDOMAIN and a late answer must still count), TTL clamped 30-600s, failures negative-cached 10s. Shell `resolve` posts the answer as a net event.
5. **TCP**: socket table (`TCP_MAX_SOCKS` 16): `tcp_open/state/send/recv/rx_avail/failed/close/abort`. Per socket: retransmit flight (16KB, RTT-estimated RTO, floor 200ms), reorder buffer (8×1500B), 256KB receive ring. **The advertised window is the ring's free space** (the old fixed 32KB window let the peer outrun a busy main loop — ACKed bytes were dropped). ACKs are batched: data arrival sets `ack_pending`, `net_poll` (right after `e1000_poll`) sends one ACK per socket. Fast retransmit counts **only pure-ACK repeats** (plen==0). SYN carries MSS 1460; ISN from RDTSC; ephemeral ports unique among live sockets.
6. **Fetch engine** (`src/net/fetch.c`): see *Browser — navigation & fetching*. HTTP framing via the shared parser below.
7. **e1000 RX release**: `RDT = index of LAST processed descriptor` — never index+1 (RDT==RDH reads as "ring full" and hardware drops everything), never write RDH. Written once per batch.

### HTTP framing (shared parser)
`http_parse_framing()` (RFC 9112 line grammar, exact-name match) feeds **both** `tls_response_complete()` and `http_dechunk()` — one parser, no differentials. Verdicts: COMPLETE (C-L satisfied / chunked terminated), SHORT (headers cut / short body / unterminated chunks), UNKNOWN (close-delimited; curl-parity accept). SHORT renders a TRUNCATED warning in the UI and never falls back to HTTP.

### Browser (okai) — rendering pipeline
okai is a shell around the web engine in `src/web/` (all integer/fixed-point: LU = 1/64 px; no FPU, no libgcc — use `w_div64`/`w_muldiv` for 64-bit division).

1. **Parse** — `whtml_parse` (WHATWG tokenizer + tree builder, scripting-off so `<noscript>` renders; charset from HTTP/meta/BOM with a 1252 fallback) → `struct wdom`.
2. **Style** — `css_set_*`: UA + quirks sheets, author sheets in document order (`<style>`, `<link>`, `@import`, `media=`), selectors L3/L4 (`:has`, `:is`, `:nth-*(of S)`), cascade origins/importance, `var()`, `calc()`, `@media`/`@supports`/nesting, presentational hints; interned computed styles.
3. **Layout** — `wlay_run`: box tree with anonymous fixups, block flow + margin collapsing, floats, abs/fixed, inline formatting (line breaking, `text-align`, `vertical-align`, inline boxes), flexbox, grid, tables, list markers, replaced elements → a **display list** + hit regions.
4. **Paint** — `wpaint` into a `struct wsurf` (AA rasterizer, TrueType glyph cache 6MB, gradients, shadows, borders/radii, images, SVG).

`struct wdoc` ties it together (`wdoc_load` → `wdoc_next_fetch`/`wdoc_fetch_done` → `wdoc_update` → `wdoc_paint` / `wdoc_hit`). Each okai window owns a page surface (viewport-sized, kmalloc'd); `okai_render_content` paints the active tab into it and the window system blits it. Scrolling shifts the surface and repaints only the exposed band (full repaint when the layout has `position:fixed` content). The chrome, scrollbar, status panel and site-identity panel are an overlay painted after the window (`okai_paint_overlays*`). **Chrome = `src/okai_ui.c`** (Firefox Proton look, palette `FX_*` in `theme.h`): drawn with the web engine's AA rasterizer (stroked icons from `seg`/`arc`/`polyline`, rounded boxes via `wr_rrect`) and Noto Sans text (`wfont_draw`) into a per-window cache that re-renders only when a state signature changes (tabs, titles, URL, focus, hover target, spinner phase); each overlay paint blits the cache (~4.5ms per re-render under TCG). `ui_layout()` is the single source of geometry for drawing AND hit tests (tabs, close boxes, '+', nav buttons, address field, identity box) and is logged as `[okai] ui ...`. Caption buttons (minimize / maximize-restore / close, `WIN_CTRL_W` 46px x tab-strip height) belong to the window system: `window_ctrl_rect`, `window_check_*_click`, `window_toggle_maximize` (repairs the old footprint via `desktop_paint_rect_pub`). Hover and the tab spinner drive repaints through `okai_ui_needs_paint` in the desktop overlay throttle. The FPS counter is a chip in the taskbar's right corner (`taskbar_paints` tells it when the taskbar repainted under it).

**Engine stack:** every engine call goes through `run_job()` → `call_on_stack()` onto a private 1MB heap stack (boot stack is 256KB; Wikipedia peaks ~20KB — the render log prints the high-water mark as `stack=NKB`).

**Logs (headless tests key on these):** `[br] [https ]parse: count=N ...` per document, `[okai] render tab=.. (page origin X,Y)` + `[okai] link[i] x= y= w= h= href=` / `[okai] field[i] ... btn=` for each page's first render (page px, viewport-relative), `[okai] click x= y= (mx= my=) hit= node=`, `[okai] LINK HIT -> url`, `[okai] focus input node=`, `[okai] toggle node= checked=`, `[okai] submit form -> url`, `[okai] error page: <title> for <url>`, `[okai] sub-res fetch: CSS|IMG url` / `sub-res done`.

### Browser — navigation & fetching
- URL bar (g to focus, Esc to exit); toolbar; j/k/arrows (48px), space/PgDn/PgUp (page), wheel (80px/notch); b/f back/forward, r reload; tabs; internal `okai:home` (an HTML page rendered by the engine). History: 8 entries/tab, redirects replace the current entry.
- **`okai_normalize_https`**: bare host defaults to `https://`, except IP literals and explicit non-443 ports (`10.0.2.2:8000/x`) which default to `http://` (local/dev servers — TLS against them fails closed with no fallback). An explicit `http://` is respected.
- **No-downgrade gate** (unchanged): cert/hostname failures → security warning page; secure-channel failures (PROTO/MAC/ALERT/RNG/OVERFLOW) → connection error page (after up to 2 same-origin resumption retries); **only transport failures** get the one-shot plain-HTTP retry. Error pages are generated HTML documents (`show_error`).
- **Fetch engine** (`src/net/fetch.c`, driven by `fetch_poll()` from the main loop right after `net_poll`): up to `FETCH_MAX` (8) GETs at once. A connection walks DNS → TCP → TLS handshake (the request rides the handshake) → response; a response **complete by HTTP framing** (Content-Length / terminal chunk / 204/304) on an HTTP/1.1 connection without `Connection: close` leaves the connection IDLE in the pool (`CONN_MAX` 8, 6 per host, 30s idle timeout; `tls_state_reuse` sends the next request on the same session). A reused connection that dies before the first response byte (server closed it while idle) re-runs the request on a fresh one. Each poll drains every available TLS record per connection (the old driver did one record per main-loop pass). Responses grow from 64KB to 8MB. Failures: TLS fail reason for secure-channel/cert failures, 0 for transport (DNS, connect, reset, 25s without progress). Reading past the end of the message is never done — www.google.com's trailing record fails AEAD on this network path (OpenSSL agrees).
- **Fetch driver** = `okai_poll()`: a request table (`struct oreq`: window, tab, document, wdoc resource id or -1 for the main document, redirect/retry state, fetch handle). Main documents start first; then each loaded tab runs up to `OKAI_SUB_PAR` (6) resources in parallel (active tab first), always leaving one engine slot for navigations. **A finished request is detached from the table before its response is processed** (delivery can run scripts that navigate; navigation cancels the tab's requests and would free the response mid-use); redirects/retries/fallbacks re-issue a new record after `fetch_end`. Navigation/tab close/window close cancel the tab's requests; results for a replaced document are dropped. Responses: status/headers parsed, `http_dechunk`, gzip inflated. Non-HTML documents are wrapped (`text/plain` → `<pre>`, `image/*` → `<img>`).
- **Sub-resources**: stylesheets, then scripts/requests, then images (`wdoc_next_fetch` order), each with its own scheme, 3xx following and one transport retry; caps `OKAI_MAX_CSS_FETCH` 24 / `OKAI_MAX_IMG_FETCH` 32 / JS 160 / requests 64. Cross-origin `fetch()`/XHR send `Origin` (CORS). Rendering is coalesced: first paint waits for stylesheets (≤3s), image arrivals repaint at most every 1.5s, plus a final paint on drain.
- **Forms**: click focuses text fields (caret drawn), typing edits the `value` attribute, Enter submits (GET; POST forms are sent as GET), Tab cycles controls, checkboxes/radios toggle, `<select>` cycles options.
- **Requests**: top-level navigations send a browser's HTML-first `Accept` (`net_accept_html`; crates.io 404s `*/*`), sub-resources and script fetches `*/*`; cookies from the shared jar (`wcookie`, SameSite against the top-level page); `Accept-Encoding: gzip`.

### Browser — page scripts (QuickJS, Phase 6)
- **Engine**: QuickJS 2026-06-04 in `src/qjs/` (see `README.okernel`: freestanding libc shim — kmalloc heap, serial stdio, RTC+PIT clock, `abort()` unwinds the realm — plus musl libm; small additions for async module loading). x87 doubles at 53-bit precision: `wjs` sets the control word on every entry, the kernel FNSAVE/FRSTORs per thread. libgcc linked for 64-bit division. QuickJS's own suites pass on the exact kernel objects (`sh tests/qjs/build.sh ...`, part of `make web-tests`).
- **Realm per document** (`src/web/wjs.c`, created lazily by `wdoc`): 96MB heap cap, 900KB stack cap, 8s per script / 1.5s per task (interrupt handler); a QuickJS assertion kills the realm only. Web API = `wjs_prelude.js` (events, DOM classes, forms, custom elements, MutationObserver, CSSOM + constructable sheets, URL, storage, Intl en-US, ...; compiled to bytecode once per boot) over C natives in `wjs_dom.c` (selectors through the CSS engine, layout queries, HTML serialization).
- **Scheduling** follows the HTML spec's shape: parser-blocking classic scripts in order (external fetched first), then defer + module scripts, DOMContentLoaded, async/dynamic as they arrive, load once scripts/sheets/images settle. Module graphs are fetched ahead (compile with `JS_EVAL_FLAG_NO_RESOLVE` to discover imports; dynamic `import()` parks until fetched). Timers/rAF/microtasks run from `okai_poll` under a budget; script DOM mutations trigger stylesheet rebuilds/image rescans and a coalesced re-render.
- **okai integration**: DOM events dispatch before default actions (click/keys/input/change/submit/focus/scroll; `preventDefault` honoured); script navigation/scroll/focus/title/pushState flow back through `wdoc_js_take_*`; fetch()/XHR go over the single connection (CORS-checked) like any sub-resource. `J` (Shift+J) toggles page scripts globally and reloads (`[okai] page scripts on|off`).
- **Host harness**: `build-host/wbrowse <url|file> w h out.ppm [secs]` runs the same engine + scripts on the host (live sites cached under `~/.cache/wbrowse`); `tests/web/js/basic.html` is the DOM/API regression page (`[js] RESULT ALLPASS 37`). Headless: `test_okai_js.py`.

---

## Build & Run

```bash
make desktop       # Desktop ISO (kanarchy-desktop.iso) — primary target
make text          # Text-mode ISO (kanarchy-text.iso)
make run           # Build + run text mode in QEMU
make run-desktop   # Build + run desktop mode in QEMU
make clean         # Remove all artifacts
make host-tests    # Host unit/suite tests (crypto, TLS, PKI, CSS, DOM, layout, HTML, RNG)
make host-tests-asan # Same, under ASan/UBSan
make diff-oracle   # Build the differential oracle (crypto)
make diff-test     # Run differential vs python-cryptography
make tls-interop   # TLS-Attacker / pyserver interop battery
```

**Run accelerated.** Windows: `run-windows.bat` (`qemu-system-x86_64 -accel whpx -accel tcg` — the Windows i386 build has no WHPX; the 32-bit kernel boots fine in x86_64 QEMU, boot to heap 2s). Linux: add `-accel kvm` (`make run-desktop` does, falling back to TCG). Plain TCG is ~10x slower on every CPU-bound step.

Desktop QEMU (must use `-vga std`; `-device e1000` for networking):
```bash
qemu-system-i386 -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std \
  -device e1000,netdev=net0 -netdev user,id=net0
```
Serial ground truth (network/TLS bring-up):
```bash
qemu-system-i386 -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std \
  -device e1000,netdev=net0 -netdev user,id=net0 \
  -nographic -serial stdio
```
Packet capture: add `-object filter-dump,id=dump0,netdev=net0,file=/tmp/net.pcap`.

**Compiler flags that matter:** `-m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -fno-pic -fno-pie -mno-red-zone -O2 -fno-strict-aliasing -DKERNEL -Isrc`. Without `-fno-pic/-fno-pie/-mno-red-zone`, GCC emits `__x86.get_pc_thunk` calls that crash a freestanding kernel.

---

## Shell Commands

**Desktop:** `about clear disk echo edit exit help ip locktest ls mem netdrop okai open ping ps reboot resolve run [path] save shutdown sysinfo` (alias `neofetch`) `terminal uptime usermode` (retired — prints a hint; use `run /bin/hello`).
**Text:** `help clear echo mem terminal exit list switch N about reboot shutdown`. (The text build is a separate, smaller terminal-only shell — it does **not** share the desktop command set.)

---

## User Mode (Ring 3)

Working end-to-end: GDT user segments + TSS, INT 0x80 gate (DPL=3), `paging_map_user` (U/S on BOTH PT and PD entries), `enter_user_mode` via IRET. The `usermode`/`spawn_user` single-slot path was **retired 2026-09-10**; ring-3 now runs through the scheduler (`sched_spawn_user`, fork/exec, wait/kill).

---

## Known Limitations

- **Single CPU** — no SMP.
- **In-memory VFS** — only files explicitly synced to PFS survive reboot.
- **TCP** — no congestion control or window scaling (64KB max window; SLIRP is local, so it does not matter in QEMU); reorder buffer 8×1500B per socket; no TIME_WAIT.
- **TLS 1.3 only** — suites ChaCha20-Poly1305-SHA256 + AES-128-GCM-SHA256 (no AES-256-GCM-SHA384, no TLS 1.2: whatwg.org, baidu.com, arstechnica.com fail the handshake). Bot protection (Akamai/Cloudflare/Fastly) answers many big sites with 403 pages — TLS works, the page is a challenge.
- **JavaScript** — no WebAssembly or workers; WebSocket (never opens), canvas (`getContext()` → null) and media elements are inert stubs; layout queries are answered from the last layout; `Intl` is en-US only.
- **Web engine** — no `position: sticky`, transforms are translate-only, no animations/transitions, `mask-image` boxes skip their background; JPEG chroma is nearest-neighbour; CJK beyond the 16px bitmap fallback table renders as tofu.
- **Fetching** — GET only (POST forms go as GET, script POST requests fail); HTTP/1.1 only (no HTTP/2 multiplexing — parallelism is 6 connections per host); capped at 24 stylesheets / 32 images per page.
- **Emulation speed** — under plain TCG every CPU-bound step is ~10x slower than under KVM/WHPX (layout of a big page ~300ms vs ~30ms, a TLS handshake's crypto ~200ms). Run accelerated (`run-windows.bat`, `make run-desktop`).

---

## Critical Bugs Found (and Fixed)

Kernel / build:
1. `section .note.GNU-stack` must be **LAST** in `.asm` files (otherwise code is swallowed into a non-executable section).
2. `-fno-pic -fno-pie -mno-red-zone` are required (freestanding).
3. Framebuffer is above 4MB — requires identity-mapped page tables before access.
4. **Cursor save/restore causes persistent artifacts (v0.3.1)** — the saved background patch goes stale against scene changes and IRQ12 can tear it mid-read → arrow ghosts + wallpaper holes in windows. Fixed by the **stateless sprite** + `desktop_paint_rect` scene repair (see *Cursor Compositor* above — it has the full invariant). **Do NOT reintroduce `cursor_bg` or draw after `mouse_paint_cursor()`.**
5. Unclipped repair painting kills FPS — every repair path must set/reset the clip rect.
6. `_kernel_end` must stay under available RAM; large static arrays can blow the identity map.
7. **16-bit offset overflow (2026, this session)** — `struct dom_attr.val_off`/`name_off` and `struct dom_node.tag_off` were `uint16_t` while `DOM_MAX_TEXT`/`DOM_MAX_NAMES` are 512KB/128KB. On a big page all attribute values wrapped and read from the wrong arena offset, corrupting `href`/`id`/`class`/`alt` (garbled leaked text, wrong links). Fix: widen to `uint32_t` (`dom.h`) and `layout_run.text_off` too (`layout.h`); drop the truncating casts.
8. **UA stylesheet was inert** — `css_apply` initialized `win[idx] = -1`, but UA rules carry specificity base `-10000`, so `spec < win[idx]` rejected every UA rule. Fix: init `win[]` to `-0x7FFFFFFF`. Verified: `body` now gets UA `background:#FFFFFF;color:#000000`.
9. **`draw_char_cell` skipped bg on NULL glyph** — control/uninitialized cells left stale backbuffer pixels (noisy dither). Fix: paint the cell background in the early-return paths.

Network / TLS:
10. e1000 register offsets: RDH/RDT/TDH/TDT at `0x02810/0x02818/0x03810/0x03818`.
11. e1000 RX buffers must be in low memory for DMA; 16-bit MMIO writes for RDH/RDT.
12. **TCP payload length from padded frame, not IP `total_length`** — Ethernet pads short frames, so zero pad bytes were fed to TLS. Fix: derive length from `ip_hdr.total_length` at the IHL offset.
13. **TLS buffers too small** — 16KB ring/response dropped bytes on multi-record pages. Fix: 64KB both.
14. **Stale-tail host buffers** — string copies didn't NUL at actual length, so a previous host leaked into the next. Fix: `net_copy_str()`.
15. **DNS definitive failure wedged the fetch owner** — NXDOMAIN left a retry flag set forever. Fix: abort the parked request; refuse empty-host URLs; always release the owner.
16. **`uint16` memcpy is little-endian on the wire** — always use `put_u16()` for TLS integer fields.
17. TLS extension `list_length` is a single 2-byte prefix — don't double-count.
18. **HKDF-Expand-Label** needs the `"tls13 "` prefix and a 1-byte label length.
19. **Transcript hash body-length** — `tls_build_client_hello` returns header+body; pass body-only to `tls_transcript_update_msg`.
20. **TLS Certificate parser** must skip `cert_request_context<0..2^8-1>` before the list length.
21. **`send_aead`** must append the 16-byte tag (not send stack garbage).
22. **Finished key** derives from the handshake traffic secret, not the application secret.
23. **Separate app-traffic sequence counters** for send and receive.
24. **TLS 1.3 transcript hash** is over `type(1)||len(3)||body`, not the record framing.

okai / browser:
25. **IntelliMouse ID read drained the byte it waited for** — wait for ACK, then for ID (bounded spins). Detection logs `[mse] wheel detect id=0x03 4-byte mode ON`.
26. **Forced full-window repaint under a covering window** tanked FPS (60→13). Fix: clip okai's overlay to the uncovered region (`okai_paint_overlays_rects`).
27. **Sub-resource JS execution wedged the kernel** — real minified bundles hang tinyjs. Fix: do not execute external scripts (see above).
28. **External CSS wasn't fetched on main-page load** — `okai_queue_sub_resources()` existed but was never called, and the fetch owner was cleared immediately (so the sub-res branch, which requires an active owner, never ran). Fix: call it after parsing and keep ownership while resources are queued.
29. **kbd offer-wake stole input for wait-parked init (2026-09-30)** — the OFFER-WAKE scan took the oldest BLOCKED slot with any staged resume, so wait-parked init (slot 1, ret=-2) always beat read-parked sh (slot 2): deterministic starvation, sh never received typed lines. Fix: skip staged ret `-2` (wait-park; re-stage untouched) so offers reach readers. Plus: `kbd_wake_armed` was set even when the offer was dropped (stale-backlog guard) — `sys_proc_kbd_offer` now returns queued/dropped and the IRQ arms only on queued.
30. **Text build triple-faulted at boot (2026-10-01)** — the LOW trampoline builds its boot page tables with `label - 0xC0000000` expressions that only resolve against HIGH-VMA links; low-linked they wrap to garbage phys (bisected: #DF at `_start+0x5D`, IDT=0). The text kernel never enables paging, so `linker.ld` now enters at `_start_high` directly (with a `mov edi, ebx` there — the trampoline used to set up the mboot pointer). Follow-ups in the same session: text GRUB entry needs `gfxpayload=text` AND its own multiboot header without the video request (`-DTEXTMODE` start object — the shared header asks GRUB for 1920x1080x32, leaving the text kernel writing an invisible 0xB8000).
31. **TCP send > MSS crashed the kernel (2026-10-07)** — a 1594-byte TLS record (long module-batch URL) went through `tcp_send_raw`'s 1514-byte stack frame; the overflow smashed the return address (EIP garbage). TCP sends are now split at the MSS.
32. **Stale objects after a header change (2026-10-07)** — the Makefile didn't list `src/crypto/*.h` as dependencies; `struct tls_state` grew (`suite`, 8KB staple) but `tls_net.o` kept the old size, so `tls_state_init`'s memset ran past `tls_s` and zeroed `tls_start_tick`: every HTTPS fetch "timed out" on its first step and downgraded to plain HTTP (14-byte pages). Fix: `-MMD -MP` dependency files + `src/crypto/*.h` in `HEADERS`.
33. **DNS race on navigation (2026-10-07)** — the resolver has one slot; a sub-resource lookup still pending when the user navigated made the new fetch skip its own query, then read the stale answer for another host as "no A record" → abort → HTTP fallback. Fix: `dns_is_pending_for(host)` (only a query for the same host is waited on; a new query's tx id drops the stale answer) and a superseded TLS lookup re-queries.
34. **Fetch timeout / ticket clock units (2026-10-07)** — `TLS_FETCH_TIMEOUT_TICKS 1200` was "66s at 18 Hz" but the PIT runs at 100 Hz (12s; now 3000 = 30s), and the ticket-age clock was fed raw ticks as ms (tickets lived 10× their lifetime; now `tick_count * 10`).
35. **One request at a time, one TLS record per main-loop pass (2026-10-07, "loading is EXTREMELY SLOW")** — every resource paid DNS + TCP + full TLS handshake + request (≥3 round trips, ~1s each on this network) strictly in sequence. Replaced by the TCP socket table + fetch engine + okai request table (parallel, keep-alive). Measured (TCG): python.org settled 32.6s → 9.6s, apple.com 17.6s (13 of 33 resources) → 5.9s (all 33), Wikipedia 10 resources/23.5s → 43 resources/22.3s.
36. **Fixed 32KB TCP window over a ring the main loop drains** — a long render let the peer fill it; bytes were ACKed then dropped. The window is now the ring's free space.
37. **Per-packet serial logs** — hex frame/descriptor dumps, a register dump every 500 polls and a line per TCP segment wrote hundreds of KB to COM1 per page (each byte = 2 port I/O exits). Gated behind `net_trace`; serial writes now poll the status register once per 16 bytes (FIFO).
38. **No `Origin` on cross-origin XHR** — servers omit Access-Control-Allow-Origin without it; BBC's sign-in check failed and its script redirected bbc.com ↔ bbc.co.uk forever (679 fetches in 2 minutes).
39. **8-second script budget froze the OS** — apple.com's gallery script loops on a layout value that never changes until interrupted; the budget (the whole UI waits) is now 4s per script evaluation, 1s per task.
40. **`udp_send` placed the UDP header over the IP header** (missing Ethernet offset) — latent until DNS started using it.
41. **FPS HUD over the browser's close button (2026-10-07)** — the black FPS box was drawn at the screen's top-right every frame, on top of the maximized browser's caption controls; the title-bar-less browser had no minimize at all. FPS moved into the taskbar; title-bar-less windows get real min/max/close (`window_ctrl_rect`).
42. **Chrome geometry duplicated in 5 places** (draw + 4 hit tests + tests, "MUST mirror" comments) — replaced by `ui_layout()` + the `[okai] ui` geometry log.
43. **Maximize/restore left the old frame on screen** — `needs_redraw` only composites dirty windows; the vacated area needs a region repair (`desktop_paint_rect_pub` of the old rect).
44. **Half-drawn hover highlights on taskbar/caption buttons (2026-10-08, WHPX)** — the cursor-erase repair (`desktop_paint_rect`, clip = old cursor rect) re-rendered the cached taskbar strip / okai chrome band when the mouse had moved since the main-loop update that frame, so the new hover state reached the screen only inside that small rect; the signature then matched and the full blit never came (half-red close button, grey patch on maximize). Only reproduces with real-mouse timing (WHPX; `tests/headless/hover_win.ps1`). Fix: clipped repairs only blit the taskbar cache (`taskbar_update` alone re-renders), and an okai chrome render under a partial clip sets `partial` so `okai_ui_needs_paint` forces the main-loop full paint.
45. **Typing went to the browser instead of the terminal (2026-10-08)** — window clicks were routed by slot index (high→low), not z-order: with okai in slot 1 under the terminal (slot 0), a click on the terminal's overlapping area focused okai. Now topmost-by-z first (`test_desktop_ui.py`).

---

## TLS traps (next agent: read these)

**Crypto/TLS math**
- Limb packing: never route >64 bits through a `uint64_t`; shifts ≥64 are UB.
- Reduction folds: the ×5/×38 factor applies to BOTH low and high parts of every folded digit, cascades included.
- `|` vs `+`: OR drops carries when limbs have slack — carry first, then pack additively.
- Wraparound test for 2^255: check the top BIT, not carry-out.
- Array initializers: a short literal silently zero-pads the top byte.
- AEAD nonce = static_iv XOR sequence counter (big-endian, left-padded to 12 bytes; high byte at nonce[4]).
- TLS record plaintext ends with the real content-type byte then zero padding; when scanning back, skip zeros only (strip-first is provably correct; type-first breaks close_notify).
- HKDF-Expand-Label info = `out_len(2)||label_len(1)||"tls13 "+label||context_len(1)||context`.

**Record/handshake framing**
- `tls_record_parse_header` checks only 5 header bytes; accept 0x0303 and 0x0304.
- Session ID may be empty or 32 zero bytes.
- Put ChangeCipherSpec AFTER the ClientHello record (Python ssl strict mode rejects it between CH and SH).
- Match OpenSSL's exact signature_algorithms set; offer enough for EC + RSA-PKCS1 + RSA-PSS.
- Start offering ONLY x25519 in key_share (some libs reject secp256r1 alongside).
- `legacy_compression_methods` must be exactly `{0x00}`.
- Supported_groups body is `{uint16 length; group[]}` (not an early-draft outer prefix).
- X25519 is constant-time and **fail-closed**: `x25519_shared_secret` returns 1/0; reject u=0/u=1.
- Hostname canonicalization is centralized in `tls_canon_host()` (applied at SNI/cert/pin/ticket boundaries).

**Trust / PKI (2026-10-07)**
- Root store = Mozilla's (`tools/gen_roots.py` → `roots.c`, 120 roots; e-Szigno's P-521 root is skipped — no P-521 support). Each entry embeds the whole root cert: SPKI, subject DN, SKI.
- Two anchors: `root_issued()` (cert's issuer DN == a root subject byte-exactly, AKI == root SKI when both present, signature verifies under the root key — servers don't send roots, so this is the normal path) and the older in-flight SPKI match. Several roots may share a name (re-keyed): every candidate is tried.
- Validity: leaf up front, each issuer as the walk reaches it. Certs past the anchor are never examined — an expired cross-sign appended by the server must not fail the path (the mutation fuzz proves the trailing example.com cross-sign is unused).
- OCSP: delegated responders (cert in the response, issued by the CA, `id-kp-OCSPSigning`, RSA ≥ 2048) and byKey responder IDs. Policy: only a verified REVOKED status or a Must-Staple leaf hard-fails; a stale/unverifiable staple is treated like an absent one (it is strippable anyway). Staples up to 8KB.
- AES-128-GCM (`aes.c`): bitsliced Boyar-Peralta S-box and masked GHASH — no table lookups on secret data. The record key length is part of the HKDF label (`tls_record_key_len`: 16 for AES, 32 for ChaCha20).
- Adversarial suite clocks come from the test PKI's file mtimes; build it with 64-bit `stat` (`_FILE_OFFSET_BITS 64`) — on WSL's `/mnt` drives 32-bit `stat()` fails with EOVERFLOW and the suite silently used a fallback date (42 bogus failures).

**Kernel**
- Heap is a segregated-fit allocator (`kmalloc`/`kfree`); big tables go on the heap, not BSS; check `_kernel_end`.
- e1000 RX release: `RDT = last processed index`.
- `tcp_handle_packet` runs in IRQ context — IRQ appends to a buffer; the main loop reads after close.
- `http_get` resets done/length at entry — a stale done-flag races the parse block.
- Makefile `%.o` depends on headers (`HEADERS` + `-MMD -MP` dep files) — never remove (bug #32).
- Mouse is relative + smoothed; converge iteratively in scripted tests.

**Process**
- Serial log (`-serial file:`) is ground truth for network/TLS bugs — grep before theorizing.
- Screenshot verification: crop + zoom; count specific pixel colors; route PNGs to a vision model rather than eyeballing.

---

## Testing

Host suites (`make host-tests`): crypto (sha256/sha1/sha512/chacha/poly/aead), AES-128/GCM (FIPS-197, GCM spec cases, 300 random vectors vs OpenSSL via `tests/aes_vectors.py`), TLS record/handshake/keysched/client, PKI, adversarial (189 checks; `make host-tests-asan` for the ASan/UBSan run), RNG, editor text, live example.com handshake + wrong-hostname rejection, and `make web-tests` (web CSS unit tests 54/54, font tests, JS regression page `[js] RESULT ALLPASS 37`, QuickJS's own suites on the kernel objects; builds `build-host/wrender` + `build-host/wbrowse`).
Real-site TLS census on the host: `build-host/tls_scan -f tests/tls_hosts.txt` (the kernel's TLS client over host sockets; prints the failure reason per site).
Web engine (needs the corpus: `python3 tools/fetch_corpus.py`, gitignored): `tests/web/html5_diff.py` (85/85 trees == html5lib), `tests/web/image_diff.py` (59/59 == PIL), every corpus page through `wrender` under ASan/UBSan (`SAN="-fsanitize=address,undefined" OPT=-O1 sh tests/web/build.sh`), `tests/web/compare.py <name> [w] [h] [--fresh]` (side-by-side vs headless Edge; `WR_DUMP_TAG=figure ./build-host/wrender <name> ...` prints a tag's boxes + ancestors). okai's built-in pages export to the corpus with `python3 tools/okai_pages.py`.
Differential tooling: `make diff-oracle` / `make diff-test` (X25519, SHA-256, HMAC, HKDF, AEAD vs python-cryptography).
Interop: pyserver + TLS-Attacker (`tests/tlsattacker-*.xml`) — full HS P-256/RSA-P384 PASS, mutilation rejection, fragmentation.
Persistent FS tests: `OkVM(tag, disk=path)` appends `-hda` for PFS tests; a stale `-hda` QEMU holds the image write-lock (kill it first).

### Headless testing (okvm)
`tests/headless/okvm.py` drives QEMU headlessly; output in `~/okvm/`; full docs in `tests/headless/TESTING.md`.
```python
import sys, time; sys.path.insert(0, 'tests/headless')
from okvm import OkVM
vm = OkVM("tag"); time.sleep(14)
vm.type_string("okai https://example.com/\n")
ok = vm.wait_for("parse: count=", timeout=140)
vm.kill()
```
- Serial log (`~/okvm/<tag>_serial.log`) is ground truth — never guess from pixels.
- `vm.wait_for(pattern)` polls serial — never raw `sleep()`.
- `vm.burst()` for mouse movement (4-sample smoothing ≈ 4× dilation).
- Closed-loop clicks: `vm.click_link(href_part)` / `vm.click_at(page_x, page_y)` (read `[okai] click x= y=` → correct → repeat) and `vm.click_screen(x, y, regex)` for chrome (reads `[mse] btn=1`). `vm.link_regions()` parses the last render's link log; `vm.premove()` dead-reckons first so the probe click never lands on a link under the cursor.
- Chrome geometry is logged by the kernel, not hard-coded: every time the chrome layout settles okai prints `[okai] ui back=x,y,w,h fwd=.. reload=.. home=.. url=.. ident=.. newtab=.. min=.. max=.. close=..` and `[okai] ui tabs n=N tab<i>=.. tabx<i>=..` (tab i and its close box). Aim with `vm.click_ui(name, regex)` / `vm.ui_center(name)` / `vm.ui_rect(name)`; for targets next to the close button pass `anchor=(x, y)` (a harmless probe click nearby — long dead-reckoned moves drift ~50px). The chrome band is 96px (44 tab strip + 52 nav bar); the page starts right below it.
- `python3 tests/headless/run_suite.py -j3 test_a.py ...` runs tests in parallel (outputs in `~/okvm/<test>.out`).
- Scripts (all green 2026-10-07): `test_okai_js.py` (page scripts: offline fixture + Wikipedia), `test_sites.py [tag] [url ...]` (real-site sweep, needs internet: OK only when the document arrived over TLS — a plain-HTTP fallback or error page fails; 15/15 on 2026-10-07), `test_okai_interact.py` (offline: CSS+image load, checkbox, link click, back, Tab+typing+Enter submit, wheel), `test_okai_page.py <url> [tag] [--scroll N]` (load + screenshot any page), `test_links.py`, `test_nav.py`, `test_tab_x.py`, `test_addrbar.py`, `test_lock.py`, `test_errors.py`, `test_https_default.py`, `test_certfail.py` (starts its own self-signed `openssl s_server`), `test_resume.py`, `test_pki_qemu.py`, `test_google.py`, `test_google_search.py`, `test_firstrender.py`, `test_stale_doc.py`, `test_fixed_header.py` (self-contained fixtures), `test_boot_mem.py`, `test_font_render.py`, `test_css_box.py`, `test_sh_hello.py`.

### Host preview (no QEMU)
`build-host/wrender <corpus-name> <w> <h> out.ppm [page_h]` runs the exact engine sources (`src/web`) on the host — the same code okai links. `tests/web/compare.py` pairs it with an Edge screenshot.

---

## Session Log (condensed)

Older, fully-resolved narratives were collapsed. Newest first.

**2026-10-08 — terminal-first fixes.** Removed the taskbar's KAnarchy launcher (the OS is driven from the terminal; `terminal` / Ctrl+Alt+T open more), fixed stale hover highlights (bug #44) and click focus by z-order (bug #45). New `test_desktop_ui.py` (passes on this build; on 90cf6c9 the launcher + focus checks fail — its hover checks don't trip under TCG, `hover_win.ps1` is the WHPX repro).

**2026-10-07 (late night) — desktop restyle + speed pass ("taskbar, plain terminal, 3D wallpaper, optimize it to hell").** New `src/taskbar.c`: cached AA strip (frosted wallpaper tint, red KAnarchy mark that opens a terminal, per-window buttons with app icons + Noto titles + red focus pill, FPS chip, RTC clock), geometry logged as `[taskbar] btn` (okvm `taskbar_rect`). Title-bar windows (terminal, editor, sysinfo) now have a GNOME-style dark header bar with round close/minimize buttons, drawn with the shared `ui_draw.h` primitives; the red terminal chrome and the KAnarchy banner/welcome text are gone — the terminal opens as a plain 72x24 shell with just `kanarchy>`, left of the wallpaper emblem. Wallpaper: a raymarched chrome K in a red ring over a reflective grid floor (`tools/wallpaper/`, WebGL → JPEG, embedded and decoded at boot; replaces the 256-colour `wallpaper.h`, -1.2MB of source). Speed: `memcpy/memset/memmove` are `rep movs/stos` (were byte loops), dirty-row flush copies coalesced runs in one memcpy, wallpaper blits/rect fills/hlines/window-drag row moves use the same primitives. Measured (TCG, `bench_ui.py`, same wheel-scroll input): page scroll step 79.8M → 26.1M cycles; FPS under synthetic mouse motion is input-bound (~28 both). `bench_ui.py` scrolls with the wheel now (j/k silently no-op'd when the address bar had focus: 0 scrolls).

**2026-10-07 (night) — okai chrome revamp ("make it look like modern Firefox").** New `src/okai_ui.c`: Proton-style light tab strip with a floating selected tab, Noto Sans titles with fade-out, globe/search favicons and a loading spinner, AA stroked nav icons (back/forward with disabled state, reload, home), rounded address field (host emphasized, `https://` trimmed, blue focus ring + caret, placeholder), site-identity button + panel (secure / not secure / built-in page), Firefox-style status panel, Windows-11 caption buttons with hover (red close). Window system: caption buttons for title-bar-less windows, maximize/restore. FPS counter moved to the taskbar. Tests aim from the `[okai] ui` geometry log; new `test_okai_chrome.py` (hover, focus ring, navigate, identity panel, tab open/close, restore/maximize, minimize + taskbar restore).

**2026-10-07 (evening) — "loading is EXTREMELY SLOW": parallel networking.** Profiled first (`perf_load.py`: host-stamped serial timeline; pcap via `OKVM_PCAP` + `pcap_tcp.py`): every resource paid a fresh DNS+TCP+TLS handshake (~1s round trips on this network) strictly one at a time, the TLS driver decrypted one record per main-loop pass, and per-packet serial dumps cost port-I/O exits. Rewrote the client side of `network.c` (TCP socket table with ring-sized windows, batched ACKs, multi-entry DNS cache), added `src/net/fetch.c` (8 parallel GETs, keep-alive pool, `tls_state_reuse`), replaced okai's single owner with a request table (6 resources per tab in parallel), moved the e1000 to a 256-entry heap ring with masked IRQs, gated packet logging. Found on the way: no `Origin` on cross-origin XHR (BBC redirect loop), an 8s script budget freezing the OS (now 4s/1s), `udp_send` header offset. Acceleration: the kernel runs under WHPX (`run-windows.bat`) and KVM — rendering/JS/crypto ~10x faster than TCG. Results (TCG, same network): python.org 32.6s → 9.6s, apple.com 17.6s/13 resources → 5.9s/33, Wikipedia 23.5s for 10 resources → 22.3s for 43; under WHPX Wikipedia settles in ~12-14s with first render at ~4s. Full headless suite green.

**2026-10-07 (late) — "half the sites give a connection or security error" fixed.** Census with `tls_scan` (kernel TLS client on the host, `tests/tls_hosts.txt`) went 31/91 → 86/91 (left: 3 TLS-1.2-only servers, netflix truncating mid-body, washingtonpost timing out). Causes and fixes: no name-bound anchoring + a tiny root store ("untrusted root" for most sites) → full Mozilla store with subject/SKI and `root_issued()`; 11 roots the strict parser rejected → `x509_parse_spki` (only the key is needed); OCSP staples from delegated/byKey responders rejected → supported, plus soft-fail for unverifiable staples; Akamai sites (eBay, Microsoft, Apple…) offer no ChaCha20 → constant-time AES-128-GCM. In the kernel, every HTTPS fetch had silently been timing out and downgrading to HTTP since the header change (bug #32), plus the DNS navigation race (#33), the stale previous-connection bytes parsed as the next ServerHello (reason=6 phase=1 on back-to-back sub-resources), timeout/ticket clock units (#34) and the `Accept: */*` 404 on crates.io. `test_sites.py` 15/15 over HTTPS (bot-protection 403s count — the TLS side works); adversarial 189/189 (+ASan), live MITM check PASS. Still failing: TLS-1.2-only servers (whatwg.org, baidu.com, arstechnica.com), netflix (server truncates), washingtonpost (times out).

**2026-10-07 — Phase 6: real JavaScript (QuickJS), commit 67fc1c7.** See *Browser — page scripts*. QuickJS hosted-style on a freestanding libc shim + musl libm, x87 per-thread state, realm-per-document with budgets and abort containment; spec-shaped script scheduling incl. module graphs and deferred `import()`; Web API prelude + C DOM natives; events before default actions; fetch/XHR; cookie jar shared with HTTP. TCP sends split at the MSS (bug #31). tinyjs retired. Verified: Wikipedia (ResourceLoader, 10 scripts, 0 errors) in QEMU, `test_okai_js.py` + full headless suite, GitHub/MDN modules clean on the host under ASan.

**2026-10-07 — new web engine integrated into the kernel (phases 0-5 done).** `src/web` linked into the desktop build (+`fontdata.asm`, `callstack.asm`); okai rewritten around `struct wdoc` (per-tab heap documents, per-window page surface via the new window pixel-surface mode, pixel hit-testing, coalesced renders, HTML error/home pages, forms incl. checkbox/radio/select, gzip, sub-resource images). Old renderer retired (`dom.c`, `css.c`, `layout.c`, `html.c` + their host tests/preview; the editor's slot map moved to `textslot.c`). Kernel fixes found on the way: boot PD widened to 0-64M PSE (bigger image put the boot stack past 4M → silent triple fault), HTTPS fetches no longer reuse the previous connection (every other sub-resource failed), TLS fetch ends at HTTP message completion (Google's trailing record fails AEAD on this path — OpenSSL too), 1024-byte request paths (Wikipedia's load.php URLs were truncated), host:port/IP default to http. Engine fixes: root/body gradient & image paint the canvas, inline-box fragments double-counted padding+border, percentage max-width on replaced elements zeroed their max-content (Wikipedia thumbnails at 100px). Verified in QEMU: home, example.com, Wikipedia (675KB gzip HTML, 2 CSS + 24 images, first render ~250-400ms), HN, Google search submit, error pages; full headless suite green; host CSS 54/54, corpus clean under ASan/UBSan.

**2026-10-01 — KAnarchy rebrand + i18n push (overnight session).** Dual identity: KAnarchy OS product, okernel kernel. Boot/desktop/shell rebranded (red block-logo banner from `kanarchy.txt`, red prompt/chrome, `kanarchy-*.iso`, GRUB entries); code identifiers, User-Agent, and history prose intentionally untouched. things.txt done: flat-red terminal chrome (per-window `red_chrome` flag, square black buttons), closeable main terminal, global Ctrl+Alt+T (new Alt tracking in the keyboard driver), wallpaper replaced with a circled-K on black. Text boot resurrected (bug #30). Wikipedia zero-???: census found 459 fallback codepoints across ~25 scripts; transliteration tables for all of them (~4300 cases, generated from Unicode names with per-script review), 4-byte UTF-8 decoder + Gothic + math folds + astral drop, and a real-glyph CJK path (Unifont-extracted 16x16 bitmaps for 228 Han/Hangul/kana, raw-UTF-8 runs flagged `LAYOUT_FLAG_CJK`, codepoints bit-packed into unused model bytes so repair repaints stay exact — zero struct changes). Verified: portal 0 fallback '?', main-page only legit punctuation, torture page clean, host suite + sh_hello green, both ISOs screenshot-verified. Known gaps: Kanji beyond the 228-glyph table still '?', JA/ZH Wikipedias need table growth (mechanism supports it).

**2026-09-30 — ring-0/ring-3 sorting, first slice (this session).** Centralized the ring-3 trap site: new `userland/usys.h`/`usys.c` carry the whole INT 0x80 ABI (0–17, raw-phys `SYS_MMAP_USER` deliberately excluded as ring-0-only); all 9 userland programs link it, 17 scattered `int $0x80` blocks deleted, `gen_*.h` reseeded. Validation caught a pre-existing kernel bug (not from this change — no `src/` file touched by it): the kbd offer-wake scan always picked wait-parked init over read-parked sh (bug #29 above), starving the shell; fixed plus the stale-arm on dropped offers. `test_sh_hello.py` fully green twice (`shell alive: PASS (sh)`).

**2026-09-29 — wikipedia-with-languages follow-up (this session).** Cursor-compositor gating fully reverted per owner (main loop back to erase → scene → sprite → flush every spin; `graphics_flush` back to `void`). Real fixes underneath the reported symptoms: (1) sub-resource pipeline was dead — driver never kicked the first fetch AND stale main-page done/len fired a bogus `sub_res_done` eating queue entry 0 (external CSS parsed 0 rules; verified by hexdump of the response buffer). Both HTTP+HTTPS paths now kick `okai_start_sub_res_fetch`; external `<script src>` queueing restored to documented fetch-but-never-execute. (2) `<nav>` blanket-drop hid the portal's whole language grid (0 link runs) — landmarks now drop only on chrome class/id signals (bare landmarks still drop). (3) Latin-Extended + Cyrillic-Extended transliteration table (~60 cases: ČčĚěŘřŮůĂăȚțʻʼƏəЇїЄєҐґЎў…; narrow-nbsp → space) so language names read instead of `?`. (4) drain re-render skipped unless a stylesheet landed (`sub_res_css_changed`); error pages clear stale `g_lay` headings; occlusion-aware `window_paint_uncovered` stops background-terminal bleed-through. Verified headless repeatedly: portal with blue top-10 links, readable language list, search, sister projects; scrolling clean. **Open/accepted:** unfounded scripts (CJK/Arabic/Hebrew/Greek/Thai/Vietnamese-combining — no font), TLS fetch flakes (resumption reason=6, ciphertext/partial races pre-date this work), an intermittent blank render seen mid-session that resisted root-causing (identical-input layout divergence; checksum/purity probes added then removed — layout proven pure back-to-back; prime suspect is fetch-byte nondeterminism, not render).

**2026-09-28 — render-performance pass + sub-resource repair (this session).** Wallpaper re-injected (`wallpaper.png` → `src/wallpaper.h` regen; the header was never regenerated after the swap, still all-zeros); icons shrunk 48→32px (cell 176→128). Typing/scroll costs cut: partial-dirty window repaint (keystroke = 1-3 cells, not full window), `draw_char_sized` fast paths (1:1 blit + hoisted coverage tables + blend early-out), `window_write_cell_rgb` skips `rgb_to_vga`, okai layout/blit split (j/k/wheel re-blit without `layout_run`; item-driven blit; quiet scroll logs), taskbar/FPS/overlay throttled (state-gated + chrome-dirty + anim check). Cursor-compositor gating was built then FULLY REVERTED per owner request — main loop is back to erase → scene → sprite → flush every spin. Sub-resource repair (real bugs, pre-existing): the fetch driver never kicked the first sub-res fetch AND the stale main-page done/len fired a bogus `sub_res_done` that ate queue entry 0 (external CSS parsed 0 rules — pages rendered unstyled); both HTTP+HTTPS paths now kick `okai_start_sub_res_fetch`. External `<script src>` queueing restored to HANDOFF parity (fetched, never executed — `n_js = 0` had dropped it); error pages clear stale `g_lay` headings. Verified headless: styled page (ext CSS 2 rules applied, JS fetched+skipped), scroll/link-click/nav all PASS at adapted fullscreen geometry (test_nav/test_links hardcode the old 1010,60 window — stale, fail on geometry, not on render). **Open:** h1 cascade color on external sheets, SYN-loss stall on refused ports (no RST handling), test_nav/test_links geometry refresh for the maximized browser.

**2026-09-26 — browser rendering overhaul (this session).** Replaced the flat-token renderer with DOM+CSS+layout (see `dom.c`/`css.c`/`layout.c`). Fixed the 16-bit arena-offset corruption (#7) and the inert UA stylesheet (#8); wired external CSS fetching (#28) and disabled external JS execution (#27). Added `draw_char_cell` bg fill (#9), HTML-decoded `<link>`/`<script>` URLs, explicit-`http://` respect, and `#define lay`/`g_lay` cleanups. Host suites green; text build green. **Open:** the `draw_char_cell` full-content-area noise (deterministic) and the stalled sub-resource fetch / `[poll]` spam.

**2026-09-12 — crypto hardening rounds 1-4 (commits 3035064, 7cfd158, 6ae95cc, 1ea10d8).** Four external reviews, all findings fixed and re-attacked: RNG tiering + fail-closed labels; certificate NC/SAN fail-closed + truncation; stapled-OCSP validation + Must-Staple; preloaded first-visit pins; deterministic-VM randomness (boot jitter + PFS seed); X25519 branchless field arithmetic + fail-closed API; hostname canonicalization; reused line-oriented HTTP framing; Poly1305 streaming API; SH reassembly; truncation integrity (mid-record EOF fatal, close_notify snapshot). Adversarial 183/183 + ASan/UBSan; differential tooling committed. **Open (accepted/documented):** OpenSSL-PSK-decline quirk, CT verification deferred, HTTP/2 future, TLS-Attacker resumption-continuity limitation.

**2026-09-11 — TLS interop + OCSP/pins.** `ocsp.c` (issuer-signed, freshness, certID SHA-1/256); pin persistence (export/import + VFS hooks + reboot-survival); independent-stack interop loop; PSK offer bugs (binder prefix, kex mode, CH1 truncation). One undiagnosed silent-TLS-stall flake recorded honestly.

**2026-09-10 — reviews, resumption, exec root-cause.** `make host-tests`; P-384 CV proof; scheduler generation guard; **exec resumed at EIP=user_esp / ESP=entered_ring3 root-caused** (PCB indexing — hence the `offsetof`-only rule); external review round 2 (78/78 + ASan); session resumption, OCSP offer, TOFU pinning; `test_links` 5/5; `resolve_href` port-drop fix; **usermode retired** (Phase 6 exit).

**2026-09-09 — PKI + adversarial.** Full TLS 1.3 server authentication (chain, hostname, CV); adversarial suite (cert-verify mutation fuzz + 19-mode mock server); [ring-3 shell cutover debugging].

**2026-09-08/07 — userland & kernel foundations.** High-half kernel (`linker-high.ld`, LOW trampoline, boot GDT, identity FB); preemptive scheduler via `process_switch_to`; syscall ABI 0-17; PFS + ATA first slice; better TCP; ring-3 entry (4 stacked GDT/IRET/paging/ESP0 bugs fixed).

**2026-08 — okai browser polish.** Firefox-blue pixel chrome + theming; clickable toolbar nav + nav test; heading hierarchy (H1=3×/H2=2×); tabs/home/z-order fixes; terminal wheel scrollback; Cyrillic/tables; host preview; FPS regression fixes; okvm headless harness.

---

## Next Steps

1. **TLS coverage**: TLS 1.2 (ECDHE + AES-GCM/ChaCha20, needed for whatwg.org, baidu.com, arstechnica.com) and the SHA-384 schedule for TLS_AES_256_GCM_SHA384. The intermittent resumption failure noted 2026-09-29 should be re-checked now that stale previous-connection bytes are dropped.
2. **JavaScript**: canvas 2D, WebSocket over TLS, layout-dependent APIs that need a fresh layout mid-script, performance on heavy SPAs (budget slices).
3. **Engine fidelity**: GitHub's right "About" sidebar and button wrapping, `position: sticky`, transforms beyond translate, smoother JPEG chroma upsampling, CJK TrueType fallback, `<select>` popup menu.
4. **Networking**: HTTP/2 (multiplexing would replace the 6-connections-per-host fan-out), POST bodies, a congestion window for real (non-SLIRP) networks, incremental layout for script layout queries (scripts that poll `offsetWidth` in a loop hit the 4s budget).
5. **Deferred (unchanged):** CT/SCT verification, HTTP/2, OpenSSL-PSK inquiry.

---

# KAnarchy OS — Handoff

**KAnarchy OS** is a from-scratch OS in C and x86 assembly (no libc, no Linux, no third-party code except fonts). The kernel is **okernel** (also the repo folder), the desktop is **Okapi**, the browser is **okai**. It runs in QEMU (WHPX/KVM), with an optional real-laptop build. Refreshed 2026-10-16.

Read order: this file (orientation), `AGENTS.md` (the living rulebook: one bullet per finding, the traps are there), `LAPTOP.md` (real-laptop image), `tests/headless/TESTING.md` (test harness).

## Working agreements (the owner's, keep them)

- **From scratch only.** No third-party code of any license. User-approved exceptions: fonts, Realtek's `rtw8821c_fw.bin` (runs on the chip), the AAC Huffman tables read out of FAAD2, CELT written from the RFC 6716 reference source.
- **Do what was asked.** An unrequested `search` command made the owner angry. No extra features.
- **QEMU first.** The real-laptop port is optional and later. Never suggest flashing the stick. Never write power/clock MSRs without the full voltage-id path (a bare PERF_CTL write froze the laptop).
- **Test at normal speed** (WHPX/KVM), not throttled to the laptop.
- **Honest identity.** No Chrome/Firefox claim, no client hints, no faked attestation. Consent dialogs are the user's to click.
- **Done means** built, tested, working in QEMU, and not blowing kernel memory.

## What exists

| Area | State |
|---|---|
| Kernel | High-half at 0xC0100000, paging, preemptive kernel threads, ring-3 processes (fork/exec/ELF/pipes, syscalls 0-17), VFS + persistent FS on ATA (PFS), spinlocks, segregated-fit heap. |
| SMP | ACPI/MADT, LAPIC/x2APIC, APs run only `smp_parallel_for` jobs (nothing else is reentrant). LAPIC one-shot timer lets the idle loop sleep during fetches. |
| Display | Backbuffer with per-row damage spans, framebuffer cursor in the mouse IRQ, write-combining mapping, backends: GRUB framebuffer, virtio-gpu (hardware cursor). Runtime resolution from the bootloader. Intel i915: read-only `gpu` report only. SSE2 only in `simd.c`/`vp9_simd.c`. |
| **Okapi** (desktop) | Window manager (`window.c`: cell-grid or pixel-surface windows), cached AA taskbar (window buttons, FPS chip, clock), GNOME-style title bars, wallpaper JPEG, topmost-by-z click routing. No launcher: everything starts from the terminal (`Ctrl+Alt+T` opens another). |
| Input | PS/2 keyboard (queued, dispatched from the main loop), PS/2 mouse with wheel, xHCI USB pointers (HID report parsing, USB2 hubs, hot-plug), I2C-HID touchpad (laptop), USB mass-storage stick log. |
| Network | e1000 (polled), TCP socket table, DNS cache, DHCP, Wi-Fi RTL8821C (USB stick and the laptop's PCIe card; WPA2, `wifigui` window), TLS 1.3 + Mozilla root store + OCSP, HTTP/2, fetch engine (16 slots, keep-alive, cache, preload scanner, streaming `fetch()` bodies, dead-host fast-fail). |
| Audio | Intel HD Audio output, polled, 48 kHz stereo; `audio_clock()` is the A/V master clock. |
| **okai** (browser) | Firefox-style chrome (`okai_ui.c`), tabs, forms incl. POST, cookies, iframes (+ cross-frame `postMessage`), 8 KB URLs and big cookie jars. Engine in `src/web`: WHATWG parser, CSS cascade, block/inline/flex/grid/table layout, AA painter, TrueType, images, SVG, Canvas 2D, WebAssembly (V8-verified), Workers, IndexedDB, SubtleCrypto, streams, MSE. |
| JS | **ojs**, our engine (`src/ojs`): ES2025, test262 ~98.9%, mark-sweep GC with conservative stack scan, x87 doubles. One realm per document. |
| Media | Own decoders: VP9, H.264, Opus (CELT), AAC-LC; WebM/MP4 demux; `meng` playback engine under `<video>`/MSE and the `play` command. VP9 decodes on a second CPU (`-smp`, `run-windows.bat` passes `-smp 4`): 720p60 plays at 60 shown/s, 1080p60 at 15-35; SSE2 colour conversion, bilinear scaling and loop filter. YouTube plays 25-50 s, then the attestation wall (below). |
| Game | `dmccl` (the owner's language, rewritten in C) + 2D physics + "KAnarchy Wheels" (`game`). |
| Text mode | Separate small VGA-text build (`make text`); also a framebuffer-text report kernel for UEFI laptops. |

## Layout

`boot/` asm entry, ISR, SMP trampoline · `src/` kernel (flat) · `src/net` NIC/TCP/fetch/h2 · `src/crypto` TLS+PKI · `src/web` web engine + prelude (`wjs_prelude.js`) · `src/ojs` JS engine · `src/media` codecs · `src/rtw` Wi-Fi · `src/dmccl`, `src/game` · `fonts/` · `firmware/` · `games/` levels · `userland/` ring-3 programs · `tools/` generators and laptop-stick scripts · `tests/` (host C tests, `tests/web`, `tests/media`, `tests/headless` QEMU suites). Per-file ownership table: AGENTS.md.

## Build, run, test

```
make desktop            # kanarchy-desktop.iso (primary)     make text   # text-mode ISO
make laptop             # BIOS+UEFI laptop ISO               make clean  # when unsure (headers are tracked, -MMD -MP)
make host-tests         # crypto, TLS, PKI, adversarial 209, h2/hpack, acpi, hid, touch, rtw...
make web-tests          # CSS/font/lazy tests, JS regression pages (basic.html must say ALLPASS 37), canvas, wasm vs node, media
make media-tests        # decoders vs ffmpeg/RFC oracles + ASan/UBSan fuzz
```

Build and test run in WSL (`MSYS_NO_PATHCONV=1 wsl -e bash -lc 'cd /mnt/d/projects/okernel && ...'`); Windows has node and Edge (used as references). **Run accelerated**: Windows `run-windows.bat` (WHPX, 1536 MB, HD Audio; add `-serial file:serial.log` for the log), Linux `-accel kvm`. TCG is ~10x slower. QEMU needs `-vga std -device e1000`.

- **Headless**: `tests/headless/okvm.py` drives QEMU; the serial log is ground truth. `python3 tests/headless/run_suite.py -j3 test_*.py`. Core browser set: `test_okai_{js,interact,frames,media,cache,preload,preview,stream,webapi,postmessage,wasm,longurl,reqheaders}.py`, `test_captcha_ddg`, `test_nav`, `test_errors`, `test_tab_x`, `test_sites.py` (15 real HTTPS sites). Fixed-port suites clash in parallel: rerun a failing one alone.
- **Windows/WHPX**: `tests/headless/perf_win.ps1 -Accel whpx -Mem 1536 -Urls <url> -Settle N -Shot out.ppm` (YouTube: `https://www.youtube.com/watch?v=aqz-KE-bpKQ`), `-PreCmd "fetchhdr"` logs request heads. Slow-CPU model: `-icount shift=0` (see AGENTS).
- **Host browser**: `sh tests/web/build.sh`, then `build-host/wbrowse URL|file W H out.ppm secs` (same engine on the host; env `OJS_NOBUDGET`, `OJS_TRACE_THROW`, `OJS_TRACE_MISS`, `WB_EVAL`, `WB_PRE`/`WB_DUMP`). Edge reference tools: `tests/web/trace_edge.mjs`, `headers_edge.mjs`, `surface_make_cmp.py`.
- Shell commands (Okapi terminal): `about audio beep clear cpu dhcp disk echo edit exit fetchhdr game gfxcheck gpu help ip ipconfig locktest ls mem neofetch netdrop nettrace okai open ping play prof ps reboot resolve run save shutdown simd smp stopplay sysinfo terminal touchpad uptime usb wifi wifigui`.

## Rules that bite (details and the story behind each: AGENTS.md)

1. Paging before the framebuffer (identity-mapped, above 4 MB); the boot PD maps phys 0-64M with 4 MB pages and the image must fit.
2. **The cursor lives on the framebuffer**, moved in the mouse IRQ. Never draw it into the backbuffer or recomposite for mouse motion. Direct backbuffer writes must call `graphics_mark_dirty`/`graphics_mark_rect`.
3. Never a bare `sti` in code reachable from an IRQ (keystrokes came out reversed). Keys are queued by IRQ1 and dispatched from the main loop; shell, navigation and page JS never run in an interrupt.
4. The main loop halts when idle; packets are polled. Anything per-iteration that touches MMIO starves the PIT.
5. XMM only in `simd.c`/`vp9_simd.c`; handlers never touch it. ISR stubs `cld` first. `memcpy` is `rep movs`.
6. New `struct process` fields go at the tail; use `offsetof`. `sched_yield` on the trap stack never switches CR3/ESP0. Syscall pointers go through `paging_user_range_valid`.
7. `-fno-pic -fno-pie -mno-red-zone`; `section .note.GNU-stack` last in `.asm`. Big tables on the heap, not BSS.
8. Web engine: integer/fixed-point (LU = 1/64 px), no libgcc (`w_div64`/`w_muldiv`), every engine call through `run_job()` (own 1 MB stack). ojs: doubles at 53-bit x87 precision, never cast double to int in hot paths (`d2i64`/`dtoi32`), values held in C memory must be GC roots.
9. TCP: the advertised window is the ring's free space; fast retransmit counts only pure-ACK repeats. e1000 RX: `RDT` = last processed index.
10. TLS: HKDF labels need `"tls13 "`; transcript hash is over handshake messages, not records; nonce = IV xor sequence; plaintext ends with the content type then zeros; offer only x25519 in `key_share`.
11. Hardware that QEMU cannot show (laptop touchpad, USB quirks, power MSRs) gets a log path (`devlog`, stick log), not blind writes.
12. Tooling: Python patch scripts mangle `\n` inside C string literals (use Edit); if WSL shows files as mode 000, `find . -path ./.git -prune -o -type f -perm 000 -print0 | xargs -0 chmod 755`.

## YouTube and the attestation wall

Watch pages load, search works, video plays smoothly (30 fps steady with `-smp 4`, A/V in sync, streaming fetch, MSE ad hand-over fixed, Range-streamed long videos). TCP window scaling (RFC 7323, shift 2 = the 256 KB receive ring) let YouTube's bandwidth estimate rise: the player now climbs 240p -> 360p -> 480p in the first seconds (it stayed on 240p with the 64 KB window). The player picks resolution from its own estimate and the player size; we do not force formats. After ~25-50 s the video servers answer 107/123-byte refusals and `youtubei/v1/player` returns a 549-byte error: BotGuard's token is not accepted. Signing in does not change it (sign-in itself works since URLs are 8 KB and cookies uncapped).

Measured, not solved (AGENTS "Why is YouTube's BotGuard token rejected"): BotGuard runs to completion here and produces a snapshot the same size as Edge's, but it computes its signals inside its own VM, so API traces cannot show what it dislikes. Fixed on the way: wire User-Agent now equals `navigator.userAgent`, `Accept-Language`, `Referer`, Fetch Metadata, Origin on unsafe methods, WebIDL shapes. Remaining differences from Edge: no Trusted Types, no `Intl`, no WebGL/Web Audio, a stub `about:blank` iframe window, the JS engine itself (not V8). Policy: no spoofing, no Chrome identity.

## Real laptop (optional, later)

Cherry Trail Atom laptop (1.6 GHz, I2C touchpad, RTL8821CE Wi-Fi). Boots via `make laptop` (BIOS and UEFI); touchpad, USB mouse and logging to the boot stick work; the first real runs found most of the hard bugs (I2C base from firmware AML, xHCI ring-end stall, MSR freeze). Wi-Fi PCIe, audio and Intel graphics are untested on hardware. Details in LAPTOP.md and AGENTS.md (touchpad, xHCI, stick-log bullets).

## Known limits

TLS 1.3 only (no TLS 1.2, no SHA-384 suites). HTTP/2 without push; no congestion control. No WebGL, Web Audio, WebRTC, EME, ServiceWorker, XPath, multipart uploads, `position: sticky`; transforms are translate-only; CJK beyond the bitmap table is tofu. Wasm: no SIMD/threads/exceptions. SILK/hybrid Opus and AAC SBR missing. Page JS is slow under TCG (use WHPX/KVM). Single scheduler CPU. In-memory VFS: only PFS-synced files survive reboot.

## Bug classes worth remembering

Stale objects after a header change (keep `-MMD -MP`); 16-bit offsets in big arenas; stale buffers leaking into the next request; DNS/fetch owner races (now a request table); polled hardware starved by a spinning loop; interrupts re-enabled inside handlers; DF set when an interrupt lands in a backward `memmove`; PS/2 init reading config bytes without waiting; clicks routed by slot instead of z-order; cached chrome re-rendered under a small clip; quadratic parser/const-table code on huge bundles; 16-bit const indices; URL/cookie buffers that silently truncate.

## Next steps

1. **BotGuard, if wanted**: Trusted Types (small), `Intl.DateTimeFormat`/`NumberFormat` (large), real `about:blank` iframe realms (large); re-measure each with the trace tools. Odds are low; the server never says why.
2. **Web**: `window` attributes as accessors, hide internal `_names`, `position: sticky`, richer transforms, WebGL/Web Audio only if a real use appears.
3. **Net/TLS**: TLS 1.2 + SHA-384 (whatwg.org, baidu.com), real congestion window for non-SLIRP networks.
4. **JS engine**: lazy function compilation, generational GC, `using`/DisposableStack, remaining test262 edge cases.
5. **Laptop** (only when the owner asks): verify PCIe Wi-Fi, audio, Intel scanout from our own buffer.
6. `sysinfo`/`about` say Okapi and show CPU (brand, MHz, count, speed factor), RAM + heap use, display backend, network + IP, audio, uptime (`sysinfo_text()` in desktop.c).
7. **YouTube masthead / search bar vanishing after sign-in (owner's report): most likely fixed, unverified while signed in.** Cause found while logged out: the `Safari/537.36` token in our UA made YouTube serve its polyfilled legacy page, which hydrated halfway (masthead without hamburger/logo/placeholder, page 1225 px tall); and `<link>` elements never fired `load`, which stalled the player start-up. Both fixed (AGENTS "Why YouTube rendered half-built"). If the search bar still disappears after a real sign-in, look at the hit list in the serial log first: the masthead boxes (hamburger x=24, search input x=657, y=16) should be in it.
8. Cosmetic YouTube leftovers: doubled consent buttons (a `dom-repeat` stamping twice), Shorts shelf titles overlap in the sidebar.

## History (one line each)

- 2026-09: high-half kernel, preemptive scheduler, ring-3 userland, PFS; TLS 1.3 + PKI hardened through four external reviews.
- 2026-09-26/10-07: new web engine (DOM, CSS, layout, painter) replaces the flat renderer; page scripting (QuickJS at first); parallel networking; Mozilla roots; AES-GCM; Firefox-style chrome; taskbar and wallpaper.
- 2026-10-08: cursor moved to the framebuffer, idle halt, key queue; mouse/keyboard/clock/memmove bugs fixed.
- 2026-10-09: SMP, SSE2, damage tracking, virtio-gpu, runtime resolution, i915 groundwork; **ojs replaces QuickJS** (all from scratch except fonts).
- 2026-10-10/11: UEFI laptop image, ACPI power, HD Audio, Opus/VP9/H.264/AAC + media engine, Canvas 2D; xHCI pointer driver, I2C touchpad, stick log.
- 2026-10-12/13: Wi-Fi (USB then PCIe), slow-CPU model, idle LAPIC sleep, iframes, forms/POST, dmccl + game.
- 2026-10-14/15: page-load speed (cache, preload scanner, HTTP/2, preview), YouTube home/search/consent fixes, layout memo, A/V sync.
- 2026-10-16: MSE ad hand-over, stale-wake-up and fetch-starvation fixes, big Web API wave (Workers, IndexedDB, SubtleCrypto, WebAssembly, streams), streaming `fetch()`, 8 KB URLs, WebIDL shapes, BotGuard investigation, honest HTTP request metadata; desktop named **Okapi**.

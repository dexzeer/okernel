# AGENTS.md — KAnarchy OS (okernel kernel)

From-scratch OS in C + x86 assembly. Kernel = okernel, desktop = **Okapi**, browser = **okai** (orientation: HANDOFF.md). Two targets: text mode (VGA text) and desktop mode (GRUB framebuffer, 1920x1080 requested). Runs in QEMU and on a real laptop. No standard library, no Linux, no BIOS calls in protected mode.

This file was condensed on 2026-10-18. The long per-feature history it used to carry (measurements, bug hunts, exact numbers) is in git: `git show b02375e:AGENTS.md`. Keep this file to rules, traps and pointers; put narrative in HANDOFF.md or commit messages.

## Build and run

```bash
make desktop       # kanarchy-desktop.iso (primary)
make text          # kanarchy-text.iso
make clean         # nukes .o, .bin, .iso, isodir/
```
Needs `gcc` (multilib), `nasm`, `ld`, `grub-mkrescue`, `xorriso`, `mtools`. Other images: `make laptop` (BIOS+UEFI), `kanarchy-res.iso VID_W= VID_H=`, `kanarchy-x2apic.iso`, `kanarchy-i915test.iso`.

```bash
qemu-system-i386 -accel kvm -accel tcg -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0
```
Use hardware acceleration (TCG is ~10x slower). Windows: `run-windows.bat` (x86_64 QEMU, WHPX, `-smp 4`, 1536 MB, AC'97 audio; the i386 QEMU build has no WHPX). Serial debug: add `-nographic -serial stdio` — serial is ground truth for network bugs.

## Kernel gotchas

- **Paging/boot.** Desktop links high-half at 0xC0100000 (`linker-high.ld`; text keeps `linker.ld`). LOW trampoline `_start` (e_entry 0x100030) + private boot GDT → `_start_high` → `kernel_main`. Boot PD maps phys 0-64M with 4 MB PSE pages; image + .bss is ~11 MB (a 4M map triple-faults before serial init). Order: memory → mboot copy → `paging_init` → process → graphics (identity FB). Framebuffer is above 4 MB: never touch it before paging. Big tables go on the heap, not BSS. `V2P`/`P2V_U32` in `src/memlayout.h`. The usable kernel address space is ~735 MB; more RAM does not help.
- **Compiler flags:** `-fno-pic -fno-pie -mno-red-zone` required. Only `src/simd.c`, `src/media/vp9_simd.c`, `src/media/vcvt.c` are built `-msse2` and only they may use XMM; everything else `-mno-sse` (x87 for ojs). Interrupt handlers must never touch XMM (not saved there). `section .note.GNU-stack` must be the LAST section in .asm files.
- **Makefile uses `-MMD -MP`:** never drop it (a stale header once left a struct at the old size and made every HTTPS fetch fail). When in doubt `make clean`.
- **Interrupts:** never a bare `sti` in code reachable from an IRQ (save/restore IF; `rng_cli/rng_sti`, `irq_save`). ISR/IRQ stubs `cld` before calling C (an IRQ inside memmove's `std` region otherwise ran string ops backwards). Keystrokes are only queued by the IRQ (`key_q`); `keys_dispatch()` in the main loop runs them with their own modifier state. Page JS (ojs is not reentrant), navigation and shell commands never run from an IRQ.
- **Main loop:** halts when idle (`okai_wants_cpu`: 1 CPU work, 2 requests in flight, 3 timed wait, 0 nothing → LAPIC one-shot or `hlt`). The halt is `cli; check key queue; sti; hlt`. `[fps] N loops=M busy=P%` is the 1 Hz perf line. During long page scripts the ojs hook calls `engine_pump` every 10 ms (media tick, network, input; it never draws or calls into the page).
- **Cursor** is NOT in the backbuffer: alpha-blended onto the framebuffer in the mouse IRQ (`src/cursor.c`). Never draw a cursor into the backbuffer. PS/2 init reads every controller reply in order with a wait; the IntelliMouse wheel handshake (200/100/80 + 0xF2, wait for ACK then ID) must keep its ordering.
- **Graphics:** backbuffer + dirty spans (`mark_span`); code writing `graphics_get_buffer()` must call `graphics_mark_dirty/graphics_mark_rect`. Shell `gfxcheck` verifies framebuffer == backbuffer. Framebuffer is write-combining (PAT entry 1). Screen size comes from the bootloader (`gfx_w/gfx_h`; static tables sized for 1920x1080 or 4096x2304). Display backends: GRUB framebuffer (fallback), virtio-gpu (`-vga virtio`, backbuffer is the resource backing; polled), i915 = read-only probe only (shell `gpu`).
- **Scheduler:** preemptive via `process_switch_to`/`context_switch`; never switches from ring-3 trap stacks. `sched_yield()` on the INT 0x80 stack only resets the slice. `process_switch` (CR3+ESP0) is handoff-point-only. New `struct process` fields go at the TAIL; use `offsetof`, never index PCB words by hand.
- **Syscalls:** INT 0x80 ABI 0-17 (print, exit, write, read, open, close, fork, exec, sbrk, pipe, dup, wait, kill, mmap, munmap...). Every user pointer goes through `paging_user_range_valid()` first. `enter_user_mode` takes eip→EAX, esp→EDX.
- **SMP** (`acpi.c`, `lapic.c`, `smp.c`, `boot/ap_tramp.asm`): APs only run jobs (`smp_parallel_for(fn, ctx, n, grain)`, `smp_async_start/done`); they never touch the scheduler, windows, serial or okai. Jobs may kmalloc/kfree and use x87/SSE. **Nothing called from inside an `smp_parallel_for` item on the boot CPU may take `pub_lock`** (`smp_async_start` returns -1 there; nested parallel jobs run inline) — violating this froze the kernel when a parallel GC ran during video. Tests: shell `smp test`, `tests/headless/test_smp.py`, `tests/test_acpi.c`. Hooks: `OKVM_SMP=n`.
- **Clocks:** `tick_count` follows the TSC under a hypervisor (QEMU on Windows loses 15-40% of PIT interrupts); anything counting timer INTERRUPTS (sched slice, `audio_tick`) runs at the lost rate. `pit_delay_us` was broken before 2026-10-10.
- **Memory:** segregated-fit heap and page allocator are spinlocked. The first write to a guest page costs ~19 µs under WHPX; `heap_pretouch` pre-faults in the idle path. Picture slots shrink/cap when free memory is low (`out_budget_px`, `frame_budget`).
- **memcpy/memset/memmove** are `rep movs/stos` inline asm (so GCC cannot turn them into self-calls); memmove's backward path sets and clears DF.
- **PFS/ATA:** OKPFS1 write-through (`fs_delete` copies the name before clearing; static hydrate/sync buffers, the trap stack is 4 KB). OkVM disk tests use `OkVM(tag, disk=path)`; a stale `-hda` QEMU holds the image lock.
- **Image size:** `_kernel_end` must stay in the boot PD's 64M window; UEFI mbi must sit below 64 MB.

## Hardware and platform work (all on a Cherry Trail laptop; details in HANDOFF.md / `LAPTOP.md`)

- **Boot/UEFI:** `kanarchy-laptop.iso` boots BIOS and UEFI; `src/fbtext.c` draws the text-mode report on the GOP framebuffer; `memory_init` frees only type-1 map pages above the kernel; MULTIBOOT flag bit 0 = mem.
- **Power:** `src/acpi_pm.c` (FADT, `_S5_` from the DSDT, reset register, 0xCF9/KBC fallbacks). Verified in QEMU only.
- **CPU:** `src/cpufreq.c` calibrates TSC/core clock and a small benchmark → speed factor (dev PC = 1, laptop ~6-8) that scales JS time budgets, blocks muted autoplay until a click, caps codec levels and enables the media governor; shell `cpu`, `cpu slow N|auto`. **It WRITES NO power/clock MSRs** (PERF_CTL ratio without the VID froze the laptop). MSR access is fault-safe (`rdmsr_safe/wrmsr_safe`; the new EIP store in `isr_handler` must be volatile + compiler barrier).
- **Input:** `src/xhci.c` + `hid.c` (USB mouse/tablet/hub driver, report-descriptor parser, bulk-only storage for the stick log, SuperSpeed switch-off on 8086:22b5; `nousb`, `usb3`, `usbss` on the command line); `i2chid.c` + `dwi2c.c` + `hidi2c.c` + `i2caml.c` + `ptp.c` (I2C-HID touchpad whose controller address is read from the firmware AML; `noi2c`, `i2cscan`); quirks of the SYNA3602 are in the comments. Shell: `usb`, `touchpad`, `input`.
- **Stick log:** `src/klog.c` writes to the last 4096 sectors of a boot stick marked `KANARCHY-LOG-V1` (`tools/stick/flash.ps1`, `readlog.ps1`): devlog, status, ACPI tables, and the serial ring (so `[okai]`, `[js]`, `[media]`, `[prof]` lines come back from the laptop).
- **Audio:** `src/hda.c` (Intel HDA, polled, fixed 48 kHz/16 bit stereo, 128 KB ring), `src/ac97.c` (QEMU AC'97; `run-windows.bat` uses it because QEMU's HDA position runs 1.0-1.24x with dropouts on Windows), `audio.h`, `audio_rate.c` (observe-only speed measurement), smoothed `audio_clock()`. Headless: `OKVM_AUDIO=out.wav`, `OKVM_AUDIO_CARD=ac97`.
- **Wi-Fi:** `src/rtw/` (Realtek RTL8821C, USB and PCIe backends, firmware blob `firmware/rtw8821c_fw.bin` is the approved exception), `wsta.c` (WPA2-PSK, CCMP), DHCP in `network.c`, window `src/wifigui.c`. Verified on real hardware via USB; PCIe untested. Tests: `make host-tests`, `tests/headless/test_dhcp.py`, `test_wifigui.py`, `tools/wifi/*`.

## Web engine, JS and okai

- **Engine rules (`src/web`):** integer/fixed-point only (LU = 1/64 px), no FPU; no libgcc (64-bit `/` `%` link `__divdi3`: use `w_div64`/`w_muldiv`); every engine call from okai goes through `run_job()` (private 1 MB stack). Sources also build on the host (`tests/web/build.sh`, `make web-tests`): fix layout bugs there against Edge (`tests/web/compare.py`) then rebuild the kernel. Exception: `wcanvas.c` and `wasm.c` use doubles.
- **ojs** (`src/ojs`, our JS engine; see `src/ojs/README`): NaN-boxed values, stack bytecode VM, mark-sweep non-moving GC with conservative C-stack scan (values kept in C heap memory must be roots: `ojs_add_root[_range]`; wjs re-registers on growth). Every realm entry sets x87 to 53-bit precision; never cast double→int in hot paths (use `d2i64/d2u32/dtoi32`). Realm heap = half the kernel heap (96-512 MB), 4 s per script / 3 s per task (× CPU speed factor). Inline caches: 2-way, prototype chains, accessors, property additions, survive GC; any change to prototype/shape handling must keep `tests/ojs/ic_semantics.js` passing (also under `OJS_GCSTRESS`). Parallel GC marking for heaps ≥ 24 MB (`gcpar on|off`, `OJS_GCPAR_VERIFY=1` checks it). Gates: test262 (`tests/ojs/run262.py`, baseline fail list `build-host/t262_base_fails.txt`, 78983/79960), corpus 11/12 vs node, smoke/ic_semantics/upvals/manyconsts.
- **Baseline JIT** (`src/ojs/jit.c`, `vm_body.c` included twice as `vm_run` and `vm_step`): ON (tiered, threshold 40 calls) by default in the kernel; OFF on the host (`OJS_JIT=1|2`, `OJS_JIT_THRESHOLD`, `OJS_JIT_STATS`, `OJS_JIT_DUMP`); shell `jit off|on|eager|N`. Values stay in the interpreter frame; stack depths are static (`jan_run`, effect table `op_fx`, checked by `-DOJS_JIT_CHECK`); anything unsupported or failing a guard runs in `vm_step`; slow paths are C helpers `jit_h_*` (exception answer 0 → unwinder); code lives in dedicated 256 KB whole-page pools (QEMU TCG invalidates translations of written pages). Every JIT change must keep the failing set of test262 identical with `OJS_JIT=2` and `OJS_JIT=1 OJS_JIT_THRESHOLD=2`, and kernel tests under `OKVM_PRECMD="jit eager"` (run them one at a time). The JIT does not speed up YouTube page load (style/layout dominate); it speeds up pure JS loops 1.5-3x.
- **Web APIs:** prefer plain JS in `src/web/wjs_prelude.js`; add a C native in `wjs_dom.c` (on the `W` object) only if it needs engine internals. Done in the prelude/natives: DOM + MutationObserver/ResizeObserver/IntersectionObserver, fetch/XHR (streaming bodies, Range, POST), Blob/File/FileReader, streams, crypto.subtle (not constant time), Workers (run in the page realm in a sandbox), MessageChannel, window.postMessage between iframes (`RemoteWindow`), IndexedDB/Cache (in memory), canvas 2D (`wcanvas.c`), WebAssembly (`wasm.c`, MVP + 2.0 subset), HTMLMediaElement/MediaSource. Missing: WebGL, WebAudio, WebRTC, EME, WebCodecs, ServiceWorker, Trusted Types, `Intl`, `navigator.userAgentData`. `navigator`/`screen`/`performance` follow WebIDL shape (getters on prototypes). `tests/web/js/surface.js` + `edge_surface.txt` diff the API surface against Edge. Tests: `make web-tests`, `tests/web/test_webapi.sh`, `tests/web/js/*.html` (`basic.html` must stay ALLPASS).
- **Paint/layout:** layout memoises BFC subtrees (`lay_box`, disable with `WB_NOMEMO=1` for A/B); hit testing follows paint order (`wlay_paint_rank`); the painter repaints only damaged item rectangles (`wpaint_inc`; anything that clears/edits a surface behind the painter must call `wdoc_paint_invalidate`; shell `damage on|off|check`); style/layout/paint loops call `W_PROGRESS()` to service media. Style (`css_compute_all`) is the dominant page-load cost; the rule index files rules under key tags. Stylesheets are shared by content hash (`d->sc[]`).
- **okai** (`okai.c`, chrome in `okai_ui.c`): tabs, request table (`struct oreq`; finished requests are DETACHED before delivery because delivery can navigate), preload scanner (`pscan.c`) + preview of large documents, lazy images, iframes as tab slots ≥ `OKAI_MAX_TABS` (own ARGB bitmap, routed input), forms incl. POST (no multipart), `<label>` forwarding. `ui_layout()` is the only chrome geometry source (draw and hit tests share it); per-frame state must be in `state_sig()`. Damage repairs never re-render cached chrome. URLs up to 8192 bytes, cookies up to ~9.8 KB. There is deliberately no search feature in the address bar.
- **Networking:** polled e1000 (256 RX descriptors) or Wi-Fi shim; TCP socket table (16 sockets, 256 KB receive rings, window scaling, SACK, 64 OOO slots; fast-retransmit counts pure ACKs only; the advertised window is the ring's free space); DNS over HTTPS first (cloudflare, google, quad9) then UDP (shell `doh`); `src/net/fetch.c` = up to 16 parallel GETs over a keep-alive pool (`CONN_MAX` 12, 6 per host), HTTP/2 (`h2.c`, `hpack.c`; one session per origin), recent-failure list (`neg_*`), in-memory cache (`fcache.c`). Per-second `[net]` line shows rx/tx/loss/unread. Per-packet logs behind shell `nettrace`.
- **TLS** (`src/crypto/`): TLS 1.3 only (ChaCha20-Poly1305, AES-128-GCM), X.509 chain verification anchored on Mozilla's roots generated from NSS `certdata.txt` by `tools/gen_roots.py` (never regenerate from the Debian PEM bundle: it loses distrust-after dates), name-bound `root_issued()`, OCSP staples (only REVOKED / Must-Staple hard-fail), signature memo, Shamir ECDSA verify. Suites: `make host-tests`, `make host-tests-asan` after crypto/TLS changes; `tls_scan` over `tests/tls_hosts.txt`.
- **Fonts and wallpaper:** Noto fonts in `fonts/` (the only third-party code allowed); wallpaper JPEG is decoded at boot (`tools/wallpaper/`).

## Media (own decoders, all written from the standards)

- **Codecs:** Opus CELT (all channel layouts incl. multistream/5.1 downmix; SILK/hybrid frames decode as SILENCE), VP9 (bit-exact on 23 ffmpeg vectors; SSE2 MC and loop filter; tile-parallel decode and a loop-filter wavefront), H.264 Baseline/Main/High progressive 8-bit (CAVLC+CABAC; interlaced/10-bit unsupported), AAC-LC (no SBR/LTP/Main), WebM and MP4 demuxers (incremental, fragmented MP4 ok), polyphase resampler to 48 kHz. Test vectors are made with a host ffmpeg (`tests/media/gen_*_vectors.py`, git-ignored `tests/media/vec/`); the RFC 6716 reference decoder in WSL (`~/opus_oracle`) is the Opus oracle. Run `make media-tests`, `sh tests/media/build_meng.sh`, `build_h264.sh`, `build_aac.sh`.
- **`meng`** (`src/media/meng.c`) is the playback core under `play <url>` and `<video>/<audio>`/MediaSource: source buffers → packet queues ordered by DTS → audio decoded ~0.5 s ahead into the card → video decoded on a worker CPU (`vw_*` ring, convert jobs in `vcvt.c`) → newest due frame copied to the page. Media clock = audio clock (rebased after underruns); with no sound card a 100 Hz tick drives it. The clock is HELD (stalled at the requested position) until tracks are known and ≥ 0.3 s is buffered (`clock_start_or_hold`; test `test_play_before_data`). One session owns the sound card (`WM_MAX` 6). `timestampOffset`, `appendWindow`, `changeType` are supported; mixed codecs in one queue are not.
- **Web side:** `src/web/wjs_media.c` + prelude (`mimeKinds`, `levelCap`, `mediaStream`: plain `<video src>` is read with 2 MB Range pieces, 45 s read-ahead; seeking past the buffered part, MP4 with moov at the end, and MP4/WebM with an audio track on a machine with NO sound card do not work). Natives are driven from the main loop (`media_pump`, `wjs_media_poll`), also from the 20 ms script hook. Shell: `vidfps` (corner overlay + `[vidfps]` serial line: dec/scr/drop/gap/buf), `vp9par`, `progress`, `play`. `[media] rate:` once a second while playing.
- **YouTube:** watch pages load and play (VP9 + Opus through MSE, SABR over POST) at 28-31 fps with ~0 dropped under WHPX. **Open issue — playback is cut after ~25-60 s:** the video servers answer SABR requests with a 107/123-byte message (UMP parts PLAYBACK_START_POLICY + STREAM_PROTECTION_STATUS = ATTESTATION_REQUIRED) and `youtubei/v1/player` returns `400 FAILED_PRECONDITION`, although the page's own BotGuard run completes and GenerateIT returns an integrity token. Signed-in did not change it. What was measured (API-trace diffs against Edge; BotGuard computes its signals inside its own VM from JS-engine behaviour, so an API trace cannot show them; the tracer tools were removed on 2026-10-20 with the rest of the investigation and can be recovered from commit `a56957e`) is in git history; known remaining differences from a real Chrome: no Trusted Types, no `Intl`, no WebGL/WebAudio, stub `about:blank` iframe realms, a different JS engine. Not a decided direction: whether and how to address this is the owner's call, not a settled project rule. From EU exit IPs a cookie-consent lightbox pauses the video until the user clicks it (`perf_win.ps1 -ClickAt "932,857@16"` in tests).
- **Local files:** `node tools/media/filesrv.js` (+ `filesrv.bat`; test `node tests/media/test_filesrv.js`) serves folders + a player page on 127.0.0.1:8010 (Host header allowlist; Range support); in the guest open `http://10.0.2.2:8010/`.
- **Repro pages (`tools/media/`):** `p720y.html` (local 720p60 + YouTube iframe), `gcvid.html` (video + big heap churn), `slowsrv.py` + `slow.html` (first `.webm` delayed), plus 720p/1080p clips under `build-host/www` (serve with WSL `python3 -m http.server 8005`, guest URL `http://10.0.2.2:8005/...`).

## Games

dmccl (user's own language, `src/dmccl/`, reference `src/dmccl/README.md`) + KAnarchy Wheels (`src/game/`, `src/gamewin.c`, levels `games/*.dmccl`; shell `game`, `game N`, `game FILE`). Own 2D rigid-body engine and renderer. Tests: `sh tests/dmccl/run.sh`, `sh tests/game/run.sh` (every shipped level must be winnable by the random-key bot `tools/game/bot.c`), `tests/headless/test_game.py`.

## Profiling and perf tooling

- Shell `prof on` logs `[prof] zone=Mcycles/calls` each second and runs the stall sampler (`[stall]` lines, resolve with `python3 tools/stallsym.py kanarchy-desktop.bin perf.log`).
- Windows/WHPX driver: `powershell -File tests\headless\perf_win.ps1` (flags: `-Accel -Smp -Mem -Prof -StampVid -Urls -Settle -Shot -ShotAt -ClickAt -AudioWav -PreCmd -NetDev -AudioCard -Display`). Pass WAV paths as literal Windows paths. **Measure at normal WHPX speed, not throttled.** The weak-CPU model (`-Accel tcg -Extra -icount,shift=0 -Slow 8`) is only for the laptop case. Never play test audio through real speakers (record to WAV; don't use `-AudioSdl` in tests).
- Host: `build-host/wbrowse url [w] [h] [out.ppm] [seconds]` (fetch cache in `~/.cache/wbrowse`; `WB_EVAL`, `WB_PRE`, `WB_DUMP`, `WB_BODY`, `OJS_NOBUDGET=1`, `OJS_JSPROF=1`, `OJS_TRACE_THROW`); gprof needs `SAN="-pg -static"`; load time under the default 8 s cap is not a fixed-work metric.
- Page-load timelines: `tests/headless/perf_load.py`; `[wdoc] update Nms` and `[js-gc]` lines in the serial log.

## File ownership

| Area | Files | Notes |
|------|-------|-------|
| Boot | `boot/start.asm`, `boot/isr.asm`, `boot/ap_tramp.asm`, `boot/smp_stubs.asm` | Multiboot header, ISR stubs, AP trampoline |
| Display | `src/graphics.c/.h`, `display.h`, `vgpu.c`, `virtio.c`, `i915.c`, `fbtext.c` | Framebuffer, dirty spans, backends |
| Taskbar/UI | `src/taskbar.c`, `src/ui_draw.h` (impl in `okai_ui.c`), `theme.h` | Cached AA strip, shared AA primitives |
| Windows | `src/window.c/.h` | Window manager; cell grid or pixel surface |
| Mouse/USB/touch | `src/cursor.c`, `xhci.c`, `hid.c`, `i2chid.c`, `dwi2c.c`, `hidi2c.c`, `i2caml.c`, `ptp.c` | See hardware section |
| Desktop | `src/desktop.c` | Main loop, shell commands, window creation |
| Text mode | `src/kernel.c`, `vga.c`, `terminal.c`, `shell.c` | Alternative build |
| Memory | `src/memory.c/.h` | Page allocator + coalescing heap, spinlocked |
| SMP/ACPI | `src/acpi.c`, `acpi_pm.c`, `lapic.c`, `smp.c` | See kernel gotchas |
| Interrupts | `src/gdt.c`, `idt.c`, `keyboard.c` | GDT+TSS, IDT, PIC |
| Paging | `src/paging.c`, `memlayout.h` | High-half map, MMIO windows, user ranges |
| Processes | `src/process.c`, `sched.c`, `syscall.c`, `sys_proc.c`, `elf.c`, `spinlock.c` | Process table, scheduler, syscalls |
| Disk/FS | `src/ata.c`, `pfs.c`, `filesystem.c`, `editor.c`, `textslot.c` | PIO ATA, OKPFS1, VFS, editor |
| Browser | `src/okai.c/.h`, `okai_ui.c` | Tabs, fetch driver, forms, chrome |
| Web engine | `src/web/*` | `wdoc`, `wdom`+`html5`, `css_*`, `lay_*`, `paint`/`raster`, `font`, `image`, `svg`, `wurl`, `wcanvas`, `wasm`, `pscan` |
| JS | `src/ojs/*`, `src/kmath.c`, `divdi3.c`, `kclock.c`, `src/web/wjs*.c`, `wjs_prelude.js`, `wcookie.c` | One realm per document |
| Networking | `src/net/*` (`pci`, `e1000`, `network`, `fetch`, `fcache`, `h2`, `hpack`), `src/rtw/*`, `wifigui.c` | TCP, DNS/DoH, HTTP/1.1+h2, Wi-Fi |
| Crypto | `src/crypto/*` | SHA-2, ChaCha20, AES-GCM, X25519, TLS 1.3, X.509, roots, OCSP |
| Media | `src/media/*`, `src/mplay.c`, `hda.c`, `ac97.c`, `audio_rate.c` | Decoders, demuxers, `meng`, audio drivers |
| Games | `src/dmccl/*`, `src/game/*`, `src/gamewin.c`, `games/` | See Games |
| Libc | `src/string.c/.h` | memcpy/memset/memmove, memcmp, strlen |
| Debug | `src/serial.c`, `io.h`, `prof.c`, `klog.c`, `devlog.c` | Serial + ring, profiler, stick log |

## Architecture in 30 seconds

1. GRUB loads the kernel at 1 MB with multiboot info (memory map + framebuffer).
2. `start.asm` LOW `_start` builds the boot PD → `_start_high` → `kernel_main()`.
3. `kernel_main()`: GDT → IDT → memory → paging → process table → graphics → windows → mouse → keyboard → USB/I2C/Wi-Fi/audio → networking → `sti` → main loop.
4. Main loop: keys → mouse → `okai_poll()` (fetches, sub-resources, coalesced renders) → JS timers/media → windows → flush dirty rows; sleeps when idle.
5. Scripts, stylesheets and images are fetched in parallel after the document parses; scripts run in the document's ojs realm under time budgets.

## Adding features

- **Shell command:** `shell_execute()` in `desktop.c` (desktop) or `shell.c` (text), `str_eq(cmd_buf, "name")` pattern.
- **Window:** `window_create()`, `window_puts()`, `window_set_close_button()`.
- **Interrupt handler:** stub in `isr.asm` (`ISR_NOERRCODE`/`ISR_ERRCODE`), register in `idt_init()`.
- **Drawing primitive:** `graphics.c`, write to `backbuffer[]`, mark dirty.
- **Web API:** see "Web APIs" above; test on the host first.
- **Process feature:** `process.c/.h`, new fields at the tail.

## Text vs desktop

Separate binaries from different source sets. Text: `kernel.c vga.c terminal.c shell.c`. Desktop: `desktop.c graphics.c window.c paging.c filesystem.c editor.c textslot.c okai.c web/* ojs/* net/* crypto/* media/* ...`. Shared: `gdt.c idt.c memory.c serial.c keyboard.c mouse.c io.h`. Test both builds when touching shared code. Networking, TLS, web engine, ojs, okai and media are desktop-only.

## Headless testing (okvm)

`tests/headless/okvm.py` drives QEMU headlessly; output in `~/okvm/`. Docs: `tests/headless/TESTING.md`; parallel runner `python3 tests/headless/run_suite.py -j3 test_*.py`.

```python
import sys, time; sys.path.insert(0, 'tests/headless')
from okvm import OkVM
vm = OkVM("tag")
vm.wait_for("[mem] heap", timeout=40); time.sleep(10)
vm.type_string("okai\n"); vm.wait_for("home rendered", timeout=30)
vm.click_link("example.com"); vm.wait_for("parse: count=", timeout=70)
vm.kill()
```

- The serial log (`~/okvm/<tag>_serial.log`) is ground truth, never guess from pixels. `wait_for` matches ANY earlier occurrence (count `parse: count=` lines for "next page loaded").
- Mouse is relative, applied 1:1. Click helpers are closed-loop on kernel log lines: `click_link`, `click_at(page_x, page_y)`, `click_ui(name)`, `click_screen(x, y)`, `taskbar_rect(win)`. Never hard-code chrome pixels. Wheel under HMP: `mouse_move 0 0 -1` scrolls DOWN.
- Run after touching window/mouse/keyboard code: `test_nav.py`, `test_okai_interact.py`, `test_desktop_ui.py`, `test_typing.py`, `test_boot_keys.py`. Hover/repaint races only show with real-mouse timing: `powershell -File tests\headless\hover_win.ps1` (WHPX). Other end-to-end tests: `test_okai_js.py`, `test_okai_page.py`, `test_sites.py` (a plain-HTTP fallback counts as failure), `test_okai_media.py` (timing-marginal under TCG; runs with `OKVM_SMP=2`), `test_okai_frames.py`, `test_okai_postmessage.py`, `test_okai_stream.py`, `test_okai_wasm.py`.
- Env knobs: `OKVM_ACCEL=kvm`, `OKVM_ISO=path` (test another build), `OKVM_PRECMD=...`, `OKVM_VGA=virtio`, `OKVM_SMP`, `OKVM_CPU`, `OKVM_TRACE='ps2_*'`, `OKVM_PCAP=1`.
- Under TCG QEMU sometimes stops running the guest for 0.2-0.6 s, and its PS/2 queue holds 8 keys: an occasional garbled typed word in `test_sites` with `lost>0` in the PS/2 trace is QEMU, not the kernel.
- Stale QEMU processes from an aborted run keep an okvm tag's serial log and make the next run hang: run kernel tests one at a time and `pkill -f "[q]emu-system"` (bracket form; the plain pattern kills the invoking shell).

## Working-environment traps

- WSL from Git Bash: `/tmp` differs between them; use `MSYS_NO_PATHCONV=1 wsl -e ...` and keep scripts inside the repo (`/mnt/d/...`). `sed -i` across drives fails (write to a temp file). Node runs on Windows only. A WSL `git stash pop` shows spurious CRLF "modified" files: check with Windows git. Once 418 files showed mode `----------` in WSL: `find . -path ./.git -prune -o -type f -perm 000 -print0 | xargs -0 chmod 755`.
- Python patch scripts put real newlines into C string literals when the source contains `\n`: make such edits with the Edit tool. Windows filenames cannot contain `<>`.
- Host servers used by tests: WSL `python3 -m http.server 8005`, slow server 8006, filesrv 8010.

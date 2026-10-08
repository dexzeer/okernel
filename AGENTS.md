# AGENTS.md — KAnarchy OS (okernel kernel)

## What this is

From-scratch OS in C + x86 assembly. Two build targets: text mode (VGA text) and desktop mode (1920x1080 GRUB framebuffer). Runs in QEMU. No standard library, no Linux, no BIOS calls in protected mode.

## Build

```bash
make desktop       # Desktop ISO (kanarchy-desktop.iso) — primary target
make text          # Text mode ISO (kanarchy-text.iso)
make clean         # Nukes all .o, .bin, .iso, isodir/
```

Requires: `gcc` (multilib), `nasm`, `ld`, `grub-mkrescue`, `xorriso`, `mtools`.

Desktop QEMU command (must use `-vga std` for framebuffer; `-device e1000` for networking — okai/HTTPS need it):
```bash
qemu-system-i386 -accel kvm -accel tcg -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0
```
**Use hardware acceleration.** Pure emulation (TCG) runs the kernel ~10x slower (a Wikipedia relayout: ~300ms TCG vs ~30ms KVM). Linux: `-accel kvm` (needs `/dev/kvm` access). Windows: `run-windows.bat` — `qemu-system-x86_64 -accel whpx -accel tcg` (the Windows **i386** build has no WHPX; the 32-bit kernel boots fine in x86_64 QEMU; verified 2026-10-07: boot to heap 2s, mouse wheel mode OK).

Serial debug variant (for TLS/network bring-up — serial is ground truth for network bugs):
```bash
qemu-system-i386 -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0 -nographic -serial stdio
```

## Gotchas that will bite you

- **BIOS interrupts don't work.** We're in protected mode after GRUB. No `int 0x10`, no `int 0x13`. All hardware access is via port I/O (`inb`/`outb`).
- **Framebuffer is above 4MB** (typically `0xFD000000`). Must set up identity-mapped page tables in `paging.c` before accessing it. Without paging, writing to the framebuffer = instant triple fault.
- **The cursor is NOT in the backbuffer (2026-10-08, `src/cursor.c`).** It is alpha-blended straight onto the framebuffer, moved inside the mouse IRQ (restore old rect from the backbuffer, blend the new one), and `graphics_flush()` copies dirty rows in short interrupt-free chunks then re-blends it (`cursor_fb_repair`). Never draw a cursor into the backbuffer or recomposite the scene because the mouse moved — the old per-iteration erase/recomposite was the main source of lag and of hover artifacts. Packets apply 1:1 (9-bit sign + overflow, no smoothing), 200 Hz sample rate. PS/2 init (`mouse_init_fb`) disables both ports, flushes, and reads EVERY controller reply with a wait, in order — the old code read the config byte without waiting, so a key held during boot was written back as the config and cleared the translate bit: 'a' typed Enter, every release retyped the key (`test_boot_keys.py`).
- **Never a bare `sti` in code reachable from an IRQ (2026-10-08).** Handlers run after EOI with IF=0; re-enabling interrupts inside one lets the next IRQ of the same line nest — `rand_stir_src` (called from the keyboard IRQ) did `cli`/`sti` and keystrokes came out LIFO ("abcdef" → "fedcba"). Save/restore EFLAGS.IF (`rng_cli`/`rng_sti`, `irq_save` in cursor.c). `test_typing.py` guards it.
- **The main loop halts when idle (2026-10-08).** No composite, no mouse motion/buttons/wheel, no drag, no fetch in flight and no okai work (`okai_wants_cpu`: loading, render pending, animating, JS due) → `hlt` until the next IRQ. Busy-spinning hammered e1000 MMIO so hard that QEMU's timer starved and PIT ticks were lost (kernel clock ~30% slow). `[fps] N loops=M busy=P%` is the 1 Hz perf line (`bench_mouse.py`). The halt is `cli; check key queue; sti; hlt` so a key queued just before it never waits a whole tick.
- **Keystrokes are queued, never handled in the IRQ (2026-10-08).** The keyboard IRQ only pushes (char, Alt-at-press) into `key_q` in `desktop.c`; `keys_dispatch()` at the top of the main loop runs `on_keypress`. Shell commands, okai navigation (frees the document), page JS (QuickJS is not reentrant) and scrolls must never run on top of the main loop from IRQ1. Modifier state for chords (Ctrl+Alt+T) travels with the key — reading `keyboard_alt_held()` at dispatch time misses chords released before the loop gets there.
- **No backbuffer flicker fix is intentional.** We use a backbuffer with dirty-row tracking. `graphics_flush()` copies only changed rows to the framebuffer. If you remove the backbuffer and write directly, you get flicker. If you remove dirty-row tracking, FPS drops from 160+ to ~5.
- **Mouse: IntelliMouse 4-byte wheel mode IS supported** (negotiated in `mouse_init_fb`, `src/cursor.c`). It sends the 200/100/80 sample-rate magic + `0xF2` (get ID), then must wait for the ACK and the ID byte **in order** — draining the output buffer then reading a not-yet-arrived byte desyncs the PS/2 stream and kills movement shortly into the session (HANDOFF bug #25). On `id == 0x03/0x04` it sets `mouse_has_wheel` and the 4th byte is read as the wheel delta. Don't break that handshake ordering.
- **High-half kernel (2026-09-07).** Desktop links at 0xC0100000 (`linker-high.ld`; text keeps `linker.ld`). LOW trampoline `_start` (e_entry=0x100030, ESP 0x7FF00) + private boot GDT (GRUB's 0x08 is invalid) → `_start_high` → `kernel_main`. The boot PD maps phys 0-64M with 4MB PSE pages (identity + high): the image + .bss (boot stack included) is ~11MB now — a 4M boot map triple-faults silently before serial init. Order: memory → mboot copy (fb@88/pitch@96) → `paging_init` (uncond) → process → graphics (identity FB, never `paging_map` window). `V2P`/`P2V_U32` in `src/memlayout.h`. Traps: LOW e_entry, boot GDT, paging-before-process, identity FB, explicit 2nd user page + full 4K copy. See HANDOFF Priority 2.
- **Scheduler is preemptive via `process_switch_to` (2026-09-08+).** `sched_tick()` (timer IRQ) switches kernel threads through the `context_switch` stub (isr.asm) with guards: never from ring-3 trap stacks (defers), never into un-entered/BLOCKED/ZOMBIE slots, `switch_busy` closes the drain window, per-slot `generation` aborts reuse-under-cli. `sched_yield()` on the INT 0x80 trap stack still NEVER switches CR3/ESP0 (slice reset only — the stub will popa+iret to ring 3). `process_switch` (lightweight CR3+ESP0) is handoff-point-only (entry/exit drains).
- **PCB offsets: offsetof only, fields go last (2026-09-10).** `idt.c`'s exec-redirect reads `user_eip/esp` via `offsetof(struct process,...)` — a past hardcoded `pcb[7]/pcb[8]` silently crossed EIP/ESP the moment a field was inserted mid-struct (exec resumed at EIP=user_esp, ESP=entered_ring3). New `struct process` fields go at the TAIL (see `generation`); never index PCB words by hand.
- **Syscall ABI (2026-09-08).** INT 0x80: 0=print, 1=exit, 2=write(fd 1→focused terminal), 3=getpid, 4=yield, 5=mmap_user. All user pointers go through `paging_user_range_valid()` (present+U/S at both levels) BEFORE dereference — a bad ring-3 pointer prints an error, never faults the kernel. `enter_user_mode` uses register convention (eip→EAX, esp→EDX, no stack args); sys_exit resumes via `user_exit_trampoline` (full caller-frame restore).
- **Yield is trap-safe (2026-09-08).** `sched_yield()` runs on the INT 0x80 trap stack (stub will popa+iret back to ring 3) — it must NEVER switch CR3/ESP0 there (the resume faulted as #PF at the next ring-3 EIP). It only resets the slice; handoffs happen at entry/exit safe points. Same class of bug as the timer-switch ban.
- **TCP discipline (2026-09-08).** Fast retransmit counts ONLY pure-ACK repeats (plen==0 threaded through `tcp_process_ack`) — data-carrying segments with a repeated ack field are NOT dup ACKs (counting them fired bogus fast-rtx mid-download and stalled the next connection). Reorder buffer (8×1500B) is per-connection (reset in `tcp_connect`); advertised window is 32KB (the old 60B stub throttled servers).
- **PFS traps (2026-09-08).** `fs_delete` copies the name before clearing (hook needs it); `pfs_sync_file` frees the old run before first-fit alloc; hydrate/sync buffers are static (32KB blows the 4KB trap stack). Editor Ctrl+S/Ctrl+X need the keyboard Ctrl tracker (0x1D) — the status bar advertised them unwired.
- **OkVM disk slot (2026-09-08).** `OkVM(tag, disk=path)` appends `-hda` for PFS tests; default runs stay diskless. A stale `-hda` QEMU holds the image write-lock (`qemu-img` fails) and its monitor socket refuses connects — kill it first.
- **`section .note.GNU-stack`** in .asm files MUST be the last section. If placed at the top, all subsequent code gets swallowed into the non-executable section and the kernel crashes on boot.
- **Compiler flags matter:** `-fno-pic -fno-pie -mno-red-zone` are required. Without them, GCC generates position-independent code with `__x86.get_pc_thunk` calls that crash in a freestanding kernel.
- **Image size** (~2.8MB text incl. 1.5MB embedded fonts, ~7.9MB BSS; `_kernel_end` ≈ 11.3MB phys). Must stay inside the boot PD's 64M window. Put big tables on the heap (segregated-fit `kmalloc`/`kfree`, RAM-sized: 414MB at `-m 512`), not in BSS.
- **Web engine rules (src/web).** Integer/fixed-point only (LU = 1/64 px) — no FPU. No libgcc: 64-bit `/` or `%` links `__divdi3` and fails; use `w_div64`/`w_muldiv`. Every engine call from okai goes through `run_job()` (private 1MB stack via `call_on_stack`). The same sources build on the host (`make web-tests`, `tests/web/build.sh`) — fix layout bugs there against Edge (`tests/web/compare.py`), then rebuild the kernel.
- **QuickJS uses the x87 (2026-10-07).** The web engine itself stays integer-only, but page JS needs doubles: every realm entry sets the x87 control word to 53-bit precision (fldcw 0x027F), the kernel FNSAVE/FRSTORs per thread on preemptive switches, `abort()` longjmps out of the realm (a QuickJS assert kills the page, not the OS). Realm limits: 96MB heap, 8s per script, 1.5s per task. libgcc is linked for QuickJS's 64-bit division only — the engine rule above still holds for `src/web`.
- **TLS trust (2026-10-07).** Chains anchor two ways: a cert ISSUED BY a store root (name-bound: issuer DN == root subject, AKI == root SKI when both exist, signature verifies under the root key — `root_issued()` in `certverify.c`; servers don't send roots), or an in-flight cert whose SPKI is a root key. Certs past the anchor are ignored (expired cross-signs must not fail the path). Root store = Mozilla's, generated from NSS `certdata.txt` itself with `python3 tools/gen_roots.py certdata.txt` (download URL in the script): only roots trusted for server auth, plus Mozilla's distrust-after dates — a chain anchored at such a root is refused when the leaf's notBefore is later (`root_distrusts`). Never regenerate from the Debian PEM bundle: it dropped both, and we were still trusting four roots Mozilla had removed (Entrust, Chunghwa, SecureSign RootCA12, Atos 2011). OCSP staples: only a verified REVOKED status or a Must-Staple leaf hard-fails; stale/unverifiable staples soft-fail like an absent one. The staple's issuer is the flight's 2nd cert ONLY after it is proven to have signed the leaf (a root-anchored or pinned leaf leaves entry 1 unverified). X.509 parsing rejects duplicate extensions and trailing bytes inside extension values (differential-parsing class); NameConstraints honour the leading-dot form (".example.com" = subdomains only). Audit regressions: `test_adversarial.c` section 6 + `MOCK_ENC_CCS`/`MOCK_LATE_CCS`/`MOCK_MS_FORGED_ISSUER` (fixtures: `tests/adversarial/mint_audit.py`). Suites: ChaCha20-Poly1305 (preferred) + AES-128-GCM (`aes.c`, constant-time; Akamai sites need it); no SHA-384 suites, no TLS 1.2.
- **Makefile tracks headers via `-MMD -MP` (2026-10-07).** Never drop it: before it, `src/crypto/*.h` weren't dependencies, a grown `struct tls_state` left `tls_net.o` at the old size, and `tls_state_init`'s memset zeroed the neighbouring fetch-timeout clock — every HTTPS fetch "timed out" instantly and silently downgraded to HTTP. When in doubt, `make clean`.
- **Networking is parallel (2026-10-07).** `network.c` has a TCP socket table (`tcp_open/send/recv/close/abort`, 16 sockets, per-socket retransmit flight + reorder buffer + 256KB receive ring; the advertised window IS the ring's free space — never ACK bytes you can't keep) and a 32-entry DNS cache with concurrent queries (`dns_lookup`: 1/0/-1; ids reused across resends; late answers still update the cache). `src/net/fetch.c` runs up to `FETCH_MAX` (8) GETs at once over a keep-alive pool (`CONN_MAX` 8, 6 per host; idle connections reused, a stale reused connection that dies before the first byte retries fresh). okai keeps a request table (`struct oreq`): main document + up to `OKAI_SUB_PAR` (6) resources per tab, one engine slot always left for navigations. A finished request is DETACHED from the table before its response is processed — delivery can run scripts that navigate, and navigation cancels the tab's requests. The e1000 is polled (IRQs masked), 256 RX descriptors on the heap. Per-packet logs are behind `net_trace` (shell `nettrace`) — serial bytes are port-I/O exits.
- **Browser chrome is `okai_ui.c` (2026-10-07).** `ui_layout()` is the only place chrome geometry is computed — draw and hit tests both use it; don't reintroduce mirrored constants. It draws with the engine's rasterizer/font on the MAIN stack (fine: no deep recursion) into a cached band re-rendered only on a state-signature change — keep anything that changes per frame (spinner phase, hover) in `state_sig()` or it will not repaint. Title-bar-less windows' caption buttons are `window_ctrl_rect()` (window.c owns min/max/close clicks). The FPS counter lives in the taskbar.
- **Taskbar + title bars are AA too (2026-10-07).** `taskbar.c` renders a cached 1920x`TASKBAR_H` strip (frosted wallpaper, window buttons, FPS chip, RTC clock in UTC — no launcher: everything starts from the terminal) only when its signature changes, and only from the unclipped main-loop `taskbar_update()` (clipped damage repairs just blit the cache — re-rendering inside a repair rect left half-lit hover states); `tb_layout()` is the single geometry source for drawing and `taskbar_hit()`, logged as `[taskbar] btn win=N x,y,w,h` (okvm `vm.taskbar_rect(win)`). Title-bar windows get a GNOME-style dark header bar drawn straight into the backbuffer via `ui_draw.h` (`ui_screen_surf`, implemented in `okai_ui.c`); `title_btn_rect()` is the one source for their close/minimize hit boxes.
- **Wallpaper is a JPEG (2026-10-07).** `tools/wallpaper/kanarchy_wallpaper.html` (WebGL raymarcher) → `tools/wallpaper/render.sh` (headless Edge/Chrome + Pillow) → `wallpaper.jpg`, `incbin`-ed by `src/wallpaper.asm` and decoded at boot by the web engine's JPEG decoder into the heap (`graphics_set_wallpaper`).
- **memcpy/memset/memmove are `rep movs/stos` (2026-10-07).** The byte loops were the hottest code on the desktop (a page scroll memmoves ~7MB). They are inline asm so GCC cannot turn them back into self-calls; memmove's backward path sets DF and always clears it. Fuzzed against byte references on the host (-O0/-O2).
- **ISR/IRQ stubs `cld` before calling C (2026-10-08).** An interrupt can land inside memmove's `std` region; without the `cld` every handler's string ops (the timer's `rand_stir` memcpy, each tick) ran backwards over memory — a scroll-up on a big page corrupted the kernel and hung it with DF=1/IF=0. `irq_handler` logs `[isr] DF set` if DF is ever seen set (`test_scroll_irq.py` fails on it).
- **Cross-origin script requests send `Origin`** (okai `req_fetch`, `q->cors`). Servers echo Access-Control-Allow-Origin only for it; without it BBC's sign-in check failed and its script bounced bbc.com <-> bbc.co.uk forever (679 fetches).

## File ownership

| Area | Files | Notes |
|------|-------|-------|
| Boot | `boot/start.asm`, `boot/isr.asm` | Entry point, multiboot header, ISR stubs, GDT flush |
| Display | `src/graphics.c/.h` | Framebuffer, 8x8 font, draw primitives, dirty-row flush (runs coalesced into one memcpy), clip rect, `graphics_blit_pixels`, wallpaper (`graphics_set_wallpaper`, `src/wallpaper.asm`, `tools/wallpaper/`) |
| Taskbar | `src/taskbar.c/.h` | Cached AA taskbar strip: window buttons, FPS chip, clock; `taskbar_hit()` |
| UI drawing | `src/ui_draw.h` | Shared AA primitives (rrect, pen strokes, Noto text, backbuffer surface) — implemented in `okai_ui.c` |
| Windows | `src/window.c/.h` | Window manager; cell grid or client pixel surface (`window_set_pixels`) |
| Mouse | `src/cursor.c/.h` | PS/2 driver (IRQ12, wheel handshake) + framebuffer-composited AA cursor (sprite built at boot with `ui_draw`, moved in the IRQ) |
| Desktop | `src/desktop.c` | Main loop, shell commands, window creation, cursor compositor |
| Text mode | `src/kernel.c`, `src/vga.c/.h`, `src/terminal.c/.h`, `src/shell.c/.h` | Alternative build — VGA text, scrollback |
| Memory | `src/memory.c/.h` | Physical page allocator (bitmap) + segregated-fit coalescing heap (`kmalloc/kcalloc/krealloc/kfree`, `heap_stats`) |
| Interrupts | `src/gdt.c/.h`, `src/idt.c/.h`, `src/keyboard.c/.h` | GDT + TSS (user segments), IDT + INT 0x80 gate, PIC, keyboard |
| Paging | `src/paging.c/.h` | High-half map (PD 768-799 kernel, FB/MMIO supervisor), `paging_map_user()` for ring 3, `paging_map_user_pd()` (build idle spaces), `paging_user_range_valid()` (syscall ptr check), `V2P`/`P2V_U32` in `src/memlayout.h` |
| Processes | `src/process.c/.h` | Process table (16 slots), private PDs (user-low private / kernel-high shared), pid-0 idle stack + ESP0, `esp0_top` + `ticks_left` per PCB, handoff-point-only switch (CR3+ESP0, never mid-frame preempt) |
| Scheduler | `src/sched.c/.h` | Slice accounting (`sched_tick` from timer IRQ, never switches itself), `sched_yield`, `sched_spawn_user` (private PD + copy via high alias), `sched_prepare`/`sched_unprepare`/`sched_reap` |
| Syscalls | `src/syscall.c/.h` | INT 0x80 ABI 0-17: print(+terminal mirror)/exit/write/read/open/close/fork/exec/sbrk/pipe/dup/wait/kill/mmap/munmap; return values via saved-EAX slot; desktop-only services via hooks (text build links, fails safe) |
| Proc backend | `src/sys_proc.c/.h` | fd table over VFS + 4KB pipe rings, keyboard line queue, fork (page copy), exec (ELF), sbrk/mmap/munmap, wait/kill/signals, lock selftest |
| ELF | `src/elf.c/.h` | ET_EXEC/i386 validation + PT_LOAD single-walk loader (map-once, zero-via-alias, overlap-safe) |
| Locks | `src/spinlock.c/.h` | xchg spinlock + IRQ-safe variants + nesting mutex (selftest via `locktest`) |
| Disk | `src/ata.c/.h` | Polling-PIO LBA28 primary-master (IDENTIFY probe, fail-safe absent); 512B sectors |
| Persist FS | `src/pfs.c/.h` | OKPFS1 (own layout: SB + 16-entry table + contiguous runs, 32KB/file); write-through VFS hooks; mount-or-format + hydrate |
| User Test | `boot/user_test.asm` | Ring 3 test program (loop + int 0x80) |
| Debug | `src/serial.c/.h`, `src/io.h` | COM1 serial output, port I/O |
| Filesystem | `src/filesystem.c/.h` | In-memory virtual filesystem (16 files, 4KB max) |
| Editor | `src/editor.c/.h`, `src/textslot.c/.h` | Text editor opened inside windows; codepoint → bitmap-font slot map |
| Browser | `src/okai.c/.h`, `src/okai_ui.c` | okai shell: tabs, address bar state, fetch driver (`okai_poll`), page surface, pixel hit-testing, forms, HTML error/home pages. `okai_ui.c` = the Firefox-style chrome (layout + AA drawing + hit tests, cached; `FX_*` palette in `theme.h`) |
| Web engine | `src/web/*` | `wdoc` (document controller), `wdom` + `html5` (WHATWG parser), `css_*` (cascade), `lay_*` (block/inline/float/abs/flex/grid/table → display list), `paint`/`raster` (AA painter), `font` + `fontdata.asm` (TrueType, Noto in `fonts/`), `image` (PNG/JPEG/GIF/BMP, inflate/gzip), `svg`, `wurl`, `callstack.asm` |
| JS Engine | `src/qjs/*` (QuickJS 2026-06-04 + `libc/` shim + musl `libm/`, see `README.okernel`), `src/web/wjs.c/.h`, `wjs_dom.c`, `wjs_int.h`, `wjs_prelude.js` (+`.asm` embed), `src/web/wcookie.c/.h` | One QuickJS realm per document (lazy). Page scripts run: parser-blocking/defer/async/dynamic/`document.write`/module graphs (fetched ahead, `JS_EVAL_FLAG_NO_RESOLVE`), timers/rAF/microtasks under time budgets, DOM events before default actions, fetch()/XHR over the single connection. Web API = JS prelude over C natives (`W` object). Cookie jar shared by HTTP and `document.cookie`. tinyjs (`src/js`) is gone. |
| Networking | `src/net/pci.c`, `src/net/e1000.c`, `src/net/network.c`, `src/net/fetch.c/.h` | PCI enum, e1000 NIC (polled, 256-entry RX ring), ARP/IP/ICMP/UDP, TCP socket table, DNS cache; fetch engine (parallel HTTP/HTTPS GETs, keep-alive pool, TLS via `tls_client`) |
| Crypto | `src/crypto/*.c/.h` | SHA-256, ChaCha20, Poly1305, HMAC, HKDF, AEAD, AES-128-GCM (`aes.c`, constant-time bitsliced), X25519, TLS 1.3 record/handshake/keysched/client, X.509 + chain verify (`certverify.c`), Mozilla root store (`roots.c`, generated by `tools/gen_roots.py`), OCSP staples (`ocsp.c`), ChaCha20 CPRNG (`rand.c`) |
| Libc | `src/string.c/.h` | freestanding memcpy/memset/memmove (`rep movs/stos` inline asm) + memcmp/strlen/strncpy (needed by crypto + JS engine) |

## Architecture in 30 seconds

1. GRUB loads kernel at 1MB, provides multiboot info (memory map + framebuffer address)
2. `start.asm` LOW `_start` (e_entry, paging OFF) builds boot PD → `_start_high` sets up high stack, calls `kernel_main()`
3. `kernel_main()` does: GDT (kernel + user segments + TSS) → IDT (exceptions + IRQs + INT 0x80) → memory → paging → process table → graphics → windows → mouse → keyboard → networking → sti → main loop
4. Main loop: handle mouse clicks/drags → `okai_poll()` (fetches, sub-resources, coalesced page renders) → check JS rerender → draw windows (+ okai chrome overlay) → draw cursor → flush dirty rows
5. Keyboard/mouse work via hardware interrupts (IRQ1/IRQ12), not polling
6. Page scripts, stylesheets and images are fetched in parallel (keep-alive connection pool, `fetch_poll()` from the main loop) after the document parses; scripts run in the document's QuickJS realm (`okai_poll` pumps it under a time budget)

## Adding new features

- **New shell command:** add to `shell_execute()` in `desktop.c` (desktop) or `shell.c` (text). Follow the `str_eq(cmd_buf, "name")` pattern.
- **New window type:** use `window_create()`, `window_puts()`, `window_set_close_button()`. Windows auto-manage content buffers.
- **New interrupt handler:** add ISR stub in `isr.asm` (use `ISR_NOERRCODE`/`ISR_ERRCODE` macros), register in `idt_init()` in `idt.c`, add handler function.
- **New drawing primitive:** add to `graphics.c`, write to `backbuffer[]`, mark dirty rows with `graphics_mark_dirty(y)`.
- **New Web API:** prefer plain JS in `src/web/wjs_prelude.js` (rebuilt into `wjs_prelude.o`); add a C native in `wjs_dom.c` (exposed on the `W` object) only when it needs engine internals (DOM arena, layout, CSS). Test on the host first: `build-host/wbrowse <url|file> ...` (`tests/web/js/basic.html` must stay ALLPASS).
- **New process feature:** extend `process.c` (process_create/destroy/switch), update `process.h` with new fields.

## Text vs Desktop mode

The Makefile builds completely separate binaries from different source sets:
- Text: `kernel.c` + `vga.c` + `terminal.c` + `shell.c`
- Desktop: `desktop.c` + `graphics.c` + `window.c` + `paging.c` + `filesystem.c` + `editor.c` + `textslot.c` + `okai.c` + `web/*` + `qjs/*` + `net/*` + `crypto/*` + `string.c` + `theme.h`

Shared: `gdt.c`, `idt.c`, `memory.c`, `serial.c`, `keyboard.c`, `mouse.c`, `io.h`

If you modify shared code, test both builds. If you modify mode-specific code, only that build is affected. Networking, TLS, the web engine, QuickJS, and okai are desktop-only.

## Headless testing (okvm)

`tests/headless/okvm.py` drives QEMU headlessly for automated testing. Output goes to `~/okvm/`.

```python
import sys, time; sys.path.insert(0, 'tests/headless')
from okvm import OkVM
vm = OkVM("tag")
vm.wait_for("[mem] heap", timeout=40); time.sleep(10)
vm.type_string("okai\n")              # internal home page (no network)
vm.wait_for("home rendered", timeout=30)
vm.click_link("example.com")         # closed loop on [okai] click x=.. y=..
ok = vm.wait_for("parse: count=", timeout=70)
vm.kill()
```

Key facts:
- Serial log (`~/okvm/<tag>_serial.log`) is ground truth — never guess from pixels
- Mouse is RELATIVE and applied 1:1 (no smoothing since 2026-10-08) — `vm.burst(dx, dy, n)` moves ~n*(dx,dy)
- `vm.click_link(href_part)` / `vm.click_at(page_x, page_y)` are closed-loop on `[okai] click x=.. y=..` (page px); `vm.click_ui(name, regex)` for chrome elements by name (geometry from the kernel's `[okai] ui name=x,y,w,h` lines — never hard-code chrome pixels; `anchor=` near the close button); `vm.click_screen(x, y, regex)` for any screen point, on `[mse] btn=1`; `vm.taskbar_rect(win)` for a window's taskbar button. Link targets come from `[okai] link[i] x= y= w= h= href=` (each page's first render; `vm.link_regions()`).
- `vm.wait_for(pattern)` matches ANY earlier occurrence — for "the next page loaded" count `parse: count=` lines instead
- Full docs in `tests/headless/TESTING.md`; parallel runner: `python3 tests/headless/run_suite.py -j3 test_*.py`
- Browser end-to-end: `test_okai_interact.py` (offline fixtures), `test_okai_js.py` (page scripts: offline JS fixture + Wikipedia), `test_okai_page.py <url> [tag] [--scroll N]`, `test_links.py`, `test_nav.py`, `test_tab_x.py`, `test_errors.py`, `test_google_search.py`, `test_sites.py [tag] [url ...]` (real-site HTTPS sweep: OK only when the page arrived over TLS — a plain-HTTP fallback counts as a failure), ...
- Page-load profiling: `tests/headless/perf_load.py [tag] [url ...]` (timeline per page: doc / first render / settled; `PERF_ALL=1` stamps every line, `PERF_PRE=nettrace` enables packet tracing, `OKVM_PCAP=1` captures `~/okvm/<tag>.pcap` → `pcap_tcp.py` per-connection timing, `OKVM_ACCEL=kvm`, `OKVM_ISO=path` to compare builds). Windows/WHPX twin: `powershell -File tests\headless\perf_win.ps1 -Accel whpx`.
- Lost/garbled typed keys: `OKVM_TRACE='ps2_*'` (any QEMU trace pattern, comma-separated) writes `~/okvm/<tag>.trace`; `tests/headless/ps2_trace.py` accounts every key byte (queued by QEMU vs read by the kernel). Under TCG (worse with the parallel suite) QEMU sometimes stops running the guest for 0.2-0.6 s — measured both ways: PIT ticks arrive late while the interrupted EIP is ordinary IF=1 code, and the PS/2 trace shows no reads at all for the window. `sendkey` keeps queueing meanwhile; its PS/2 queue holds 16 bytes (8 keys) and silently drops the rest (captured: 18 bytes queued in 370 ms, 16 read, 2 lost). That is the rare one-word garble in `test_sites` ("www.micro.com") — present on older builds too, not a kernel bug. lost>0 = QEMU dropped it; lost=0 with a garbled string = a kernel bug.
- Host TLS: `make host-tests` (crypto, AES-GCM vs OpenSSL, PKI, adversarial 209, `http_dechunk` (extracted from network.c: zero-padded sizes like Google's "00002a8f", saturation) + live example.com MITM check), `make host-tests-asan` after crypto/TLS changes, `build-host/tls_scan` over `tests/tls_hosts.txt` for a real-site handshake census.

Run after touching window.c/mouse/keyboard code: `python3 tests/headless/test_nav.py`, `test_okai_interact.py`, `test_desktop_ui.py`, `test_typing.py` and `test_boot_keys.py`. Hover/repaint races only show with real-mouse timing: `powershell -File tests\headless\hover_win.ps1` (WHPX) and look at the dumps.
- **Damage repairs never re-render cached chrome.** `desktop_paint_rect` runs with a small clip (the cursor erase); if it re-renders a cache (new hover/state), only that rect shows it and the signature then suppresses the full blit — stale half-drawn buttons. Clicks are routed topmost-by-z, never by slot index.



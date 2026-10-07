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
qemu-system-i386 -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0
```

Serial debug variant (for TLS/network bring-up — serial is ground truth for network bugs):
```bash
qemu-system-i386 -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0 -nographic -serial stdio
```

## Gotchas that will bite you

- **BIOS interrupts don't work.** We're in protected mode after GRUB. No `int 0x10`, no `int 0x13`. All hardware access is via port I/O (`inb`/`outb`).
- **Framebuffer is above 4MB** (typically `0xFD000000`). Must set up identity-mapped page tables in `paging.c` before accessing it. Without paging, writing to the framebuffer = instant triple fault.
- **No backbuffer flicker fix is intentional.** We use a backbuffer with dirty-row tracking. `graphics_flush()` copies only changed rows to the framebuffer. If you remove the backbuffer and write directly, you get flicker. If you remove dirty-row tracking, FPS drops from 160+ to ~5.
- **Mouse: IntelliMouse 4-byte wheel mode IS supported** (negotiated in `mouse_init_fb`, `src/window.c`). It sends the 200/100/80 sample-rate magic + `0xF2` (get ID), then must wait for the ACK and the ID byte **in order** — draining the output buffer then reading a not-yet-arrived byte desyncs the PS/2 stream and kills movement shortly into the session (HANDOFF bug #25). On `id == 0x03/0x04` it sets `mouse_has_wheel` and the 4th byte is read as the wheel delta. Don't break that handshake ordering.
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
- **TLS trust (2026-10-07).** Chains anchor two ways: a cert ISSUED BY a store root (name-bound: issuer DN == root subject, AKI == root SKI when both exist, signature verifies under the root key — `root_issued()` in `certverify.c`; servers don't send roots), or an in-flight cert whose SPKI is a root key. Certs past the anchor are ignored (expired cross-signs must not fail the path). Root store = Mozilla's, regenerated with `python3 tools/gen_roots.py` (embeds whole certs: subject + SKI + SPKI). OCSP staples: only a verified REVOKED status or a Must-Staple leaf hard-fails; stale/unverifiable staples soft-fail like an absent one. Suites: ChaCha20-Poly1305 (preferred) + AES-128-GCM (`aes.c`, constant-time; Akamai sites need it); no SHA-384 suites, no TLS 1.2.
- **Makefile tracks headers via `-MMD -MP` (2026-10-07).** Never drop it: before it, `src/crypto/*.h` weren't dependencies, a grown `struct tls_state` left `tls_net.o` at the old size, and `tls_state_init`'s memset zeroed the neighbouring fetch-timeout clock — every HTTPS fetch "timed out" instantly and silently downgraded to HTTP. When in doubt, `make clean`.
- **okai fetches are serialized** on the single connection (`okai_fetch_owner` + `fetch_tab`, driven by `okai_poll()`); each HTTPS fetch opens a fresh TCP connection and ends at HTTP message completion. Never start a second request while one is in flight.

## File ownership

| Area | Files | Notes |
|------|-------|-------|
| Boot | `boot/start.asm`, `boot/isr.asm` | Entry point, multiboot header, ISR stubs, GDT flush |
| Display | `src/graphics.c/.h` | Framebuffer, 8x8 font, draw primitives, dirty-row flush, clip rect, `graphics_blit_pixels` |
| Windows | `src/window.c/.h` | Window manager, PS/2 mouse driver, stateless cursor sprite; cell grid or client pixel surface (`window_set_pixels`) |
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
| Browser | `src/okai.c/.h` | okai shell: tabs, chrome overlay, address bar, fetch driver (`okai_poll`), page surface, pixel hit-testing, forms, HTML error/home pages |
| Web engine | `src/web/*` | `wdoc` (document controller), `wdom` + `html5` (WHATWG parser), `css_*` (cascade), `lay_*` (block/inline/float/abs/flex/grid/table → display list), `paint`/`raster` (AA painter), `font` + `fontdata.asm` (TrueType, Noto in `fonts/`), `image` (PNG/JPEG/GIF/BMP, inflate/gzip), `svg`, `wurl`, `callstack.asm` |
| JS Engine | `src/qjs/*` (QuickJS 2026-06-04 + `libc/` shim + musl `libm/`, see `README.okernel`), `src/web/wjs.c/.h`, `wjs_dom.c`, `wjs_int.h`, `wjs_prelude.js` (+`.asm` embed), `src/web/wcookie.c/.h` | One QuickJS realm per document (lazy). Page scripts run: parser-blocking/defer/async/dynamic/`document.write`/module graphs (fetched ahead, `JS_EVAL_FLAG_NO_RESOLVE`), timers/rAF/microtasks under time budgets, DOM events before default actions, fetch()/XHR over the single connection. Web API = JS prelude over C natives (`W` object). Cookie jar shared by HTTP and `document.cookie`. tinyjs (`src/js`) is gone. |
| Networking | `src/net/pci.c`, `src/net/e1000.c`, `src/net/network.c`, `src/net/tls_net.c` | PCI enum, e1000 NIC (TX+RX), ARP/IP/ICMP/UDP/TCP/DNS/HTTP, TLS 1.3 client wrapper |
| Crypto | `src/crypto/*.c/.h` | SHA-256, ChaCha20, Poly1305, HMAC, HKDF, AEAD, AES-128-GCM (`aes.c`, constant-time bitsliced), X25519, TLS 1.3 record/handshake/keysched/client, X.509 + chain verify (`certverify.c`), Mozilla root store (`roots.c`, generated by `tools/gen_roots.py`), OCSP staples (`ocsp.c`), ChaCha20 CPRNG (`rand.c`) |
| Libc | `src/string.c/.h` | freestanding memcpy/memset/memcmp/memmove/strlen/strncpy (needed by crypto + JS engine) |

## Architecture in 30 seconds

1. GRUB loads kernel at 1MB, provides multiboot info (memory map + framebuffer address)
2. `start.asm` LOW `_start` (e_entry, paging OFF) builds boot PD → `_start_high` sets up high stack, calls `kernel_main()`
3. `kernel_main()` does: GDT (kernel + user segments + TSS) → IDT (exceptions + IRQs + INT 0x80) → memory → paging → process table → graphics → windows → mouse → keyboard → networking → sti → main loop
4. Main loop: handle mouse clicks/drags → `okai_poll()` (fetches, sub-resources, coalesced page renders) → check JS rerender → draw windows (+ okai chrome overlay) → draw cursor → flush dirty rows
5. Keyboard/mouse work via hardware interrupts (IRQ1/IRQ12), not polling
6. Page scripts, stylesheets and images are fetched sequentially through the single connection after the document parses; scripts run in the document's QuickJS realm (`okai_poll` pumps it under a time budget)

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
- Mouse is RELATIVE + 4-sample smoothing (~4× dilation) — use `vm.burst()` for movement
- `vm.click_link(href_part)` / `vm.click_at(page_x, page_y)` are closed-loop on `[okai] click x=.. y=..` (page px); `vm.click_screen(x, y, regex)` for chrome, on `[mse] btn=1`. Link targets come from `[okai] link[i] x= y= w= h= href=` (each page's first render; `vm.link_regions()`).
- `vm.wait_for(pattern)` matches ANY earlier occurrence — for "the next page loaded" count `parse: count=` lines instead
- Full docs in `tests/headless/TESTING.md`; parallel runner: `python3 tests/headless/run_suite.py -j3 test_*.py`
- Browser end-to-end: `test_okai_interact.py` (offline fixtures), `test_okai_js.py` (page scripts: offline JS fixture + Wikipedia), `test_okai_page.py <url> [tag] [--scroll N]`, `test_links.py`, `test_nav.py`, `test_tab_x.py`, `test_errors.py`, `test_google_search.py`, `test_sites.py [tag] [url ...]` (real-site HTTPS sweep: OK only when the page arrived over TLS — a plain-HTTP fallback counts as a failure), ...
- Host TLS: `make host-tests` (crypto, AES-GCM vs OpenSSL, PKI, adversarial 189 + live example.com MITM check), `make host-tests-asan` after crypto/TLS changes, `build-host/tls_scan` over `tests/tls_hosts.txt` for a real-site handshake census.

Run after touching window.c/mouse code: `python3 tests/headless/test_nav.py` and `test_okai_interact.py`



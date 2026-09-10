# AGENTS.md — okernel

## What this is

From-scratch OS in C + x86 assembly. Two build targets: text mode (VGA text) and desktop mode (1920x1080 GRUB framebuffer). Runs in QEMU. No standard library, no Linux, no BIOS calls in protected mode.

## Build

```bash
make desktop       # Desktop ISO (okernel-desktop.iso) — primary target
make text          # Text mode ISO (okernel-text.iso)
make clean         # Nukes all .o, .bin, .iso, isodir/
```

Requires: `gcc` (multilib), `nasm`, `ld`, `grub-mkrescue`, `xorriso`, `mtools`.

Desktop QEMU command (must use `-vga std` for framebuffer; `-device e1000` for networking — okai/HTTPS need it):
```bash
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0
```

Serial debug variant (for TLS/network bring-up — serial is ground truth for network bugs):
```bash
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0 -nographic -serial stdio
```

## Gotchas that will bite you

- **BIOS interrupts don't work.** We're in protected mode after GRUB. No `int 0x10`, no `int 0x13`. All hardware access is via port I/O (`inb`/`outb`).
- **Framebuffer is above 4MB** (typically `0xFD000000`). Must set up identity-mapped page tables in `paging.c` before accessing it. Without paging, writing to the framebuffer = instant triple fault.
- **No backbuffer flicker fix is intentional.** We use a backbuffer with dirty-row tracking. `graphics_flush()` copies only changed rows to the framebuffer. If you remove the backbuffer and write directly, you get flicker. If you remove dirty-row tracking, FPS drops from 160+ to ~5.
- **Mouse: IntelliMouse 4-byte wheel mode IS supported** (negotiated in `mouse_init_fb`, `src/window.c`). It sends the 200/100/80 sample-rate magic + `0xF2` (get ID), then must wait for the ACK and the ID byte **in order** — draining the output buffer then reading a not-yet-arrived byte desyncs the PS/2 stream and kills movement shortly into the session (HANDOFF bug #25). On `id == 0x03/0x04` it sets `mouse_has_wheel` and the 4th byte is read as the wheel delta. Don't break that handshake ordering.
- **High-half kernel (2026-09-07).** Desktop links at 0xC0100000 (`linker-high.ld`; text keeps `linker.ld`). LOW trampoline `_start` (e_entry=0x100030, ESP 0x7FF00) + private boot GDT (GRUB's 0x08 is invalid) → `_start_high` → `kernel_main`. Order: memory → mboot copy (fb@88/pitch@96) → `paging_init` (uncond: boot PD covers only 0-4M) → process → graphics (identity FB, never `paging_map` window). `V2P`/`P2V_U32` in `src/memlayout.h`. Traps: LOW e_entry, boot GDT, paging-before-process, identity FB, explicit 2nd user page + full 4K copy. See HANDOFF Priority 2.
- **Scheduler is preemptive via `process_switch_to` (2026-09-08+).** `sched_tick()` (timer IRQ) switches kernel threads through the `context_switch` stub (isr.asm) with guards: never from ring-3 trap stacks (defers), never into un-entered/BLOCKED/ZOMBIE slots, `switch_busy` closes the drain window, per-slot `generation` aborts reuse-under-cli. `sched_yield()` on the INT 0x80 trap stack still NEVER switches CR3/ESP0 (slice reset only — the stub will popa+iret to ring 3). `process_switch` (lightweight CR3+ESP0) is handoff-point-only (entry/exit drains).
- **PCB offsets: offsetof only, fields go last (2026-09-10).** `idt.c`'s exec-redirect reads `user_eip/esp` via `offsetof(struct process,...)` — a past hardcoded `pcb[7]/pcb[8]` silently crossed EIP/ESP the moment a field was inserted mid-struct (exec resumed at EIP=user_esp, ESP=entered_ring3). New `struct process` fields go at the TAIL (see `generation`); never index PCB words by hand.
- **Syscall ABI (2026-09-08).** INT 0x80: 0=print, 1=exit, 2=write(fd 1→focused terminal), 3=getpid, 4=yield, 5=mmap_user. All user pointers go through `paging_user_range_valid()` (present+U/S at both levels) BEFORE dereference — a bad ring-3 pointer prints an error, never faults the kernel. `enter_user_mode` uses register convention (eip→EAX, esp→EDX, no stack args); sys_exit resumes via `user_exit_trampoline` (full caller-frame restore).
- **Yield is trap-safe (2026-09-08).** `sched_yield()` runs on the INT 0x80 trap stack (stub will popa+iret back to ring 3) — it must NEVER switch CR3/ESP0 there (the resume faulted as #PF at the next ring-3 EIP). It only resets the slice; handoffs happen at entry/exit safe points. Same class of bug as the timer-switch ban.
- **TCP discipline (2026-09-08).** Fast retransmit counts ONLY pure-ACK repeats (plen==0 threaded through `tcp_process_ack`) — data-carrying segments with a repeated ack field are NOT dup ACKs (counting them fired bogus fast-rtx mid-download and stalled the next connection). Reorder buffer (8×1500B) is per-connection (reset in `tcp_connect`); advertised window is 32KB (the old 60B stub throttled servers).
- **PFS traps (2026-09-08).** `fs_delete` copies the name before clearing (hook needs it); `pfs_sync_file` frees the old run before first-fit alloc; hydrate/sync buffers are static (32KB blows the 4KB trap stack). Editor Ctrl+S/Ctrl+X need the keyboard Ctrl tracker (0x1D) — the status bar advertised them unwired.
- **OkVM disk slot (2026-09-08).** `OkVM(tag, disk=path)` appends `-hda` for PFS tests; default runs stay diskless. A stale `-hda` QEMU holds the image write-lock (`qemu-img` fails) and its monitor socket refuses connects — kill it first.
- **`section .note.GNU-stack`** in .asm files MUST be the last section. If placed at the top, all subsequent code gets swallowed into the non-executable section and the kernel crashes on boot.
- **Compiler flags matter:** `-fno-pic -fno-pie -mno-red-zone` are required. Without them, GCC generates position-independent code with `__x86.get_pc_thunk` calls that crash in a freestanding kernel.
- **BSS is large** (~8MB backbuffer [1920×1080×32bpp] + per-window content buffers + page tables + crypto/CSS tables; `kernel_end` ~6.3MB). The kernel's BSS must fit within identity-mapped memory. If you add large static arrays, verify `_kernel_end` in `linker.ld` stays below available RAM.

## File ownership

| Area | Files | Notes |
|------|-------|-------|
| Boot | `boot/start.asm`, `boot/isr.asm` | Entry point, multiboot header, ISR stubs, GDT flush |
| Display | `src/graphics.c/.h` | Framebuffer, 8x8 font, draw primitives, dirty-row flush, clip rect |
| Windows | `src/window.c/.h` | Window manager, PS/2 mouse driver, stateless cursor sprite |
| Desktop | `src/desktop.c` | Main loop, shell commands, window creation, cursor compositor |
| Text mode | `src/kernel.c`, `src/vga.c/.h`, `src/terminal.c/.h`, `src/shell.c/.h` | Alternative build — VGA text, scrollback |
| Memory | `src/memory.c/.h` | Physical page allocator (bitmap) + bump heap |
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
| Editor | `src/editor.c/.h` | Text editor opened inside windows |
| Browser | `src/okai.c/.h`, `src/html.c/.h`, `src/css.c/.h` | okai web browser (HTTPS), HTML parser, from-scratch CSS engine |
| JS Engine | `src/js/js_os.h/.c`, `src/js/js.h`, `src/js/js_var.c`, `src/js/js_lex.c`, `src/js/js_parse.c`, `src/js/js_funcs.c`, `src/js/js_math.c`, `src/js/js_dom.c/.h` | tinyjs ok edition — C port of tiny-js (MIT, substantially rewritten). DOM bridge COMPLETE (getElementById, querySelector, setText, setStyle). |
| Networking | `src/net/pci.c`, `src/net/e1000.c`, `src/net/network.c`, `src/net/tls_net.c` | PCI enum, e1000 NIC (TX+RX), ARP/IP/ICMP/UDP/TCP/DNS/HTTP, TLS 1.3 client wrapper |
| Crypto | `src/crypto/*.c/.h` | SHA-256, ChaCha20, Poly1305, HMAC, HKDF, AEAD, X25519, TLS 1.3 record/handshake/keysched/client, ChaCha20 CPRNG (`rand.c`) |
| Libc | `src/string.c/.h` | freestanding memcpy/memset/memcmp/memmove/strlen/strncpy (needed by crypto + JS engine) |

## Architecture in 30 seconds

1. GRUB loads kernel at 1MB, provides multiboot info (memory map + framebuffer address)
2. `start.asm` LOW `_start` (e_entry, paging OFF) builds boot PD → `_start_high` sets up high stack, calls `kernel_main()`
3. `kernel_main()` does: GDT (kernel + user segments + TSS) → IDT (exceptions + IRQs + INT 0x80) → memory → paging → process table → graphics → windows → mouse → keyboard → networking → sti → main loop
4. Main loop: handle mouse clicks/drags → drive okai fetches (sub-resources) → check JS rerender → draw wallpaper → draw windows → draw cursor → flush dirty rows
5. Keyboard/mouse work via hardware interrupts (IRQ1/IRQ12), not polling
6. External CSS/JS fetched sequentially through single TCP connection after main page loads

## Adding new features

- **New shell command:** add to `shell_execute()` in `desktop.c` (desktop) or `shell.c` (text). Follow the `str_eq(cmd_buf, "name")` pattern.
- **New window type:** use `window_create()`, `window_puts()`, `window_set_close_button()`. Windows auto-manage content buffers.
- **New interrupt handler:** add ISR stub in `isr.asm` (use `ISR_NOERRCODE`/`ISR_ERRCODE` macros), register in `idt_init()` in `idt.c`, add handler function.
- **New drawing primitive:** add to `graphics.c`, write to `backbuffer[]`, mark dirty rows with `graphics_mark_dirty(y)`.
- **New JS DOM method:** add callback in `js_dom.c`, register via `js_add_native()` in `js_init()`, or create native function var directly on element in `js_dom_get_element()`.
- **New process feature:** extend `process.c` (process_create/destroy/switch), update `process.h` with new fields.

## Text vs Desktop mode

The Makefile builds completely separate binaries from different source sets:
- Text: `kernel.c` + `vga.c` + `terminal.c` + `shell.c`
- Desktop: `desktop.c` + `graphics.c` + `window.c` + `paging.c` + `filesystem.c` + `editor.c` + `okai.c` + `html.c` + `css.c` + `net/*` + `crypto/*` + `string.c` + `theme.h`

Shared: `gdt.c`, `idt.c`, `memory.c`, `serial.c`, `keyboard.c`, `mouse.c`, `io.h`

If you modify shared code, test both builds. If you modify mode-specific code, only that build is affected. Networking, TLS, the CSS engine, the JS engine, and okai are desktop-only.

## Headless testing (okvm)

`tests/headless/okvm.py` drives QEMU headlessly for automated testing. Output goes to `~/okvm/`.

```python
import sys, time; sys.path.insert(0, 'tests/headless')
from okvm import OkVM
vm = OkVM("tag")
time.sleep(14)
vm.type_string("okai https://example.com/\n")
ok = vm.wait_for("https parse: count=", timeout=70)
vm.kill()
```

Key facts:
- Serial log (`~/okvm/<tag>_serial.log`) is ground truth — never guess from pixels
- Mouse is RELATIVE + 4-sample smoothing (~4× dilation) — use `vm.burst()` for movement
- `vm.click_link()` is closed-loop: click → read `[okai] click row=.. col=..` → correct → repeat
- `vm.wait_for(pattern)` polls serial — never raw `sleep()`
- Full docs in `tests/headless/TESTING.md`
- Test scripts: `test_link_click.py`, `test_errors.py`, `test_google.py`, `test_nav.py`, `test_tab_x.py`, etc.

Run after touching window.c/mouse code: `python3 tests/headless/test_nav.py`



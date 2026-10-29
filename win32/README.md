# okai for Windows

The KAnarchy browser shell (`src/okai.c` + `src/okai_ui.c`) and the whole
underlying stack — web engine (`src/web`), ojs (`src/ojs`), TLS 1.3 + HTTP/2 +
the fetch engine (`src/net`, `src/crypto`), the media engine (`src/media`) —
built as a 32-bit Windows application. One `okai.exe`, no DLLs, no Qt, no
Chromium: the only Windows-specific code is this directory.

```sh
make win                      # from WSL: builds build-host/okai.exe (stripped)
./build-host/okai.exe         # or double-click it; okai.exe URL also works
```

`make win` produces two files from the same link: **`okai.exe`** (release,
stripped, ~11 MB) and **`okai-debug.exe`** (same bytes plus DWARF and the COFF
symbol table, ~19 MB) — keep the pair together so a crash log's addresses can
be resolved against the debug one. `STRIP=0 make win` skips the strip while
iterating.

The log (the port's "serial console") is written to `okai.log` in the working
directory; `OKAI_LOG=0` silences it, `OKAI_LOG=path` moves it. `OKAI_TRACE=1`
adds the first frame's main-loop stages and the left-button/hit-test lines —
useful when a real click or drag does nothing.

## Toolchain

32-bit is not a preference: `crypto/rand.c` uses `rdtsc` with `"=A"`,
`wcommon.h` inlines `divl`, and the host build of the engine is `-m32`. GNU C is
required (computed goto in ojs, `__attribute__((packed))`, inline asm).

```sh
sudo apt install gcc-mingw-w64-i686   # or: sh win32/setup-toolchain.sh (no root)
make win
```

`setup-toolchain.sh` unpacks the same packages under `~/mingw/root`; `build.sh`
uses that fallback when `i686-w64-mingw32-gcc` is not on `PATH`.

## What this layer is

The engine is the kernel's code, unchanged in behaviour; these files supply
what the kernel provides:

| File | Provides |
|---|---|
| `win_gfx.c` | `graphics_*`: a 32-bit DIB backbuffer, kernel-style damage tracking (row band + clip rect), `BitBlt` of the dirty band, text/rect primitives |
| `win_window.c` | `window_*`: one window, the page pixel surface, partial-surface repaint, caption-button geometry, title → `SetWindowTextW` |
| `win_sys.c` | `kmalloc/kfree` over the CRT, `serial_*` → `okai.log`, the 100 Hz `tick_count` thread, `prof_*`, `cpufreq_*`, pointer state, entropy, `smp_*` jobs over a thread pool, `mmap` over `VirtualAlloc` |
| `win_net.c` | `tcp_*` + `dns_lookup` over Winsock (non-blocking connect, per-socket send ring, FIN detection, DNS on its own thread) — our TLS/H2/fetch run on top unchanged |
| `win_audio.c` | `audio_*` over waveOut (48 kHz stereo, 8 × 250 ms buffers, position-based `audio_clock`) |
| `win_heap.c` | the process allocator: `malloc/free/realloc/calloc/strdup` + `kmalloc/kfree`; `OKAI_HCHK=1` = guarded mode |
| `win_main.c` | WinMain, the message pump, and `src/desktop.c`'s browser loop + input routing |
| `test-gui.ps1` | input/chrome test (posted messages, assertions against the log) |
| `test-frameless.ps1` | the frameless-window contract (`WM_NCHITTEST` move/resize regions, maximise clamp) |

`-DOKAI_PORT` marks this build for the few places where the engine asks "is
there a real platform here?": the CPRNG (`tls_client.c`), the sound-card env and
frame budget (`wjs_media.c`), the video worker/tile helpers (`meng.c`) and
parallel GC (`wjs_sys.c`). Kernel builds are unaffected (the macro is never
defined there).

Screen model: the backbuffer is exactly the client area (`gfx_w × gfx_h`); the
port has no taskbar, so `okai_open()`'s `SCREEN_H - TASKBAR_H` window is
stretched back to the full client (`win_window_fit_full`).

The window is **frameless**: `WS_POPUP | WS_THICKFRAME` with `WM_NCCALCSIZE`
returning 0, so okai's own 2px frame and caption cells are the only chrome and
the page runs to the edge. The two things a caption used to provide are done by
hand, and `test-frameless.ps1` checks them:

- `WM_NCHITTEST`: the tab-strip background is `HTCAPTION` (drag), okai's tabs,
  `+`, address bar and min/max/close cells stay `HTCLIENT` (so clicks reach the
  browser), and the outer 6 px are the resize edges/corners.
- `WM_GETMINMAXINFO`: maximise is clamped to the monitor work area (a frameless
  window would otherwise cover the taskbar), minimum 480×360.

Physical drag/resize needs a human: synthetic mouse input does not reach the
window in a non-interactive session, so the test asserts the hit-test answers
the shell reads to start the move/size loop, plus the alive/crash checks.

## Tests

```powershell
powershell -ExecutionPolicy Bypass -File win32\test-gui.ps1        # input + chrome
powershell -ExecutionPolicy Bypass -File win32\test-frameless.ps1  # window contract
```

Real `WM_CHAR` / `WM_LBUTTONDOWN` / `WM_MOUSEWHEEL` messages are posted to the
window and every expectation is checked against `okai.log`: start page render,
link click → navigation, Back button, `+` new tab, wheel sign, address-bar
focus + typing + backspace + Enter, process responsiveness, no crash record.
Geometry comes from okai's own hit-test log lines, never from fixed pixels.
`-KeepOpen` leaves the browser running.

Manual checks used while bringing the port up: `https://example.com/` (TLS 1.3,
certificate chain, HTTP/2, gzip, page script), `https://news.ycombinator.com/`,
a 720p/1080p VP9+Opus WebM held at 60 shown fps with tile-parallel loop filter,
and H.264+AAC MP4 playback.

## Debugging a crash

A crash writes a `[okai-win] CRASH code=… at=… eip=… esp=… ebp=…` block plus a
stack scan into `okai.log` (`win_crash_log` uses a raw `WriteFile` handle, so it
works even when the CRT heap is already broken, and a vectored exception handler
catches faults that never reach the unhandled filter). To turn it into a
`file:line`:

```sh
# runtime address -> RVA (image base 0x400000) -> symbol
i686-w64-mingw32-addr2line -e build-host/okai-debug.exe -f -C -i 0x57f8ae
```

`okai-debug.exe` is the same link as the `okai.exe` that crashed. If `okai.log`
has *nothing* after the last line, the fault was inside a system callback or a
fast-fail (`0xC0000409`), which no handler can catch — check the `Application`
event log / Reliability Monitor for the exception code and the faulting module.

`OKAI_HCHK=1` runs the guarded allocator: a private heap with a pointer-sized
header per block (size + allocation site), 16 guard bytes after the payload, a
live-block list and a 2 Hz sweep from the main loop. An overrun is reported at
the allocation that was overflowed:

```
[heap] BUFFER OVERRUN (sweep): block size=512 site=0x0057f8ae ...
```

Use it like `set OKAI_HCHK=1` before launching; it slows everything down but it
is the only way to find a write that ntdll would otherwise report as a heap
corruption minutes later. Two rules it enforces (both were real bugs): never
`free()` memory that did not come from the allocator, and never write past the
end of a block — including the waveOut buffers, whose headers are prepared once
and may not be touched while the driver still owns them.

## Known limits (port-specific)

- The H.264/MP4 path follows the kernel's demuxer: files whose `elst` uses an
  **empty edit** (a leading gap, `media_time = -1`) do not start — same as in
  QEMU. WebM and clean MP4s are fine.
- The ojs JIT is off (it is off for hosted builds; `jit.c` would need
  `VirtualAlloc`-backed pools to be re-enabled).
- Video decodes on worker threads through the port's `smp_*` pool; there is no
  x87/SSE state handling beyond what the CRT does, and no `-mno-sse` discipline
  like the kernel's (a hosted build has a full FPU to itself).
- No downloads, no file pickers, no clipboard integration beyond what okai
  already exposes, and the address bar accepts ASCII (okai's own limitation).
- No taskbar or window decorations of Okapi's (no `winfx` shadows/rounding):
  the window is a single frameless browser window.

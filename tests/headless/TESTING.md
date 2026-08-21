# okernel Headless Testing Guide

The complete playbook for testing this OS (GUI, mouse, networking, okai browser) with zero human interaction, written for an agent following in these footsteps. Everything here was actually used to find and fix real bugs (HANDOFF.md bugs #21–#24).

**Companion files (in this directory):**
- `okvm.py` — the driver library (boot, type, click, screenshot, serial)
- `test_link_click.py` — link-click regression (bug #21/#22)
- `test_google.py` — google.com render regression (bug #23)
- `test_errors.py` — error-page + fetch-owner recovery regression (bug #22)

---

## 0. The Prime Directive: the serial log is ground truth

Never guess system state from screenshots alone. The kernel prints every network event to COM1. A bug that looks like "black screen" in pixels is, in the serial log, an exact story:

```
[okai] LINK HIT li=0 -> https://iana.org/domains/example
[okai] opened: https://iana.org/domains/example
[dns] querying 'iana.org'...
[dns] response, answers=1
[dns] resolved: 192.0.43.8
[tls-net] fetch timed out          ← HERE is the bug, 60 seconds before you'd see it
```

**Method: reproduce the bug, read the serial log, and find the FIRST line where the working case and the broken case diverge.** That divergence point is usually within 10 lines of the root cause. (The stale-tail DNS bug was found exactly this way: fetch 1's log had "DNS resolved, opening TCP:443", fetch 2's log just... stopped after "resolved".)

If the serial log doesn't tell you what you need — **add instrumentation**. The kernel already has `serial_printf()`; it's a 2-line change and a rebuild (HANDOFF.md says builds take seconds). The `[okai] click row=.. col=..` lines in desktop.c exist precisely because pixel-guessing wasn't working. Instrument, rebuild, re-run.

## 1. The QEMU harness

Boot the desktop ISO headless with serial-to-file and a monitor socket:

```
qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std \
  -device e1000,netdev=net0 -netdev user,id=net0 \
  -serial file:/tmp/ok/<tag>_serial.log \
  -display none \
  -monitor unix:/tmp/ok/<tag>_mon.sock,server,nowait \
  -no-reboot
```

- `-display none` = headless. No window ever opens.
- `-serial file:...` = everything the kernel prints lands in a file you can `grep`. **This is the single most important flag.**
- `-monitor unix:...,server,nowait` = a control socket. You'll send keystrokes/mouse/screendumps through it.
- `-netdev user` = SLIRP: guest 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3, real internet forwarding.
- `<tag>` every run: parallel runs must never share a log or socket.

Boot takes ~8s; sleep 14 to be safe. That's all in `OkVM.__init__` — just use it.

## 2. Keyboard input (sendkey)

Type through the monitor socket, one `sendkey` per character:

```
sendkey o
sendai k     ← no. `sendkey k`
sendkey spc
sendkey ret
```

The keymap that works (in `okvm.py type_string`):
- `space`→`spc`, `/`→`slash`, `.`→`dot`, `:`→`shift-semicolon`, `-`→`minus`, `?`→`shift-slash`, `\n`→`ret`
- Uppercase → `shift-<lowercase>`, digits pass through as-is.
- ~50ms between keys, ~80ms is safer if the system is busy fetching.

So "type a command into the shell" is:
```python
vm.type_string("okai https://example.com/\n")
```

**Gotcha:** the okai window STEALS FOCUS when opened. Every keystroke after `okai <url>` goes to the browser (g = address bar, j/k = scroll), NOT the terminal. If your script types two commands back to back, the second becomes browser input and can navigate to garbage. Type one command, wait for its effect, then decide.

## 3. Waiting for things (serial polling)

Never `sleep(30)` and hope. Poll the serial log for the expected lifecycle line:

```python
def wait_for(log_path, pattern, timeout=70):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pattern in open(log_path, errors='replace').read():
            return True
        time.sleep(1)
    return False
```

Key patterns to wait for:
| Event | Pattern |
|---|---|
| Page fetched + parsed (okai) | `[br] parse: count=` / `[br] https parse: count=` |
| Redirect followed | `[okai] redirect` |
| DNS done | `DNS resolved` |
| TLS response buffered | `[tls-net] received` |
| Fetch failed | `giving up` / `timed out` / `FAILED` |
| Kernel panic | `triple fault` (always treat as hard fail) |

A normal page fetch over TLS takes 2–10s. google.com with 86KB chunked body takes ~60–120s (many tiny SLIRP segments). Give timeouts generous margins but ALWAYS bound them.

## 4. Screenshots (screendump) and pixel analysis

`monitor: screendump /tmp/ok/tag.ppm` → raw PPM (1024×768). Load with PIL:

```python
from PIL import Image
img = Image.open('/tmp/ok/tag.ppm').convert('RGB')
```

Pixel counting answers visual questions WITHOUT a vision model:
- **Is the page blank/black?** Count colors in the content area. The okai window is created at (30,20,520,400) → content area x∈[32,548), y∈[46,416). If >90% of pixels are `000000`, the page didn't render.
- **Did the white background apply?** Top color in content area should be `ffffff` after the CSS fix.
- **Is the blue link row present?** VGA color 9 = `(85,85,255)`; a row with ≥5 such pixels is a link line.
- **Did the error page render?** Scan for light-red text pixels (VGA 12 = `(255,85,85)`, tolerance) in the top of the content area.

The VGA palette is in `src/window.c` `vga_to_rgb[]` — map palette indices to expected RGB before writing pixel checks.

## 5. Vision analysis (when pixels aren't enough)

For "is this garbled / readable / correctly laid out", dispatch a vision-capable subagent:

```
actor spawn, model opencode-go/mimo-v2.5, prompt:
  "The image is at /tmp/ok/x.ppm (convert to PNG first if you can't read PPM:
   python3 -c \"from PIL import Image; Image.open('...').save('...png')\").
   Report: window layout, content of the browser window, cursor position,
   anything that looks like a rendering bug."
```

Rules that made this work:
- PPM often needs the PIL→PNG conversion first.
- Give the model context: window geometry, what was supposed to happen, the known cursor sprite (12×16 white arrow).
- Ask for facts ("what text is visible"), not conclusions ("is it good?").
- Vision is the LAST resort, for final visual confirmation. Pixel counts are faster and more precise for numeric questions; serial is authoritative for everything else.

## 6. The mouse problem — and the closed-loop solution

This is the hardest part of GUI testing here. The facts:

1. QEMU HMP `mouse_move dx dy` is RELATIVE, not absolute.
2. okernel's PS/2 driver smooths with a 4-sample moving average, so a single move is diluted by stale samples in the ring (net ≈ 0.25–0.6× the requested delta).
3. The cursor is a white arrow — invisible against white page backgrounds, so "find the white arrow" pixel detection is unreliable.

**Solution: forget where the cursor IS. Ask the kernel where the click LANDED.**

The kernel (desktop.c, this is already merged) prints for EVERY click inside an okai content area:

```
[okai] click row=13 col=2 (mx=49 my=266) links=1
```

and for every rendered link (okai.c):

```
[okai] link[0] row=13 col0=0 col1=11 href=https://iana.org/domains/example
[okai] LINK HIT li=0 -> https://iana.org/domains/example
```

So clicking a link becomes a feedback-control loop (this is `okvm.py click_link()`):

```
loop up to 12 times:
    click (mouse_button 1, then mouse_button 0)
    parse the LAST `[okai] click ...` line from serial  ← exact ground truth
    if row/col inside the link region → done, check for LINK HIT line
    else: burst-correct toward the target and repeat
```

Movement correction uses **bursts**: send the same delta 5× so the smoothing ring fills with it (net displacement ≈ 3.75 × delta). Empirically: `burst(-30,0)×5` moved the cursor −90px; single `mouse_move -150` also moved ≈ −88px. Correct in ~3px/step units, iterate. Convergence took 4–5 attempts reliably.

If you need to click something that is NOT an okai link (taskbar, close button), use the same loop but read the cursor position from screendump diffs: dump → `mouse_move 12 0` ×5 → dump → changed-pixel clusters ≈ 12×16 in the second dump = the new cursor position (white-core filter kills the blinking-caret false positive).

## 7. Host-side unit tests (fast, no QEMU)

The kernel's parser code is plain C — compile and test it ON THE HOST against real captured data:

```bash
gcc -O1 -o /tmp/htest tests/test_html_google.c src/html.c src/serial.c -Isrc -include string.h
./tmp/htest downloaded_page.html
```

This found the google.com bugs in minutes: "parsed 21 tokens from an 85KB page" and "LINK '&#1055;&#1086;...'" pointed straight at truncation and missing entity decode. Steps:
1. `curl -H 'User-Agent: okernel/0.4' -A ... -o page.html http://site/` — capture EXACTLY what the server sends our UA (google serves a Russian page to this IP; UA matters).
2. Run the kernel's parser on it, dump tokens.
3. Compare token count/content to expectations; a healthy page yields headings+text+links, a broken parse yields header links only.

Existing host tests (run these after ANY change): `tests/test_css.c` (link with `../src/css.c ../src/html.c ../src/serial.c`), `test_sha256`, `test_tls_crypto`. A test that fails to LINK is a broken test command, not a code failure — check `undefined reference` before concluding anything.

## 8. Root-cause discipline (how the bugs were actually found)

- **Reproduce first, theorize second.** The "black window" report became a reproducible serial-log stall in HP_DNS before any fix was attempted.
- **Diff working vs broken.** Fetch 1 (example.com) printed `DNS resolved, opening TCP:443`; fetch 2 stopped after `[dns] resolved:`. The missing line told us which code path died, and `dns_host_matches` reading `iana.orgcom` explained it.
- **One variable at a time.** Instrument → rebuild → re-run → compare logs. Never change two things between runs.
- **Beware tools that lie.** The serial formatter printed `total 6729 bytes` for an 86692-byte response (4-digit counter wrap). When numbers look impossible, verify the INSTRUMENT before believing the data (or the bug).
- **Check assumptions about the environment**: is the ISO newer than the sources you just edited? `ls --time-style` both. An ISO rebuild you forgot, or (as actually happened) another agent concurrently editing `src/`, silently invalidates every test run. Verify `make desktop` rebuilt from YOUR sources before blaming the kernel.

## 9. Full regression checklist (after any okai/network change)

```
1. make desktop && make text        — both build, zero NEW warnings in edited files
2. Host unit tests: test_css, test_sha256, test_tls_crypto
3. tests/headless/test_link_click.py — link click → new window → fetch → render
4. tests/headless/test_errors.py     — empty-host refusal, NXDOMAIN abort, error
                                        page pixels, owner RELEASED (recovery works)
5. tests/headless/test_google.py     — redirect + 85KB chunked + white bg + ≥40 tokens
   (needs internet; skip with a note if offline)
6. Regression: example.com still loads (it's the canary — smallest page, any
   buffer/buffering change that breaks IT is fatal)
```

Each headless test: boots QEMU (~15s), drives the scenario, prints `VERDICT: PASS/FAIL`, exits non-zero on fail. Verdicts come from the serial log + pixel counts, never vibes.

## 10. Environment gotchas that actually bit

- **/tmp gets wiped** (found all yesterday's drivers gone). Anything you want to keep → project tree. /tmp is scratch only.
- **~/d is a SYMLINK to an external NTFS volume** ("Новый том"). If `~/d` looks empty, the symlink was moved to trash — the real project is at `/media/notdexy/Новый том/projects/okernel`. Do NOT `mkdir -p ~/d/...`; you'll create a shadow directory and test the wrong tree (this happened).
- **Other agents may be editing the same tree concurrently.** Check `ps aux | grep mimocode`, compare source mtimes to the ISO before diagnosing "regressions". A failed test during concurrent edits proves nothing.
- QEMU 8.2.2, `qemu-system-i386`. Monitor socket needs ~0.5s after boot before it accepts commands.
- google.com content is IP-localized (Russian here) — don't assume English when writing pixel/text assertions.

## 11. Quick reference: the 30-second test

```python
import sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

vm = OkVM("quick")            # boots headless QEMU
time.sleep(14)
vm.type_string("okai https://example.com/\n")
ok = vm.wait_for("https parse: count=", timeout=70)
w, h, px = vm.dump()          # screenshot
# ... pixel checks / serial greps ...
vm.kill()
print("PASS" if ok else "FAIL")
```

That's the whole harness: boot → type → poll serial → screendump → assert → kill. Everything else in this document is a refinement of that loop.

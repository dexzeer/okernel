# KAnarchy Headless Testing Guide

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

If the serial log doesn't tell you what you need — **add instrumentation**. The kernel already has `serial_printf()`; it's a 2-line change and a rebuild (HANDOFF.md says builds take seconds). The `[okai] click x=.. y=..` lines in okai.c exist precisely because pixel-guessing wasn't working. Instrument, rebuild, re-run.

## 1. The QEMU harness

Boot the desktop ISO headless with serial-to-file and a monitor socket:

```
qemu-system-i386 -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std \
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

The kernel prints, for EVERY click inside an okai page (page px, relative to the page origin below the 96px chrome):

```
[okai] click x=958 y=442 (mx=960 my=540) hit=1 node=39
[okai] LINK HIT -> http://example.com/
```

and after each page's first render, its link / form-control regions in view (same coordinates):

```
[okai] render tab=0 1916x936 doc_h=936 scroll=0 in 80ms (page origin 2,98) stack=13KB
[okai] link[0] x=368 y=276 w=379 h=102 href=http://example.com/
[okai] field[2] x=26 y=128 w=216 h=28 node=19 btn=0
```

So clicking becomes a feedback-control loop (`okvm.py`):

```
vm.click_link("example.com")      # first link region whose href contains it
vm.click_at(page_x, page_y, done=lambda log: ...)
vm.click_screen(x, y, r"nav action=5")   # chrome: tabs, toolbar, lock (reads [mse] btn=1)
```

Each one first dead-reckons the cursor toward the target (`premove`, so the probe click never lands on whatever link sits under the cursor), then: click → parse the LAST logged position → burst-correct → repeat until the expected log line appears.

Movement correction uses **bursts**: send the same delta 5× so the smoothing ring fills with it (net displacement ≈ 4.8 × delta). Correct in small steps and iterate; convergence takes 2–5 clicks.

Chrome geometry (okai opens maximized at (0,0), border 2, no title bar): tab strip y 2..40 (tab i at x 6+302i, 300 wide, x-box centered at +290; '+' right after the last tab), toolbar y 40..98 (back/fwd/reload/home centers x 25/63/101/139 at y 69; address bar from x 172; lock hit box x 174..194, y 48..68), page from (2, 98).

## 7. Host-side unit tests (fast, no QEMU)

The web engine (`src/web`) is plain C and builds unchanged on the host — debug rendering there, not in QEMU:

```bash
make web-tests                                   # CSS unit tests, font tests, builds build-host/wrender
python3 tools/fetch_corpus.py                    # real-site corpus into tests/web/corpus/ (gitignored)
./build-host/wrender wikipedia_os 1916 936 out.ppm 4000
python3 tests/web/compare.py wikipedia_os 1916 936 --fresh   # side-by-side vs headless Edge
WR_DUMP_TAG=figure ./build-host/wrender wikipedia_os 1916 936 out.ppm 100   # boxes of <figure> + ancestors
SAN="-fsanitize=address,undefined" OPT=-O1 sh tests/web/build.sh            # ASan/UBSan renderer
python3 tests/web/html5_diff.py ; python3 tests/web/image_diff.py           # parser vs html5lib, decoders vs PIL
```

To capture what a server sends OUR client: `curl -H 'User-Agent: okernel/0.4' -H 'Accept-Encoding: gzip' ...` (google serves a localized page to this IP; UA matters). To reproduce a layout bug, cut the page down in a corpus directory and bisect (styles vs structure vs classes) until one rule remains — that is how the Wikipedia thumbnail bug (percentage `max-width` on a replaced element) was found.

Crypto/TLS host suites: `make host-tests` (also runs `web-tests`). A test that fails to LINK is a broken test command, not a code failure — check `undefined reference` before concluding anything.

## 8. Root-cause discipline (how the bugs were actually found)

- **Reproduce first, theorize second.** The "black window" report became a reproducible serial-log stall in HP_DNS before any fix was attempted.
- **Diff working vs broken.** Fetch 1 (example.com) printed `DNS resolved, opening TCP:443`; fetch 2 stopped after `[dns] resolved:`. The missing line told us which code path died, and `dns_host_matches` reading `iana.orgcom` explained it.
- **One variable at a time.** Instrument → rebuild → re-run → compare logs. Never change two things between runs.
- **Beware tools that lie.** The serial formatter printed `total 6729 bytes` for an 86692-byte response (4-digit counter wrap). When numbers look impossible, verify the INSTRUMENT before believing the data (or the bug).
- **Check assumptions about the environment**: is the ISO newer than the sources you just edited? `ls --time-style` both. An ISO rebuild you forgot, or (as actually happened) another agent concurrently editing `src/`, silently invalidates every test run. Verify `make desktop` rebuilt from YOUR sources before blaming the kernel.

## 9. Full regression checklist (after any okai/network change)

```
1. make desktop && make text        — both build, zero NEW warnings in edited files
2. make web-tests (+ the corpus under ASan if you touched src/web)
3. python3 tests/headless/run_suite.py -j3 test_okai_interact.py test_links.py test_nav.py \
       test_tab_x.py test_errors.py test_addrbar.py test_lock.py test_stale_doc.py \
       test_fixed_header.py test_https_default.py test_certfail.py test_boot_mem.py
4. Network (needs internet): test_google_search.py, test_resume.py, test_pki_qemu.py,
   test_firstrender.py, and `test_okai_page.py https://en.wikipedia.org/wiki/Operating_system wiki --scroll 3`
5. Regression: example.com still loads (the canary)
```

Each headless test boots QEMU (~25s), drives the scenario, prints PASS/FAIL, exits non-zero on fail. Verdicts come from the serial log (+ pixel counts where the log can't say), never vibes.

**`wait_for` matches any EARLIER occurrence.** "Did the second page load?" must count `parse: count=` lines before and after, not `wait_for("parse: count=")` — several old tests raced exactly this way.

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
vm.wait_for("[mem] heap", timeout=40); time.sleep(10)
vm.type_string("okai https://example.com/\n")
ok = vm.wait_for("https parse: count=", timeout=70)
w, h, px = vm.dump()          # screenshot
# ... pixel checks / serial greps ...
vm.kill()
print("PASS" if ok else "FAIL")
```

That's the whole harness: boot → type → poll serial → screendump → assert → kill. Everything else in this document is a refinement of that loop.

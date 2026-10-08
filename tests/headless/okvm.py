#!/usr/bin/env python3
"""okvm.py — base library for driving KAnarchy headlessly in QEMU.

Boots the desktop ISO with a serial-file log and a UNIX-socket QEMU monitor,
and provides: keystroke injection, screendump loading, empirical mouse
movement (bursts that defeat the kernel's 4-sample PS/2 smoothing), clicks,
and a closed-loop click-convergence helper driven by the kernel's own
`[okai] click row=.. col=..` serial instrumentation lines.

Usage pattern (see test_link_click.py for a full example):
    from okvm import OkVM
    vm = OkVM("tag")           # boots QEMU
    time.sleep(14)             # wait for kernel boot
    vm.type_string("okai https://example.com/\\n")
    ...                        # poll vm.serial() for lifecycle lines
    vm.kill()

Ground rules that make this work (read TESTING.md §0-§2):
  - The SERIAL LOG is ground truth. Never guess state from pixels alone.
  - The kernel logs `[okai] click x=X y=Y (mx=.. my=..) hit=K node=N` for
    every click in an okai page (page px), and after each page's first render
    `[okai] link[i] x=.. y=.. w=.. h=.. href=..` for the links in view.
    Convergence = click, read, correct (click_at / click_link).
"""
import subprocess, time, os, signal, socket, re

# Resolve the project/ISO relative to this file so the driver works from any cwd.
HERE     = os.path.dirname(os.path.abspath(__file__))
PROJECT  = os.path.dirname(os.path.dirname(HERE))
ISO      = os.environ.get("OKVM_ISO") or os.path.join(PROJECT, "kanarchy-desktop.iso")
# Durable output dir — /tmp gets aggressively cleaned on this host and wiped
# logs mid-run, killing tests (QEMU keeps writing to the deleted inode).
OUTDIR   = os.path.expanduser("~/okvm")

BOOT_WAIT = 14          # seconds; kernel boots in ~8s, 14 is safe


class OkVM:
    def __init__(self, tag="vm", extra_net=True, disk=None):
        os.makedirs(OUTDIR, exist_ok=True)
        self.LOG  = os.path.join(OUTDIR, f"{tag}_serial.log")
        self.SOCK = os.path.join(OUTDIR, f"{tag}_mon.sock")
        self.PPM  = os.path.join(OUTDIR, f"{tag}.ppm")
        self.PPM2 = os.path.join(OUTDIR, f"{tag}b.ppm")
        for f in [self.LOG, self.SOCK, self.PPM, self.PPM2]:
            try: os.unlink(f)
            except FileNotFoundError: pass

        cmd = ["qemu-system-i386", "-m", os.environ.get("OKVM_MEM", "512"), "-cdrom", ISO, "-boot", "d", "-vga", "std",
               "-serial", f"file:{self.LOG}", "-display", "none",
               "-monitor", f"unix:{self.SOCK},server,nowait", "-no-reboot"]
        if extra_net:  # e1000 + SLIRP user networking (10.0.2.x)
            cmd += ["-device", "e1000,netdev=net0", "-netdev", "user,id=net0"]
            if os.environ.get("OKVM_PCAP"):   # packet capture: ~/okvm/<tag>.pcap
                cmd += ["-object", f"filter-dump,id=dump0,netdev=net0,file={os.path.join(OUTDIR, tag + '.pcap')}"]
        if disk:  # raw ATA disk image for the persistent-FS layer
            cmd += ["-hda", disk]
        if os.environ.get("OKVM_TRACE"):  # QEMU trace events, e.g. OKVM_TRACE='ps2_*,serial_write' -> ~/okvm/<tag>.trace
            cmd += ["-msg", "timestamp=on"]
            for pat in os.environ["OKVM_TRACE"].split(","):   # file= on each: a bare -trace resets it
                cmd += ["-trace", f"enable={pat},file={os.path.join(OUTDIR, tag + '.trace')}"]
        if os.environ.get("OKVM_ACCEL"):  # e.g. OKVM_ACCEL=kvm (needs /dev/kvm access)
            cmd += ["-accel", os.environ["OKVM_ACCEL"]]
        self.q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, cwd=PROJECT)
        # Wait for the monitor socket to appear
        for _ in range(40):
            if os.path.exists(self.SOCK): break
            time.sleep(0.5)
        if not os.path.exists(self.SOCK):
            raise RuntimeError("QEMU monitor socket never appeared")

    # ---- QEMU monitor (HMP) ----------------------------------------------

    def mon(self, cmd, wait=0.06):
        """Send one HMP command over the monitor socket."""
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(self.SOCK)
            s.sendall((cmd + "\n").encode())
            time.sleep(wait)
            s.close()
            return True
        except Exception as e:
            print("mon err", e)
            return False

    def type_string(self, s, delay=0.05):
        """Type a string via sendkey. Only the keymap below is guaranteed —
        extend it if you need new punctuation (check `qemu sendkey` names)."""
        km = {' ': 'spc', '/': 'slash', '.': 'dot', ':': 'shift-semicolon',
              '-': 'minus', '\n': 'ret', '=': 'equal', '_': 'shift-minus',
              '?': 'shift-slash', '\t': 'tab', '\b': 'backspace',
              '\x1b': 'esc'}
        for ch in s:
            k = km.get(ch, ch.lower() if ch.isalpha() else ch)
            if ch.isupper(): k = f"shift-{ch.lower()}"
            elif ch.isdigit(): k = ch
            self.mon(f"sendkey {k}", delay)

    # ---- Serial -----------------------------------------------------------

    def serial(self):
        """Full serial log so far (it's ground truth — poll it, grep it)."""
        try:
            with open(self.LOG, 'r', errors='replace') as f: return f.read()
        except FileNotFoundError: return ""

    def wait_for(self, pattern, timeout=70, poll=1.0, fail_pats=None):
        """Wait until `pattern` (substring) appears in the serial log.
        Returns True/False. Also aborts on CRASH markers."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(poll)
            log = self.serial()
            if pattern in log: return True
            if fail_pats and any(p in log for p in fail_pats): return False
            if "triple fault" in log: return False
        return False

    def click_lines(self):
        """Parse kernel click instrumentation: exact ground-truth coords.
        Returns list of (x, y, mx, my): page px (relative to the page origin)
        and the screen position the kernel saw."""
        out = []
        for line in self.serial().splitlines():
            m = re.search(r"\[okai\] click x=(-?\d+) y=(-?\d+) \(mx=(\d+) my=(\d+)\)", line)
            if m:
                out.append(tuple(int(m.group(i)) for i in range(1, 5)))
        return out

    def link_regions(self):
        """Link regions of the LAST full render (page px, viewport-relative):
        list of (i, x, y, w, h, href)."""
        out = []
        for line in self.serial().splitlines():
            if "[okai] render tab=" in line and "page origin" in line:
                out = []
            # a listing restarts at index 0 (scripts moved content: re-logged)
            if re.search(r"\[okai\] (link|field)\[0\] ", line):
                out = []
            m = re.search(r"\[okai\] link\[(\d+)\] x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) href=(\S*)", line)
            if m:
                out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)),
                            int(m.group(4)), int(m.group(5)), m.group(6)))
        return out

    def page_origin(self):
        """Screen position of the page's (0,0) from the last full render."""
        org = (2, 98)
        for m in re.finditer(r"page origin (\d+),(\d+)", self.serial()):
            org = (int(m.group(1)), int(m.group(2)))
        return org

    def find_link(self, href_part):
        """First link region of the last render whose href contains href_part."""
        for r in self.link_regions():
            if href_part in r[5]:
                return r
        return None
    # ---- Screenshots -------------------------------------------------------

    def dump(self, path=None):
        """Screendump to PPM; returns (w, h, pixel bytes)."""
        path = path or self.PPM
        self.mon(f"screendump {path}", 0.5)
        data = open(path, 'rb').read()
        parts = data.split(b'\n', 3)
        w, h = map(int, parts[1].split())
        return w, h, parts[3]

    # ---- Mouse -------------------------------------------------------------

    def burst(self, dx, dy, n=5):
        """Send the same relative delta n times. The driver applies packets
        1:1 (cursor.c, no smoothing since 2026-10-08): net displacement is
        ~= n * (dx, dy)."""
        for _ in range(n):
            self.mon(f"mouse_move {dx} {dy}", 0.09)
        time.sleep(0.12)

    def click(self):
        self.mon("mouse_button 1", 0.22)
        self.mon("mouse_button 0", 0.35)

    def mse_presses(self):
        """Screen positions of every left-button press ([mse] btn=1 lines,
        logged anywhere on screen)."""
        return [(int(m.group(1)), int(m.group(2))) for m in
                re.finditer(r"\[mse\] btn=1 x=(\d+) y=(\d+)", self.serial())]

    def premove(self, tx, ty, gain=4.8):
        """Dead-reckon the cursor toward SCREEN (tx, ty) WITHOUT clicking,
        from the last logged press (or the boot position, screen center), so
        the first probe click of a closed loop does not land on whatever sits
        under the cursor (e.g. a link that would navigate away)."""
        ps = self.mse_presses()
        x, y = ps[-1] if ps else (960, 540)
        for _ in range(10):
            dx, dy = tx - x, ty - y
            if abs(dx) <= 6 and abs(dy) <= 6:
                break
            bx = max(-60, min(60, int(dx / gain)))
            by = max(-60, min(60, int(dy / gain)))
            self.burst(bx, by)
            x += bx * gain
            y += by * gain
        for _ in range(5):
            self.mon("mouse_move 0 0", 0.05)
        time.sleep(0.3)

    def ui_rect(self, name):
        """Latest chrome rect (x, y, w, h) for `name` from okai's `[okai] ui`
        geometry lines: back fwd reload home url ident newtab min max close,
        tab<i> tabx<i> (tab i / its close box). None if never logged."""
        pat = re.compile(r"\[okai\] ui .*?\b%s=(-?\d+),(-?\d+),(\d+),(\d+)" % re.escape(name))
        for line in reversed(self.serial().splitlines()):
            m = pat.search(line)
            if m:
                return tuple(int(v) for v in m.groups())
        return None

    def taskbar_rect(self, win):
        """Latest taskbar button rect (x, y, w, h) of window id `win`, from
        taskbar.c's `[taskbar] btn win=N x,y,w,h` lines (logged whenever the
        bar's layout changes). None if never logged."""
        pat = re.compile(r"\[taskbar\] btn win=%d (\d+),(\d+),(\d+),(\d+)" % win)
        for line in reversed(self.serial().splitlines()):
            if "[taskbar] layout " in line:
                break                      # older layout: not current
            m = pat.search(line)
            if m:
                return tuple(int(v) for v in m.groups())
        return None

    def ui_center(self, name, timeout=20):
        """Center of chrome element `name` (waits for its geometry line)."""
        end = time.time() + timeout
        while True:
            r = self.ui_rect(name)
            if r or time.time() > end:
                break
            time.sleep(0.5)
        if not r:
            raise RuntimeError("no [okai] ui geometry for " + name)
        return r[0] + r[2] // 2, r[1] + r[3] // 2

    def anchor(self, x, y):
        """Click once at a harmless spot near (x, y) so the next dead-reckoned
        move starts from a logged press (long moves drift ~50px — enough to
        hit a neighbouring caption button such as close)."""
        n = len(self.mse_presses())
        self.premove(x, y)
        self.click()
        time.sleep(0.4)
        return len(self.mse_presses()) > n

    def click_ui(self, name, expect, tries=12, anchor=None):
        """Closed-loop click on chrome element `name` until `expect` logs.
        anchor=(x, y): probe-click there first (a harmless spot near it)."""
        x, y = self.ui_center(name)
        if anchor:
            self.anchor(*anchor)
        return self.click_screen(x, y, expect, tries)

    def click_screen(self, tx, ty, expect, tries=12, gain=4.8):
        """CLOSED-LOOP click at SCREEN (tx, ty) until the regex `expect`
        gains a match in the serial log (chrome buttons, tabs, anything
        outside the page). Corrects with bursts from the logged press."""
        self.premove(tx, ty, gain)
        for _ in range(tries):
            before = len(re.findall(expect, self.serial()))
            self.click()
            time.sleep(0.6)
            if len(re.findall(expect, self.serial())) > before:
                return True
            ps = self.mse_presses()
            if not ps:
                self.burst(0, 10)
                continue
            cx, cy = ps[-1]
            dx, dy = tx - cx, ty - cy
            if abs(dx) <= 3 and abs(dy) <= 3:
                continue
            self.burst(max(-60, min(60, int(dx / gain))), max(-60, min(60, int(dy / gain))))
            for _ in range(5):
                self.mon("mouse_move 0 0", 0.05)
            time.sleep(0.3)
        return False

    def click_at(self, tx, ty, max_iters=20, done=None):
        """CLOSED-LOOP click at page px (tx, ty): click, read the kernel-logged
        page coords, correct with a burst, repeat. done(serial) -> bool stops
        early (e.g. when the expected LINK HIT shows up). Returns the last
        (x, y) the kernel reported, or None."""
        last = None
        ox, oy = self.page_origin()
        self.premove(ox + tx, oy + ty)
        for attempt in range(max_iters):
            before = len(self.click_lines())
            self.click()
            time.sleep(0.6)
            if done and done(self.serial()):
                return last
            clicks = self.click_lines()
            if len(clicks) <= before:
                # click outside the page (chrome band): nudge down/right
                self.burst(0, 8)
                continue
            x, y, mx, my = clicks[-1]
            last = (x, y)
            dx, dy = tx - x, ty - y
            if abs(dx) <= 3 and abs(dy) <= 3:
                return last
            bx = max(-60, min(60, int(dx / 4.0)))
            by = max(-60, min(60, int(dy / 4.0)))
            if bx == 0 and abs(dx) > 3: bx = 1 if dx > 0 else -1
            if by == 0 and abs(dy) > 3: by = 1 if dy > 0 else -1
            self.burst(bx, by)
        return last

    def click_link(self, href_part, max_iters=20):
        """Click the first rendered link whose href contains href_part
        (closed loop on the kernel's own click/LINK HIT logs). Returns True
        iff a LINK HIT for that href appears."""
        r = self.find_link(href_part)
        if not r:
            return False
        _, x, y, w, h, href = r
        tx, ty = x + min(w // 2, 20), y + h // 2
        hits0 = self.serial().count("[okai] LINK HIT")
        def done(log):
            return log.count("[okai] LINK HIT") > hits0
        self.click_at(tx, ty, max_iters=max_iters, done=done)
        log = self.serial()
        if log.count("[okai] LINK HIT") <= hits0:
            return False
        last = log[log.rindex("[okai] LINK HIT"):].splitlines()[0]
        return href_part in last

    # ---- Teardown -----------------------------------------------------------

    def kill(self):
        self.q.send_signal(signal.SIGTERM)
        try: self.q.wait(timeout=5)
        except Exception:
            self.q.kill()

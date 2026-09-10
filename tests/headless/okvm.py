#!/usr/bin/env python3
"""okvm.py — base library for driving okernel headlessly in QEMU.

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
  - The kernel logs `[okai] click row=R col=C (mx=X my=Y)` for every click
    inside an okai content area, and `[okai] link[i] row=.. col0=.. col1=..`
    for every rendered link region. Convergence = click, read, correct.
"""
import subprocess, time, os, signal, socket, re

# Resolve the project/ISO relative to this file so the driver works from any cwd.
HERE     = os.path.dirname(os.path.abspath(__file__))
PROJECT  = os.path.dirname(os.path.dirname(HERE))
ISO      = os.path.join(PROJECT, "okernel-desktop.iso")
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

        cmd = ["qemu-system-i386", "-cdrom", ISO, "-boot", "d", "-vga", "std",
               "-serial", f"file:{self.LOG}", "-display", "none",
               "-monitor", f"unix:{self.SOCK},server,nowait", "-no-reboot"]
        if extra_net:  # e1000 + SLIRP user networking (10.0.2.x)
            cmd += ["-device", "e1000,netdev=net0", "-netdev", "user,id=net0"]
        if disk:  # raw ATA disk image for the persistent-FS layer
            cmd += ["-hda", disk]
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
              '?': 'shift-slash'}
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
        Returns list of (row, col, mx, my)."""
        out = []
        for line in self.serial().splitlines():
            m = re.search(r"\[okai\] click row=(\d+) col=(\d+) \(mx=(\d+) my=(\d+)\)", line)
            if m:
                out.append((int(m.group(1)), int(m.group(2)),
                            int(m.group(3)), int(m.group(4))))
        return out

    def link_regions(self):
        """Parse rendered link regions: list of (i, row, col0, col1, href)."""
        out = []
        for line in self.serial().splitlines():
            m = re.search(r"\[okai\] link\[(\d+)\] row=(\d+) col0=(\d+) col1=(\d+) href=(\S*)", line)
            if m:
                out.append((int(m.group(1)), int(m.group(2)),
                            int(m.group(3)), int(m.group(4)), m.group(5)))
        return out

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
        """Send the same relative delta n times so the kernel's 4-sample
        moving-average smoothing ring fills with it. Net displacement is
        ~= 3.75 * (dx, dy). Single HMP mouse_move moves are diluted by the
        stale samples in the ring — that's why bursts."""
        for _ in range(n):
            self.mon(f"mouse_move {dx} {dy}", 0.09)
        time.sleep(0.12)

    def click(self):
        self.mon("mouse_button 1", 0.22)
        self.mon("mouse_button 0", 0.35)

    def click_link(self, row, col0, col1, max_iters=20, expect_href=None):
        """CLOSED-LOOP link click: click, read the kernel-reported coordinates
        from serial, correct with a burst, repeat until inside the region.
        This is the single most reliable way to click anything in okernel.
        Geometry (verified 2026-09-10 against the kernel hit-test): content
        cells are CONTENT_GW x CONTENT_GH = 12x24 px (font_scale 1);
        serial link rows are DOC rows, the hit-test uses BUFFER rows
        (doc + CHROME_ROWS(3), scroll 0). expect_href (exact logged href)
        rejects WRONG-link hits: any hit for another href returns False
        immediately instead of compounding clicks in the new page.
        Returns True iff the expected HIT appears (or any HIT when
        expect_href is None)."""
        # content origin: FIRST okai window at (1010,60), no_titlebar, so the
        # content grid starts at x+2/y+2 (border).
        ox, oy = 1012, 62
        tcol = (col0 + col1) // 2
        tx = ox + tcol * 12 + 6
        ty = oy + (row + 3) * 24 + 12  # +CHROME_ROWS for the reserved chrome
        # Baseline LINK HIT count: stop at the FIRST hit. Verifying via a
        # later click leaves the cursor over the NEW window's links, where
        # stray confirmation clicks navigate again and pollute the lifecycle.
        hits0 = self.serial().count("[okai] LINK HIT")
        for attempt in range(max_iters):
            before = len(self.click_lines())
            self.click()
            time.sleep(0.6)
            if self.serial().count("[okai] LINK HIT") > hits0:
                if expect_href is None:
                    return True
                return expect_href in self.serial()
            clicks = self.click_lines()
            if len(clicks) <= before:
                continue  # click not registered (shouldn't happen)
            c_row, c_col, mx, my = clicks[-1]
            if c_row == row + 3 and col0 <= c_col <= col1:
                time.sleep(1)
                return "[okai] LINK HIT" in self.serial()
            dx, dy = tx - mx, ty - my
            bx = max(-60, min(60, int(dx / 4.0)))
            by = max(-60, min(60, int(dy / 4.0)))
            if bx == 0 and abs(dx) > 6: bx = 1 if dx > 0 else -1
            if by == 0 and abs(dy) > 6: by = 1 if dy > 0 else -1
            self.burst(bx, by)
        return "[okai] LINK HIT" in self.serial()

    # ---- Teardown -----------------------------------------------------------

    def kill(self):
        self.q.send_signal(signal.SIGTERM)
        try: self.q.wait(timeout=5)
        except Exception:
            self.q.kill()

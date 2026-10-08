#!/usr/bin/env python3
"""Keystroke accounting from a QEMU PS/2 trace: did every key byte QEMU queued
reach the kernel?

    OKVM_TRACE='ps2_*' python3 tests/headless/test_sites.py mytag
    python3 tests/headless/ps2_trace.py ~/okvm/mytag.trace

One queued key event (translated set) = one byte for the guest;
ps2_read_data on the keyboard device = one byte consumed (FIFO). queued >
read means QEMU's 16-byte PS/2 queue overflowed (it drops silently) — the
guest never saw those keys — or the VM was killed with keys pending (see
"unread at end"). "max outstanding" near 16 / a long "worst read latency" =
IRQ1 serviced late. If a typed string arrives garbled while lost=0, the bug
is in the kernel.
"""
import sys, os, re

PRE = re.compile(r'^\d+@(\d+\.\d+):')   # present with -msg timestamp=on

for path in sys.argv[1:]:
    recs = []
    for l in open(path, errors='replace').read().splitlines():
        m = PRE.match(l)
        recs.append((float(m.group(1)) if m else None, l[m.end():] if m else l))
    # QEMU traces ps2_keyboard_event BEFORE checking that scanning is enabled
    # (keys pressed while the kernel holds the port disabled during init are
    # discarded there): an event counts only if a ps2_put_keycode follows it.
    queued = set()
    for i, (t, l) in enumerate(recs):
        if not l.startswith('ps2_keyboard_event'):
            continue
        for t2, l2 in recs[i + 1:]:
            if l2.startswith('ps2_put_keycode'):
                queued.add(i); break
            if l2.startswith('ps2_keyboard_event'):
                break
    if not queued:
        print(f"{os.path.basename(path)}: no key events")
        continue
    kb = recs[min(queued)][1].split()[1]
    pend, mx, reads, worst = [], 0, 0, 0.0
    for i, (t, l) in enumerate(recs):
        if i in queued:
            pend.append(t)
        elif l.startswith('ps2_read_data') and l.split()[1] == kb and pend:
            reads += 1
            t0 = pend.pop(0)
            if t is not None and t0 is not None:
                worst = max(worst, t - t0)
        mx = max(mx, len(pend))
    end = recs[-1][0]
    tail = sum(1 for t in pend if t is not None and end is not None and end - t < 1.0)
    print(f"{os.path.basename(path)}: key bytes queued={len(queued)} read={reads} "
          f"lost={len(queued) - reads} (unread at end: {tail}) max outstanding={mx} "
          f"worst read latency={worst * 1000:.0f}ms")

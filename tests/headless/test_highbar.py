#!/usr/bin/env python3
"""Devices above 4 GB: make kanarchy-highbar.iso, whose USB driver moves its controller's BAR above 4 GB before using it (the firmware leaves small BARs
low). A 2 GB device opens QEMU's 64-bit PCI window. Reads the real addresses from QEMU's monitor (a run where nothing moved proves nothing and fails), then
checks the driver works there: the controller finds its tablet."""
import os, re, socket, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["OKVM_MEM"] = os.environ.get("OKVM_MEM", "2048")
os.environ.setdefault("OKVM_ISO", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "kanarchy-highbar.iso"))
os.environ["OKVM_EXTRA"] = "-object memory-backend-ram,id=hm,size=2G -global i440FX-pcihost.pci-hole64-size=16G -device ivshmem-plain,memdev=hm -device qemu-xhci,id=x -device usb-tablet,bus=x.0"
from okvm import OkVM

fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

def monitor(vm, cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(vm.SOCK)
    s.settimeout(1.0)
    s.sendall((cmd + "\n").encode())
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
    except socket.timeout:
        pass
    s.close()
    return out.decode(errors="replace").replace("\r", "")

vm = OkVM("highbar")
try:
    check(vm.wait_for("[mem] heap", timeout=120), "desktop boots")
    time.sleep(12)
    pci = monitor(vm, "info pci")
    blocks = re.split(r"\n\s*Bus\s+", pci)
    high = {}
    for b in blocks:
        m = re.search(r"(?:USB controller|Audio controller|RAM controller|Ethernet controller|VGA controller)[^\n]*", b)
        for a in re.findall(r"BAR\d+: 64 bit (?:prefetchable )?memory at (0x[0-9a-f]+)", b):
            if int(a, 16) >= 1 << 32:
                high.setdefault(m.group(0).split(":")[0] if m else "?", []).append(a)
    print("BARs above 4 GB:", high)
    usb_high = any("USB" in k for k in high)
    check(usb_high, "the USB controller's BAR really is above 4 GB")
    before = len(vm.serial())
    vm.type_string("usb\n")
    time.sleep(6)
    out = vm.serial()
    print("\n".join(l[:120] for l in out[before:].splitlines() if not l.startswith(("[fps]", "[net]", "[taskbar")))[:900])
    check("BAR0 unusable" not in out, "the driver accepts the BAR")
    check(re.search(r"tablet|absolute|slot \d+ ptr", out, re.I) is not None, "the controller works: the tablet is found")
    check("[ISR]" not in out and "[panic]" not in out, "no fault")
finally:
    vm.kill()
print("RESULT " + ("ALLPASS" if not fails else "FAIL: " + "; ".join(fails)))
sys.exit(1 if fails else 0)

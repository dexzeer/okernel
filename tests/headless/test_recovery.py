#!/usr/bin/env python3
"""The recovery path on an installed disk (make disk): the desktop panics, saves a crash record, sets GRUB's one-shot
next_entry and reboots on R; GRUB starts the recovery system, whose tools read the crash record and the log of the
crashed boot, check the data partition, restore the kernel; `knowngood` boots the recovery partition's kernel once;
after that the desktop starts normally again.

    make disk && python3 tests/headless/test_recovery.py
"""
import sys, os, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import okvm
from okvm import OkVM

IMG = os.path.join(okvm.OUTDIR, "recovery_disk.img")
os.makedirs(okvm.OUTDIR, exist_ok=True)
shutil.copy(os.environ.get("RECOVERY_DISK") or os.path.join(okvm.PROJECT, "kanarchy-disk.img"), IMG)
fails = []

def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

def wait_exit(vm, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if vm.q.poll() is not None: return True
        time.sleep(0.5)
    return False

def grubenv():
    # the boot partition starts at LBA 2048 (tools/disk/mkdisk.py)
    r = subprocess.run(["mtype", "-i", f"{IMG}@@{2048 * 512}", "::/boot/grub/grubenv"], capture_output=True, text=True)
    return r.stdout

def command(vm, cmd, expect, timeout=120):
    log0 = vm.serial()
    n = log0.count("[recovery] done")
    vm.type_string(cmd + "\n")
    t0 = time.time()
    while time.time() - t0 < timeout and vm.serial().count("[recovery] done") == n:
        time.sleep(0.5)
    new = vm.serial()[len(log0):]
    if "---- end of" in new:                    # a viewer window opened (and has the focus): close it
        time.sleep(1)
        vm.mon("sendkey q", 0.3)
        time.sleep(1)
    check(vm.serial().count("[recovery] done") > n and expect in new, f"recovery `{cmd}` prints {expect!r}")

# 1. desktop from the disk, then a kernel fault
vm = OkVM("recA", boot_disk=IMG)
try:
    check(vm.wait_for("[mem] heap", timeout=120), "desktop boots from the disk")
    check(vm.wait_for("[pfs] grubenv found", timeout=30), "the desktop finds grubenv")
    check(vm.wait_for("log on the disk's log partition: boot 1", timeout=30), "klog writes the log partition")
    time.sleep(10)
    vm.type_string("run /bin/panic fault\n")
    check(vm.wait_for("[panic] countdown 30", timeout=60), "panic screen up")
    log = vm.serial()
    check("invalid opcode" in log and "syscall_handler+0x" in log, "the report names the exception and the function")
    check("saved as crash record #1" in log, "crash record saved")
    check("next_entry=recovery: set" in log, "grubenv set")
    check("next_entry=recovery" in grubenv(), "grubenv on the disk says recovery")
    vm.mon(f"screendump {okvm.OUTDIR}/rec_panic.ppm", 1.0)
    vm.mon("sendkey r")
    check(wait_exit(vm), "R reboots")
finally:
    vm.kill()

# 2. GRUB boots the recovery desktop (the known-good kernel with `recovery`)
vm = OkVM("recB", boot_disk=IMG)
try:
    check(vm.wait_for("[boot] recovery mode", timeout=90), "the recovery desktop starts")
    check(vm.wait_for("[mem] heap", timeout=90), "recovery desktop up")
    time.sleep(10)
    log = vm.serial()
    check("log on the disk" not in log and "[pfs] mounted" not in log, "recovery leaves the log and the data partition alone")
    check("[part] 4 partitions" in log, "recovery sees the partitions")
    check(not re.search(r"next_entry=[^\s#]", grubenv()), "GRUB cleared the one-shot entry")
    m = re.search(r"\[recovery\] crash window close (\d+),(\d+)", log)
    check(m and "invalid opcode\n  syscall_handler+0x" in log and "You're on your own now." in log,
          "crash window: cause, function, message")
    vm.mon(f"screendump {okvm.OUTDIR}/rec_crashwin.ppm", 1.0)
    n = log.count("[recovery] done")
    vm.type_string("part\n")                    # the crash window has the focus and takes no input
    time.sleep(3)
    check(vm.serial().count("[recovery] done") == n, "crash window ignores typing")
    check(m and vm.click_screen(int(m.group(1)), int(m.group(2)), r"\[recovery\] crash window closed"), "crash window closes")
    time.sleep(1)
    vm.type_string("okai\n")
    vm.type_string("sysinfo\n")
    time.sleep(3)
    check(vm.serial().count("[recovery] not found") == 2, "okai and sysinfo do not exist in recovery")
    command(vm, "crash list", "#1  boot 1")
    command(vm, "crash", "syscall_handler+0x")
    command(vm, "log", "sys_panic mode 1")
    command(vm, "log dev", "log on the disk's log partition")
    command(vm, "part", "recovery")
    command(vm, "ls", "files")
    command(vm, "fsck", "clean")
    command(vm, "ls recovery /known", "KANARCHY.BIN")
    command(vm, "hexdump 0", "55 aa")
    command(vm, "restore", "restored, verified")
    command(vm, "memtest 8", "8 MB ok")
    vm.type_string("knowngood\n")
    check(wait_exit(vm), "knowngood reboots")
    check("next_entry=knowngood" in grubenv(), "grubenv says knowngood")
finally:
    vm.kill()

# 3. the known-good kernel (once), 4. the normal desktop again
for tag in ("recC", "recD"):
    vm = OkVM(tag, boot_disk=IMG)
    try:
        check(vm.wait_for("[mem] heap", timeout=120) and "[boot] recovery mode" not in vm.serial(), f"{tag}: the desktop starts")
        check(vm.wait_for("[pfs] mounted", timeout=30), f"{tag}: data partition mounted")
        if tag == "recC":
            check(not re.search(r"next_entry=[^\s#]", grubenv()), "the known-good boot was one-shot")
            vm.type_string("reboot\n")
            wait_exit(vm)
    finally:
        vm.kill()

print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)

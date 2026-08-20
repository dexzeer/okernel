#!/usr/bin/env python3
"""Headless test for okernel networking — tests with google.com."""
import subprocess, time, os, sys, signal

SERIAL_LOG = "/tmp/okernel-serial.log"
MONITOR_SOCK = "/tmp/okernel-monitor.sock"
ISO = "okernel-desktop.iso"

def cleanup():
    for f in [SERIAL_LOG, MONITOR_SOCK]:
        try: os.unlink(f)
        except: pass

def send_monitor_cmd(sock_path, cmd):
    import socket
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sock_path)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.05)
        resp = s.recv(4096)
        s.close()
        return resp.decode(errors='replace')
    except Exception as e:
        print(f"Monitor error: {e}")
        return ""

def type_string(sock_path, s):
    keymap = {
        ' ': 'spc', '/': 'slash', '.': 'dot', ':': 'shift-semicolon',
        '-': 'minus', '\n': 'ret',
    }
    for ch in s:
        if ch in keymap:
            key = keymap[ch]
        elif ch.isupper():
            key = f"shift-{ch.lower()}"
        elif ch.isdigit():
            key = ch
        elif ch.isalpha():
            key = ch.lower()
        else:
            key = ch
        send_monitor_cmd(sock_path, f"sendkey {key}")
        time.sleep(0.08)

def read_serial():
    try:
        with open(SERIAL_LOG, 'r', errors='replace') as f:
            return f.read()
    except:
        return ""

cleanup()

print("=== Starting QEMU ===")
qemu = subprocess.Popen([
    "qemu-system-i386",
    "-cdrom", ISO,
    "-boot", "d",
    "-vga", "std",
    "-device", "e1000,netdev=net0",
    "-netdev", "user,id=net0",
    "-serial", f"file:{SERIAL_LOG}",
    "-display", "none",
    "-monitor", f"unix:{MONITOR_SOCK},server,nowait",
    "-no-reboot",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for i in range(20):
    if os.path.exists(MONITOR_SOCK):
        break
    time.sleep(0.5)

print("=== Waiting for kernel boot (15s) ===")
time.sleep(15)

# Test 1: resolve google.com
print("\n=== TEST: resolve google.com ===")
type_string(MONITOR_SOCK, "resolve google.com\n")
print("Waiting 10s for DNS...")
time.sleep(10)

log = read_serial()
print("--- DNS results ---")
for line in log.strip().split('\n'):
    if any(k in line for k in ['[dns]', 'Resolved', 'resolve']):
        print(f"  {line}")

# Test 2: browse google.com
print("\n=== TEST: browse google.com / ===")
type_string(MONITOR_SOCK, "browse google.com /\n")
print("Waiting 20s for HTTP response...")
time.sleep(20)

log = read_serial()
print("--- TCP/HTTP results ---")
for line in log.strip().split('\n'):
    if any(k in line for k in ['[tcp]', '[http]', 'ESTABLISHED', 'CLOSED', 'HTTP', 'SYN']):
        print(f"  {line}")

# Save full log
with open("/tmp/okernel-full2.log", "w") as f:
    f.write(log)
print(f"\nFull log: {len(log)} bytes -> /tmp/okernel-full2.log")

qemu.send_signal(signal.SIGTERM)
try: qemu.wait(timeout=5)
except: qemu.kill()
cleanup()
print("=== Done ===")

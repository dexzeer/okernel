#!/usr/bin/env python3
"""Headless test for okernel networking via QEMU monitor + serial log."""
import subprocess, time, os, sys, signal

SERIAL_LOG = "/tmp/okernel-serial.log"
MONITOR_SOCK = "/tmp/okernel-monitor.sock"
ISO = "okernel-desktop.iso"

def cleanup():
    for f in [SERIAL_LOG, MONITOR_SOCK]:
        try: os.unlink(f)
        except: pass

def send_monitor_cmd(sock_path, cmd):
    """Send a command to QEMU monitor via unix socket."""
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
    """Type a string via QEMU sendkey."""
    keymap = {
        ' ': 'spc', '/': 'slash', '.': 'dot', ':': 'shift-semicolon',
        '-': 'minus', '_': 'shift-minus', '\n': 'ret',
        '=': 'equal', '+': 'shift-equal', '!': 'shift-1',
        '@': 'shift-2', '#': 'shift-3',
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

print(f"QEMU PID: {qemu.pid}")

# Wait for monitor socket
for i in range(20):
    if os.path.exists(MONITOR_SOCK):
        break
    time.sleep(0.5)

if not os.path.exists(MONITOR_SOCK):
    print("ERROR: Monitor socket not found")
    qemu.kill()
    sys.exit(1)

# Wait for kernel to boot
print("=== Waiting for kernel boot (15s) ===")
time.sleep(15)

log = read_serial()
print("--- Boot log (last 20 lines) ---")
for line in log.strip().split('\n')[-20:]:
    print(f"  {line}")

# Test 1: resolve example.com
print("\n=== TEST 1: resolve example.com ===")
type_string(MONITOR_SOCK, "resolve example.com\n")
print("Waiting 10s for DNS response...")
time.sleep(10)

log = read_serial()
print("--- Serial log after resolve ---")
# Filter for DNS-related lines
for line in log.strip().split('\n'):
    if any(k in line for k in ['[dns]', '[arp]', '[e1000_tx]', 'Resolved', 'resolve', 'query']):
        print(f"  {line}")

# Test 2: browse example.com /
print("\n=== TEST 2: browse example.com / ===")
type_string(MONITOR_SOCK, "browse example.com /\n")
print("Waiting 15s for HTTP response...")
time.sleep(15)

log = read_serial()
print("--- Serial log after browse (relevant lines) ---")
for line in log.strip().split('\n'):
    if any(k in line for k in ['[dns]', '[tcp]', '[http]', '[arp]', 'Resolved', 'browse', 'BROWSING']):
        print(f"  {line}")

# Save full log
with open("/tmp/okernel-full.log", "w") as f:
    f.write(log)
print(f"\nFull log saved to /tmp/okernel-full.log ({len(log)} bytes)")

# Cleanup
print("\n=== Killing QEMU ===")
qemu.send_signal(signal.SIGTERM)
try:
    qemu.wait(timeout=5)
except:
    qemu.kill()
cleanup()
print("=== Done ===")

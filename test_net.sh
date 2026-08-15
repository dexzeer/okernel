#!/bin/bash
# Headless test script for okernel networking
# Boots QEMU, sends keystrokes via monitor, captures serial output

SERIAL_LOG="/tmp/okernel-serial.log"
MONITOR_SOCK="/tmp/okernel-monitor.sock"
ISO="okernel-desktop.iso"
TIMEOUT=30

# Cleanup
rm -f "$SERIAL_LOG" "$MONITOR_SOCK"

echo "=== Starting QEMU ==="
qemu-system-i386 \
    -cdrom "$ISO" \
    -boot d \
    -vga std \
    -device e1000,netdev=net0 \
    -netdev user,id=net0 \
    -serial file:"$SERIAL_LOG" \
    -display none \
    -monitor unix:"$MONITOR_SOCK",server,nowait \
    -no-reboot &
QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

# Wait for QEMU monitor to be ready
sleep 2

send_monitor() {
    echo "sendkey $1" | socat - UNIX-CONNECT:"$MONITOR_SOCK"
}

type_string() {
    local str="$1"
    for (( i=0; i<${#str}; i++ )); do
        local char="${str:$i:1}"
        case "$char" in
            ' ') send_monitor spc ;;
            '/') send_monitor slash ;;
            '.') send_monitor dot ;;
            ':') send_monitor shift-semicolon ;;
            '-') send_monitor minus ;;
            '\n') send_monitor ret ;;
            'A') send_monitor shift-a ;;
            'B') send_monitor shift-b ;;
            'C') send_monitor shift-c ;;
            'D') send_monitor shift-d ;;
            'E') send_monitor shift-e ;;
            'F') send_monitor shift-f ;;
            'G') send_monitor shift-g ;;
            'H') send_monitor shift-h ;;
            'I') send_monitor shift-i ;;
            'J') send_monitor shift-j ;;
            'K') send_monitor shift-k ;;
            'L') send_monitor shift-l ;;
            'M') send_monitor shift-m ;;
            'N') send_monitor shift-n ;;
            'O') send_monitor shift-o ;;
            'P') send_monitor shift-p ;;
            'Q') send_monitor shift-q ;;
            'R') send_monitor shift-r ;;
            'S') send_monitor shift-s ;;
            'T') send_monitor shift-t ;;
            'U') send_monitor shift-u ;;
            'V') send_monitor shift-v ;;
            'W') send_monitor shift-w ;;
            'X') send_monitor shift-x ;;
            'Y') send_monitor shift-y ;;
            'Z') send_monitor shift-z ;;
            *) send_monitor "$char" ;;
        esac
        sleep 0.1
    done
}

# Wait for kernel to boot (serial output should show init messages)
echo "=== Waiting for kernel boot (15s) ==="
sleep 15

echo "=== Boot log so far ==="
cat "$SERIAL_LOG" 2>/dev/null | tail -30

echo ""
echo "=== Typing 'resolve example.com' ==="
type_string "resolve example.com"
sleep 0.2
send_monitor ret

echo "=== Waiting 10s for DNS response ==="
sleep 10

echo "=== Serial log after resolve ==="
cat "$SERIAL_LOG" 2>/dev/null

echo ""
echo "=== Typing 'browse example.com /' ==="
type_string "browse example.com /"
sleep 0.2
send_monitor ret

echo "=== Waiting 15s for HTTP response ==="
sleep 15

echo "=== Full serial log ==="
cat "$SERIAL_LOG" 2>/dev/null

echo ""
echo "=== Killing QEMU ==="
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
echo "=== Done ==="

@echo off
rem Boot KAnarchy OS (desktop ISO) in Windows QEMU.
rem
rem Uses the Windows Hypervisor Platform (WHPX) for near-native CPU speed:
rem the 32-bit kernel boots fine in qemu-system-x86_64, which (unlike the
rem i386 build) has the whpx accelerator. If WHPX is unavailable, QEMU falls
rem back to software emulation (TCG) automatically. To enable WHPX: Windows
rem Features -> "Windows Hypervisor Platform", then reboot.
rem
rem Extra arguments are passed to QEMU (e.g. -serial stdio for the log).

setlocal
set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not exist "%QEMU%" (
    echo qemu-system-x86_64.exe not found at "%QEMU%"
    echo Install QEMU ^(choco install qemu^) or edit QEMU= in this file.
    exit /b 1
)
"%QEMU%" -accel whpx -accel tcg -m 512 -cdrom "%~dp0kanarchy-desktop.iso" -boot d -vga std ^
    -device e1000,netdev=net0 -netdev user,id=net0 %*

@echo off
rem Boot KAnarchy OS (desktop ISO) in Windows QEMU.
rem
rem Uses the Windows Hypervisor Platform (WHPX) for near-native CPU speed:
rem the 32-bit kernel boots fine in qemu-system-x86_64, which (unlike the
rem i386 build) has the whpx accelerator. If WHPX is unavailable, QEMU falls
rem back to software emulation (TCG) automatically. To enable WHPX: Windows
rem Features -> "Windows Hypervisor Platform", then reboot.
rem
rem Sound: an Intel HD Audio card wired to the Windows sound output (SDL; the DirectSound backend refuses to start without a microphone) (the kernel has its own driver for it).
rem CPUs: 4 (-smp 4): the kernel runs video decoding on a second CPU, which is what keeps 720p60 smooth; one CPU still works.
rem Memory: 1536 MB, because browsing real sites (YouTube) needs it; lower it with -m on the command line if you must.
rem
rem Extra arguments are passed to QEMU (e.g. -serial stdio for the log).

setlocal
set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not exist "%QEMU%" (
    echo qemu-system-x86_64.exe not found at "%QEMU%"
    echo Install QEMU ^(choco install qemu^) or edit QEMU= in this file.
    exit /b 1
)
"%QEMU%" -accel whpx -accel tcg -smp 4 -m 1536 -cdrom "%~dp0kanarchy-desktop.iso" -boot d -vga std ^
    -device e1000,netdev=net0 -netdev user,id=net0 ^
    -audiodev sdl,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 %*

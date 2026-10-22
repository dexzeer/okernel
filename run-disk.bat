@echo off
rem Boot the installed KAnarchy disk (kanarchy-disk.img from `make disk`) in Windows QEMU: GRUB from the disk's MBR,
rem then the desktop; after a crash (try `run /bin/panic` in a terminal) the panic screen reboots into the recovery
rem system. Layout: src/part.h. Same machine as run-windows.bat (WHPX, 4 CPUs, 1536 MB, AC'97 sound, e1000).
rem
rem The image is used in place: files you save, the logs and the crash records stay on it. `make disk` builds a fresh
rem one (and overwrites it). Booting it with UEFI firmware instead of the BIOS works too:
rem   run-disk.bat -bios "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
rem
rem Extra arguments are passed to QEMU (e.g. -serial stdio for the log).

setlocal
set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
set "DISK=%~dp0kanarchy-disk.img"
if not exist "%QEMU%" (
    echo qemu-system-x86_64.exe not found at "%QEMU%"
    echo Install QEMU ^(choco install qemu^) or edit QEMU= in this file.
    exit /b 1
)
if not exist "%DISK%" (
    echo kanarchy-disk.img not found: build it first with `make disk` ^(in WSL^).
    exit /b 1
)
"%QEMU%" -accel whpx -accel tcg -smp 4 -m 1536 -drive "file=%DISK%,format=raw,if=ide,index=0" -boot c -vga std ^
    -device e1000,netdev=net0 -netdev user,id=net0 ^
    -audiodev sdl,id=snd0,out.buffer-length=120000,in.voices=0,out.frequency=48000 -device AC97,audiodev=snd0 %*

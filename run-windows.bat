@echo off
rem Boot KAnarchy OS (desktop ISO) in Windows QEMU.
rem
rem Uses the Windows Hypervisor Platform (WHPX) for near-native CPU speed:
rem the 32-bit kernel boots fine in qemu-system-x86_64, which (unlike the
rem i386 build) has the whpx accelerator. If WHPX is unavailable, QEMU falls
rem back to software emulation (TCG) automatically. To enable WHPX: Windows
rem Features -> "Windows Hypervisor Platform", then reboot.
rem
rem Sound: an AC'97 card wired to the Windows sound output through SDL (the DirectSound backend refuses to start without a microphone, and SDL
rem does the same for AC'97 unless in.voices=0 says there is no input; QEMU then prints a few "Could not create a backend for voice" lines: harmless).
rem Why AC'97 and not Intel HD Audio: QEMU's HD Audio model feeds the card from a timer and re-syncs to the backend in 1-4 ms steps; on a Windows
rem host its position ran 1.0-1.24x too fast (varying from run to run) and whole 42 ms buffers were thrown away: the sound skipped, pitch/speed
rem were off and the video could not keep up with the clock. The AC'97 model only takes what the backend plays: 0.999x and no skips.
rem The kernel has drivers for both (HD Audio on real PCs).
rem CPUs: 4 (-smp 4): the kernel runs video decoding on a second CPU, which is what keeps 720p60 smooth; one CPU still works.
rem Sound buffer: 120 ms (out.buffer-length): QEMU's default is tiny, and any stall of its main loop then shows up as dropouts / a clock that runs ahead of what you hear.
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
    -audiodev sdl,id=snd0,out.buffer-length=120000,in.voices=0,out.frequency=48000 -device AC97,audiodev=snd0 %*

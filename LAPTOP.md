# Running KAnarchy on an old BIOS laptop

`make laptop` builds `kanarchy-laptop.iso` (about 24 MB): one image with a boot menu, made for a laptop with a
legacy BIOS, a PS/2 keyboard and touchpad, and a 1366x768 panel. Nothing here has run on real hardware yet;
the first goal is to find out what that machine is.

## Putting it on a USB stick (or a CD)

The ISO is a hybrid image, so it can be written straight to a USB stick. This erases the stick.

* **Windows:** Rufus, choose the ISO, and when it asks pick **"DD Image mode"** (not "ISO mode").
  Or balenaEtcher: choose the ISO and the stick.
* **Linux:** `sudo dd if=kanarchy-laptop.iso of=/dev/sdX bs=4M status=progress` (check X with `lsblk`).
* **CD/DVD:** burn it as an ordinary ISO image.

Boot from it: press the boot-menu key at power-on (usually F12, F9, Esc or F10 on an old laptop) and pick the USB
stick, or change the boot order in the BIOS setup. If the BIOS offers "UEFI" and "Legacy/CSM" modes, use Legacy.
A laptop that old may not be able to boot USB at all; then use the CD.

## The menu

1. **Hardware report (text mode) - START HERE.** Plain text screen that works on any PC. It prints the CPU and the
   graphics chip by itself. Photograph the screen. Then type these commands and photograph each result:
   `pci`, `memmap`, `acpi`, and `pcidump 0:2.0` (the Intel graphics chip, if `pci` lists it as 00:02.0).
   `all` prints everything (Page Up / Page Down scroll).
2. **Desktop 1366x768** - the real thing at the panel's size. Try this second.
3. **Desktop 1280x800**, 4. **1024x768**, 5. **let the video BIOS choose** - fallbacks if 2 shows nothing or a
   scrambled picture. If a mode is not usable the screen shows a one-line message and the machine stops;
   reboot and pick another entry.
6. **List the video modes this laptop offers** - GRUB prints every graphics mode the video BIOS has (with bits per
   pixel) and the panel's preferred mode (EDID). It pauses after each screen: press Space for the next. Photograph every
   screen: the kernel needs a 32 bits-per-pixel mode.

## What to send back

Photos of: entry 1 as it appears at boot, `pci`, `acpi`, `memmap`, `pcidump 0:2.0`, and the screen of entry 6; and
what entries 2-5 do (picture, black screen, message, reboot).

## Known limits on such a machine

* Keyboard and touchpad work only if the laptop's own are PS/2 (almost all old laptops).
* No Wi-Fi, no USB mouse/keyboard, no hard disk access, no sound codecs tested: the network driver is for an Intel
  e1000 card, which a laptop will not have. The browser will not reach the internet.
* The kernel needs no SSE2 and runs on a Pentium III or later; 256 MB of RAM or more.

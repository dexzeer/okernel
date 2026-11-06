#!/bin/bash
# Adds Ubuntu's prebuilt DOSBox and every library it needs to a test root, for tests/headless/test_lx_dosbox.py.
# The packages are only unpacked (their install scripts do not run) and nothing here is executed on the host: readelf
# reads the library lists. Test inputs only: nothing of this goes into the product or the repo.
#   sh tools/lx/real_progs.sh build-host/extra_real && bash tools/lx/dosbox_root.sh build-host/extra_real
#   HOSTBINS=1 sh tools/lx/build_tests.sh build-host/extra_real
set -eu
E=$(realpath "${1:-build-host/extra_real}")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/debs" "$W/root"
(cd "$W/debs" && apt-get download -q dosbox libsdl1.2debian libsdl-net1.2 libsdl-sound1.2 libsdl2-2.0-0 libmikmod3 \
    libvorbisfile3 libvorbis0a libflac12t64 libogg0 libspeex1 libgl1 libglvnd0 libglx0 libxext6 libopenal1 libsndio7.0 libx11-data \
    libasound2t64 libasound2-data alsa-utils)
for d in "$W"/debs/*.deb; do dpkg-deb -x "$d" "$W/root"; done
A=usr/lib/x86_64-linux-gnu
R=$W/root
mkdir -p "$E/bin" "$E/etc" "$E/$A/pulseaudio" "$E/usr/share/X11/locale"
cp -L "$R/usr/bin/dosbox" "$E/bin/dosbox"
cp -L "$R/usr/bin/aplay" "$E/bin/aplay"
mkdir -p "$E/usr/share/alsa" "$E/data"
cp -a "$R/usr/share/alsa/." "$E/usr/share/alsa/"
python3 -I - "$E/data/tone.wav" <<'P'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b"".join(struct.pack("<hh", v, v) for v in (int(12000 * math.sin(2 * math.pi * 1000 * i / 48000)) for i in range(48000))))
P
# Xlib refuses to set window titles without these, even in the C locale
cp -a "$R/usr/share/X11/locale/C" "$R/usr/share/X11/locale/locale.alias" "$R/usr/share/X11/locale/locale.dir" "$R/usr/share/X11/locale/compose.dir" "$E/usr/share/X11/locale/"

find_lib() { for r in "$R/$A" "$R/$A/pulseaudio" "/$A" "/$A/pulseaudio" /lib/x86_64-linux-gnu; do [ -e "$r/$1" ] && { echo "$r/$1"; return; }; done; return 0; }
seen=" "; todo="$R/usr/bin/dosbox $R/usr/bin/aplay $R/$A/libSDL2-2.0.so.0"
miss=""
while [ -n "$todo" ]; do
    set -- $todo; f=$1; shift; todo="$*"
    for n in $(readelf -d "$f" | grep NEEDED | sed 's/.*\[//;s/\]//'); do
        case "$seen" in *" $n "*) continue;; esac
        seen="$seen$n "
        p=$(find_lib "$n")
        if [ -z "$p" ]; then miss="$miss $n"; continue; fi
        todo="$todo $p"
        case "$n" in
            libpulsecommon*) cp -L "$p" "$E/$A/pulseaudio/";;
            ld-linux*|libc.so.6|libm.so.6) [ -e "$E/$A/$n" ] || cp -L "$p" "$E/$A/$n";;
            *) cp -L "$p" "$E/$A/$n";;
        esac
    done
done
cp -L "$R/$A/libSDL2-2.0.so.0" "$E/$A/"                          # SDL 1.2 opens it with dlopen, no NEEDED line points at it
[ -z "$miss" ] || { echo "missing libraries:$miss" >&2; exit 1; }

cat > "$E/bin/dosrun" <<'S'
export HOME=/tmp DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=alsa SDL_RENDER_DRIVER=software SDL12COMPAT_OPENGL_SCALING=0
exec /bin/dosbox -conf /etc/dosbox.conf "$@"
S
cat > "$E/bin/dosout" <<'S'
cat /tmp/[Oo][Uu][Tt].[Tt][Xx][Tt]
S
cat > "$E/bin/dosbench" <<'S'
cat /dos/[Bb][Ee][Nn][Cc][Hh].[Tt][Xx][Tt]
S
cat > "$E/bin/dosinput" <<'S'
cat /dos/[Ii][Nn][Pp][Uu][Tt].[Tt][Xx][Tt]
S
cat > "$E/bin/doomdemo" <<'S'
cat /dos/doom/[Dd][Ee][Mm][Oo].[Tt][Xx][Tt]
S
mkdir -p "$E/dos"
HERE=$(cd "$(dirname "$0")" && pwd)
nasm -f bin -o "$E/dos/vgabench.com" "$HERE/../../lx/test/dos/vgabench.asm"
nasm -f bin -o "$E/dos/inptest.com" "$HERE/../../lx/test/dos/inptest.asm"
nasm -f bin -o "$E/dos/beep.com" "$HERE/../../lx/test/dos/beep.asm"
# A real game, optional: Doom's freely distributed shareware episode, from a zip you put there yourself.
Z=${DOOM_ZIP:-$HERE/../../build-host/dosgame/dl/doom.zip}
if [ -f "$Z" ]; then
    mkdir -p "$E/dos/doom"
    (cd "$W" && unzip -q -o -j "$Z" DOOM.EXE DOOM1.WAD DEFAULT.CFG -d doomfiles)
    sed -i 's/^use_mouse[[:space:]]*0/use_mouse\t\t1/' "$W/doomfiles/DEFAULT.CFG"
    cp "$W"/doomfiles/* "$E/dos/doom/"
fi
cat > "$E/etc/dosbox.conf" <<'S'
[sdl]
fullscreen=false
output=surface
[render]
aspect=false
[cpu]
core=dynamic
cycles=max
[mixer]
rate=48000
blocksize=1024
prebuffer=60
[sblaster]
sbtype=sb16
oplrate=48000
[gus]
gus=false
[speaker]
pcspeaker=true
pcrate=48000
tandy=off
disney=false
S
du -sh "$E" | cut -f1

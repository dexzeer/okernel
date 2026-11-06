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
    libvorbisfile3 libvorbis0a libflac12t64 libogg0 libspeex1 libgl1 libglvnd0 libglx0 libxext6 libopenal1 libsndio7.0 libx11-data)
for d in "$W"/debs/*.deb; do dpkg-deb -x "$d" "$W/root"; done
A=usr/lib/x86_64-linux-gnu
R=$W/root
mkdir -p "$E/bin" "$E/etc" "$E/$A/pulseaudio" "$E/usr/share/X11/locale"
cp -L "$R/usr/bin/dosbox" "$E/bin/dosbox"
# Xlib refuses to set window titles without these, even in the C locale
cp -a "$R/usr/share/X11/locale/C" "$R/usr/share/X11/locale/locale.alias" "$R/usr/share/X11/locale/locale.dir" "$R/usr/share/X11/locale/compose.dir" "$E/usr/share/X11/locale/"

find_lib() { for r in "$R/$A" "$R/$A/pulseaudio" "/$A" "/$A/pulseaudio" /lib/x86_64-linux-gnu; do [ -e "$r/$1" ] && { echo "$r/$1"; return; }; done; return 0; }
seen=" "; todo="$R/usr/bin/dosbox $R/$A/libSDL2-2.0.so.0"
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
export HOME=/tmp DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software SDL12COMPAT_OPENGL_SCALING=0
exec /bin/dosbox -conf /etc/dosbox.conf "$@"
S
cat > "$E/bin/dosout" <<'S'
cat /tmp/[Oo][Uu][Tt].[Tt][Xx][Tt]
S
cat > "$E/etc/dosbox.conf" <<'S'
[sdl]
fullscreen=false
output=surface
autolock=false
[render]
aspect=false
scaler=none
[cpu]
core=dynamic
cycles=fixed 3000
[mixer]
nosound=true
[sblaster]
sbtype=none
[gus]
gus=false
[speaker]
pcspeaker=false
tandy=off
disney=false
S
du -sh "$E" | cut -f1

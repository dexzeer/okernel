#!/bin/sh
# Fills a directory with real programs of this machine, for tests/headless/test_lx_real.py: Debian/Ubuntu
# binaries with their libraries, and Microsoft's where.exe, winver.exe and notepad.exe when run where
# /mnt/c/Windows exists. Test inputs only: nothing of this goes into the product or the repo.
#   sh tools/lx/real_progs.sh build-host/extra_real && HOSTBINS=1 sh tools/lx/build_tests.sh build-host/extra_real
#   python3 tests/headless/test_lx_real.py
set -e
E=${1:-build-host/extra_real}
rm -rf "$E"
mkdir -p "$E/bin" "$E/win/en-US" "$E/data" "$E/tmp"
printf 'apple pie\nbanana split\ncherry tart\napricot jam\nblueberry\n' > "$E/data/words.txt"
for b in grep sed awk find tar gzip nano vim less make curl ssh git python3; do
    p=$(command -v $b) || continue
    cp -L "$p" "$E/bin/$b"
    for l in $(ldd "$p" 2>/dev/null | grep -o "/[^ ]*\.so[^ ]*"); do mkdir -p "$E$(dirname $l)"; cp -L "$l" "$E$l"; done
done
P=$(python3 -c 'import sys; print(sys.prefix + "/lib/python%d.%d" % sys.version_info[:2])')
mkdir -p "$E$P"
(cd "$P" && cp -a ./*.py encodings collections importlib json re "$OLDPWD/$E$P/" 2>/dev/null) || true
rm -rf "$E$P/__pycache__"
for d in /lib/terminfo /usr/share/terminfo; do
    if [ -e "$d/x/xterm" ]; then mkdir -p "$E$d/x"; cp -L "$d/x/xterm" "$E$d/x/"; fi
done
W=/mnt/c/Windows/System32
if [ -d "$W" ]; then
    for x in where winver notepad; do
        cp "$W/$x.exe" "$E/win/"
        cp "$W/en-US/$x.exe.mui" "$E/win/en-US/" 2>/dev/null || true
    done
fi
du -sh "$E" | cut -f1

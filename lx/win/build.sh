#!/bin/sh
# Build the Windows side: our system DLLs and the test programs, into build-host/winroot (a directory tree that
# tools/lx/build_tests.sh adds to the Linux root).
#   sh lx/win/build.sh
set -e
cd "$(dirname "$0")/../.."
PEB="sh tools/pe/pebuild.sh"
OUT=build-host/winroot
SYS=$OUT/windows/system32
mkdir -p $SYS $OUT/win
T=build-host/winobj; rm -rf $T; mkdir -p $T

# exported names: every "EXPORT type name(" line, plus the C library functions kernel32 also answers for (as ntdll)
exports() { grep -h '^EXPORT ' "$@" | sed -E 's/^EXPORT [^(]*[ *]([A-Za-z_0-9]+)\(.*/\1/' | grep -E '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u; }

exports lx/win/kernel32/*.c > $T/k32.exp
printf '%s\n' memcpy memmove memset memcmp strlen >> $T/k32.exp
$PEB $SYS/kernel32.dll dll 0x6C000000 - $T/k32.exp lx/win/kernel32/*.c

if [ -d lx/win/msvcrt ]; then
    exports lx/win/msvcrt/*.c > $T/crt.exp
    [ -f lx/win/msvcrt/extra.exp ] && cat lx/win/msvcrt/extra.exp >> $T/crt.exp
    sort -u $T/crt.exp -o $T/crt.exp
    $PEB $SYS/msvcrt.dll dll 0x6D000000 lx/win/msvcrt/imports.txt $T/crt.exp lx/win/msvcrt/*.c
fi

# the import list of the test programs: everything our two DLLs export (memcpy & co. come from msvcrt)
grep -vx -e memcpy -e memmove -e memset -e memcmp -e strlen $T/k32.exp > $T/k32_imp.exp
for d in lx/win/tests/*/; do
    n=$(basename $d)
    kind=exe; [ -f $d/gui ] && kind=gui
    $PEB $OUT/win/$n.exe $kind 0x00400000 $d/imports.txt - $d/*.c
done
echo built $OUT

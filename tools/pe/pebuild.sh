#!/bin/sh
# Build one Windows binary from C sources: gcc (Microsoft x64 calling convention) -> ld -> mkpe.
#   pebuild.sh OUT.{exe,dll} KIND BASE IMPORTS EXPORTS SRC...
#     KIND     exe | gui | dll
#     BASE     image base, below 3 GB (0x10000000 ...); our DLLs get distinct fixed bases
#     IMPORTS  file "DLL: func func ..." or -
#     EXPORTS  file with one export name per line, or -  (for a dll)
set -e
OUT=$1; KIND=$2; BASE=$3; IMPORTS=$4; EXPORTS=$5; shift 5
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
OBJS=""
for f in "$@"; do
    o=$T/$(basename $f).o
    gcc -c -O2 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mabi=ms -mno-red-zone -fasynchronous-unwind-tables -fno-shrink-wrap \
        -fno-ident -fno-builtin -Wall -Wno-unused-function -I"$HERE" -I"$(dirname $f)" -Ilx/win -Ilx/win/msvcrt -o $o $f
    OBJS="$OBJS $o"
done
EXTRA=""
if [ "$IMPORTS" != "-" ]; then
    python3 $HERE/mkpe.py gen-imports $IMPORTS $T/imports.S
    gcc -c -o $T/imports.o $T/imports.S
    OBJS="$OBJS $T/imports.o"
    EXTRA="--imports $IMPORTS"
fi
ENTRY=""
case $KIND in
    exe|gui) ENTRY="-e entry"; FLAGS="" ; [ "$KIND" = gui ] && FLAGS="--gui";;
    dll) ENTRY="-e DllMain"; FLAGS="--dll";;
esac
ld -m elf_x86_64 -static -q -T $HERE/pe.ld --defsym=__base=$BASE $ENTRY -o $T/out.elf $OBJS
EXP=""
if [ "$EXPORTS" != "-" ]; then EXP="--export $(tr '\n' ',' < $EXPORTS | sed 's/,$//')"; fi
python3 $HERE/mkpe.py $T/out.elf $OUT $FLAGS --base $BASE $EXTRA $EXP --name $(basename $OUT)
rm -rf $T

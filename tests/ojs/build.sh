#!/bin/sh
# Host build of the ojs engine + the test runner (32-bit x87, like the kernel).
# usage: tests/ojs/build.sh [extra cflags]   -> build-host/ojs_run
# Objects are rebuilt when their source or anything they include changed
# (gcc -MMD dependency files); FORCE=1 rebuilds everything.
set -e
cd "$(dirname "$0")/../.."
OUT=build-host/ojs
mkdir -p $OUT
CFLAGS="-m32 -fno-pie -fno-pic -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fno-strict-aliasing $*"
SRCS="gc str obj util conv num unicode unicode_data lex parse compile ops vm realm b_object b_function b_error b_iter b_array b_string b_number b_global b_promise generator b_map b_proxy b_typed bigint b_json b_date re b_regexp module api srctext"

stale() {   # stale <obj> <dep file>
    [ -n "$FORCE" ] && return 0
    [ -f "$1" ] && [ -f "$2" ] || return 0
    for d in $(sed -e 's/^[^:]*://' -e 's/\\$//' "$2"); do
        [ -f "$d" ] || continue
        [ "$d" -nt "$1" ] && return 0
    done
    return 1
}

OBJS=""
for f in $SRCS; do
    if stale $OUT/$f.o $OUT/$f.d; then
        # the interpreter: let GCC copy the dispatch jump into every instruction
        X=""; [ $f = vm ] && X="--param max-goto-duplication-insns=64"
        gcc $CFLAGS $X -MMD -MF $OUT/$f.d -c src/ojs/$f.c -o $OUT/$f.o
    fi
    OBJS="$OBJS $OUT/$f.o"
done
if stale $OUT/kmath.o $OUT/kmath.d; then gcc $CFLAGS -DKMATH_NO_STD_NAMES -MMD -MF $OUT/kmath.d -c src/kmath.c -o $OUT/kmath.o; fi
if stale $OUT/run.o $OUT/run.d; then gcc $CFLAGS -MMD -MF $OUT/run.d -c tests/ojs/run.c -o $OUT/run.o; fi
gcc $CFLAGS -no-pie $OBJS $OUT/kmath.o $OUT/run.o -o build-host/ojs_run -lm
echo built build-host/ojs_run

#!/bin/sh
# dmccl host tests: ASan + UBSan, 32-bit like the kernel.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-host
S="src/dmccl/dm_lex.c src/dmccl/dm_compile.c src/dmccl/dm_vm.c src/dmccl/dm_lib.c"
gcc -m32 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_dmccl tests/dmccl/test_dmccl.c $S -lm
ASAN_OPTIONS=detect_leaks=1 ./build-host/t_dmccl
gcc -m32 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build-host/dmccl tools/dmccl/cli.c $S -lm

#!/bin/sh
# game engine host tests (physics, ...): ASan + UBSan, 32-bit x87 like the kernel.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-host
gcc -m32 -O1 -g -mfpmath=387 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_phys tests/game/test_phys.c src/game/phys.c -lm
ASAN_OPTIONS=detect_leaks=1 ./build-host/t_phys

#!/bin/sh
# game engine host tests: physics, game behaviour, and "every shipped level can be won" (a random-key bot finds a way).
# ASan + UBSan, 32-bit x87 like the kernel.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-host
FLAGS="-m32 -O1 -g -mfpmath=387 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined"
LANGSRC="src/dmccl/dm_lex.c src/dmccl/dm_compile.c src/dmccl/dm_vm.c src/dmccl/dm_lib.c"
GAMESRC="src/game/phys.c src/game/game.c src/game/game_chars.c src/game/game_gfx.c"
gcc $FLAGS -o build-host/t_phys tests/game/test_phys.c src/game/phys.c -lm
ASAN_OPTIONS=detect_leaks=1 ./build-host/t_phys
gcc $FLAGS -o build-host/t_game tests/game/test_game.c $LANGSRC $GAMESRC -lm
ASAN_OPTIONS=detect_leaks=1 ./build-host/t_game
# solvability: optimized build (the bot plays minutes of game time). Seeds/tries are fixed, so this is deterministic.
gcc -m32 -O2 -mfpmath=387 -o build-host/gamebot tools/game/bot.c $LANGSRC $GAMESRC -lm
for spec in "1 5 3" "2 600 5" "3 50 5" "4 400 3"; do
    set -- $spec
    out=$(./build-host/gamebot games/level$1.dmccl $2 $3 | head -1)
    echo "level $1: $out"
    case "$out" in WON*) ;; *) echo "FAIL level $1 could not be won"; exit 1;; esac
done

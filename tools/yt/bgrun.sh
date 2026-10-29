#!/bin/bash
# Assemble and run the BotGuard differential rig: shim + VM + program + driver
# as one script, under Node (V8) and under ojs_run — outputs diffed.
# usage: sh tools/yt/bgrun.sh
set -e
cd "$(dirname "$0")/../.."

# 1. the program + global name from a fresh watch page
node tools/yt/bgprogram.mjs > build-host/bginv/program.txt

PROGRAM=$(python3 -c "print(open('build-host/bginv/program.txt').read().splitlines()[0])")
GLOBALNAME=$(python3 -c "print(open('build-host/bginv/program.txt').read().splitlines()[1])")
BINDING=$(python3 -c "print(open('build-host/bginv/program.txt').read().splitlines()[2])")
echo "program: ${#PROGRAM} chars, globalName=$GLOBALNAME, binding=${#BINDING} chars"

# 2. assemble: shim + program literals + VM source + driver
{
  cat build-host/bginv/bgshim.js
  echo "var __PROGRAM_INLINE = \"$(echo "$PROGRAM" | sed 's/"/\\"/g')\";"
  echo "var __GLOBALNAME_INLINE = \"$GLOBALNAME\";"
  echo "var __BINDING_INLINE = \"$(echo "$BINDING" | sed 's/"/\\"/g')\";"
  cat build-host/bginv/bg_vm.js
  cat build-host/bginv/bgdriver.js
} > build-host/bginv/bgrun_combined.js
echo "combined: $(wc -c < build-host/bginv/bgrun_combined.js) bytes"

# 3. run under both engines
node build-host/bginv/bgrun_combined.js > build-host/bginv/bgrun_node.txt 2>&1 || echo "node rc=$?"
./build-host/ojs_run build-host/bginv/bgrun_combined.js > build-host/bginv/bgrun_ojs.txt 2>&1 || echo "ojs rc=$?"

# 4. diff
echo "=== node lines: $(grep -c '^BG ' build-host/bginv/bgrun_node.txt) / ojs lines: $(grep -c '^BG ' build-host/bginv/bgrun_ojs.txt)"
diff <(grep '^BG ' build-host/bginv/bgrun_node.txt | sed 's/[0-9]\+\.[0-9]\+/N/g') \
     <(grep '^BG ' build-host/bginv/bgrun_ojs.txt | sed 's/[0-9]\+\.[0-9]\+/N/g') | head -60
echo "=== node snapshot/signal lines:"
grep "SNAPSHOT\|SIG \|vmSetupCallback\|VM MISSING\|THREW" build-host/bginv/bgrun_node.txt | head -12
echo "=== ojs snapshot/signal lines:"
grep "SNAPSHOT\|SIG \|vmSetupCallback\|VM MISSING\|THREW" build-host/bginv/bgrun_ojs.txt | head -12

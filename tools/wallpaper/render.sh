#!/usr/bin/env bash
# Render the KAnarchy wallpaper (kanarchy_wallpaper.html, a WebGL raymarcher)
# to wallpaper.jpg, which the kernel embeds (src/wallpaper.asm) and decodes
# at boot with the web engine's JPEG decoder.
#
# Needs Edge or Chrome (headless, SwiftShader WebGL) and python3 + Pillow.
# Run from Git Bash on Windows or any shell with a Chromium on PATH:
#     tools/wallpaper/render.sh            # 1920x1080, 2x2 supersampled
set -euo pipefail
cd "$(dirname "$0")"
W=${W:-1920}; H=${H:-1080}; SS=${SS:-2}
BROWSER=${BROWSER:-}
for c in "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" \
         "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "$(command -v chromium || true)" "$(command -v google-chrome || true)"; do
  [ -z "$BROWSER" ] && [ -n "$c" ] && [ -x "$c" ] && BROWSER="$c"
done
[ -n "$BROWSER" ] || { echo "no Edge/Chrome found (set BROWSER=)"; exit 1; }
here="$PWD"
if command -v cygpath >/dev/null; then here="$(cygpath -w "$PWD")"; fi
url="file:///$(echo "$here" | tr '\\' '/')/kanarchy_wallpaper.html?w=$W&h=$H&ss=$SS"
"$BROWSER" --headless=new --use-angle=swiftshader --enable-unsafe-swiftshader \
  --hide-scrollbars --window-size=$W,$H --virtual-time-budget=900000 \
  --screenshot="$here/wallpaper.png" "$url" 2>&1 | grep -v task_manager || true
CONVERT='from PIL import Image
im = Image.open("wallpaper.png").convert("RGB")
im.save("wallpaper.jpg", quality=90, optimize=True, subsampling=0)
print("wallpaper.jpg", im.size)'
PY=""
for p in python3 python; do
  if command -v $p >/dev/null && $p -c "import PIL" 2>/dev/null; then PY=$p; break; fi
done
if [ -n "$PY" ]; then $PY -c "$CONVERT"
elif command -v wsl >/dev/null; then MSYS_NO_PATHCONV=1 wsl -e python3 -c "$CONVERT"   # Pillow in WSL
else echo "need python3 + Pillow"; exit 1; fi
rm -f wallpaper.png

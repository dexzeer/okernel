#!/bin/sh
# Subset the CJK fallback face (Noto Sans SC + KR, TrueType glyf outlines).
# The source is Google Fonts' variable Noto Sans SC / KR
# (https://github.com/google/fonts/tree/main/ofl/notosanssc, OFL); pass the
# downloaded TTFs as arguments:
#   sh tools/subset_cjk.sh NotoSansSC[wght].ttf NotoSansKR[wght].ttf
# Output: fonts/NotoSansCJK-Regular.ttf (embedded in src/web/fontdata.asm).
set -e
cd "$(dirname "$0")/.."
SC="$1"
KR="$2"
[ -f "$SC" ] || { echo "usage: sh tools/subset_cjk.sh <NotoSansSC[wght].ttf> [NotoSansKR[wght].ttf]" >&2; exit 1; }
U=$(python3 tools/gen_cjk_codes.py)
SUB="python3 -m fontTools.subset"
DROP="--drop-tables+=GSUB,GPOS,GDEF,STAT,DSIG,gasp,prep,fpgm,cvt,gvar,HVAR,avar,fvar,vhea,vmtx,BASE"
$SUB "$SC" --unicodes="$U" --no-hinting --layout-features="" $DROP \
  --output-file=build-host/cjk-sc.ttf
if [ -n "$KR" ] && [ -f "$KR" ]; then
  $SUB "$KR" --unicodes="U+AC00-D7A3,U+1100-11FF,U+3130-318F" --no-hinting --layout-features="" $DROP \
    --output-file=build-host/cjk-kr.ttf
  python3 - <<'EOF'
# merge: the KR subset contributes the hangul glyphs the SC subset lacks
from fontTools.merge import Merger, Options
mega = Merger(Options()).merge(["build-host/cjk-sc.ttf", "build-host/cjk-kr.ttf"])
mega.save("fonts/NotoSansCJK-Regular.ttf")
EOF
else
  cp build-host/cjk-sc.ttf fonts/NotoSansCJK-Regular.ttf
fi
ls -la fonts/NotoSansCJK-Regular.ttf

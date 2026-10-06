set -e
cd "$(dirname "$0")/.."
mkdir -p fonts
SUB="python3 -m fontTools.subset"
U="U+0000-02FF,U+0300-036F,U+0370-03FF,U+0400-052F,U+1E00-1EFF,U+2000-206F,U+2070-209F,U+20A0-20CF,U+2100-214F,U+2150-218F,U+2190-21FF,U+2200-22FF,U+2300-23FF,U+2460-24FF,U+2500-257F,U+25A0-25FF,U+2600-26FF,U+2700-27BF,U+FB00-FB06,U+FFFD"
for f in NotoSans-Regular NotoSans-Bold NotoSans-Italic NotoSans-BoldItalic NotoSerif-Regular NotoSerif-Bold NotoSerif-Italic NotoSansMono-Regular NotoSansMono-Bold; do
  $SUB /usr/share/fonts/truetype/noto/$f.ttf --unicodes="$U" --no-hinting --layout-features="" \
    --drop-tables+=GSUB,GPOS,GDEF,STAT,DSIG,gasp,prep,fpgm,cvt --output-file=fonts/$f.ttf
done
$SUB /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf --unicodes="U+0530-058F,U+10A0-10FF,U+2000-2BFF,U+FFFD" \
  --no-hinting --layout-features="" --drop-tables+=GSUB,GPOS,GDEF,gasp,prep,fpgm,cvt,kern --output-file=fonts/DejaVuSans-Symbols.ttf

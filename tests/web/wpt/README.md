# WPT CSS reftests for the okernel web engine

Runs the [Web Platform Tests](https://web-platform-tests.org) CSS reftest
corpus against the host build of the engine (`build-host/wbrowse`), the same
sources that ship in okai/okernel. A reftest passes when our rendering of the
test page and our rendering of its reference page are pixel-identical (or
deliberately different, for `rel=mismatch`), within the tolerance a test may
declare via `<meta name=fuzzy>`.

## One-time setup

The WPT tree is big; sparse-clone it into WSL (not in the repo):

```bash
git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/web-platform-tests/wpt.git ~/wpt
cd ~/wpt && git sparse-checkout set css fonts tools
```

Build the host browser: `sh tests/web/build.sh` (repo root, WSL). `WB_OUT=build-host/wbrowse2 sh tests/web/build.sh`
builds to another file, so a run that is in progress keeps its binary.

## Running

```bash
# count what an area would run
python3 tests/web/wpt/run_wpt.py --list css-flexbox

# run areas (800x600 viewport, wbrowse, threaded local server)
python3 tests/web/wpt/run_wpt.py -j8 CSS2 css-box css-display selectors

# the 38 areas of the baseline table below
python3 tests/web/wpt/run_wpt.py -j8 -o build-host/wpt_full CSS2 css-align css-backgrounds css-borders css-box css-break \
    css-cascade css-color css-conditional css-contain css-counter-styles css-display css-flexbox css-fonts css-grid \
    css-image-animation css-images css-inline css-lists css-masking css-multicol css-overflow css-page css-position \
    css-pseudo css-ruby css-scrollbars css-sizing css-syntax css-tables css-text css-text-decor css-ui css-values \
    css-variables css-view-transitions css-writing-modes selectors

# results land in build-host/wpt_out/:
#   summary.txt   per-area pass/fail/error/skip
#   results.json  every test + reason
#   fails.txt     one line per failure
python3 tests/web/wpt/run_wpt.py -o build-host/wpt_out_big <areas>
```

An area can be a sub-directory (`css-text/white-space`) or one test file. Environment: `WBROWSE=path` picks the binary,
`WPT_PORT=n` the local server port (two runs at the same time need two ports), `WPT_XHT=1` also renders references
written as `.xht` / `.xhtml` / `.htm` (see below), `WPT_XHT=2` also discovers tests written that way (CSS2 alone has about
6000). `-j8` is fine on 12 cores; much more makes renders miss their 2.5 s budget.

`error` = the engine crashed or a page failed to render/compare (runner
problem; a failed render is tried three times); `fail` = rendered but differs from the reference
(engine bug or unimplemented feature); `skip` = non-HTML reference (without `WPT_XHT`: the 952 tests whose reference is
an `.xht` file, a handful of svg/png refs).

## Eyeballing a failure

```bash
python3 tests/web/wpt/wpt_diff.py <test/path/rel.html>   # test | ref | diff -> build-host/diff.png, prints the differing bbox
python3 tests/web/wpt/compare_runs.py <old_out> <new_out> [prefix [n]]   # gains and losses between two runs
python3 tests/web/wpt/by_dir.py <out> [prefix]           # failures per test-name family, biggest first
WB_DUMP=1 build-host/wbrowse <url> 800 600 out.ppm 2.5   # prints every text fragment of the layout (FR x y w "text")
```

## Baselines

| date       | areas                                                  | result                          |
|------------|--------------------------------------------------------|---------------------------------|
| 2026-10-29 | CSS2, css-box, css-display, selectors (648 rendered)   | 303 pass (46.8%)                |
| 2026-10-29 | 16 areas, clean wide pass (5646 rendered, 957 skipped) | 2050 pass (36.3%)               |
| 2026-10-30 | 16 areas after named globals / BFC / margin-trim /     | 2207 pass (39.0%)               |
|            | display:contents fixes                                 |                                 |
| 2026-10-30 | 16 areas, HTML refs only (5666 rendered)               | 3450 pass (60.9%)      |
| 2026-10-30 | same, `WPT_XHT=1` (6618 rendered: + the 952 tests with | 4000 pass (60.4%)        |
|            | an .xht reference)                                     |                                 |
| 2026-10-31 | 44 areas (the 38 + css-transforms, mediaqueries,       | 7445 pass (48.3%)               |
|            | printing, css-transitions, cssom, cssom-view;          |                                 |
|            | 15415 rendered, `WPT_XHT=1`)                           |                                 |
| 2026-11-01 | same 44 areas, after bidi + predefined counter styles  | **7540 pass (48.9%)**           |

The first three rows were measured before backgrounds loaded their images (see below), so some of their passes were two
equally broken pages that happened to match.

Per-area (2026-11-01, `WPT_XHT=1`): CSS2 60.3%, css-align 38.7%, css-backgrounds 67.8%, css-borders 29.4%, css-box 87.5%,
css-break 12.8%, css-cascade 38.8%, css-color 77.8%, css-conditional 45.7%, css-contain 56.9%, css-counter-styles 40.2%,
css-display 67.1%, css-flexbox 64.7%, css-fonts 35.1%, css-grid 27.9%, css-image-animation 100%, css-images 59.5%,
css-inline 19.9%, css-lists 70.1%, css-masking 31.8%, css-multicol 31.2%, css-overflow 51.4%, css-page 44.5%,
css-position 47.8%, css-pseudo 41.8%, css-ruby 26.2%, css-scrollbars 100%, css-sizing 58.2%, css-syntax 100% (1 test),
css-tables 48.5%, css-text 62.0%, css-text-decor 44.0%, css-transforms 42.0%, css-ui 92.9%, css-values 37.4%,
css-variables 53.0%, css-view-transitions 21.1%, css-writing-modes 35.0%, mediaqueries 49.1%, printing 71.8%,
css-transitions 100%, cssom 41.7%, cssom-view 54.5%, selectors 77.2%.

Areas probed and left out (net-negative or feature-less): css-logical (0), css-scroll-snap (1), css-anchor-position 7.4%,
css-gaps 5.8%, css-shapes 3.9%, css-shadow 14%, css-will-change 14%, css-nesting 17.6%, css-forms 20%, css-viewport 13.9%,
css-content 12.5%, css-highlight-api 12.6%, motion 8.2%, compositing 8.5%, css-layout-api 4.4%, css-paint-api 1.7%.

## What changed on 2026-11-01 (bidi + counter styles)

- **Bidi (UAX #9)**: `src/web/bidi.c` — X/W/N/I rules over an isolated-run split, L2 reordering and L4 mirroring, with the
  per-codepoint class table generated from Python's unicodedata (`bdata.h`). The inline formatter engages it only when the
  paragraph or some content needs it (rtl base, formatting characters, `unicode-bidi` elements): pure-ltr lines keep the
  plain path. `direction: rtl` lines are now laid out visually (frag order + rtl runs reversed with mirrored brackets)
  instead of the old whole-line mirror. `unicode-bidi: embed/isolate/bidi-override/isolate-override/plaintext` parse and
  inject synthetic controls; `dir` attributes map to direction+isolate in the UA sheet. css-writing-modes 24.4% -> 35.0%.
  Known limits: per-line (not per-paragraph) embedding state across lines, span backgrounds keep approximate positions,
  ruby annotations are skipped on bidi lines.
- **Predefined counter styles** (css-counter-styles-3): `src/web/cstyles.c` + a generated table (`cstyles_data.h`, from
  Firefox's counterstyles.css plus hand-coded hebrew/ethiopic/CJK specials) — 50 named styles for `list-style-type` and
  `counter(name, style)` via the numeric/alphabetic/symbolic/additive/fixed systems and the hebrew/ethiopic/CJK
  algorithms. css-counter-styles 22.5% -> 40.2%, css-lists 68.4% -> 70.1%.

## What changed on 2026-10-31 (fourth session: scale, more areas)

- Six more areas adopted after probing: css-transforms (41.7% — the `transform: translate*()` functions already worked),
  printing (71.8%), css-transitions, cssom, cssom-view and mediaqueries. Suites probed and left out: css-viewport 13.9%,
  css-content 12.5%, css-highlight-api 12.6%, motion 8.2%, compositing 8.5%, css-layout-api 4.4%, css-paint-api 1.7%.
- `transform: scale()` / `scaleX/Y` / `scale3d` (z = 1 only) / pure-scale `matrix` and the standalone `scale` property
  now render: a paint-time transform (origin from `transform-origin`) scales every display item's geometry and the text
  runs (font size, letter/word spacing) — layout is untouched, as transforms require. Percentage scale arguments are
  supported. Rotation/skew/3d remain unsupported (those ops are skipped).

## What changed on 2026-10-31 (later: ruby, emphasis, containment)

- `contain: paint` and `contain: layout`: paint containment clips descendants to the padding box (also positioned ones,
  which it now contains) and both establish an independent formatting context; `contain: content/strict` inherit both.
  css-contain 52.1% -> 56.9%.
- Ruby: `display: ruby` / `ruby-text` are real now — `<rt>` text becomes a zero-width ruby annotation centered over the
  base run it follows, extending the line box above (or below, `ruby-position: under`; left/right in vertical modes) by
  the annotation font's line. `text-emphasis-style` / `-position` / shorthand parse to a mark codepoint that paints per
  glyph at half the font size over (or beside, in vertical writing) the text, with the same line-box extent the rt gets —
  so the emphasis tests match their ruby-based refs pixel for pixel, horizontal and vertical. css-text-decor 29.2% ->
  44.0%.

## What changed on 2026-10-31 (third session)

The corpus grew from 16 to 31 areas (every area whose probe pass rate was net-positive was adopted; css-logical and
css-scroll-snap were probed and left out at 0/1 passes). Engine fixes this session:

- `:lang()` rewritten: RFC 4647 extended filtering (wildcard ranges, subtag skipping with the singleton rule),
  comma-separated range lists, quoted-string arguments, CSS escape processing (`:lang(\*-FR)`), and range validation
  (an invalid range invalidates the selector). Selectors area 72.3% -> 77.2%, all 25 lang tests pass.
- Flex column containers: the automatic minimum size (content size suggestion) is now measured even when flex-basis is a
  definite length (the item used to collapse to 0: `flex-basis: 0` + `min-height: auto` + tall content).
- aspect-ratio: min/max block sizes transfer through the ratio to the inline size (`max-height` clamps a
  `width: max-content`); a grid item's transferred size beats align/justify stretch in both axes.
- Gradient stop lengths are clamped to +/- 2^30 LU at parse time (a `calc()` product like 131070000px used to overflow
  the int32 stop encoding).
- Legacy `-webkit-line-clamp`: lines inside BFC descendants are skipped — they render in full and do not count toward N
  (`L->clamp_skip`; verified +6 / 0 losses on the area before the final run).

## What changed on 2026-10-30 (second session)

Counters (`counter-reset/increment/set`, `counter()` in `content`, list markers), CJK/UAX 14 line breaking, `ch`/`ex`/`lh`
units measured from the element's font, `text-transform` (case tables in `wcase.h`), `line-break`, `hyphens`, `text-align-last`,
`tab-size`, `text-overflow` (also strings), `line-clamp` / `-webkit-line-clamp` (incl. a custom block ellipsis string),
`overflow-clip-margin`, `object-position`, `contain: size`, CSS color functions (`lab`, `oklab`, `hwb`, `color()`),
background images/tiling/`background-origin`/`-clip` and SVG backgrounds, `aspect-ratio` (width from a definite height, flex items,
abspos, intrinsic sizes, min-content minimum), `fit-content(<length-percentage>)` and `min-width`/`max-width` with
`min-content`/`max-content`/`fit-content`, a canvas's `width`/`height` attributes are no longer CSS sizes, percentage heights of
replaced elements look through auto-height ancestors, soft wrap opportunities next to atomic inlines (GL/WJ/ZWJ/CM neighbours),
ideographic spaces hang at the line end in every `white-space` mode, `word-space-transform: space | ideographic-space`,
flex auto minimum size from the content, flex stretch of form controls, offsetWidth of inline elements, inline abspos containing
blocks. Harness: `wbrowse` lays out in its wait loop (CSS background images load), script `focus()`, per-process fetch cache key
(parallel runs no longer race), render retries, `WPT_XHT`, `WPT_PORT`, `WBROWSE`, `wpt_diff.py` / `compare_runs.py` / `by_dir.py`.

Writing modes (`writing-mode`, `text-orientation`; logical properties `inline-size`, `margin-inline-start`, ... resolve against the
element's own mode and direction): every box in a vertical mode is laid out by the horizontal code in its own logical frame, against
a copy of its style with width/height and the four-sided properties swapped (`wm_style` in `lay_main.c`); where a box's frame differs
from its parent's, the finished subtree is transformed into the parent's frame (`wm_xform`). `lbox.pst` is the physical style (paint,
finalize), `st` what the parent's layout sees, `st_in` the box's own. Vertical text is painted upright, one glyph per cell (sideways
Latin is not rotated). `direction: rtl`: flex rows, line mirroring in inline formatting contexts (each text fragment keeps its order),
static positions of abspos boxes. `border-image` (source url, slice, width, outset, repeat; not gradients as source) paints nine pieces
as clipped display items (`emit_border_image` in `lay_dl.c`).

Later the same day: element scrolling (`scrollTop` / `scrollLeft` / `scrollTo` / `scrollBy` on scroll containers: offsets live in
`wdom.scr`, `finalize` shifts what a scroll container holds, `wlay_scroll_info` gives the scrollable overflow; `position: sticky` is
resolved there against the nearest scroll container; `background-attachment: local`), WOFF 1.0 web fonts (`woff_to_sfnt` in `font.c`;
WOFF2 / Brotli is not done), `<object data>` / `<embed src>` images, the root background of an SVG image, no-dictionary fallback
breaking for Thai / Lao / Khmer / Myanmar / Javanese / Balinese, `word-break: manual`, `text-overflow` in text fields, Turkic
`text-transform`, the intrinsic width of inline content no longer counts trailing collapsible spaces, percentage heights of replaced
elements resolve through auto-height ancestors, stretched flex items and percentage / aspect-ratio heights, abspos children of a flex
container are aligned like a lone flex item. `text-wrap: balance` is implemented (`lay_ifc` re-lays each paragraph with a narrower
measure) but off (`lay_balance_enabled = 0` in `lay_inline.c`): it lost more WPT tests than it won.
Known gaps worth a look next (biggest reftest counts in brackets): `display: grid-lanes` (css-grid's largest test families:
subgrid 254, track-sizing 160, alignment 138 — a separate display type, not our grid), subgrid (~40 classic-grid tests),
ruby layout + text-emphasis (~200 combined across css-ruby/css-text-decor; the emphasis refs are ruby-based, so ruby has to
land first), predefined @counter-style tables (214), text-decoration skip-ink/inset (~40), WOFF2/Brotli (blocks ic-unit 13,
ch-unit 13 and many css-fonts), bidi reordering + bracket mirroring (blocks pre-wrap-align-center), textarea pre-wrap
alignment, `::selection`/custom highlights (~25), multicol, 2D transforms, real `::first-letter`/`::first-line` styling,
line-clamp with floats / abspos (the BFC-counting case is done), `white-space: break-spaces` details, tab stops after
atomic inlines.

## Notes

- WPT assumes an 800x600 viewport with overlay scrollbars; our engine does
  not reserve scrollbar space, so that matches.
- Ahem (the block font most reftests rely on) loads through our real
  `@font-face` implementation (`font.c` webfont registry, `css_parse.c` @font-face, `wdoc.c` RES_FONT). Units in
  `ch` / `ex` are measured with the element's own font (they were fixed at 0.572 / 0.5 em before: that alone fixed ~100 tests).
- Tests that need `wptserve` dynamic responses or `.any.js` harness files
  are not part of the reftest discovery; only static reftests run. Tests that report through testharness.js
  (`/resources/testharness.js` is not in the sparse clone) only count when they also have a reference.
- `.xht` files are XHTML: the HTML parser takes them (the CDATA wrapper of an XHTML `<style>` is stripped when the sheet is
  added); anything that needs XML well-formedness semantics (`<div/>` closing a div) renders as in HTML.
- wbrowse (the harness binary, not the engine) now lays the page out inside its wait loop, so images that only a layout
  discovers (CSS `background-image`) are fetched before the screenshot, and script `focus()` sets the `:focus` /
  `:focus-within` flags the way the shell does. Before this every CSS background image of every test was missing from both
  the test and its reference.

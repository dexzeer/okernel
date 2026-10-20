# YouTube work: what was tried, what was learned, where the code went

**Status:** all three attempts are reverted (2026-10-20, owner's call). This file is the record
so none of the findings are lost. Nothing here is loaded or compiled by the kernel.

## Where things are

| What | Where |
|------|-------|
| Working tree / code state now | `5f1d23c` + `0969948` (the BotGuard revert) |
| Last good behaviour (YouTube plays 30-60 s via the page's own player) | `5f1d23c` (2026-10-18) |
| BotGuard-legit attempt (tracers, header wiring, miss log, `fetchhdr`) | commit `a56957e` (2026-10-16), reverted by `0969948` |
| Chrome-spoof + iOS-client attempt, all the session fixes, probes and logs | branch `backup/youtube-attempts-2026-10-20` (commit `cab91a2`) |
| Older backup branch | `backup/desktop-pre-1080p` |

Recover any single file from a snapshot with e.g.
`git show backup/youtube-attempts-2026-10-20:src/media/ytplay.c > /tmp/ytplay.c`.

## Baseline (5f1d23c): what actually worked

- YouTube watch pages **load and play through the page's own player**: VP9 + Opus over MSE,
  SABR POSTs, 28-31 fps with ~0 dropped under WHPX. This is the "smooth" state the owner wants.
- Playback is cut after **~25-60 s**: video servers answer SABR with a 107/123-byte UMP message
  (`PLAYBACK_START_POLICY` + `STREAM_PROTECTION_STATUS` = `ATTESTATION_REQUIRED`), and
  `youtubei/v1/player` answers `400 FAILED_PRECONDITION`. Signing in does not change it.
- The page is the **native** page (not the polyfilled legacy one) because the UA does not end in
  `Safari/537.36` (measured: page height 3834 px native vs 1225 px legacy).
- EU exit IPs: a cookie-consent lightbox pauses playback until clicked
  (`perf_win.ps1 -ClickAt "932,857@16"`).

## Attempt 1 — pass BotGuard legitimately (commit a56957e, reverted)

What was built:
- API/eval tracers (`tests/web/js/trace_api.js`, `trace_eval.js`, `trace_edge.mjs`,
  `headers_edge.mjs`) run in headless Edge and in our engine and compared.
- Engine-side "miss log" (`OJS_TRACE_MISS=1|2`, host builds only) reporting every failed
  property lookup / `in` test, to see which browser APIs pages probe for.
- Request metadata wiring: wire UA == `navigator.userAgent`, `Accept-Language`, `Referer`,
  Fetch Metadata (`Sec-Fetch-*`), `Upgrade-Insecure-Requests`, `Origin` on unsafe methods.
- `fetchhdr` shell command logging request heads.

What it found:
- BotGuard **runs to completion** in our engine and `GenerateIT` returns a snapshot the same size
  as Edge's, but the server still refuses the token. BotGuard computes its signals **inside its own
  VM** from JS-engine behaviour, so API/eval surface diffs cannot show what it dislikes.
- Remaining differences from Chrome: no Trusted Types, no `Intl`, no WebGL/WebAudio, stub
  `about:blank` iframe realms, and a different JS engine (not V8).
- Verdict: **not viable**. API completeness and headers do not move the verdict.

## Attempt 2 — spoof Chrome (uncommitted, in the backup branch)

What was tried: a Chrome-looking User-Agent (and related client-hint/identity strings) to get past
Google's sign-in/attestation checks.

What it found:
- Google sign-in refuses unknown browsers by a **UA whitelist**; a forged Chrome string did not open it.
- The attestation wall is **UA-independent** (measured in both directions): spoofing Chrome changed
  nothing about the SABR refusals.
- Conclusion: no benefit, only dishonesty. Removed; honest `okai/1.1` identity was the interim state.

## Attempt 3 — pretend to be the iOS client (uncommitted, in the backup branch)

Idea: the **IOS player client** of `youtubei/v1/player` hands out plain `Range` URLs with no
SABR, no PO token and no attestation. Resolve with it, then play in our own engine, bypassing
the page's player.

What was built:
- `src/media/ytplay.c/.h`: kernel-side resolver (`youtubei/v1/player`, IOS app client), picks
  both tracks from the same container family, ≤720p, prefers lower fps, audio highest bitrate
  (itag 251/140), 256 KB Range chunks, HTTP/1.1 on a fresh connection per request
  (`fetch_opts.no_h2`, never reused), MSS 1200.
- Prelude `ytAttach`/`ytFeed` (`src/web/wjs_prelude.js`): an own `<video>` + MediaSource placed
  over the page's player; the page's own player is refused an engine session (`__ytOwn` in
  `mediaSession`) because a second decoder plus SABR retry loops exhausted the heap
  (confirmed: `session5: 0` in every run).
- New engine API `meng_sb_end` + `a_feed_end`/`v_feed_end` (dead track stops capping
  `all_ranges`, playback continues on the surviving track) and native `mediaSbEnd`.

Results: the **first** instrumented run played a full 213 s at ~25 shown/s with clean frames at
100/150/200 s and 0 dropped. Later runs regressed (see the bug list); the last runs stalled at
0.17-4.27 s with the player stuck on screen. That is why the owner asked for the revert.

### Bugs found and fixed along the way (worth reusing)

1. **Double decoder / heap exhaustion.** The page's player and our overlay both decoding (plus
   SABR retry loops) exhausted the heap → `session5: 0`; refuse foreign elements a session while
   the overlay owns playback.
2. **Packet arena held the whole movie** (`meng.c`): queues grew until a 16 MB `kmalloc` failed at
   ~94 s and the picture froze. Fix: `pq_reap` reclaims consumed history before the arena grows.
   Verified: kmalloc failures 58 196 → 0.
3. **Short content-range answers treated as EOF.** googlevideo answers smaller ranges than asked;
   the feed ended a track. Fix: drive the read position from `content-range` (`rstart`/`rend`),
   never from the requested end; a short chunk is retried, not EOF.
4. **Permanent stall when one track died.** `all_ranges` intersected both tracks, so a dead track
   capped playback forever. Fix: `meng_sb_end` — a track whose feed ended stops capping, and a
   403 is retried twice before only that track ends.
5. **Mid-stream `200` ordering bug.** A `200` body was appended *before* the "unexpected 200"
   check, rewinding the demuxer. Fix: check `!o.partial` first.
6. **No timeout on media fetches.** Requests that never got a response hung a feed forever
   (observed: requests 168/169 never answered, feeds silent, no terminal log).
7. **Fetch-slot / connection starvation.** Page XHRs can hold all slots/connections; media
   requests then wait unboundedly.
8. **Session-slot reuse artifacts** (`4294967283 shown/s`): counters of a freed session compared
   against a new one in the same slot; benign, but confusing in logs.

### The unfixed root cause at revert time

`pq_reap`'s hard pass dropped every packet before the decode cursor. The cursor runs **ahead of
the playhead**, so the buffered range start could jump past `clock`, `ahead_of()` returned -1,
`readyState` fell to 1 (`HAVE_METADATA`), the `waiting` event fired, the clock stopped — and since
new appends only extend the range end, the stall was permanent. The proposed fix (never drop
packets newer than playhead - keep-window) was diagnosed but not implemented.

## Exceptions kept from the reverted commit (2026-10-20, measured)

Reverting `a56957e` took the wire UA back to the bare `okernel/0.4` and dropped the request
metadata. A YouTube run then showed `[okai] render ... doc_h=1225` — the **legacy polyfilled
page**, not the native one (`3834`-`4008` px at `5f1d23c`): YouTube serves the native page only to
a browser-like UA, and the bare token is not one. With the honest UA back, the page was native
again but the player fired `event error t=5.27` after a run of `403` on its `videoplayback` GETs —
the removed `Referer`/`Origin`/`Sec-Fetch-*` metadata.

So these parts of `a56957e` are kept, because they are required for the known-good page behaviour
and are standard browser behaviour, not BotGuard machinery:

- `src/web/wua.h` (`okai/0.6`, no `Safari/537.36` token),
- `fetch.c`: `User-Agent: OKAI_USER_AGENT` + `Accept-Language: OKAI_ACCEPT_LANGUAGE`,
- `wjs.c`: `W.userAgent` native; the prelude's `navigator.userAgent` reads it (wire == scripts),
- `okai.c`/`okai.h`: `add_request_meta` (Referer / Fetch Metadata / Origin) and the
  `nav_from`/`nav_user` plumbing (`nav_ctx_set`).

Still reverted (the BotGuard-attempt part): the API/eval tracers, the `OJS_TRACE_MISS` miss log,
the `fetchhdr` request-head logger and `test_okai_reqheaders.py`. Recover them from `a56957e`.

## Useful test/verification recipes (still valid)

- WHPX YouTube run:
  `powershell -File tests\headless\perf_win.ps1 -Urls "https://www.youtube.com/watch?v=<id>" -Settle 400 -Shot out.ppm -ShotAt "60 120 205" -AudioWav C:\...\out.wav -Mem 1024 -Smp 2`
  Serial log is ground truth (`perf_<accel>_<port>.log` in the same output dir); watch
  `[media] rate:`, `[vidfps]`, `[okai] parse/update`, `[js-gc]`, `kmalloc FAIL`.
- Playback health: `[media] rate` lines once a second while playing; `[vidfps] ... clock Nms ...
  buffered Nms`.
- `tests/ojs/memlimit.js` failure is **pre-existing**: it fails at clean HEAD too (proven in the
  `D:\projects\okernel-base` worktree; baseline fails at `memlimit.js:35`, the session tree at `:28`).
- Media host tests: `make media-tests`, `sh tests/media/build_meng.sh`.

## What to do if YouTube is picked up again

Start from `5f1d23c` behaviour (page player, 25-60 s). The attestation wall was only ever beaten
by the IOS-client resolver, and that path needs the packet-queue fix above plus feed timeouts
before it is stable. Do not re-derive the findings above; read them here or in `a56957e` /
`backup/youtube-attempts-2026-10-20`.

// Watch Edge's video playback over ~2 minutes: currentTime samples + SABR request timeline.
// usage: node tools/ui/edge_yt_long.mjs <url> <out.json> [seconds]
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2];
const out = process.argv[3];
const secs = Number(process.argv[4] || 130);
const MSEDGE = process.env.EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';

const profile = mkdtempSync(join(tmpdir(), 'edge-ytlong-'));
const flags = ['--disable-gpu', '--no-first-run', '--mute-audio', '--window-size=1920,1080', '--remote-debugging-port=0', '--user-data-dir=' + profile, 'about:blank'];
if (!process.env.EDGE_HEADED) flags.unshift('--headless=new');
const edge = spawn(MSEDGE, flags, { stdio: ['ignore', 'ignore', 'pipe'] });
let port = 0;
edge.stderr.on('data', d => { const m = String(d).match(/DevTools listening on ws:\/\/127\.0\.0\.1:(\d+)/); if (m) port = Number(m[1]); });
const sleep = ms => new Promise(r => setTimeout(r, ms));
for (let i = 0; i < 100 && !port; i++) await sleep(100);
const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = list.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(r => { ws.onopen = r; });
let mid = 0; const pending = new Map();
function send(method, params) { return new Promise(res => { const id = ++mid; pending.set(id, res); ws.send(JSON.stringify({ id, method, params })); }); }
ws.onmessage = ev => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); } };
await send('Page.enable');
await send('Network.enable', { maxPostDataSize: 4096 });
const events = [];
ws.onmessage2 = null;
const t0 = Date.now();
ws.addEventListener('message', ev => {
  const m = JSON.parse(ev.data);
  if (m.method === 'Network.requestWillBeSent' && /videoplayback|youtubei\/v1\/player\?|GenerateIT/.test(m.params.request.url)) {
    events.push({ t: Date.now() - t0, ev: 'req', m: m.params.request.method, u: m.params.request.url.replace(/\?.*/, ''), post: m.params.request.hasPostData ? (m.params.request.postData || '').length : -1 });
  }
});
await send('Page.navigate', { url });
const playExpr = `(function(){ const v = document.querySelector('video.html5-main-video') || document.querySelector('video'); if (!v) return 'novideo'; const p = v.play(); return p ? p.then(() => 'playing', e => 'playfail ' + e.name).then(s => s + ' state=' + v.readyState) : 'played'; })()`;
const timeExpr = `(function(){ const v = document.querySelector('video.html5-main-video') || document.querySelector('video'); if (!v) return 'novideo'; return 't=' + v.currentTime.toFixed(1) + ' paused=' + v.paused + ' buffered=' + (v.buffered.length ? v.buffered.end(v.buffered.length - 1).toFixed(1) : '0') + ' rs=' + v.readyState; })()`;
let played = false;
const samples = [];
const nav0 = Date.now();
while (Date.now() - nav0 < secs * 1000) {
  await sleep(3000);
  const now = Date.now() - nav0;
  try {
    if (!played && now > 20000) {
      const r = await send('Runtime.evaluate', { expression: playExpr, returnByValue: true, awaitPromise: true, userGesture: true });
      events.push({ t: now, ev: 'act', result: String(r.result && r.result.value).slice(0, 120) });
      played = true;
    } else if (played) {
      const r = await send('Runtime.evaluate', { expression: timeExpr, returnByValue: true, awaitPromise: false, userGesture: true });
      const s = String(r.result && r.result.value);
      samples.push({ t: now, v: s });
      events.push({ t: now, ev: 'time', result: s });
    }
  } catch (e) { events.push({ t: now, ev: 'err', e: String(e).slice(0, 80) }); }
}
writeFileSync(out, JSON.stringify({ url, secs, samples, events }, null, 1));
console.log('samples:');
for (const s of samples) console.log('  %ss %s', Math.round(s.t / 1000), s.v);
ws.close(); edge.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch {}
process.exit(0);

// CDP Network capture of YouTube's player/GenerateIT/SABR flow in headless Edge.
// Sees requests from ALL realms (workers included) and their POST bodies.
// usage: node tools/ui/edge_yt_cdp.mjs <url> <out.json> [seconds]
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2];
const out = process.argv[3];
const secs = Number(process.argv[4] || 150);
const MSEDGE = process.env.EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const MATCH = /youtubei\/v1\/|\/api\/jnn\/|videoplayback|\.wasm|\/wcm\/|play.google.com\/log/;
const clicks = process.argv[5] ? process.argv[5].split(';').map(s => { const m = s.match(/^([\d.]+),([\d.]+)@([\d.]+)$/); return m ? { t: Number(m[3]) * 1000, x: Number(m[1]), y: Number(m[2]) } : null; }).filter(Boolean) : [];

const profile = mkdtempSync(join(tmpdir(), 'edge-ytcdp-'));
const edge = spawn(MSEDGE, ['--headless=new', '--disable-gpu', '--no-first-run', '--mute-audio', '--window-size=1920,1080', '--remote-debugging-port=0', '--user-data-dir=' + profile, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
let port = 0;
edge.stderr.on('data', d => { const m = String(d).match(/DevTools listening on ws:\/\/127\.0\.0\.1:(\d+)/); if (m) port = Number(m[1]); });
const sleep = ms => new Promise(r => setTimeout(r, ms));
for (let i = 0; i < 100 && !port; i++) await sleep(100);
if (!port) { console.error('no devtools port'); process.exit(1); }
const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = list.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(r => { ws.onopen = r; });
let mid = 0; const pending = new Map();
function send(method, params) { return new Promise(res => { const id = ++mid; pending.set(id, res); ws.send(JSON.stringify({ id, method, params })); }); }

const recs = [];
const t0 = Date.now();
const postCache = new Map();   // requestId -> body string
async function onMessage(m) {
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); return; }
  if (!m.method || !m.params) return;
  if (m.method === 'Network.requestWillBeSent') {
    const r = m.params.request;
    if (!MATCH.test(r.url)) return;
    const rec = { t: Date.now() - t0, ev: 'req', reqId: m.params.requestId, m: r.method, url: r.url.slice(0, 3000), type: m.params.type, frame: m.params.frameId ? 'f' : 'p' };
    if (r.hasPostData) {
      postCache.set(m.params.requestId, true);
      try { const pd = await send('Network.getRequestPostData', { requestId: m.params.requestId }); rec.post = pd ? pd.postData : null; } catch (e) { rec.post = 'ERR:' + e.message; }
    }
    if (r.postData) rec.post = r.postData;
    recs.push(rec);
  } else if (m.method === 'Network.responseReceived') {
    const r = m.params.response;
    if (!MATCH.test(r.url)) return;
    recs.push({ t: Date.now() - t0, ev: 'res', reqId: m.params.requestId, status: r.status, mime: r.mimeType, url: r.url.slice(0, 200), hdrs: Object.fromEntries(Object.entries(r.headers || {}).filter(([k]) => /^content-(type|length)|^alt-svc$|^x-srv/i.test(k))) });
  } else if (m.method === 'Network.loadingFinished') {
    if (!postCache.has(m.params.requestId)) return;
    try {
      const b = await send('Network.getResponseBody', { requestId: m.params.requestId });
      if (b && b.body) {
        const body = b.base64Encoded ? Buffer.from(b.body, 'base64').toString('latin1') : b.body;
        // the interesting bits of a big player response: streamingData URLs
        const urls = [...body.matchAll(/https:\/\/[^"\\]*googlevideo\.com\/videoplayback[^"\\]*/g)].map(x => x[0]).slice(0, 6);
        recs.push({ t: Date.now() - t0, ev: 'body', reqId: m.params.requestId, len: body.length, head: body.slice(0, 700), gvsUrls: urls.map(u => u.slice(0, 1200)) });
      }
    } catch (e) { recs.push({ ev: 'bodyerr', reqId: m.params.requestId, e: String(e).slice(0, 120) }); }
  }
}
ws.onmessage = ev => { const p = JSON.parse(ev.data); onMessage(p); };
await send('Network.enable', { maxPostDataSize: 65536 });
await send('Page.enable');
await send('Page.navigate', { url });
const consentExpr = `(function(){
  const dlg = document.querySelector('ytd-consent-bump-v2-lightbox, tp-yt-paper-dialog, ytd-enforcement-message-view-model');
  if (!dlg) return 'nodialog';
  const btns = [...dlg.querySelectorAll('button')];
  const b = btns.find(x => /reject all|alle ablehnen|accept all|alle akzeptieren|refuser/i.test(x.textContent || ''));
  if (b) { b.click(); return 'clicked: ' + (b.textContent || '').trim().slice(0, 40); }
  return 'buttons: ' + btns.map(x => (x.textContent || '').trim().slice(0, 25)).join(' | ').slice(0, 300);
})()`;
const playExpr = `(function(){ const v = document.querySelector('video.html5-main-video') || document.querySelector('video'); if (!v) return 'novideo'; const inner = (function(){ const p = v.play(); return p ? p.then(() => 'playing t=' + v.currentTime, e => 'playfail ' + e.name) : 'played'; })(); return Promise.race([Promise.resolve(inner), new Promise(r => setTimeout(() => r('TIMEOUT(3s) state=' + v.readyState + ' paused=' + v.paused), 3000))]); })()`;
const actions = [
  { t: 16000, expr: consentExpr },
  { t: 21000, expr: consentExpr },
  { t: 26000, expr: playExpr },
  { t: 36000, expr: playExpr },
];
let ci = 0;
const nav0 = t0;
while (Date.now() - nav0 < secs * 1000) {
  await sleep(250);
  const now = Date.now() - nav0;
  while (ci < actions.length && actions[ci].t <= now) {
    const a = actions[ci++];
    try {
      const r = await send('Runtime.evaluate', { expression: a.expr, returnByValue: true, awaitPromise: true, userGesture: true });
      recs.push({ t: Date.now() - nav0, ev: 'act', at: a.t, result: r.result ? r.result.value : JSON.stringify(r).slice(0, 200) });
    } catch (e) { recs.push({ t: Date.now() - nav0, ev: 'act', at: a.t, err: String(e).slice(0, 150) }); }
  }
}
writeFileSync(out, JSON.stringify({ url, secs, recs }, null, 1));
console.log(`captured ${recs.length} records -> ${out}`);
ws.close(); edge.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch {}
process.exit(0);

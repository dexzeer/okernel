// Capture YouTube's player/GenerateIT/SABR requests WITH bodies from headless Edge.
// usage: node tools/ui/edge_yt_cap.mjs <url> <out.json> [seconds]
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2];
const out = process.argv[3];
const secs = Number(process.argv[4] || 150);
const MSEDGE = process.env.EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';

const init = `
window.__netcap = [];
(function () {
  const MATCH = /youtubei\\/v1\\/|\\/api\\/jnn\\/|videoplayback|googlevideo\\.com\\/generate_204|\\/api\\/stats\\//;
  const cap = window.__netcap;
  const t0 = performance.now();
  function rec(o) { if (cap.length < 400) { o.t = Math.round(performance.now() - t0); cap.push(o); } }
  function bodyOf(b) {
    try {
      if (b == null) return null;
      if (typeof b === 'string') return b.slice(0, 8192);
      if (b instanceof Uint8Array) return 'b64:' + btoa(String.fromCharCode.apply(null, b.slice(0, 1024)));
      if (b instanceof ArrayBuffer) return 'b64:' + btoa(String.fromCharCode.apply(null, new Uint8Array(b).slice(0, 1024)));
      return Object.prototype.toString.call(b);
    } catch (e) { return 'ERR:' + e.message; }
  }
  function resOf(x) {
    try {
      if (!x.responseType || x.responseType === 'text') return String(x.responseText || '').slice(0, 600);
      if (x.responseType === 'arraybuffer') { const u = new Uint8Array(x.response); return 'b64:' + btoa(String.fromCharCode.apply(null, u.slice(0, 256))); }
      if (x.responseType === 'json') return JSON.stringify(x.response).slice(0, 600);
      return 'rt:' + x.responseType;
    } catch (e) { return 'ERR:' + e.message; }
  }
  const OO = XMLHttpRequest.prototype.open, OS = XMLHttpRequest.prototype.send, OH = XMLHttpRequest.prototype.setRequestHeader;
  XMLHttpRequest.prototype.open = function (m, u) { this.__m = m; this.__u = String(u); return OO.apply(this, arguments); };
  XMLHttpRequest.prototype.setRequestHeader = function (k, v) { (this.__h = this.__h || {})[k] = v; return OH.apply(this, arguments); };
  XMLHttpRequest.prototype.send = function (b) {
    const x = this;
    if (MATCH.test(x.__u)) rec({ k: 'xhr', d: '>', m: x.__m, u: x.__u.slice(0, 4096), h: x.__h, b: bodyOf(b) });
    x.addEventListener('load', function () {
      if (MATCH.test(x.__u)) rec({ k: 'xhr', d: '<', m: x.__m, u: x.__u.slice(0, 4096), s: x.status, r: resOf(x) });
    });
    x.addEventListener('error', function () { if (MATCH.test(x.__u)) rec({ k: 'xhr', d: '<', m: x.__m, u: x.__u.slice(0, 4096), e: 'error' }); });
    return OS.apply(this, arguments);
  };
  const of = window.fetch;
  window.fetch = function (input, init) {
    const u = typeof input === 'string' ? input : (input && input.url) || String(input);
    const p = of.apply(this, arguments);
    if (MATCH.test(u)) {
      let h = null;
      if (init && init.headers) { h = {}; try { new Headers(init.headers).forEach((v, k) => h[k] = v); } catch (e) {} }
      rec({ k: 'fetch', d: '>', m: (init && init.method) || 'GET', u: String(u).slice(0, 4096), h, b: bodyOf(init && init.body) });
      p.then(res => {
        rec({ k: 'fetch', d: '<', m: '', u: String(u).slice(0, 4096), s: res.status });
        try { const c = res.clone(); c.arrayBuffer().then(ab => { const u8 = new Uint8Array(ab); rec({ k: 'fetch', d: 'B', u: String(u).slice(0, 4096), s: res.status, r: 'b64:' + btoa(String.fromCharCode.apply(null, u8.slice(0, 256))), rlen: u8.length }); }).catch(() => {}); } catch (e) {}
      }, err => rec({ k: 'fetch', d: '<', u: String(u).slice(0, 4096), e: String(err) }));
    }
    return p;
  };
})();
`;

const profile = mkdtempSync(join(tmpdir(), 'edge-ytcap-'));
const edge = spawn(MSEDGE, ['--headless=new', '--disable-gpu', '--no-first-run', '--mute-audio', '--remote-debugging-port=0', '--user-data-dir=' + profile, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
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
ws.onmessage = ev => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); } };
await send('Page.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: init });
await send('Runtime.enable');
const consoleLines = [];
ws.onmessage2 = null;
ws.addEventListener('message', ev => {
  const m = JSON.parse(ev.data);
  if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') {
    const txt = (m.params.args || []).map(a => a.value !== undefined ? String(a.value) : (a.description || '')).join(' ');
    consoleLines.push({ t: m.params.timestamp, txt: String(txt).slice(0, 300) });
  }
});
await send('Page.navigate', { url });
const all = [];
const t0 = Date.now();
while (Date.now() - t0 < secs * 1000) {
  await sleep(4000);
  try {
    const r = await send('Runtime.evaluate', { expression: 'JSON.stringify(window.__netcap||[])', returnByValue: true });
    if (r.result && r.result.value) {
      const batch = JSON.parse(r.result.value);
      if (batch.length) { all.push(...batch); await send('Runtime.evaluate', { expression: 'window.__netcap.length=0' }); }
    }
  } catch (e) { /* page navigating */ }
}
writeFileSync(out, JSON.stringify({ url, secs, recs: all, consoleErrors: consoleLines }, null, 1));
console.log(`captured ${all.length} records -> ${out}`);
const kinds = {};
for (const r of all) { const key = r.d + (r.u || '').replace(/\?.*/, '').slice(0, 60); kinds[key] = (kinds[key] || 0) + 1; }
console.log(JSON.stringify(kinds, null, 1));
ws.close(); edge.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch {}
process.exit(0);

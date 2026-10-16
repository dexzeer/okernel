// What request headers a real browser (headless Edge) sends: DevTools Network.requestWillBeSentExtraInfo shows them as they go
// out on the wire (cookies included, values redacted here). One sample per (method, resource type, same-site-ness).
//   node tests/web/headers_edge.mjs <url> [seconds=25]      (EDGE_PROXY=http://host:port if direct access fails)
import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, secs = '25'] = process.argv.slice(2);
const edge = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const port = 9533 + Math.floor(Math.random() * 200);
const prof = mkdtempSync(join(tmpdir(), 'edge_hdr_'));
const args = ['--headless=new', '--remote-debugging-port=' + port, '--user-data-dir=' + prof, '--no-first-run', '--disable-extensions', '--disable-quic', 'about:blank'];
if (process.env.EDGE_PROXY) args.unshift('--proxy-server=' + process.env.EDGE_PROXY);
const proc = spawn(edge, args, { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let target;
for (let i = 0; i < 60 && !target; i++) { try { const j = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); target = j.find(t => t.type === 'page'); } catch { } if (!target) await sleep(500); }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r, { once: true }));
let id = 0;
const send = (method, params = {}) => ws.send(JSON.stringify({ id: ++id, method, params }));
const reqs = new Map(), seen = new Set(), out = [];
ws.addEventListener('message', ev => {
    const m = JSON.parse(ev.data);
    if (m.method === 'Network.requestWillBeSent') reqs.set(m.params.requestId, { url: m.params.request.url, method: m.params.request.method, type: m.params.type });
    if (m.method === 'Network.requestWillBeSentExtraInfo') {
        const r = reqs.get(m.params.requestId) || {};
        const h = m.params.headers;
        const host = r.url ? new URL(r.url).host : '?';
        const key = `${r.method} ${r.type} ${h['sec-fetch-site'] || h['Sec-Fetch-Site'] || ''} ${host}`;
        if (seen.has(key) || out.length > 40) return;
        seen.add(key);
        const red = {};
        for (const k of Object.keys(h)) red[k] = /^(cookie|authorization|x-goog-authuser|x-goog-visitor-id)$/i.test(k) ? `<${String(h[k]).length} chars>` : (String(h[k]).length > 120 ? String(h[k]).slice(0, 120) + '...' : h[k]);
        out.push({ url: (r.url || '').slice(0, 110), method: r.method, type: r.type, headers: red });
    }
});
send('Network.enable');
send('Page.enable');
send('Page.navigate', { url });
await sleep(Number(secs) * 1000);
for (const o of out) { console.log('\n' + o.method + ' [' + o.type + '] ' + o.url); for (const k of Object.keys(o.headers)) console.log('   ' + k + ': ' + o.headers[k]); }
try { proc.kill(); } catch { }
process.exit(0);

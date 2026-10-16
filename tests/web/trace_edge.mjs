// Run tests/web/js/trace_api.js in headless Edge (DevTools protocol) against a URL and save the trace.
//   node tests/web/trace_edge.mjs <url> <out.json> [seconds=45] [headless=new|headed]
// Edge is the REFERENCE measurement: what a real browser does on the page, so the same script's trace from our engine can be
// compared (tests/web/trace_compare.py). Needs node >= 22 (global WebSocket and fetch).
import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, out, secs = '45', mode = 'new'] = process.argv.slice(2);
if (!url || !out) { console.error('usage: node trace_edge.mjs <url> <out.json> [seconds] [new|headed]'); process.exit(2); }
const tracer = readFileSync(process.env.TRACER ? process.env.TRACER : new URL('./js/trace_api.js', import.meta.url), 'utf8');
const edge = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const port = 9333 + Math.floor(Math.random() * 200);
const prof = mkdtempSync(join(tmpdir(), 'edge_trace_'));
const args = ['--remote-debugging-port=' + port, '--user-data-dir=' + prof, '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--disable-quic', '--window-size=' + (process.env.EDGE_SIZE || '1280,800'), 'about:blank'];
if (process.env.EDGE_PROXY) args.unshift('--proxy-server=' + process.env.EDGE_PROXY);
if (mode !== 'headed') args.unshift('--headless=new');
const proc = spawn(edge, args, { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function targets() {
    for (let i = 0; i < 60; i++) {
        try { const r = await fetch(`http://127.0.0.1:${port}/json`); const j = await r.json(); const p = j.find(t => t.type === 'page'); if (p) return p; } catch { }
        await sleep(500);
    }
    throw new Error('Edge did not start');
}
const t = await targets();
const ws = new WebSocket(t.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r, { once: true }));
let id = 0;
const pending = new Map();
ws.addEventListener('message', ev => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
await send('Page.enable');
await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: tracer });
await send('Page.navigate', { url });
await sleep(Number(secs) * 1000);
const r = await send('Runtime.evaluate', { expression: 'window.__traceDump && window.__traceDump()', returnByValue: true });
const val = r.result && r.result.result && r.result.result.value;
if (!val) { console.error('no trace', JSON.stringify(r).slice(0, 400)); } else { writeFileSync(out, val); console.log('trace saved', out, val.length, 'bytes'); }
try { await send('Browser.close'); } catch { }
try { proc.kill(); } catch { }
process.exit(val ? 0 : 1);

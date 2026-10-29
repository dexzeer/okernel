// Evaluate a JS file inside headless Edge on a URL and print the result.
// usage: node tools/ui/edge_probe.mjs <url> <script-file> [seconds]
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2];
const script = readFileSync(process.argv[3], 'utf8');
const secs = Number(process.argv[4] || 20);
const MSEDGE = process.env.EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const profile = mkdtempSync(join(tmpdir(), 'edge-probe-'));
const edge = spawn(MSEDGE, ['--headless=new', '--disable-gpu', '--no-first-run', '--remote-debugging-port=0', '--user-data-dir=' + profile, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
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
await send('Page.navigate', { url });
await sleep(secs * 1000);
const r = await send('Runtime.evaluate', { expression: script, returnByValue: true, awaitPromise: true });
if (r.exceptionDetails) console.log('EXC: ' + JSON.stringify(r.exceptionDetails.exception?.description || r.exceptionDetails.text).slice(0, 500));
else console.log(String(r.result.value));
ws.close(); edge.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch {}
process.exit(0);

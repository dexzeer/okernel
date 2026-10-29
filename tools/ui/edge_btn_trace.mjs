import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2] || 'https://www.youtube.com/watch?v=aqz-KE-bpKQ';
const secs = Number(process.argv[3] || 25);
const MSEDGE = process.env.EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const profile = mkdtempSync(join(tmpdir(), 'edge-btn-'));
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
ws.onmessage = ev => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); }
  if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'log') {
    const t = (m.params.args || []).map(a => a.value !== undefined ? a.value : '').join(' ');
    if (t.startsWith('BCE')) console.log(t);
  } };
await send('Page.enable');
await send('Runtime.enable');
const hook = `
(function() {
  var D = customElements.define.bind(customElements);
  var seq = 0; var ids = new WeakMap();
  customElements.define = function(name, ctor, opts) {
    try {
      if (name === 'ytd-button-renderer' || name === 'yt-button-shape') {
        var P = ctor.prototype;
        var occ = P.connectedCallback;
        if (occ) P.connectedCallback = function() {
          if (!ids.has(this)) ids.set(this, ++seq);
          console.log('BCE' + ids.get(this) + ' <' + name + '> conn#' + ((this.__cc = (this.__cc || 0) + 1)) + ' kids=' + this.children.length);
          return occ.apply(this, arguments);
        };
        var odp = P.disconnectedCallback;
        if (odp) P.disconnectedCallback = function() { console.log('BCE' + ids.get(this) + ' DISC'); return odp.apply(this, arguments); };
      }
    } catch (e) {}
    return D(name, ctor, opts);
  };
})()
`;
await send('Page.addScriptToEvaluateOnNewDocument', { source: hook });
await send('Page.navigate', { url });
await sleep(secs * 1000);
ws.close(); edge.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch {}
process.exit(0);

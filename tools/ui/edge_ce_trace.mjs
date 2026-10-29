// Run the CE lifecycle hook inside headless Edge and print its event log.
// usage: node tools/ui/edge_ce_trace.mjs [url] [seconds]
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const url = process.argv[2] || 'https://www.youtube.com/watch?v=aqz-KE-bpKQ';
const secs = Number(process.argv[3] || 25);
const MSEDGE = process.env.EDGE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';

const profile = mkdtempSync(join(tmpdir(), 'edge-ce-'));
const edge = spawn(MSEDGE, [
  '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--remote-debugging-port=0', '--user-data-dir=' + profile, 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function findTarget() {
  for (let i = 0; i < 50; i++) {
    try {
      const r = await fetch('http://127.0.0.1:0/json');
    } catch {}
    await sleep(200);
  }
}

// read the chosen port from stderr (DevTools listening line)
let port = 0;
edge.stderr.on('data', d => {
  const m = String(d).match(/DevTools listening on ws:\/\/127\.0\.0\.1:(\d+)/);
  if (m) port = Number(m[1]);
});
for (let i = 0; i < 100 && !port; i++) await sleep(100);
if (!port) { console.error('no devtools port'); process.exit(1); }

const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = list.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(r => { ws.onopen = r; });

let mid = 0;
const pending = new Map();
function send(method, params, sessionId) {
  return new Promise((res, rej) => {
    const id = ++mid;
    pending.set(id, { res, rej });
    ws.send(JSON.stringify({ id, method, params, sessionId }));
  });
}
ws.onmessage = ev => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id).res(m.result); pending.delete(m.id); }
};

await send('Page.enable');
await send('Runtime.enable');
const hook = `
(function() {
  var D = customElements.define.bind(customElements);
  var seq = 0; var ids = new WeakMap();
  customElements.define = function(name, ctor, opts) {
    try {
      var P = ctor && ctor.prototype;
      if (P && P.connectedCallback && (name.indexOf('paper') >= 0 || name.indexOf('consent') >= 0)) {
        var occ = P.connectedCallback;
        P.connectedCallback = function() {
          if (!ids.has(this)) ids.set(this, ++seq);
          console.log('CE' + ids.get(this) + ' <' + name + '> conn id=' + (this.id || ''));
          return occ.apply(this, arguments);
        };
      }
      if (P && P.disconnectedCallback && (name.indexOf('paper') >= 0 || name.indexOf('consent') >= 0)) {
        var odc = P.disconnectedCallback;
        P.disconnectedCallback = function() {
          var t = ids.get(this);
          if (t) console.log('CE' + t + ' <' + name + '> DISCONNECT');
          return odc.apply(this, arguments);
        };
      }
    } catch (e) {}
    return D(name, ctor, opts);
  };
  console.log('CEHOOK installed');
})()
`;
await send('Page.addScriptToEvaluateOnNewDocument', { source: hook });
ws.send(JSON.stringify({ id: ++mid, method: 'Runtime.consoleAPICalled', params: {} })); // noop
// subscribe to console events
ws.addEventListener('message', ev => {
  const m = JSON.parse(ev.data);
  if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'log') {
    const text = (m.params.args || []).map(a => a.value !== undefined ? a.value : a.description || '').join(' ');
    if (text.startsWith('CE')) console.log(text);
  }
});
await send('Page.navigate', { url });
await sleep(secs * 1000);
ws.close();
edge.kill();
rmSync(profile, { recursive: true, force: true });
process.exit(0);

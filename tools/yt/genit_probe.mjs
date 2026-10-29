// Post a snapshot to GenerateIT and report the integrity token's shape.
// usage: node tools/yt/genit_probe.mjs <snapshotB64>
const snap = process.argv[2];
if (!snap) { console.error('usage: genit_probe.mjs <snapshot>'); process.exit(1); }
// a page visit first: the endpoint rejects callers without the session cookies
const jar = new Map();
const page = await fetch('https://www.youtube.com/', { headers: { 'user-agent': 'okai/1.1', 'accept-language': 'en-US,en;q=0.9' } });
for (const line of (page.headers.getSetCookie ? page.headers.getSetCookie() : [])) {
  const nv = line.split(';')[0]; const i = nv.indexOf('=');
  if (i > 0) jar.set(nv.slice(0, i).trim(), nv.slice(i + 1).trim());
}
const vis = /"visitorData":"([^"]+)"/.exec(await page.text().then(t => { global.__html = t; return t; }));
const r = await fetch('https://www.youtube.com/api/jnn/v1/GenerateIT', {
  method: 'POST',
  headers: {
    'content-type': 'application/json+protobuf', 'user-agent': 'okai/1.1', 'accept-language': 'en-US,en;q=0.9',
    'x-goog-api-key': 'AIzaSyDyT5W0Jh49F30Pqqtyfdf7pDLFKLJoAnw',
    origin: 'https://www.youtube.com', referer: 'https://www.youtube.com/',
    cookie: [...jar.entries()].map(([k, v]) => k + '=' + v).join('; '),
    'x-goog-request-time': String(Math.floor(Date.now() / 1000)),
    ...(vis ? { 'x-goog-visitor-id': vis[1] } : {}),
  },
  body: JSON.stringify(['O43z0dpjhgX20SCx4KAo', snap]),
});
const txt = await r.text();
console.log('status', r.status, 'bytes', txt.length);
try {
  const j = JSON.parse(txt);
  if (Array.isArray(j)) {
    console.log('elements:', j.length);
    j.forEach((e, i) => console.log('  [%d] %s %s', i, typeof e, typeof e === 'string' ? 'len=' + e.length + ' head=' + e.slice(0, 40) : JSON.stringify(e)));
  } else console.log(txt.slice(0, 300));
} catch (e) { console.log(txt.slice(0, 300)); }

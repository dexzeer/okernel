// Mimic a real browser session: GET the watch page (cookies established), then
// POST youtubei/v1/player as WEB with those cookies + the page's visitor data.
// usage: node tools/yt/session_probe.mjs <videoId> [userAgent]
const vid = process.argv[2] || 'aqz-KE-bpKQ';
const UA = process.argv[3] || 'okai/1.1';
const jar = new Map();
const t0 = Date.now();

function cookieHeader() {
  return [...jar.entries()].map(([k, v]) => k + '=' + v).join('; ');
}
function absorb(resp) {
  const set = resp.headers.getSetCookie ? resp.headers.getSetCookie() : [];
  for (const line of set) {
    const [nv] = line.split(';');
    const i = nv.indexOf('=');
    if (i > 0) jar.set(nv.slice(0, i).trim(), nv.slice(i + 1).trim());
  }
}

const page = await fetch('https://www.youtube.com/watch?v=' + vid + '&hl=en', {
  headers: { 'user-agent': UA, 'accept-language': 'en-US,en;q=0.9', accept: 'text/html' },
});
absorb(page);
const html = await page.text();
console.log(`watch page status=${page.status} bytes=${html.length} cookies=${jar.size} in ${Date.now() - t0}ms`);

const visitor = /"visitorData":"([^"]+)"/.exec(html);
const ptok = /"poToken":"([^"]+)"/.exec(html);
console.log('visitorData:', visitor ? visitor[1].slice(0, 40) + '...' : 'MISSING');
// find the embedded player response
const ipr = /ytInitialPlayerResponse\s*=\s*(\{.+?\});/s.exec(html);
let formats = [];
if (ipr) {
  try {
    const pr = JSON.parse(ipr[1]);
    const sd = pr.streamingData || {};
    formats = [...(sd.formats || []), ...(sd.adaptiveFormats || [])];
    console.log('HTML player response: playability=' + (pr.playabilityStatus ? pr.playabilityStatus.status : '?') +
      ' formats=' + formats.length + ' spcUrls=' + JSON.stringify(formats.slice(0, 1).map(f => /spc=/.test(f.url || ''))));
  } catch (e) { console.log('HTML player response parse failed:', e.message); }
} else console.log('no ytInitialPlayerResponse in HTML');

const body = {
  context: {
    client: { clientName: 'WEB', clientVersion: '2.20261016.01.00', hl: 'en', visitorData: visitor ? decodeURIComponent(visitor[1]) : undefined },
  },
  videoId: vid, contentCheckOk: true, racyCheckOk: true,
};
const r = await fetch('https://www.youtube.com/youtubei/v1/player?prettyPrint=false', {
  method: 'POST',
  headers: { 'content-type': 'application/json', 'user-agent': UA, 'accept-language': 'en-US,en;q=0.9', cookie: cookieHeader(), 'x-youtube-client-name': '1', 'x-youtube-client-version': '2.20261016.01.00', origin: 'https://www.youtube.com', referer: 'https://www.youtube.com/watch?v=' + vid },
  body: JSON.stringify(body),
});
absorb(r);
const txt = await r.text();
let j = null;
try { j = JSON.parse(txt); } catch (e) {}
console.log(`player POST status=${r.status} bytes=${txt.length} cookies now=${jar.size}`);
if (j) {
  const ps = j.playabilityStatus || {};
  const sd = j.streamingData || {};
  const fmts = [...(sd.formats || []), ...(sd.adaptiveFormats || [])];
  console.log(`player: playability=${ps.status} reason=${JSON.stringify(ps.reason || '')} formats=${fmts.length} hls=${!!sd.hlsManifestUrl}`);
  for (const f of fmts.slice(0, 2)) {
    const u = f.url;
    if (!u) { console.log(`itag=${f.itag} no url (signatureCipher?)`); continue; }
    const rr = await fetch(u, { headers: { 'user-agent': UA, range: 'bytes=0-32768', referer: 'https://www.youtube.com/' } });
    const ab = await rr.arrayBuffer();
    console.log(`itag=${f.itag} ${f.mimeType && f.mimeType.split(';')[0]} GET status=${rr.status} bytes=${ab.byteLength} cr=${rr.headers.get('content-range')}`);
  }
  if (ps.status !== 'OK') console.log(txt.slice(0, 300));
}

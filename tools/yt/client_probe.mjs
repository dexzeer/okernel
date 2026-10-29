// Probe YouTube innertube clients for unattested playback URLs (host-side).
// usage: node tools/yt/client_probe.mjs <videoId> [client]
const vid = process.argv[2] || 'aqz-KE-bpKQ';
const which = process.argv[3] || 'ios';

const clients = {
  ios: {
    body: {
      context: { client: { clientName: 'IOS', clientVersion: '19.45.4', deviceMake: 'Apple', deviceModel: 'iPhone16,2', osName: 'iPhone', osVersion: '18.1.0.22B83', hl: 'en', timeZone: 'UTC', utcOffsetMinutes: 0 } },
      videoId: vid, contentCheckOk: true, racyCheckOk: true,
    },
    ua: 'com.google.ios.youtube/19.45.4 (iPhone16,2; U; CPU iOS 18_1_0 like Mac OS X;)',
  },
  android: {
    body: {
      context: { client: { clientName: 'ANDROID', clientVersion: '19.44.38', androidSdkVersion: 34, osName: 'Android', osVersion: '14', hl: 'en' } },
      videoId: vid, contentCheckOk: true, racyCheckOk: true,
    },
    ua: 'com.google.android.youtube/19.44.38 (Linux; U; Android 14) gzip',
  },
  web: {
    body: {
      context: { client: { clientName: 'WEB', clientVersion: '2.20261016.01.00', hl: 'en' } },
      videoId: vid, contentCheckOk: true, racyCheckOk: true,
    },
    ua: 'okai/1.1',
  },
  tv: {
    body: {
      context: { client: { clientName: 'TVHTML5', clientVersion: '7.20260122.15.00', hl: 'en', timeZone: 'UTC', utcOffsetMinutes: 0 } },
      videoId: vid, contentCheckOk: true, racyCheckOk: true,
    },
    ua: 'Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version',
  },
  tv_embedded: {
    body: {
      context: { client: { clientName: 'TVHTML5_SIMPLY_EMBEDDED_PLAYER', clientVersion: '2.0', hl: 'en' }, thirdParty: { embedUrl: 'https://www.youtube.com/' } },
      videoId: vid, contentCheckOk: true, racyCheckOk: true,
    },
    ua: 'Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version',
  },
  android_vr: {
    body: {
      context: { client: { clientName: 'ANDROID_VR', clientVersion: '1.60.19', deviceMake: 'Oculus', deviceModel: 'Quest 3', osName: 'Android', osVersion: '12L', hl: 'en' } },
      videoId: vid, contentCheckOk: true, racyCheckOk: true,
    },
    ua: 'com.google.android.apps.youtube.vr.oculus/1.60.19 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip',
  },
};
const c = clients[which];
const t0 = Date.now();
const r = await fetch('https://www.youtube.com/youtubei/v1/player?prettyPrint=false', {
  method: 'POST',
  headers: { 'content-type': 'application/json', 'user-agent': c.ua, 'accept-language': 'en-US,en;q=0.9', 'x-youtube-client-name': which === 'ios' ? '5' : which === 'android' ? '30' : which === 'tv' ? '7' : which === 'tv_embedded' ? '85' : which === 'android_vr' ? '28' : '1', 'x-youtube-client-version': c.body.context.client.clientVersion },
  body: JSON.stringify(c.body),
});
const txt = await r.text();
console.log(`[${which}] status=${r.status} bytes=${txt.length} in ${Date.now() - t0}ms`);
let j = null;
try { j = JSON.parse(txt); } catch (e) { console.log(txt.slice(0, 300)); process.exit(0); }
const ps = j.playabilityStatus || {};
console.log(`[${which}] playability=${ps.status} reason=${JSON.stringify(ps.reason || '')}`);
const sd = j.streamingData || {};
const fmts = [...(sd.formats || []), ...(sd.adaptiveFormats || [])];
console.log(`[${which}] formats=${fmts.length} hls=${sd.hlsManifestUrl ? 'yes' : 'no'} dash=${sd.dashManifestUrl ? 'yes' : 'no'}`);
if (j.playabilityStatus && j.playabilityStatus.status !== 'OK') console.log(txt.slice(0, 400));
// try a Range fetch on the first progressive URL
for (const f of fmts) {
  if (!f.url) continue;
  const u = f.url;
  try {
    const rr = await fetch(u, { headers: { 'user-agent': c.ua, range: 'bytes=0-65535' } });
    const ab = await rr.arrayBuffer();
    console.log(`[${which}] itag=${f.itag} ${f.mimeType && f.mimeType.split(';')[0]} range-get status=${rr.status} bytes=${ab.byteLength} clen=${rr.headers.get('content-range')}`);
    if (rr.status === 200 || rr.status === 206) { console.log(`[${which}] STREAM OK`); break; }
  } catch (e) { console.log(`[${which}] itag=${f.itag} fetch error ${e.message}`); }
  break;
}
if (!fmts.some(f => f.url) && sd.hlsManifestUrl) {
  try {
    const rr = await fetch(sd.hlsManifestUrl, { headers: { 'user-agent': c.ua } });
    const t = await rr.text();
    console.log(`[${which}] HLS manifest status=${rr.status} bytes=${t.length}`);
  } catch (e) { console.log(`[${which}] hls error ${e.message}`); }
}

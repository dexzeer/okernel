// Extract the BotGuard challenge pieces from a watch page. The challenge sits
// in the HTML with \xNN-escaped quotes: ...\x22bgChallenge\x22:\x7b\x22interpreterUrl\x22...
// Prints three lines: program, globalName (trayride), contentBinding.
const vid = process.argv[2] || 'aqz-KE-bpKQ';
const r = await fetch('https://www.youtube.com/watch?v=' + vid + '&hl=en', { headers: { 'user-agent': 'okai/1.1', 'accept-language': 'en-US,en;q=0.9' } });
const html = await r.text();
const raw = html.replace(/\\x([0-9a-fA-F]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16)));

const pm = /"program":"([^"]+)"/.exec(raw);
if (!pm) { console.error('no program in the HTML'); process.exit(1); }
const program = pm[1].replace(/\\\//g, '/').replace(/\\u0026/g, '&').replace(/\\\\u0026/g, '&');
const gnm = /"globalName":"([^"]+)"/.exec(raw);

// the content binding: the field right before bgChallenge ("hh"-signed blob)
const ci = raw.indexOf('"bgChallenge"');
let binding = 'MISSING';
if (ci > 0) {
  const before = raw.slice(Math.max(0, ci - 1200), ci);
  const cm = /"contentBinding":"([^"]+)"/.exec(before) || /"([a-z0-9]+(?:&|\\u0026)[^"]+hh=[^"]+)"/.exec(before) || /"([^"]*(?:\\u0026|&)[^"]*hh=[^"]*)"$/.exec(before.trim());
  if (cm) binding = cm[1].replace(/\\u0026/g, '&').replace(/\\\\u0026/g, '&').replace(/\\\//g, '/').replace(/\\"/g, '"');
  if (binding === 'MISSING') { const v2 = /"visitorData":"([^"]+)"/.exec(raw); if (v2) binding = v2[1]; }
}
console.log(program);
console.log(gnm ? gnm[1] : 'trayride');
console.log(binding);

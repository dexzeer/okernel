#!/usr/bin/env node
// filesrv.js - a tiny local web server that lets okai browse folders on this PC and play the video / audio files in them.
//
//   node tools/media/filesrv.js [--port 8010] [--host 127.0.0.1] [folder ...]
//   (or double-click tools\media\filesrv.bat; with no folder it serves Videos, Music, Downloads and Desktop of the current user)
//
// In okai (QEMU's user-mode network reaches this PC as 10.0.2.2) open:   http://10.0.2.2:8010/
//
// Only the folders named on the command line (and everything below them) are ever served. The server listens on 127.0.0.1 only and
// refuses requests whose Host header is not this machine, so web pages cannot reach your files through it. No dependencies.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');

// ---- arguments
let port = 8010, host = '127.0.0.1', verbose = false;
const dirs = [];
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port' || a === '-p') port = parseInt(argv[++i], 10);
    else if (a === '--host') host = argv[++i];
    else if (a === '--verbose' || a === '-v') verbose = true;
    else if (a === '--help' || a === '-h') { console.log('usage: node filesrv.js [--port N] [--host ADDR] [--verbose] [folder ...]'); process.exit(0); }
    else dirs.push(path.resolve(a));
}
if (!Number.isInteger(port) || port < 1 || port > 65535) { console.error('bad port'); process.exit(2); }
if (!dirs.length) {
    const home = os.homedir();
    for (const n of ['Videos', 'Music', 'Downloads', 'Desktop']) dirs.push(path.join(home, n));
}
const roots = dirs.filter(d => { try { return fs.statSync(d).isDirectory(); } catch (e) { return false; } });
if (!roots.length) { console.error('none of the folders exist: ' + dirs.join(', ')); process.exit(2); }
const rootNames = roots.map((r, i) => {
    let n = path.basename(r) || r;
    if (roots.some((o, j) => j < i && (path.basename(o) || o) === n)) n += ' (' + (i + 1) + ')';
    return n;
});

// ---- file types. okai decodes VP9 / H.264 video and Opus / AAC audio in WebM and MP4 containers.
const TYPES = {
    '.webm': 'video/webm', '.mp4': 'video/mp4', '.m4v': 'video/mp4', '.m4a': 'audio/mp4', '.mkv': 'video/x-matroska',
    '.mka': 'audio/x-matroska', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg', '.oga': 'audio/ogg', '.opus': 'audio/ogg',
    '.wav': 'audio/wav', '.flac': 'audio/flac', '.aac': 'audio/aac', '.mov': 'video/quicktime', '.avi': 'video/x-msvideo',
    '.wmv': 'video/x-ms-wmv', '.flv': 'video/x-flv', '.ts': 'video/mp2t', '.mpg': 'video/mpeg', '.mpeg': 'video/mpeg', '.ogv': 'video/ogg',
    '.3gp': 'video/3gpp', '.txt': 'text/plain; charset=utf-8', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.png': 'image/png',
    '.gif': 'image/gif'
};
const LIKELY = new Set(['.webm', '.mp4', '.m4v', '.m4a']);        // containers okai plays (if the codecs inside are VP9/H.264/Opus/AAC)
const MEDIA = new Set(Object.keys(TYPES).filter(e => /^(video|audio)\//.test(TYPES[e])));
const ext = n => path.extname(n).toLowerCase();
const isAudio = n => /^audio\//.test(TYPES[ext(n)] || '');

// ---- helpers
const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const encPath = parts => parts.map(encodeURIComponent).join('/');
function fmtSize(n) {
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(0) + ' KB';
    if (n < 1073741824) return (n / 1048576).toFixed(1) + ' MB';
    return (n / 1073741824).toFixed(2) + ' GB';
}
const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

// "/b/2/a/b%20c" -> { root: 2, parts: ['a','b c'], abs: '<root>\a\b c' } or null when the path is not allowed
function resolveUrl(prefix, pathname) {
    const rest = pathname.slice(prefix.length);                 // "2/a/b%20c"
    const segs = rest.split('/');
    const ri = parseInt(segs[0], 10);
    if (!(ri >= 0 && ri < roots.length) || String(ri) !== segs[0]) return null;
    const parts = [];
    for (const raw of segs.slice(1)) {
        if (raw === '') continue;
        let s;
        try { s = decodeURIComponent(raw); } catch (e) { return null; }
        if (s === '.' || s === '..' || /[\\/\0:*?"<>|]/.test(s)) return null;      // no traversal, no drive letters / streams / wildcards
        parts.push(s);
    }
    const abs = path.join(roots[ri], ...parts);
    const rel = path.relative(roots[ri], abs);
    if (rel === '..' || rel.startsWith('..' + path.sep) || path.isAbsolute(rel)) return null;
    return { root: ri, parts, abs };
}

const CSS = `body{margin:0;background:#15171c;color:#e8eaf0;font:20px sans-serif}
a{color:#7fb2ff;text-decoration:none}
.wrap{padding:24px 32px}
h1{font-size:28px;margin:0 0 6px 0}
.crumbs{font-size:20px;margin:0 0 18px 0;color:#9aa3b5}
.row{display:flex;justify-content:space-between;padding:9px 12px;border-bottom:1px solid #262a33;color:#e8eaf0}
.sz{color:#8a93a6;margin-left:24px}
.dir{color:#ffd479}
.ok{color:#8be28b}
.dim{color:#8a93a6}
.note{color:#8a93a6;font-size:17px;margin-top:22px}
button{font:20px sans-serif;padding:8px 18px;margin:0 6px 0 0;background:#2b3140;color:#e8eaf0;border:1px solid #3d4558}
#bar{height:16px;background:#2b3140;margin:14px 0;width:100%}
#fill{height:16px;background:#7fb2ff;width:0}
#msg{color:#ffb4a8;margin-top:12px}`;

function page(title, body) {
    return '<!doctype html><html><head><meta charset="utf-8"><title>' + esc(title) + '</title><style>' + CSS +
           '</style></head><body><div class="wrap">' + body + '</div></body></html>';
}

function crumbs(ri, parts) {
    let h = '<a href="/">Home</a>';
    let acc = [];
    h += ' / <a href="/b/' + ri + '/">' + esc(rootNames[ri]) + '</a>';
    for (let i = 0; i < parts.length; i++) {
        acc.push(parts[i]);
        h += ' / ' + (i === parts.length - 1 ? esc(parts[i]) : '<a href="/b/' + ri + '/' + encPath(acc) + '/">' + esc(parts[i]) + '</a>');
    }
    return '<div class="crumbs">' + h + '</div>';
}

function homePage() {
    let b = '<h1>Files on this PC</h1><div class="crumbs">Pick a folder</div>';
    roots.forEach((r, i) => { b += '<a class="row" href="/b/' + i + '/"><span class="dir">' + esc(rootNames[i]) + '</span> <span class="dim">' + esc(r) + '</span></a>'; });
    b += '<div class="note">okai plays WebM and MP4 files (VP9 or H.264 video, Opus or AAC sound). Other files are listed but most will not play.</div>';
    return page('Files', b);
}

function listPage(u) {
    let names;
    try { names = fs.readdirSync(u.abs, { withFileTypes: true }); } catch (e) { return null; }
    const dirsL = [], filesL = [];
    for (const d of names) {
        if (d.name.startsWith('.')) continue;
        let isDir = d.isDirectory(), size = 0;
        if (d.isSymbolicLink()) { try { const st = fs.statSync(path.join(u.abs, d.name)); isDir = st.isDirectory(); size = st.size; } catch (e) { continue; } }
        if (isDir) dirsL.push(d.name);
        else if (d.isFile() || d.isSymbolicLink()) {
            if (!size) { try { size = fs.statSync(path.join(u.abs, d.name)).size; } catch (e) { continue; } }
            filesL.push({ name: d.name, size });
        }
    }
    dirsL.sort(collator.compare);
    filesL.sort((a, b) => collator.compare(a.name, b.name));
    let b = '<h1>' + esc(u.parts.length ? u.parts[u.parts.length - 1] : rootNames[u.root]) + '</h1>' + crumbs(u.root, u.parts);
    for (const n of dirsL) b += '<a class="row" href="/b/' + u.root + '/' + encPath(u.parts.concat(n)) + '/"><span class="dir">' + esc(n) + '/</span></a>';
    let media = 0;
    for (const f of filesL) {
        const e = ext(f.name);
        if (MEDIA.has(e)) {
            media++;
            const cls = LIKELY.has(e) ? 'ok' : 'dim';
            b += '<a class="row" href="/p/' + u.root + '/' + encPath(u.parts.concat(f.name)) + '"><span class="' + cls + '">' + '&#9654; ' +
                 esc(f.name) + '</span><span class="sz">' + fmtSize(f.size) + '</span></a>';
        } else {
            b += '<div class="row dim">' + esc(f.name) + '<span class="sz">' + fmtSize(f.size) + '</span></div>';
        }
    }
    if (!dirsL.length && !filesL.length) b += '<div class="note">This folder is empty.</div>';
    else if (!media) b += '<div class="note">No video or audio files in this folder.</div>';
    return page(u.parts.length ? u.parts[u.parts.length - 1] : rootNames[u.root], b);
}

function playerPage(u) {
    const name = u.parts[u.parts.length - 1];
    const src = '/f/' + u.root + '/' + encPath(u.parts);
    const up = '/b/' + u.root + '/' + encPath(u.parts.slice(0, -1)) + (u.parts.length > 1 ? '/' : '');
    const audio = isAudio(name);
    let b = '<h1>' + esc(name) + '</h1>' + crumbs(u.root, u.parts.slice(0, -1)) +
        (audio ? '<audio id="v" src="' + src + '" preload="auto"></audio>' :
                 '<video id="v" src="' + src + '" preload="auto" style="width:100%;max-width:1600px;max-height:60vh;background:#000"></video>') +
        '<div id="bar"><div id="fill"></div></div>' +
        '<div><button id="pp">Play</button><button id="b30">-30s</button><button id="b10">-10s</button><button id="f10">+10s</button>' +
        '<button id="f30">+30s</button><button id="mu">Mute</button> <span id="tm">0:00 / 0:00</span></div>' +
        '<div id="msg"></div><div class="note"><a href="' + up + '">&larr; back to the folder</a></div>' +
        `<script>
var v = document.getElementById('v'), pp = document.getElementById('pp'), fill = document.getElementById('fill'), tm = document.getElementById('tm'), msg = document.getElementById('msg');
function t(s) { s = Math.max(0, Math.floor(s || 0)); var h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, x = s % 60; return (h ? h + ':' + (m < 10 ? '0' : '') : '') + m + ':' + (x < 10 ? '0' : '') + x; }
function show() {
  var d = v.duration;
  tm.textContent = t(v.currentTime) + ' / ' + (isFinite(d) ? t(d) : '--:--');
  fill.style.width = (isFinite(d) && d > 0 ? Math.min(100, 100 * v.currentTime / d) : 0) + '%';
  pp.textContent = v.paused ? (v.ended ? 'Replay' : 'Play') : 'Pause';
}
function seek(dt) { var d = v.duration; var to = v.currentTime + dt; if (to < 0) to = 0; if (isFinite(d) && to > d) to = d; v.currentTime = to; show(); }
pp.onclick = function () { if (v.paused) { if (v.ended) v.currentTime = 0; var p = v.play(); if (p && p.catch) p.catch(function () {}); } else v.pause(); show(); };
document.getElementById('b30').onclick = function () { seek(-30); };
document.getElementById('b10').onclick = function () { seek(-10); };
document.getElementById('f10').onclick = function () { seek(10); };
document.getElementById('f30').onclick = function () { seek(30); };
document.getElementById('mu').onclick = function () { v.muted = !v.muted; this.textContent = v.muted ? 'Unmute' : 'Mute'; };
document.getElementById('bar').onclick = function (e) {
  var r = this.getBoundingClientRect(), d = v.duration;
  if (isFinite(d) && r.width > 0) { v.currentTime = d * Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)); show(); }
};
['timeupdate', 'play', 'pause', 'ended', 'durationchange', 'loadedmetadata', 'seeked'].forEach(function (n) { v.addEventListener(n, show); });
v.addEventListener('error', function () {
  msg.textContent = 'okai cannot play this file here (the container or codec inside is not supported: it plays VP9 / H.264 video and Opus / AAC sound in WebM and MP4).';
  console.log('PLAYER error ' + (v.error && v.error.code));
});
v.addEventListener('playing', function () { console.log('PLAYER playing ' + (v.videoWidth || 0) + 'x' + (v.videoHeight || 0)); });
var stuck = 0;
setInterval(function () {      // asked to play but nothing arrives: usually a codec okai cannot decode
  show();
  if (!v.paused && v.readyState < 3 && !v.error) { if (++stuck === 16) msg.textContent = 'Nothing is playing yet. If this stays, the video or sound inside the file uses a codec okai cannot decode.'; }
  else { if (stuck >= 16 && v.readyState >= 3) msg.textContent = ''; stuck = 0; }
}, 500);
var p0 = v.play(); if (p0 && p0.catch) p0.catch(function () { msg.textContent = 'Press Play to start.'; });
show();
</script>`;
    return page(name, b);
}

// ---- raw file with Range support
function sendFile(req, res, abs, name) {
    fs.stat(abs, (err, st) => {
        if (err || !st.isFile()) return plain(res, 404, 'not found');
        const type = TYPES[ext(name)] || 'application/octet-stream';
        const size = st.size;
        const h = { 'Content-Type': type, 'Accept-Ranges': 'bytes', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' };
        let start = 0, end = size - 1, code = 200;
        const rg = req.headers['range'];
        if (rg) {
            const m = /^bytes=(\d*)-(\d*)$/.exec(rg.trim());
            if (!m || (m[1] === '' && m[2] === '')) { h['Content-Range'] = 'bytes */' + size; res.writeHead(416, h); return res.end(); }
            if (m[1] === '') { const n = parseInt(m[2], 10); start = Math.max(0, size - n); }
            else { start = parseInt(m[1], 10); if (m[2] !== '') end = Math.min(end, parseInt(m[2], 10)); }
            if (start >= size || start > end) { h['Content-Range'] = 'bytes */' + size; res.writeHead(416, h); return res.end(); }
            code = 206;
            h['Content-Range'] = 'bytes ' + start + '-' + end + '/' + size;
        }
        h['Content-Length'] = size === 0 ? 0 : end - start + 1;
        res.writeHead(code, h);
        if (req.method === 'HEAD' || size === 0) return res.end();
        const rs = fs.createReadStream(abs, { start, end });
        rs.on('error', () => res.destroy());
        res.on('close', () => rs.destroy());
        rs.pipe(res);
    });
}

function plain(res, code, text) {
    res.writeHead(code, { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(text);
}
function html(res, code, body) {
    const buf = Buffer.from(body, 'utf8');
    res.writeHead(code, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': buf.length, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
    res.end(buf);
}

// the Host header must name this machine: a web page cannot make okai (or a browser) read these files through a DNS-rebinding name
const HOST_OK = new RegExp('^(localhost|127\\.0\\.0\\.1|10\\.0\\.2\\.2|\\[::1\\])(:' + port + ')?$', 'i');

const server = http.createServer((req, res) => {
    if (verbose) res.on('finish', () => console.log(res.statusCode + ' ' + req.method + ' ' + req.url + (req.headers['range'] ? '  [' + req.headers['range'] + ']' : '') + (res.getHeader('content-range') ? '  -> ' + res.getHeader('content-range') : '')));
    if (req.method !== 'GET' && req.method !== 'HEAD') return plain(res, 405, 'method not allowed');
    if (!HOST_OK.test(req.headers['host'] || '') && !(process.env.FILESRV_ANYHOST === '1')) return plain(res, 403, 'forbidden host');
    let pathname;
    try { pathname = new URL(req.url, 'http://x').pathname; } catch (e) { return plain(res, 400, 'bad request'); }
    if (pathname === '/') {
        if (roots.length === 1) { res.writeHead(302, { Location: '/b/0/' }); return res.end(); }
        return html(res, 200, homePage());
    }
    if (pathname === '/favicon.ico') { res.writeHead(204); return res.end(); }
    let u;
    if (pathname.startsWith('/b/')) {
        u = resolveUrl('/b/', pathname);
        if (!u) return plain(res, 404, 'not found');
        const body = listPage(u);
        return body === null ? plain(res, 404, 'not found') : html(res, 200, body);
    }
    if (pathname.startsWith('/p/') || pathname.startsWith('/f/')) {
        u = resolveUrl(pathname.slice(0, 3), pathname);
        if (!u || !u.parts.length) return plain(res, 404, 'not found');
        if (pathname[1] === 'f') return sendFile(req, res, u.abs, u.parts[u.parts.length - 1]);
        let st;
        try { st = fs.statSync(u.abs); } catch (e) { return plain(res, 404, 'not found'); }
        if (!st.isFile()) return plain(res, 404, 'not found');
        return html(res, 200, playerPage(u));
    }
    plain(res, 404, 'not found');
});

server.on('error', e => { console.error(e.code === 'EADDRINUSE' ? 'port ' + port + ' is already in use (try --port N)' : String(e)); process.exit(1); });
server.listen(port, host, () => {
    console.log('Serving ' + roots.length + ' folder(s) on http://' + host + ':' + port + '/');
    roots.forEach((r, i) => console.log('  ' + rootNames[i] + ' = ' + r));
    console.log('');
    console.log('In okai (inside the emulator) open:  http://10.0.2.2:' + port + '/');
    console.log('Press Ctrl+C to stop.');
});

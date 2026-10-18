// test_filesrv.js - HTTP-level checks of tools/media/filesrv.js: listing, escaping, Range, traversal, Host header.
//   node tests/media/test_filesrv.js
'use strict';
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'filesrv-'));
const root = path.join(tmp, 'media');
const secret = path.join(tmp, 'secret.txt');
fs.mkdirSync(path.join(root, 'Clips & Cuts'), { recursive: true });
fs.mkdirSync(path.join(root, 'Empty'));
const data = Buffer.alloc(100000);
for (let i = 0; i < data.length; i++) data[i] = (i * 7 + (i >> 8)) & 255;
fs.writeFileSync(path.join(root, 'a b.webm'), data);
fs.writeFileSync(path.join(root, 'Clips & Cuts', 'xé.mp4'), data.subarray(0, 5000));
fs.writeFileSync(path.join(root, 'song.mp3'), 'x');
fs.writeFileSync(path.join(root, 'zero.webm'), '');
fs.writeFileSync(path.join(root, 'notes.txt'), 'hi');
fs.writeFileSync(secret, 'SECRET');

const PORT = 18010 + (process.pid % 500);
let fails = 0, total = 0;
function check(name, ok, extra) { total++; if (!ok) { fails++; console.log('FAIL ' + name + (extra ? ' :: ' + extra : '')); } }

function get(p, headers, method) {
    return new Promise((resolve, reject) => {
        const req = http.request({ host: '127.0.0.1', port: PORT, path: p, method: method || 'GET', headers: Object.assign({}, headers || {}) }, res => {
            const chunks = [];
            res.on('data', c => chunks.push(c));
            res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }));
        });
        req.on('error', reject);
        req.end();
    });
}

(async () => {
    const srv = spawn(process.execPath, [path.join(__dirname, '..', '..', 'tools', 'media', 'filesrv.js'), '--port', String(PORT), root], { stdio: ['ignore', 'pipe', 'inherit'] });
    await new Promise((res, rej) => { srv.stdout.on('data', d => { if (/Serving/.test(String(d))) res(); }); srv.on('exit', () => rej(new Error('server exited'))); setTimeout(() => rej(new Error('server did not start')), 5000); });
    try {
        let r = await get('/');
        check('single root redirects to its listing', r.status === 302 && r.headers.location === '/b/0/', r.status + ' ' + r.headers.location);
        r = await get('/b/0/');
        let t = r.body.toString();
        check('listing 200 html', r.status === 200 && /text\/html/.test(r.headers['content-type']));
        check('listing has the media file linked to the player', t.includes('href="/p/0/a%20b.webm"'));
        check('listing has the folder with escaped name', t.includes('href="/b/0/Clips%20%26%20Cuts/"') && t.includes('Clips &amp; Cuts'));
        check('non-media file is listed without a link', t.includes('notes.txt') && !t.includes('/p/0/notes.txt'));
        check('mp3 is listed (maybe unsupported)', t.includes('/p/0/song.mp3'));
        check('webm sorts before mp3 only by name (a b first)', t.indexOf('a b.webm') < t.indexOf('song.mp3'));
        r = await get('/b/0/Clips%20%26%20Cuts/');
        check('sub folder listing with unicode file', r.status === 200 && r.body.toString().includes('/p/0/Clips%20%26%20Cuts/x%C3%A9.mp4'));
        r = await get('/b/0/Empty/');
        check('empty folder note', r.status === 200 && /empty/.test(r.body.toString()));

        r = await get('/p/0/a%20b.webm');
        t = r.body.toString();
        check('player page', r.status === 200 && t.includes('<video id="v" src="/f/0/a%20b.webm"') && t.includes('back to the folder'));
        r = await get('/p/0/song.mp3');
        check('audio player uses <audio>', r.status === 200 && r.body.toString().includes('<audio id="v"'));
        r = await get('/p/0/notes.txt');
        check('player page for a text file still renders (not special-cased)', r.status === 200 || r.status === 404);

        r = await get('/f/0/a%20b.webm');
        check('whole file', r.status === 200 && r.body.equals(data) && r.headers['accept-ranges'] === 'bytes' && r.headers['content-type'] === 'video/webm');
        r = await get('/f/0/a%20b.webm', { Range: 'bytes=10-19' });
        check('range 206', r.status === 206 && r.body.equals(data.subarray(10, 20)) && r.headers['content-range'] === 'bytes 10-19/100000', r.headers['content-range']);
        r = await get('/f/0/a%20b.webm', { Range: 'bytes=99990-' });
        check('open-ended range', r.status === 206 && r.body.equals(data.subarray(99990)) && r.headers['content-range'] === 'bytes 99990-99999/100000');
        r = await get('/f/0/a%20b.webm', { Range: 'bytes=-5' });
        check('suffix range', r.status === 206 && r.body.equals(data.subarray(99995)));
        r = await get('/f/0/a%20b.webm', { Range: 'bytes=0-9999999' });
        check('range end past the file is clamped', r.status === 206 && r.body.length === 100000);
        r = await get('/f/0/a%20b.webm', { Range: 'bytes=100000-' });
        check('range start past the end is 416', r.status === 416 && r.headers['content-range'] === 'bytes */100000');
        r = await get('/f/0/a%20b.webm', { Range: 'bytes=abc' });
        check('garbage range is 416', r.status === 416);
        r = await get('/f/0/a%20b.webm', null, 'HEAD');
        check('HEAD has length, no body', r.status === 200 && r.headers['content-length'] === '100000' && r.body.length === 0);
        r = await get('/f/0/zero.webm');
        check('empty file', r.status === 200 && r.body.length === 0);
        r = await get('/f/0/Clips%20%26%20Cuts/x%C3%A9.mp4', { Range: 'bytes=0-99' });
        check('mp4 content type + range', r.status === 206 && r.headers['content-type'] === 'video/mp4' && r.body.length === 100);

        // nothing outside the root
        for (const p of ['/f/0/..%2fsecret.txt', '/f/0/%2e%2e/secret.txt', '/f/0/../secret.txt', '/b/0/..%2f', '/f/0/..%5csecret.txt', '/f/0/%2E%2E%5Csecret.txt',
                         '/f/0/C%3A%5CWindows%5Cwin.ini', '/f/0/a%20b.webm%00.txt', '/f/1/a', '/f/-1/a', '/f/0x0/a%20b.webm', '/p/0/', '/f/0/', '/f/0/..', '/f/0/Clips%20%26%20Cuts/..%2f..%2fsecret.txt']) {
            r = await get(p);
            check('refused ' + p, (r.status === 404 || r.status === 400) && !r.body.toString().includes('SECRET'), String(r.status));
        }
        // a raw request path that tries to climb out (node's http client does not normalise)
        r = await get('/f/0/%2e%2e/%2e%2e/secret.txt');
        check('double dot-dot encoded', r.status === 404);
        r = await get('/f/0/a%20b.webm', { Host: 'evil.example.com' });
        check('foreign Host header is refused', r.status === 403);
        r = await get('/f/0/a%20b.webm', { Host: '10.0.2.2:' + PORT });
        check('10.0.2.2 Host is accepted', r.status === 200);
        r = await get('/f/0/a%20b.webm', { Host: 'localhost:' + PORT });
        check('localhost Host is accepted', r.status === 200);
        r = await get('/f/0/a%20b.webm', null, 'POST');
        check('POST refused', r.status === 405);
        r = await get('/nope');
        check('unknown path 404', r.status === 404);
    } catch (e) {
        check('no exception', false, e && e.stack);
    } finally {
        srv.kill();
        fs.rmSync(tmp, { recursive: true, force: true });
    }
    console.log(fails ? 'FAILED ' + fails + ' of ' + total : 'ALL OK (' + total + ' checks)');
    process.exit(fails ? 1 : 0);
})();

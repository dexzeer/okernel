// libs: pako.js cryptojs.js
// byte crunching: typed arrays, 32-bit integer math, bit ops
var data = new Uint8Array(200000);
for (var i = 0; i < data.length; i++) data[i] = (i % 251) ^ ((rand() * 8) | 0) ^ (i >> 9);
var hex = function (u8) { var s = ''; for (var i = 0; i < u8.length; i++) s += (u8[i] < 16 ? '0' : '') + u8[i].toString(16); return s; };
attempt('deflate', function () { var z = pako.deflate(data, { level: 6 }); return z.length + ' ' + __fnv(hex(z)); });
attempt('roundtrip', function () { var z = pako.gzip(data); var back = pako.ungzip(z); var same = back.length === data.length; for (var i = 0; same && i < data.length; i++) same = back[i] === data[i]; return [z.length, same]; });
attempt('raw strings', function () { return pako.inflate(pako.deflate('hello hello hello hello ünïcödé ✓'), { to: 'string' }); });
attempt('levels', function () { var r = []; for (var l = 0; l <= 9; l += 3) r.push(pako.deflateRaw(data.subarray(0, 30000), { level: l, strategy: l === 3 ? 1 : 0 }).length); return r; });
var C = CryptoJS;
attempt('hashes', function () { return [C.MD5('abc').toString(), C.SHA1('abc').toString(), C.SHA256('The quick brown fox').toString(), C.SHA512('').toString(), C.SHA3('x', { outputLength: 256 }).toString(), C.RIPEMD160('abc').toString()]; });
attempt('hmac', function () { return [C.HmacSHA256('message', 'key').toString(C.enc.Base64), C.PBKDF2('pw', 'salt', { keySize: 8, iterations: 50 }).toString()]; });
attempt('aes', function () {
    var key = C.enc.Hex.parse('000102030405060708090a0b0c0d0e0f'), iv = C.enc.Hex.parse('0f0e0d0c0b0a09080706050403020100');
    var ct = C.AES.encrypt('secret message ✓ with unicode', key, { iv: iv, mode: C.mode.CBC, padding: C.pad.Pkcs7 });
    var pt = C.AES.decrypt(ct, key, { iv: iv }).toString(C.enc.Utf8);
    return [ct.toString(), pt, C.TripleDES.encrypt('x', key, { iv: iv }).toString(), C.RC4.encrypt('y', key).ciphertext.toString()];
});
attempt('encodings', function () { var w = C.enc.Utf8.parse('héllo'); return [C.enc.Base64.stringify(w), C.enc.Hex.stringify(w), C.enc.Utf16.stringify(w), C.enc.Latin1.stringify(w)]; });

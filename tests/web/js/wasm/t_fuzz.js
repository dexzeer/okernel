// Mutation fuzz of the decoder/validator/interpreter: valid random programs are damaged (bit flips, truncation, inserted and
// duplicated bytes) and fed to validate/compile/instantiate/call. Nothing may crash (run it under ASan/UBSan:
// SAN="-fsanitize=address,undefined" sh tests/web/build.sh); the printed counts are informational. Not compared with V8.
const LIMIT = typeof FUZZ_SEEDS !== 'undefined' ? FUZZ_SEEDS : 12;
const MUT = typeof FUZZ_MUTATIONS !== 'undefined' ? FUZZ_MUTATIONS : 150;
let st = 12345;
const rn = n => { st = (Math.imul(st, 1103515245) + 12345) >>> 0; return (st >>> 8) % n; };
function mutate(b) {
    b = Array.from(b);
    const k = rn(6);
    if (k === 0) { const n = 1 + rn(4); for (let i = 0; i < n; i++) b[rn(b.length)] ^= 1 << rn(8); }
    else if (k === 1) b.length = 8 + rn(b.length - 8);
    else if (k === 2) { const at = rn(b.length); b.splice(at, 0, ...Array.from({ length: 1 + rn(6) }, () => rn(256))); }
    else if (k === 3) { const at = 8 + rn(b.length - 8), n = 1 + rn(20); b.splice(rn(b.length), 0, ...b.slice(at, at + n)); }
    else if (k === 4) { const at = 8 + rn(b.length - 8); b.splice(at, 1 + rn(8)); }
    else { const n = 1 + rn(3); for (let i = 0; i < n; i++) b[8 + rn(b.length - 8)] = [0, 1, 0x7f, 0x80, 0xff, 0x0b, 0x0c, 0x40][rn(8)]; }
    return new Uint8Array(b);
}
const stats = { valid: 0, invalid: 0, instantiated: 0, linkfail: 0, calls: 0, traps: 0, other: 0 };
const imports = new Proxy({}, { get: (_, mod) => new Proxy({}, { get: (_, name) => (...a) => a.length ? a[0] : 0 }) });
const seeds = [];
for (let s = 1; s <= LIMIT; s++) { runSeed(s); seeds.push(lastBytes); }
let si = 0, mi = 0;
function step() {
    const t0 = Date.now();
    while (si < seeds.length && Date.now() - t0 < 600) {
      try {
        const m = mutate(seeds[si]);
        mi++;
        let ok = false;
        try { ok = WebAssembly.validate(m); } catch (e) { stats.other++; }
        if (ok) {
            stats.valid++;
            try {
                const ins = new WebAssembly.Instance(new WebAssembly.Module(m), imports);
                stats.instantiated++;
                for (const k of Object.keys(ins.exports)) {
                    const f = ins.exports[k];
                    if (typeof f !== 'function') continue;
                    for (let r = 0; r < 2; r++) {
                        stats.calls++;
                        try { f(...Array.from({ length: f.length }, () => 1)); } catch (e) { if (e && e.name === 'RuntimeError') stats.traps++; }
                    }
                }
            } catch (e) { stats.linkfail++; }
        } else stats.invalid++;
        if (mi >= MUT) { mi = 0; si++; }
      } catch (e) { stats.other++; mi++; if (mi >= MUT) { mi = 0; si++; } }
    }
    if (si < seeds.length) setTimeout(step, 0);
    else { out('fuzz', JSON.stringify(stats)); done(); }
}
step();

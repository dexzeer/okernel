// Reference vectors for ojs number conversion, produced by V8 (node).
//   node tests/ojs/gen_num_vectors.js > build-host/num_vectors.txt
// Line formats (hex = IEEE bits of the double):
//   S <hex> <String(x)>
//   F <hex> <digits> <x.toFixed(digits)>
//   E <hex> <digits> <x.toExponential(digits)>       (digits -1 = undefined)
//   P <hex> <prec> <x.toPrecision(prec)>
//   R <hex> <radix> <x.toString(radix)>              (integers and simple fractions only)
//   N <string> <hex of Number(string)>
const buf = new DataView(new ArrayBuffer(8));
const hex = x => { buf.setFloat64(0, x); return buf.getBigUint64(0).toString(16).padStart(16, '0'); };
let seed = 12345n;
const rnd64 = () => { seed = (seed * 6364136223846793005n + 1442695040888963407n) & 0xFFFFFFFFFFFFFFFFn; return seed; };
const randDouble = () => {
    for (;;) {
        buf.setBigUint64(0, rnd64());
        const x = buf.getFloat64(0);
        if (Number.isFinite(x)) return x;
    }
};
const out = [];
const specials = [0, -0, 1, -1, 0.1, 0.2, 0.3, 1 / 3, 2 / 3, 5e-324, 2.2250738585072014e-308, 2.225073858507201e-308,
    1.7976931348623157e308, 1e21, 1e-7, 1e-6, 123456789012345680000, 0.000001, 1.5, 2.5, -2.5, 1e100, 1e-100,
    9007199254740993, 4.35, 1.005, 0.5, 0.05, 0.005, 1.45, 8.345, 1.255, 1234.5678, 999.995, 0.0000001234,
    100, 1e20, 1e22, 123e-20, 0.1 + 0.2, Math.PI, Math.E, 2 ** 53, 2 ** 64, 2 ** -1074, 2 ** -1022, 3e-323];
const xs = specials.slice();
for (let i = 0; i < 20000; i++) xs.push(randDouble());
for (let i = 0; i < 4000; i++) xs.push(Math.round(Math.random() * 1e6) / 100);   // "money" values
for (let i = 0; i < 2000; i++) xs.push((Math.random() - 0.5) * 2 ** (Math.random() * 120 - 60));
for (const x of xs) {
    out.push(`S ${hex(x)} ${String(x)}`);
    if (Math.abs(x) < 1e21) for (const d of [0, 1, 2, 5, 10, 20]) out.push(`F ${hex(x)} ${d} ${x.toFixed(d)}`);
    for (const d of [-1, 0, 1, 3, 10, 20]) out.push(`E ${hex(x)} ${d} ${d < 0 ? x.toExponential() : x.toExponential(d)}`);
    for (const p of [1, 2, 6, 15, 21]) out.push(`P ${hex(x)} ${p} ${x.toPrecision(p)}`);
}
for (let i = 0; i < 3000; i++) {
    const x = Math.floor(Math.random() * 2 ** 53) * (Math.random() < 0.5 ? -1 : 1);
    for (const r of [2, 8, 16, 36]) out.push(`R ${hex(x)} ${r} ${x.toString(r)}`);
}
for (const x of [0.5, 0.25, 0.75, 0.125, 255.5, -0.5, 1 / 1024]) for (const r of [2, 4, 8, 16, 32])
    out.push(`R ${hex(x)} ${r} ${x.toString(r)}`);
// parsing: random decimal strings of varying length and exponent
const strs = ['0', '-0', '1', '.5', '5.', '1e5', '1E-5', '  42  ', '0x1F', '0b101', '0o17', 'Infinity', '-Infinity',
    '+1.5', '1e400', '1e-400', '2.4703282292062328e-324', '2.4703282292062327e-324', '4.9406564584124654e-324',
    '1.7976931348623158e308', '1.7976931348623159e308', '0.1e1', '00012', '1_000', '', ' ', 'abc', '1.2.3',
    '9007199254740993', '9007199254740995', '123456789012345678901234567890', '0x10000000000000001',
    '1.0000000000000002', '0.30000000000000004', ' 1﻿', '-.5e-3'];
for (let i = 0; i < 20000; i++) {
    let s = '';
    const nd = 1 + Math.floor(Math.random() * (Math.random() < 0.1 ? 120 : 25));
    for (let k = 0; k < nd; k++) s += Math.floor(Math.random() * 10);
    if (Math.random() < 0.5) { const at = Math.floor(Math.random() * nd); s = s.slice(0, at) + '.' + s.slice(at); }
    if (Math.random() < 0.7) s += 'e' + (Math.floor(Math.random() * 700) - 350);
    strs.push(s);
}
for (const s of strs) out.push(`N ${JSON.stringify(s)} ${hex(Number(s))}`);
process.stdout.write(out.join('\n') + '\n');

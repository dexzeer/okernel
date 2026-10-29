// Math fingerprint battery: the values BotGuard-style VMs hash from inside.
// Run under ojs (build-host/ojs_run) and under Node; diff the two outputs.
const out = [];
function h(name, fn) {
    try { out.push(name + '=' + fn()); } catch (e) { out.push(name + '=ERR:' + e.message); }
}
const f64 = new Float64Array(1), u32 = new Uint32Array(f64.buffer);
function hex(x) { f64[0] = x; return (u32[1] >>> 0).toString(16) + ':' + (u32[0] >>> 0).toString(16); }

const INPUTS = [
    0.1, 0.5, 1.1, 1.5, 2.3, 3.7, 10.0, 100.001, 1234.5678, 0.7071067811865476,
    1e-10, 1e10, -0.3, -7.7, 1.7976931348623157e308 * 0.25, Math.PI, Math.E, Math.LN2,
    1.5707963267948966, 3.141592653589793, 0.0000001, 42.42, 6.02e23, 1 / 3,
];
const FNS = ['sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'sinh', 'cosh', 'tanh', 'asinh', 'acosh', 'atanh', 'exp', 'expm1', 'log', 'log1p', 'log2', 'log10', 'cbrt', 'sqrt', 'abs', 'floor', 'ceil', 'round', 'trunc'];
for (const fn of FNS) h('Math.' + fn, () => INPUTS.map(x => hex(Math[fn](x))).join(','));
h('Math.atan2', () => {
    let s = '';
    for (let i = 0; i < INPUTS.length; i++) s += hex(Math.atan2(INPUTS[i], INPUTS[(i + 7) % INPUTS.length])) + ',';
    return s;
});
h('Math.pow', () => {
    let s = '';
    for (let i = 0; i < INPUTS.length; i++) s += hex(Math.pow(INPUTS[i], INPUTS[(i + 5) % INPUTS.length])) + ',';
    return s;
});
h('Math.hypot', () => {
    let s = '';
    for (let i = 0; i < INPUTS.length - 2; i++) s += hex(Math.hypot(INPUTS[i], INPUTS[i + 1], INPUTS[i + 2])) + ',';
    return s;
});
h('fma-ish', () => hex(0.1 + 0.2), () => {});
h('sum-chain', () => { let s = 0.0; for (let i = 1; i <= 1000; i++) s += 1 / i; return hex(s); });
h('mul-chain', () => { let s = 1.0; for (let i = 1; i <= 100; i++) s = s * 1.0000001 + 1e-13; return hex(s); });
h('newton', () => { let x = 2.0; for (let i = 0; i < 60; i++) x = x - (x * x - 2) / (2 * x); return hex(x); });
h('Math.fround', () => INPUTS.map(x => hex(Math.fround(x))).join(','));
h('imul', () => [0x7fffffff, 123456789, -987654321].map(a => [1, 2, 65537].map(b => Math.imul(a, b)).join(',')).join(';'));
h('clamp-cast', () => { let s = ''; for (const x of [0.5, 1.5, 2.5, -0.5, -1.5, 3.7]) s += (x | 0) + ',' + (x >>> 0) + ','; return s; });
h('divmod', () => { let s = ''; for (const p of [[7, 3], [-7, 3], [7, -3], [123456789, 10000], [-123456789, -977]]) s += (p[0] % p[1]) + ':' + Math.trunc(p[0] / p[1]) + ','; return s; });
h('date-epoch', () => Math.floor(Date.UTC(2026, 9, 28, 12, 0, 0) / 1000));
h('toFixed', () => [0.1, 1 / 3, 2.675, 1e21, 123.456].map(x => x.toFixed(x < 1 ? 20 : 10)).join(';'));
h('toPrecision', () => [0.000123456, 123456.789].map(x => x.toPrecision(8)).join(';'));
h('toString-radix', () => [0.1, 1 / 3, 255.5].map(x => x.toString(16) + '|' + x.toString(36)).join(';'));
h('parse-float', () => ['0.1', '1e308', '3.141592653589793238462643', '0x1p-1'].map(s => hex(parseFloat(s))).join(','));
console.log(out.join('\n'));

// Every plain numeric operator against a set of interesting operands (results printed; compared with V8's).
const V = {
    i32: [0, 1, -1, 2, 7, 31, 32, 33, 0x7fffffff, -0x80000000, 0x12345678, -12345, 255, 65535, 0x80],
    i64: [0n, 1n, -1n, 2n, 63n, 64n, 65n, 0x7fffffffffffffffn, -0x8000000000000000n, 0x123456789abcdef0n, -12345n, 255n, 4294967296n, 4294967295n, -4294967297n],
    f32: [0, -0, 1, -1, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 3.14159, 1e10, -1e10, 2147483648, -2147483904, 4294967296, 1e30, 1e-30, Infinity, -Infinity, NaN, 16777217, 0.1],
    f64: [0, -0, 1, -1, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 3.14159, 1e10, -1e10, 2147483648, -2147483649, 4294967296, 9223372036854775808, -9223372036854777856, 18446744073709551616, 1e300, 1e-300, Infinity, -Infinity, NaN, 9007199254740993, 0.1],
};
const TY = { i: 'i32', I: 'i64', f: 'f32', F: 'f64' };
const CODE = { i: WB.T.i32, I: WB.T.i64, f: WB.T.f32, F: WB.T.f64 };
const un = (base, list, a, o) => list.map((n, i) => [base + i, n, [a], o]);
const bin = (base, list, a, o) => list.map((n, i) => [base + i, n, [a, a], o]);
const CMP = ['eq', 'ne', 'lt_s', 'lt_u', 'gt_s', 'gt_u', 'le_s', 'le_u', 'ge_s', 'ge_u'];
const ARITH = ['add', 'sub', 'mul', 'div_s', 'div_u', 'rem_s', 'rem_u', 'and', 'or', 'xor', 'shl', 'shr_s', 'shr_u', 'rotl', 'rotr'];
const FUN = ['abs', 'neg', 'ceil', 'floor', 'trunc', 'nearest', 'sqrt'];
const FBIN = ['add', 'sub', 'mul', 'div', 'min', 'max', 'copysign'];
const FCMP = ['eq', 'ne', 'lt', 'gt', 'le', 'ge'];
const OPS = [
    [0x45, 'i32.eqz', ['i'], 'i'], ...bin(0x46, CMP.map(n => 'i32.' + n), 'i', 'i'),
    [0x50, 'i64.eqz', ['I'], 'i'], ...bin(0x51, CMP.map(n => 'i64.' + n), 'I', 'i'),
    ...bin(0x5b, FCMP.map(n => 'f32.' + n), 'f', 'i'), ...bin(0x61, FCMP.map(n => 'f64.' + n), 'F', 'i'),
    ...un(0x67, ['i32.clz', 'i32.ctz', 'i32.popcnt'], 'i', 'i'), ...bin(0x6a, ARITH.map(n => 'i32.' + n), 'i', 'i'),
    ...un(0x79, ['i64.clz', 'i64.ctz', 'i64.popcnt'], 'I', 'I'), ...bin(0x7c, ARITH.map(n => 'i64.' + n), 'I', 'I'),
    ...un(0x8b, FUN.map(n => 'f32.' + n), 'f', 'f'), ...bin(0x92, FBIN.map(n => 'f32.' + n), 'f', 'f'),
    ...un(0x99, FUN.map(n => 'f64.' + n), 'F', 'F'), ...bin(0xa0, FBIN.map(n => 'f64.' + n), 'F', 'F'),
    [0xa7, 'i32.wrap_i64', ['I'], 'i'], [0xa8, 'i32.trunc_f32_s', ['f'], 'i'], [0xa9, 'i32.trunc_f32_u', ['f'], 'i'], [0xaa, 'i32.trunc_f64_s', ['F'], 'i'], [0xab, 'i32.trunc_f64_u', ['F'], 'i'],
    [0xac, 'i64.extend_i32_s', ['i'], 'I'], [0xad, 'i64.extend_i32_u', ['i'], 'I'], [0xae, 'i64.trunc_f32_s', ['f'], 'I'], [0xaf, 'i64.trunc_f32_u', ['f'], 'I'], [0xb0, 'i64.trunc_f64_s', ['F'], 'I'], [0xb1, 'i64.trunc_f64_u', ['F'], 'I'],
    [0xb2, 'f32.convert_i32_s', ['i'], 'f'], [0xb3, 'f32.convert_i32_u', ['i'], 'f'], [0xb4, 'f32.convert_i64_s', ['I'], 'f'], [0xb5, 'f32.convert_i64_u', ['I'], 'f'], [0xb6, 'f32.demote_f64', ['F'], 'f'],
    [0xb7, 'f64.convert_i32_s', ['i'], 'F'], [0xb8, 'f64.convert_i32_u', ['i'], 'F'], [0xb9, 'f64.convert_i64_s', ['I'], 'F'], [0xba, 'f64.convert_i64_u', ['I'], 'F'], [0xbb, 'f64.promote_f32', ['f'], 'F'],
    [0xbc, 'i32.reinterpret_f32', ['f'], 'i'], [0xbd, 'i64.reinterpret_f64', ['F'], 'I'], [0xbe, 'f32.reinterpret_i32', ['i'], 'f'], [0xbf, 'f64.reinterpret_i64', ['I'], 'F'],
    [0xc0, 'i32.extend8_s', ['i'], 'i'], [0xc1, 'i32.extend16_s', ['i'], 'i'], [0xc2, 'i64.extend8_s', ['I'], 'I'], [0xc3, 'i64.extend16_s', ['I'], 'I'], [0xc4, 'i64.extend32_s', ['I'], 'I'],
];
const SAT = [['i32.trunc_sat_f32_s', 'f', 'i'], ['i32.trunc_sat_f32_u', 'f', 'i'], ['i32.trunc_sat_f64_s', 'F', 'i'], ['i32.trunc_sat_f64_u', 'F', 'i'], ['i64.trunc_sat_f32_s', 'f', 'I'], ['i64.trunc_sat_f32_u', 'f', 'I'], ['i64.trunc_sat_f64_s', 'F', 'I'], ['i64.trunc_sat_f64_u', 'F', 'I']];
SAT.forEach(([n, a, o], i) => OPS.push([[0xfc, i], n, [a], o]));
const fmt = r => typeof r === 'number' && r !== r ? 'NaN' : (typeof r === 'number' && Object.is(r, -0)) ? '-0' : typeof r === 'bigint' ? r + 'n' : String(r);
let count = 0;
const fnv = str => { let h = 0x811c9dc5; for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 0x01000193); } return (h >>> 0).toString(16); };
for (const [code, name, ins, outT] of OPS) {
    if (typeof ONLY !== 'undefined' && !name.includes(ONLY)) continue;
    const params = ins.map(c => CODE[c]);
    const body = [...ins.flatMap((_, i) => [0x20, i]), ...(Array.isArray(code) ? [code[0], ...WB.uleb(code[1])] : [code])];
    const f = inst(simple(params, [CODE[outT]], [], body)).exports.f;
    const pools = ins.map(c => V[TY[c]]);
    const combos = ins.length === 1 ? pools[0].map(a => [a]) : pools[0].flatMap(a => pools[1].map(b => [a, b]));
    const rows = [];
    for (const args of combos) {
        let r;
        try { r = fmt(f(...args)); } catch (e) { r = e.name; }
        rows.push(r);
        count++;
    }
    if (typeof DETAIL !== 'undefined') { for (let k = 0; k < rows.length; k += 40) out(name, k, rows.slice(k, k + 40).join(' ')); }
    else out(name, rows.length, fnv(rows.join(' ')));
}
out('cases', count);
done();

// A tiny WebAssembly assembler + output helpers shared by the wasm tests; the same scripts run under node (expected
// output) and under the kernel's JS engine (tests/web/test_wasm.sh diffs them).
(function (G) {
const lines = [];
G.out = (...a) => lines.push(a.map(show).join(' '));
G.done = () => { for (const l of lines) console.log(l); console.log('WASM-DONE'); };
function show(v) {
    if (typeof v === 'number') { if (v !== v) return 'NaN'; if (Object.is(v, -0)) return '-0'; return String(v); }
    if (typeof v === 'bigint') return v + 'n';
    if (v && typeof v === 'object' && v.name && /Error$/.test(v.name)) return v.name;
    if (Array.isArray(v)) return '[' + v.map(show).join(',') + ']';
    if (v instanceof ArrayBuffer) return 'AB(' + v.byteLength + ')';
    if (ArrayBuffer.isView(v)) return 'V' + Array.from(v).join('.');
    return String(v);
}
G.errName = f => { try { f(); return 'ok'; } catch (e) { return e && e.name ? e.name : String(e); } };
const uleb = n => { const b = []; n = BigInt(n); do { let x = Number(n & 0x7Fn); n >>= 7n; if (n) x |= 0x80; b.push(x); } while (n); return b; };
const sleb = n => { const b = []; n = BigInt(n); for (;;) { const x = Number(n & 0x7Fn); n >>= 7n; if ((n === 0n && !(x & 0x40)) || (n === -1n && (x & 0x40))) { b.push(x); return b; } b.push(x | 0x80); } };
const str = s => { const u = Array.from(new TextEncoder().encode(s)); return [...uleb(u.length), ...u]; };
const vec = items => [...uleb(items.length), ...items.flat()];
const sec = (id, body) => body === null ? [] : [id, ...uleb(body.length), ...body];
const T = { i32: 0x7f, i64: 0x7e, f32: 0x7d, f64: 0x7c, funcref: 0x70, externref: 0x6f };
const f32b = x => Array.from(new Uint8Array(new Float32Array([x]).buffer));
const f64b = x => Array.from(new Uint8Array(new Float64Array([x]).buffer));
G.WB = { T, uleb, sleb, str, vec, sec, f32b, f64b };
G.i32 = n => [0x41, ...sleb(n | 0)];
G.i64 = n => [0x42, ...sleb(BigInt.asIntN(64, BigInt(n)))];
G.f32 = x => [0x43, ...f32b(x)];
G.f64 = x => [0x44, ...f64b(x)];
G.lget = i => [0x20, ...uleb(i)]; G.lset = i => [0x21, ...uleb(i)]; G.ltee = i => [0x22, ...uleb(i)];
G.gget = i => [0x23, ...uleb(i)]; G.gset = i => [0x24, ...uleb(i)];
G.call = i => [0x10, ...uleb(i)];
G.br = i => [0x0c, ...uleb(i)]; G.brif = i => [0x0d, ...uleb(i)];
G.mem = (op, align, off) => [op, ...uleb(align), ...uleb(off)];
G.END = 0x0b; G.ELSE = 0x05;
// module builder: { types:[[[params],[results]]], imports:[...], funcs:[{type, locals:[[n,t]], body:[...]}], tables, mems, globals, exports, start, elems, datas, datacount, customs }
G.build = function (m) {
    let b = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0];
    const types = (m.types || []).map(([p, r]) => [0x60, ...vec(p.map(x => [x])), ...vec(r.map(x => [x]))]);
    b = b.concat(sec(1, types.length || m.types ? vec(types) : null));
    const imps = (m.imports || []).map(im => [...str(im.module), ...str(im.name), im.kind, ...im.desc]);
    b = b.concat(sec(2, imps.length ? vec(imps) : null));
    b = b.concat(sec(3, m.funcs && m.funcs.length ? vec(m.funcs.map(f => uleb(f.type))) : null));
    b = b.concat(sec(4, m.tables ? vec(m.tables) : null));
    b = b.concat(sec(5, m.mems ? vec(m.mems) : null));
    b = b.concat(sec(6, m.globals ? vec(m.globals) : null));
    b = b.concat(sec(7, m.exports ? vec(m.exports.map(([n, k, i]) => [...str(n), k, ...uleb(i)])) : null));
    if (m.start !== undefined) b = b.concat(sec(8, uleb(m.start)));
    b = b.concat(sec(9, m.elems ? vec(m.elems) : null));
    if (m.datacount !== undefined) b = b.concat(sec(12, uleb(m.datacount)));
    b = b.concat(sec(10, m.funcs && m.funcs.length ? vec(m.funcs.map(f => { const body = [...vec((f.locals || []).map(([n, t]) => [...uleb(n), t])), ...f.body.flat(), 0x0b]; return [...uleb(body.length), ...body]; })) : null));
    b = b.concat(sec(11, m.datas ? vec(m.datas) : null));
    for (const c of (m.customs || [])) b = b.concat(sec(0, [...str(c[0]), ...c[1]]));
    return new Uint8Array(b);
};
G.inst = (bytes, imports) => new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);
// shorthand: one exported function "f" with a given signature and body
G.simple = function (params, results, locals, body, extra) {
    const m = Object.assign({ types: [[params, results]], funcs: [{ type: 0, locals, body }], exports: [['f', 0, 0]] }, extra || {});
    return build(m);
};
})(typeof globalThis !== 'undefined' ? globalThis : this);

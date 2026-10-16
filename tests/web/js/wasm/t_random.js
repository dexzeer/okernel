// Random well-typed programs (expressions, structured control flow, memory, calls, globals) run on both engines; the
// printed hash of every call's result and of the final memory/globals must agree.
const T = WB.T, u = WB.uleb;
const END = 0x0b;
let seed = 1;
const rnd = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296; };
const R = n => Math.floor(rnd() * n);
const pick = a => a[R(a.length)];
const TYPES = [T.i32, T.i64, T.f32, T.f64];
const NAME = { [T.i32]: 'i', [T.i64]: 'I', [T.f32]: 'f', [T.f64]: 'F' };
const i32c = n => [0x41, ...WB.sleb(n | 0)], i64c = n => [0x42, ...WB.sleb(BigInt.asIntN(64, BigInt(n)))];
const consts = {
    [T.i32]: () => pick([() => i32c(R(8) - 2), () => i32c(R(1 << 20) - (1 << 19)), () => i32c(pick([0x7fffffff, -0x80000000, 255, 65535, -1, 31, 32]))])(),
    [T.i64]: () => pick([() => i64c(R(8) - 2), () => i64c(BigInt(R(1 << 30)) * BigInt(R(1 << 30)) - (1n << 40n)), () => i64c(pick([0x7fffffffffffffffn, -0x8000000000000000n, 4294967296n, -1n, 63n, 64n]))])(),
    [T.f32]: () => [0x43, ...WB.f32b(pick([0, -0, 1, -1.5, 0.5, 100.25, 1e10, -3.75, 2147483648, NaN, Infinity, R(1000) / 7]))],
    [T.f64]: () => [0x44, ...WB.f64b(pick([0, -0, 1, -1.5, 0.5, 100.25, 1e10, -3.75, 4294967296.5, 9.3e18, NaN, -Infinity, R(100000) / 13]))],
};
// unary / binary operator tables per type: [opcode]
const BIN = { [T.i32]: [0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78], [T.i64]: [0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a],
    [T.f32]: [0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98], [T.f64]: [0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6] };
const UN = { [T.i32]: [0x67, 0x68, 0x69, 0xc0, 0xc1], [T.i64]: [0x79, 0x7a, 0x7b, 0xc2, 0xc3, 0xc4], [T.f32]: [0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91], [T.f64]: [0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f] };
const CMP = { [T.i32]: [0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f], [T.i64]: [0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a], [T.f32]: [0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60], [T.f64]: [0x61, 0x62, 0x63, 0x64, 0x65, 0x66] };
// conversions: [from, to, opcode bytes]
const CONV = [[T.i64, T.i32, [0xa7]], [T.f32, T.i32, [0xa8]], [T.f32, T.i32, [0xa9]], [T.f64, T.i32, [0xaa]], [T.f64, T.i32, [0xab]], [T.f32, T.i32, [0xfc, 0]], [T.f64, T.i32, [0xfc, 3]],
    [T.i32, T.i64, [0xac]], [T.i32, T.i64, [0xad]], [T.f32, T.i64, [0xae]], [T.f64, T.i64, [0xb0]], [T.f64, T.i64, [0xb1]], [T.f64, T.i64, [0xfc, 6]], [T.f32, T.i64, [0xfc, 5]],
    [T.i32, T.f32, [0xb2]], [T.i32, T.f32, [0xb3]], [T.i64, T.f32, [0xb4]], [T.i64, T.f32, [0xb5]], [T.f64, T.f32, [0xb6]],
    [T.i32, T.f64, [0xb7]], [T.i32, T.f64, [0xb8]], [T.i64, T.f64, [0xb9]], [T.i64, T.f64, [0xba]], [T.f32, T.f64, [0xbb]],
    ];            // (no reinterpret: it would expose NaN payloads, which engines are free to choose)
const LOADS = { [T.i32]: [[0x28, 2], [0x2c, 0], [0x2d, 0], [0x2e, 1], [0x2f, 1]], [T.i64]: [[0x29, 3], [0x30, 0], [0x31, 0], [0x32, 1], [0x33, 1], [0x34, 2], [0x35, 2]], [T.f32]: [[0x2a, 2]], [T.f64]: [[0x2b, 3]] };
const STORES = { [T.i32]: [[0x36, 2], [0x3a, 0], [0x3b, 1]], [T.i64]: [[0x37, 3], [0x3c, 0], [0x3d, 1], [0x3e, 2]], [T.f32]: [[0x38, 2]], [T.f64]: [[0x39, 3]] };

function genFunc(fi, sigs) {
    const sig = sigs[fi];
    const locals = [];                 // extra locals (type per index after the params)
    const nextLocal = t => { locals.push(t); return sig.params.length + locals.length - 1; };
    for (let k = 0; k < 2 + R(3); k++) nextLocal(pick(TYPES));
    const ltypes = () => [...sig.params, ...locals];
    const labels = [];                 // innermost last: null (void) or a value type
    let budget = 60;
    const counters = new Set();       // loop counters: never written by generated code
    const localOf = (t, write) => { const c = ltypes().map((x, i) => x === t && !(write && counters.has(i)) ? i : -1).filter(i => i >= 0); return c.length ? pick(c) : -1; };
    const callable = t => { const c = []; for (let j = 0; j < fi; j++) if (sigs[j].result === t) c.push(j); return c; };
    function expr(t, d) {
        budget--;
        const choices = ['const'];
        if (localOf(t) >= 0) choices.push('local', 'local');
        if (d > 0 && budget > 0) {
            choices.push('bin', 'bin', 'un', 'sel', 'load', 'ifexpr', 'blockexpr', 'tee');
            if (CONV.some(c => c[1] === t)) choices.push('conv', 'conv');
            if (t === T.i32) choices.push('cmp', 'cmp', 'eqz');
            if (callable(t).length) choices.push('call', 'call');
            choices.push('gget');
        }
        switch (pick(choices)) {
        case 'const': return consts[t]();
        case 'local': return [0x20, ...u(localOf(t))];
        case 'tee': { const l = localOf(t, true); return l < 0 ? consts[t]() : [...expr(t, d - 1), 0x22, ...u(l)]; }
        case 'bin': return [...expr(t, d - 1), ...expr(t, d - 1), pick(BIN[t])];
        case 'un': return [...expr(t, d - 1), pick(UN[t])];
        case 'sel': return [...expr(t, d - 1), ...expr(t, d - 1), ...expr(T.i32, d - 1), 0x1b];
        case 'cmp': { const ot = pick(TYPES); return [...expr(ot, d - 1), ...expr(ot, d - 1), pick(CMP[ot])]; }
        case 'eqz': { const it = pick([T.i32, T.i64]); return [...expr(it, d - 1), it === T.i32 ? 0x45 : 0x50]; }
        case 'conv': { const c = pick(CONV.filter(x => x[1] === t)); return [...expr(c[0], d - 1), ...c[2]]; }
        case 'load': { const [op, al] = pick(LOADS[t]); return [...maskedAddr(d - 1, t), op, al, ...u(t === T.f32 || t === T.f64 ? R(8) * 8 : R(64))]; }
        case 'call': { const j = pick(callable(t)); return [...sigs[j].params.flatMap(p => expr(p, d - 1)), 0x10, ...u(j)]; }
        case 'ifexpr': { labels.push(t); const a = expr(t, d - 1), b = expr(t, d - 1); labels.pop(); return [...expr(T.i32, d - 1), 0x04, t, ...a, 0x05, ...b, END]; }
        case 'blockexpr': { labels.push(t); const body = [...stmts(d - 1, 2), ...maybeBrOut(t, d - 1), ...expr(t, d - 1)]; labels.pop(); return [0x02, t, ...body, END]; }
        case 'gget': { const g = { [T.i32]: 0, [T.i64]: 1, [T.f64]: 2 }[t]; return g === undefined ? consts[t]() : [0x23, g]; }
        }
        return consts[t]();
    }
    // integer data lives in [0, 0x800), float data in [0x1000, 0x1800): NaN payloads (engine specific) never reach integer-visible state
    const maskedAddr = (d, t) => [...expr(T.i32, d), ...i32c(t === T.f32 || t === T.f64 ? 0x7f8 : 0x7ff), 0x71, ...(t === T.f32 ? [...i32c(0x1000), 0x6a] : t === T.f64 ? [...i32c(0x2000), 0x6a] : [])];
    function maybeBrOut(t, d) {      // a value-carrying conditional branch to the innermost label of type t (leaves the stack unchanged)
        if (!labels.length || R(3)) return [];
        const li = labels.length - 1 - R(Math.min(labels.length, 2));
        const lt = labels[li];
        if (lt === 'loop') return [];
        const dist = labels.length - 1 - li;
        if (lt === null) return [...expr(T.i32, d), 0x0d, ...u(dist)];
        return [...expr(lt, d), ...expr(T.i32, d), 0x0d, ...u(dist), 0x1a];
    }
    function stmts(d, n) { const out = []; for (let i = 0; i < n && budget > 0; i++) out.push(...stmt(d)); return out; }
    function stmt(d) {
        budget--;
        const k = pick(['set', 'set', 'store', 'if', 'block', 'loop', 'call', 'gset', 'br', 'brtable', 'drop', 'ret']);
        switch (k) {
        case 'set': { const t = pick(TYPES), l = localOf(t, true); return l < 0 ? [] : [...expr(t, d), 0x21, ...u(l)]; }
        case 'gset': { const t = pick([T.i32, T.i64, T.f64]); return [...expr(t, d), 0x24, { [T.i32]: 0, [T.i64]: 1, [T.f64]: 2 }[t]]; }
        case 'store': { const t = pick(TYPES), [op, al] = pick(STORES[t]); return [...maskedAddr(d, t), ...expr(t, d), op, al, ...u(t === T.f32 || t === T.f64 ? R(8) * 8 : R(64))]; }
        case 'drop': return [...expr(pick(TYPES), d), 0x1a];
        case 'call': { const js = []; for (let j = 0; j < fi; j++) js.push(j); if (!js.length) return []; const j = pick(js); return [...sigs[j].params.flatMap(p => expr(p, d)), 0x10, ...u(j), ...(sigs[j].result === null ? [] : [0x1a])]; }
        case 'if': { if (d <= 0) return []; labels.push(null); const a = stmts(d - 1, 1 + R(2)), b = R(2) ? stmts(d - 1, 1 + R(2)) : null; labels.pop(); return [...expr(T.i32, d - 1), 0x04, 0x40, ...a, ...(b ? [0x05, ...b] : []), END]; }
        case 'block': { if (d <= 0) return []; labels.push(null); const b = [...stmts(d - 1, 1 + R(2)), ...maybeBrOut(null, d - 1), ...stmts(d - 1, 1)]; labels.pop(); return [0x02, 0x40, ...b, END]; }
        case 'loop': {
            if (d <= 0 || labels.filter(x => x === 'loop').length >= 2) return [];
            const c = nextLocal(T.i32);
            counters.add(c);
            labels.push('loop');                              // br targets of loops are only taken through the counter test below
            const body = stmts(d - 1, 1 + R(2));
            labels.pop();
            // counter: loop { body; c++; if c < N br 0 }   (labels inside the body see the loop as a void label: remove 'loop' entries from br choices)
            return [...i32c(0), 0x21, ...u(c), 0x03, 0x40, ...body, 0x20, ...u(c), ...i32c(1), 0x6a, 0x22, ...u(c), ...i32c(2 + R(3)), 0x48, 0x0d, 0, END];
        }
        case 'br': { const vs = labels.map((l, i) => [l, i]).filter(([l]) => l === null); if (!vs.length) return []; const [, i] = pick(vs); return [...expr(T.i32, d - 1), 0x0d, ...u(labels.length - 1 - i)]; }
        case 'brtable': {
            const vs = labels.map((l, i) => [l, i]).filter(([l]) => l === null);
            if (!vs.length || R(2)) return [];
            const ids = [pick(vs)[1], pick(vs)[1], pick(vs)[1]].map(i => labels.length - 1 - i);
            return [...expr(T.i32, d - 1), 0x0e, 2, ...ids.map(i => i)];
        }
        case 'ret': { if (R(4) || d < 2) return []; return [...expr(T.i32, d - 1), 0x04, 0x40, ...(sig.result === null ? [] : expr(sig.result, d - 1)), 0x0f, END]; }
        }
        return [];
    }
    // labels of kind 'loop' must not be branched to by `br`/`br_table` generation: filter them (they are neither null nor typed)
    const body = [...stmts(3, 3 + R(4)), ...(sig.result === null ? [] : expr(sig.result, 3))];
    return { type: fi, locals: locals.map(t => [1, t]), body };
}

const results = [];
function runSeed(s) {
    seed = s * 7919 + 13;
    const nf = 6;
    const sigs = [];
    for (let i = 0; i < nf; i++) sigs.push({ params: Array.from({ length: R(4) }, () => pick(TYPES)), result: R(5) ? pick(TYPES) : null });
    const funcs = [];
    for (let i = 0; i < nf; i++) funcs.push(genFunc(i, sigs));
    const types = sigs.map(g => [g.params, g.result === null ? [] : [g.result]]);
    const bytes = build({
        types, funcs: funcs.map((f, i) => ({ type: i, locals: f.locals, body: f.body })),
        mems: [[0, 1]], globals: [[T.i32, 1, 0x41, 5, 0x0b], [T.i64, 1, 0x42, 7, 0x0b], [T.f64, 1, 0x44, ...WB.f64b(1.5), 0x0b]],
        exports: [...funcs.map((_, i) => ['f' + i, 0, i]), ['mem', 2, 0], ['g0', 3, 0], ['g1', 3, 1], ['g2', 3, 2]],
    });
    globalThis.lastBytes = bytes;
    if (typeof DUMP !== 'undefined' && typeof disasm !== 'undefined') for (let fi = 0; fi < funcs.length; fi++) if (DUMP === 'all' || DUMP === fi) { out('  func ' + fi + ' params=' + sigs[fi].params.join(',') + ' result=' + sigs[fi].result); for (const l of disasm(funcs[fi].body.flat()).split('\n')) out('    ' + l); }
    let e;
    try { e = inst(bytes).exports; } catch (err) { return 'seed ' + s + ' compile:' + err.name; }
    let h = 0x811c9dc5;
    const mix = str => { for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 0x01000193); } };
    const fmt = r => typeof r === 'number' && r !== r ? 'NaN' : (typeof r === 'number' && Object.is(r, -0)) ? '-0' : typeof r === 'bigint' ? r + 'n' : String(r);
    for (let call = 0; call < 24; call++) {
        const fi = call % nf, g = sigs[fi];
        const args = g.params.map(p => p === T.i32 ? R(2000) - 1000 : p === T.i64 ? BigInt(R(2000) - 1000) * 1000003n : p === T.f32 ? Math.fround(R(2000) / 8 - 100) : R(2000) / 8 - 100);
        let r;
        try { r = fmt(e['f' + fi](...args)); } catch (err) { r = err.name; }
        mix(fi + ':' + r + ';');
        if (typeof DETAIL !== 'undefined') out('  call f' + fi + '(' + args.map(fmt).join(',') + ') = ' + r);
    }
    mix(fmt(e.g0.value) + fmt(e.g1.value) + fmt(e.g2.value));
    const mv = new Uint8Array(e.mem.buffer);
    let mh = 0; for (let i = 0; i < 0x800 + 80; i++) mh = (Math.imul(mh, 31) + mv[i]) | 0;
    mix(String(mh));
    if (typeof DETAIL !== 'undefined') { out('  globals', fmt(e.g0.value), fmt(e.g1.value), fmt(e.g2.value), 'mem', mh); const nz = []; for (let i = 0; i < 0x800; i++) if (mv[i]) nz.push(i + ':' + mv[i]); out('  nonzero', nz.join(' ')); }
    return 'seed ' + s + ' ' + (h >>> 0).toString(16);
}
const FIRST = typeof FIRST_SEED !== 'undefined' ? FIRST_SEED : 1, LAST = typeof LAST_SEED !== 'undefined' ? LAST_SEED : 120;
for (let s = FIRST; s <= LAST; s++) out(runSeed(s));
done();

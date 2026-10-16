// the JS API: imports/exports, host functions, globals, tables, reference types, errors (compared with V8)
const T = WB.T, u = WB.uleb;
const END = 0x0b;
const i32 = n => [0x41, ...WB.sleb(n | 0)];
const lget = i => [0x20, ...u(i)];
const call = i => [0x10, ...u(i)];
const mk = (m) => new WebAssembly.Module(build(m));
const log = [];
// ---- host functions of every type, re-entrancy, exceptions
const types = [
    [[T.i32, T.i64, T.f32, T.f64], [T.f64]],          // 0 host: mixes
    [[T.i32], [T.i32, T.i32]],                         // 1 multi-value host
    [[T.externref], [T.externref]],                    // 2 externref identity through the host
    [[T.i32], [T.i32]],                                // 3
    [[], []],                                          // 4
    [[T.i32], [T.i32]],                                // 5 re-entrant
    [[T.funcref], [T.i32]],                            // 6 funcref in
    [[], [T.funcref]],                                 // 7 funcref out
];
const bytes = build({
    types,
    imports: [
        { module: 'h', name: 'mix', kind: 0, desc: [0] },       // func 0
        { module: 'h', name: 'multi', kind: 0, desc: [1] },     // func 1
        { module: 'h', name: 'ext', kind: 0, desc: [2] },       // func 2
        { module: 'h', name: 'boom', kind: 0, desc: [3] },      // func 3
        { module: 'h', name: 'tick', kind: 0, desc: [4] },      // func 4
        { module: 'h', name: 'reenter', kind: 0, desc: [5] },   // func 5
    ],
    funcs: [
        { type: 0, locals: [], body: [lget(0), lget(1), lget(2), lget(3), call(0)] },              // 6 callmix
        { type: 1, locals: [], body: [lget(0), call(1)] },                                          // 7 callmulti
        { type: 2, locals: [], body: [lget(0), call(2)] },                                          // 8 callext
        { type: 3, locals: [], body: [lget(0), call(3)] },                                          // 9 callboom
        { type: 4, locals: [], body: [call(4), call(4)] },                                          // 10 ticktock
        { type: 3, locals: [], body: [lget(0), i32(1), [0x6a], call(5)] },                           // 11 outer: reenter(x+1)
        { type: 3, locals: [], body: [lget(0), i32(10), [0x6c]] },                                  // 12 times10
        { type: 6, locals: [], body: [lget(0), [0xd1]] },                                           // 13 isnull
        { type: 7, locals: [], body: [[0xd2, 12]] },                                                // 14 getfunc (ref.func times10)
    ],
    exports: [['callmix', 0, 6], ['callmulti', 0, 7], ['callext', 0, 8], ['callboom', 0, 9], ['ticktock', 0, 10], ['outer', 0, 11], ['times10', 0, 12], ['isnull', 0, 13], ['getfunc', 0, 14]],
    elems: [[3, 0, 1, 12]],        // declarative segment: makes ref.func 12 valid
});
const ext = { tag: 'extobj' };
let ex;
const h = {
    mix: (a, b, c, d) => { log.push(['mix', a, b, c, d]); return a + Number(b) + c + d; },
    multi: x => [x + 1, x * 2],
    ext: x => { log.push(['ext', x === ext]); return x; },
    boom: x => { const e = new Error('boom' + x); e.tag = x; throw e; },
    tick: () => { log.push('tick'); },
    reenter: x => ex.times10(x),
};
ex = new WebAssembly.Instance(new WebAssembly.Module(bytes), { h }).exports;
out('mix', ex.callmix(1, 2n, 3.5, 4.25), JSON.stringify(log.shift(), (k, v) => typeof v === 'bigint' ? v + 'n' : v));
out('i64-needs-bigint', errName(() => ex.callmix(1, 2, 3, 4)));
out('i64-wrap', ex.callmix(0, 2n ** 64n + 5n, 0, 0), ex.callmix(0, -(2n ** 63n), 0, 0));
out('multi', ex.callmulti(5));
out('ext', ex.callext(ext) === ext, ex.callext(null), ex.callext(undefined), ex.callext(7), ex.callext('s'), log.splice(0).length);
let caught = null; try { ex.callboom(9); } catch (e) { caught = e; }
out('exception', caught && caught.message, caught && caught.tag, caught instanceof Error);
ex.ticktock(); out('tick', log.splice(0));
out('reenter', ex.outer(4), ex.times10(3), ex.times10.length, ex.times10.name, typeof ex.times10);
out('funcref', ex.isnull(null), ex.isnull(ex.times10), errName(() => ex.isnull(() => 1)), ex.getfunc() === ex.times10, ex.getfunc()(7));
out('arity', ex.times10(), ex.times10(1, 2, 3), ex.times10('5'), ex.times10({ valueOf: () => 6 }), ex.times10(2 ** 32 + 3), ex.times10(1.9), ex.times10(-1.9));
// ---- modules: imports / exports / custom sections / validate
const m1 = mk({ types: [[[], []]], imports: [{ module: 'a', name: 'f', kind: 0, desc: [0] }, { module: 'a', name: 'g', kind: 3, desc: [T.i32, 1] }, { module: 'b', name: 't', kind: 1, desc: [0x70, 0, 1] }, { module: 'b', name: 'm', kind: 2, desc: [0, 1] }], funcs: [{ type: 0, locals: [], body: [] }], exports: [['x', 0, 1], ['y', 3, 0]], customs: [['note', [1, 2, 3]], ['note', [4]]] });
out('Module.imports', JSON.stringify(WebAssembly.Module.imports(m1)));
out('Module.exports', JSON.stringify(WebAssembly.Module.exports(m1)));
out('customSections', WebAssembly.Module.customSections(m1, 'note').map(b => Array.from(new Uint8Array(b))), WebAssembly.Module.customSections(m1, 'none').length);
out('validate', WebAssembly.validate(build({ types: [[[], []]], funcs: [{ type: 0, locals: [], body: [] }] })), WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0])), WebAssembly.validate(new Uint8Array([1, 2, 3])), WebAssembly.validate(new Uint8Array(0)));
out('CompileError', errName(() => new WebAssembly.Module(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]))), errName(() => new WebAssembly.Module()), errName(() => new WebAssembly.Module(5)), new WebAssembly.CompileError('x') instanceof Error, new WebAssembly.CompileError('x').name);
// type mismatch bodies
const bad1 = build({ types: [[[], [T.i32]]], funcs: [{ type: 0, locals: [], body: [] }] });
const bad2 = build({ types: [[[], []]], funcs: [{ type: 0, locals: [], body: [[0x6a]] }] });
const bad3 = build({ types: [[[], [T.i32]]], funcs: [{ type: 0, locals: [], body: [i32(1), i32(2)] }] });
const bad4 = build({ types: [[[], []]], funcs: [{ type: 0, locals: [], body: [[0x20, 5]] }] });
const bad5 = build({ types: [[[], []]], funcs: [{ type: 0, locals: [], body: [[0x0c, 3]] }] });
const bad6 = build({ types: [[[], [T.i32]]], funcs: [{ type: 0, locals: [], body: [[0x42, 1], ] }] });
const bad7 = build({ types: [[[], []]], mems: [[0, 1]], funcs: [{ type: 0, locals: [], body: [i32(0), [0x28, 3, 0], [0x1a]] }] });
const ok1 = build({ types: [[[], [T.i32]]], funcs: [{ type: 0, locals: [], body: [[0x00]] }] });                 // unreachable: polymorphic stack
const ok2 = build({ types: [[[], [T.i32]]], funcs: [{ type: 0, locals: [], body: [[0x00], [0x6a]] }] });
const ok3 = build({ types: [[[], [T.i32]]], funcs: [{ type: 0, locals: [], body: [[0x0c, 0]] }] });                // br 0 at function level
out('validation', [bad1, bad2, bad3, bad4, bad5, bad6, bad7].map(b => WebAssembly.validate(b)), [ok1, ok2, ok3].map(b => WebAssembly.validate(b)));
// ---- start function and instantiate promise API
const sbytes = build({ types: [[[], []]], imports: [{ module: 'h', name: 'tick', kind: 0, desc: [0] }], funcs: [{ type: 0, locals: [], body: [call(0)] }], start: 1, exports: [['s', 0, 1]] });
const sm = new WebAssembly.Module(sbytes);
new WebAssembly.Instance(sm, { h: { tick: () => log.push('start') } });
out('start', log.splice(0));
const trapStart = build({ types: [[[], []]], funcs: [{ type: 0, locals: [], body: [[0x00]] }], start: 0 });
out('start-trap', errName(() => new WebAssembly.Instance(new WebAssembly.Module(trapStart))));
// ---- link errors
out('link', errName(() => new WebAssembly.Instance(sm)), errName(() => new WebAssembly.Instance(sm, {})), errName(() => new WebAssembly.Instance(sm, { h: {} })), errName(() => new WebAssembly.Instance(sm, { h: { tick: 5 } })), errName(() => new WebAssembly.Instance(m1, { a: { f() {}, g: 1 }, b: { t: 1, m: 2 } })));
// ---- globals
const gm = mk({ types: [[[], [T.i32]], [[T.i32], []]], imports: [{ module: 'g', name: 'imm', kind: 3, desc: [T.i32, 0] }, { module: 'g', name: 'mut', kind: 3, desc: [T.i32, 1] }, { module: 'g', name: 'big', kind: 3, desc: [T.i64, 0] }],
    funcs: [{ type: 0, locals: [], body: [[0x23, 0], [0x23, 1], [0x6a]] }, { type: 1, locals: [], body: [lget(0), [0x24, 1]] }],
    globals: [[T.f64, 1, 0x44, ...WB.f64b(2.5), 0x0b]],
    exports: [['sum', 0, 0], ['setmut', 0, 1], ['own', 3, 3], ['mut', 3, 1]] });
const gmut = new WebAssembly.Global({ value: 'i32', mutable: true }, 40);
const ge = new WebAssembly.Instance(gm, { g: { imm: 2, mut: gmut, big: 5n } }).exports;
out('globals', ge.sum(), ge.setmut(100), gmut.value, ge.sum(), ge.own.value, ge.mut === gmut, ge.own === ge.own, errName(() => { ge.own.value = 1; }));
out('global-link', errName(() => new WebAssembly.Instance(gm, { g: { imm: 2, mut: 5, big: 5n } })), errName(() => new WebAssembly.Instance(gm, { g: { imm: 2, mut: gmut, big: 5 } })), errName(() => new WebAssembly.Instance(gm, { g: { imm: 'x', mut: gmut, big: 5n } })), errName(() => new WebAssembly.Instance(gm, { g: { imm: 2, mut: new WebAssembly.Global({ value: 'i64', mutable: true }, 1n), big: 5n } })));
const gf = new WebAssembly.Global({ value: 'f32', mutable: true }, 1.1);
const g64 = new WebAssembly.Global({ value: 'i64' }, 2n ** 63n);
out('Global', gf.value, (gf.value = 2.2, gf.value), g64.value, g64.valueOf(), new WebAssembly.Global({ value: 'i32' }).value, new WebAssembly.Global({ value: 'f64', mutable: true }).value, errName(() => new WebAssembly.Global({ value: 'x' })), errName(() => new WebAssembly.Global({ value: 'i64' }, 1)), new WebAssembly.Global({ value: 'externref' }, ext).value === ext, new WebAssembly.Global({ value: 'externref' }).value);
// ---- tables and reference types
const tm = mk({
    types: [[[T.i32], [T.i32]], [[T.i32], [T.externref]], [[T.i32, T.externref], []], [[], [T.i32]], [[T.i32, T.i32, T.i32], []], [[T.i32], [T.funcref]]],
    imports: [{ module: 't', name: 'tab', kind: 1, desc: [0x70, 1, 1, 6] }],
    funcs: [
        { type: 0, locals: [], body: [lget(0), i32(100), [0x6a]] },                                   // 1: +100
        { type: 1, locals: [], body: [lget(0), [0x25, 1]] },                                          // 2: ext.get(i)
        { type: 2, locals: [], body: [lget(0), lget(1), [0x26, 1]] },                                 // 3: ext.set(i, v)
        { type: 3, locals: [], body: [[0xfc, 16, 0]] },                                               // 4: size of imported funcref table
        { type: 4, locals: [], body: [lget(0), lget(1), lget(2), [0xfc, 14, 0, 0]] },                 // 5: table.copy t0 <- t0
        { type: 5, locals: [], body: [lget(0), [0x25, 0]] },                                          // 6: table.get funcref
        { type: 4, locals: [], body: [lget(0), [0xd2, 0], lget(2), [0xfc, 17, 0]] },                   // 7: table.fill(i, ref.func 1, n)
        { type: 0, locals: [], body: [[0xd0, T.funcref], lget(0), [0xfc, 15, 0]] },                    // 8: table.grow(ref.null, n)
    ],
    tables: [[T.externref, 0, 3]],                                                                    // table 1: externref, min 3
    exports: [['inc', 0, 0], ['eget', 0, 1], ['eset', 0, 2], ['size', 0, 3], ['tcopy', 0, 4], ['fget', 0, 5], ['fill', 0, 6], ['grow', 0, 7], ['etab', 1, 1]],
    elems: [[0, 0x41, 0, 0x0b, 1, ...u(0)], [3, 0, 1, 0]],                                              // t0[0] = inc ; declarative (ref.func 1 in fill)
});
const ftab = new WebAssembly.Table({ element: 'anyfunc', initial: 1, maximum: 6 });
const te = new WebAssembly.Instance(tm, { t: { tab: ftab } }).exports;
out('table0', ftab.length, ftab.get(0) === te.inc, ftab.get(0)(1), te.size(), te.fget(0) === te.inc);
out('table-grow', te.grow(2), te.size(), ftab.length, ftab.get(1), te.grow(3), te.grow(1), ftab.grow(0), errName(() => ftab.grow(100)));
te.tcopy(3, 0, 1); out('table-copy', ftab.get(3) === te.inc, errName(() => te.tcopy(5, 0, 5)), errName(() => te.tcopy(0, 5, 5)));
te.fill(1, 0, 3); out('table-fill', ftab.get(1) === te.inc, ftab.get(2) === te.inc, ftab.get(3) === te.inc, ftab.get(4), errName(() => te.fill(5, 0, 3)));
ftab.set(5, te.inc); ftab.set(4, null); out('table-set', ftab.get(5) === te.inc, ftab.get(4), errName(() => ftab.set(6, null)), errName(() => ftab.set(0, () => 1)), errName(() => ftab.get(-1)));
const objs = [{ a: 1 }, 'str', 7, null, undefined];
te.eset(0, objs[0]); te.eset(1, objs[1]); te.eset(2, objs[2]);
out('externref', te.eget(0) === objs[0], te.eget(1), te.eget(2), te.eget(0) === te.etab.get(0), errName(() => te.eget(3)));
te.etab.set(1, objs[0]); out('etab', te.eget(1) === objs[0], te.etab.length, te.etab.grow(1, 'g'), te.etab.get(3), te.etab.length);
const etab2 = new WebAssembly.Table({ element: 'externref', initial: 2 }, 'init');
out('Table', etab2.get(0), errName(() => new WebAssembly.Table({ element: 'i32', initial: 1 })), errName(() => new WebAssembly.Table({ element: 'anyfunc' })), errName(() => new WebAssembly.Table({ element: 'anyfunc', initial: 2, maximum: 1 })), new WebAssembly.Table({ element: 'anyfunc', initial: 2 }, null).get(1));
// import checks for tables
out('table-link', errName(() => new WebAssembly.Instance(tm, { t: { tab: new WebAssembly.Table({ element: 'anyfunc', initial: 0, maximum: 6 }) } })), errName(() => new WebAssembly.Instance(tm, { t: { tab: new WebAssembly.Table({ element: 'anyfunc', initial: 1 }) } })), errName(() => new WebAssembly.Instance(tm, { t: { tab: new WebAssembly.Table({ element: 'anyfunc', initial: 1, maximum: 9 }) } })));
// ---- importing an exported wasm function keeps its identity; wasm-to-wasm calls
const chain = new WebAssembly.Instance(mk({ types: [[[T.i32], [T.i32]]], imports: [{ module: 'w', name: 'f', kind: 0, desc: [0] }], funcs: [{ type: 0, locals: [], body: [lget(0), call(0), call(0)] }], exports: [['twice', 0, 1], ['f', 0, 0]] }), { w: { f: te.inc } }).exports;
out('chain', chain.twice(1), chain.f === te.inc);
out('chain-mismatch', errName(() => new WebAssembly.Instance(mk({ types: [[[], []]], imports: [{ module: 'w', name: 'f', kind: 0, desc: [0] }] }), { w: { f: te.inc } })));
// ---- promise API
WebAssembly.instantiate(bytes, { h }).then(r => {
    out('instantiate', Object.keys(r).sort().join(','), r.module instanceof WebAssembly.Module, r.instance instanceof WebAssembly.Instance, r.instance.exports.times10(2));
    return WebAssembly.instantiate(r.module, { h });
}).then(i => {
    out('instantiate-module', i instanceof WebAssembly.Instance, typeof i.exports.times10);
    return WebAssembly.compile(bytes);
}).then(m => {
    out('compile', m instanceof WebAssembly.Module);
    return WebAssembly.instantiate(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]));
}).then(() => out('should not happen'), e => {
    out('instantiate-error', e.name);
    return WebAssembly.instantiate(bytes, {}).then(() => out('should not happen'), e2 => out('instantiate-import-error', e2.name));
}).then(() => {
    out('namespace', Object.prototype.toString.call(WebAssembly), Object.prototype.toString.call(ftab), Object.prototype.toString.call(ex), typeof WebAssembly.Module, WebAssembly.Module.name, WebAssembly.RuntimeError.name);
    done();
});


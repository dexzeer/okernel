// linear memory: loads/stores, bounds, grow, data segments, bulk memory, JS-side Memory (compared with V8)
const T = WB.T, u = WB.uleb;
const END = 0x0b;
const i32 = n => [0x41, ...WB.sleb(n | 0)];
const lget = i => [0x20, ...u(i)];
const types = [
    [[T.i32], [T.i32]],                       // 0 i32 -> i32 (loads)
    [[T.i32], [T.i64]],                       // 1 i32 -> i64
    [[T.i32], [T.f32]], [[T.i32], [T.f64]],   // 2, 3
    [[T.i32, T.i32], []],                     // 4 store i32 at addr
    [[T.i32, T.i64], []],                     // 5
    [[T.i32, T.f32], []], [[T.i32, T.f64], []], // 6, 7
    [[], [T.i32]],                            // 8
    [[T.i32, T.i32, T.i32], []],              // 9 (a, b, c)
    [[], []],                                 // 10
];
const funcs = [], exports = [];
const add = (name, type, body) => { funcs.push({ type, locals: [], body }); exports.push([name, 0, funcs.length - 1]); };
const ld = (name, type, op) => add(name, type, [lget(0), [op, 0, 0]]);
ld('i32.load', 0, 0x28); ld('i64.load', 1, 0x29); ld('f32.load', 2, 0x2a); ld('f64.load', 3, 0x2b);
ld('i32.load8_s', 0, 0x2c); ld('i32.load8_u', 0, 0x2d); ld('i32.load16_s', 0, 0x2e); ld('i32.load16_u', 0, 0x2f);
ld('i64.load8_s', 1, 0x30); ld('i64.load8_u', 1, 0x31); ld('i64.load16_s', 1, 0x32); ld('i64.load16_u', 1, 0x33); ld('i64.load32_s', 1, 0x34); ld('i64.load32_u', 1, 0x35);
const st = (name, type, op) => add(name, type, [lget(0), lget(1), [op, 0, 0]]);
st('i32.store', 4, 0x36); st('i64.store', 5, 0x37); st('f32.store', 6, 0x38); st('f64.store', 7, 0x39);
st('i32.store8', 4, 0x3a); st('i32.store16', 4, 0x3b); st('i64.store8', 5, 0x3c); st('i64.store16', 5, 0x3d); st('i64.store32', 5, 0x3e);
add('size', 8, [[0x3f, 0]]);
add('grow', 0, [lget(0), [0x40, 0]]);
add('loadoff', 0, [lget(0), [0x28, 2, ...u(65532)]]);                // offset arithmetic: addr + 65532
add('fill', 9, [lget(0), lget(1), lget(2), [0xfc, 11, 0]]);   // dst, value, n
add('copy', 9, [lget(0), lget(1), lget(2), [0xfc, 10, 0, 0]]);  // dst, src, n
add('init', 9, [lget(0), lget(1), lget(2), [0xfc, 8, 1, 0]]);   // dst, src, n from data segment 1
add('drop', 10, [[0xfc, 9, 1]]);
const bytes = build({
    types, funcs,
    mems: [[1, 1, 3]],                                        // min 1, max 3
    exports: exports.concat([['mem', 2, 0]]),
    datacount: 2,
    datas: [
        [0, ...i32(16), END, ...u(5), ...Array.from(new TextEncoder().encode('hello'))],      // active at 16
        [1, ...u(4), 9, 8, 7, 6],                                                             // passive
    ],
});
const inst0 = new WebAssembly.Instance(new WebAssembly.Module(bytes));
const e = inst0.exports, mem = e.mem;
const text = (a, n) => new TextDecoder().decode(new Uint8Array(mem.buffer, a, n));
out('data', text(16, 5), mem.buffer.byteLength, e.size());
e['i32.store'](100, 0x80ff7f01); e['i64.store'](200, 0x8070605040302010n);
out('i32', e['i32.load'](100), e['i32.load8_s'](100), e['i32.load8_u'](101), e['i32.load16_s'](102), e['i32.load16_u'](102), e['i32.load8_s'](103));
out('i64', e['i64.load'](200), e['i64.load8_s'](207), e['i64.load8_u'](207), e['i64.load16_s'](206), e['i64.load16_u'](206), e['i64.load32_s'](204), e['i64.load32_u'](204));
e['f32.store'](300, 1.5); e['f64.store'](308, -2.25);
out('floats', e['f32.load'](300), e['f64.load'](308), e['i32.load'](300), e['f32.load'](308));
e['i32.store8'](400, 0x1234); e['i32.store16'](402, 0x12345678); e['i64.store8'](410, 0x1ffn); e['i64.store16'](412, 0x1ffffn); e['i64.store32'](416, 0x1ffffffffn);
out('narrow', Array.from(new Uint8Array(mem.buffer, 400, 4)), Array.from(new Uint8Array(mem.buffer, 410, 2)), Array.from(new Uint8Array(mem.buffer, 412, 2)), Array.from(new Uint8Array(mem.buffer, 416, 4)));
out('oob', errName(() => e['i32.load'](65533)), errName(() => e['i32.load'](65532)), errName(() => e['i32.load'](-1)), errName(() => e['i64.load'](65529)), errName(() => e['i32.store'](65536, 1)), errName(() => e['i32.store8'](65535, 1)));
out('offset', errName(() => e.loadoff(0)), errName(() => e.loadoff(1)), errName(() => e.loadoff(-1)));
// bulk memory
e.fill(1000, 0xab, 8); out('fill', Array.from(new Uint8Array(mem.buffer, 998, 12)));
e.copy(2000, 16, 5); out('copy', text(2000, 5));
e.copy(18, 16, 5); out('overlap', text(16, 7));
e.init(3000, 1, 3); out('init', Array.from(new Uint8Array(mem.buffer, 3000, 4)));
out('bulk-oob', errName(() => e.fill(65530, 1, 7)), errName(() => e.copy(0, 65530, 7)), errName(() => e.init(0, 2, 3)), errName(() => e.fill(65536, 1, 0)), errName(() => e.fill(65537, 1, 0)));
e.drop(); out('dropped', errName(() => e.init(0, 0, 1)), errName(() => e.init(0, 0, 0)));
// grow
const before = mem.buffer;
out('grow', e.size(), e.grow(1), e.size(), before.byteLength, mem.buffer.byteLength, before === mem.buffer, mem.buffer === mem.buffer);
out('grow-data-kept', text(16, 5), e['i32.load'](100));
out('grow-limit', e.grow(1), e.grow(1), e.size(), mem.buffer.byteLength);
out('js-grow', errName(() => mem.grow(1)), mem.buffer.byteLength);
// JS-created memory, imported
const jm = new WebAssembly.Memory({ initial: 1, maximum: 2 });
const imp = build({ types: [[[T.i32], [T.i32]]], imports: [{ module: 'm', name: 'mem', kind: 2, desc: [1, 1, 2] }], funcs: [{ type: 0, locals: [], body: [lget(0), [0x28, 2, 0]] }], exports: [['ld', 0, 0]] });
const ie = new WebAssembly.Instance(new WebAssembly.Module(imp), { m: { mem: jm } }).exports;
new Uint32Array(jm.buffer)[10] = 0xdeadbeef;
out('import', ie.ld(40), jm.grow(1), jm.buffer.byteLength, errName(() => jm.grow(1)), ie.ld(40));
out('memory-api', errName(() => new WebAssembly.Memory({})), errName(() => new WebAssembly.Memory({ initial: 2, maximum: 1 })), new WebAssembly.Memory({ initial: 0 }).buffer.byteLength, errName(() => new WebAssembly.Instance(new WebAssembly.Module(imp), { m: { mem: new WebAssembly.Memory({ initial: 1 }) } })));
// data segment out of bounds at instantiation
const bad = build({ mems: [[0, 1]], datas: [[0, ...i32(65534), END, ...u(4), 1, 2, 3, 4]] });
out('data-oob', errName(() => new WebAssembly.Instance(new WebAssembly.Module(bad))));
done();

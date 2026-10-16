// control flow, calls, globals, multi-value, call_indirect (compared with V8)
const T = WB.T, u = WB.uleb;
const END = 0x0b, BLOCK_I32 = 0x7f, VOID = 0x40;
const i32 = n => [0x41, ...WB.sleb(n | 0)], i64 = n => [0x42, ...WB.sleb(BigInt(n))];
const lget = i => [0x20, ...u(i)], lset = i => [0x21, ...u(i)], ltee = i => [0x22, ...u(i)];
const gget = i => [0x23, ...u(i)], gset = i => [0x24, ...u(i)];
const call = i => [0x10, ...u(i)], br = i => [0x0c, ...u(i)], brif = i => [0x0d, ...u(i)];
const types = [
    [[T.i64], [T.i64]],                  // 0 fac
    [[T.i32], [T.i32]],                  // 1 unary
    [[T.i32, T.i32], [T.i32]],           // 2 binary
    [[], [T.i32]],                       // 3 counter
    [[T.i32], [T.i32, T.i32]],           // 4 two results
    [[], []],                            // 5 void
    [[T.i32, T.i32], [T.i32, T.i32]],    // 6 block type with params and results
    [[T.i32, T.i32, T.i32], [T.i32]],    // 7 call_indirect helper
];
const funcs = [], exports = [];
const add = (name, type, locals, body) => { funcs.push({ type, locals, body }); exports.push([name, 0, funcs.length - 1]); return funcs.length - 1; };
// fac(n: i64) = n <= 1 ? 1 : n * fac(n-1)
add('fac', 0, [], [lget(0), i64(1), [0x57], [0x04, 0x7e], i64(1), [0x05], lget(0), lget(0), i64(1), [0x7d], call(0), [0x7e], [END]]);
// fib(n), iterative (locals: 1 = a, 2 = b, 3 = tmp)
add('fib', 1, [[3, T.i32]], [
    i32(1), lset(2), [0x02, VOID], [0x03, VOID], lget(0), [0x45], brif(1), lget(1), lget(2), [0x6a], lset(3), lget(2), lset(1), lget(3), lset(2),
    lget(0), i32(1), [0x6b], lset(0), br(0), [END], [END], lget(1)]);
// sum 1..n with a loop and br_if
add('sum', 1, [[1, T.i32]], [[0x03, VOID], lget(0), lget(1), [0x6a], lset(1), lget(0), i32(1), [0x6b], ltee(0), brif(0), [END], lget(1)]);
// dispatch(n) via br_table: 0->10 1->20 2->30 else 99
add('dispatch', 1, [], [
    [0x02, BLOCK_I32], [0x02, VOID], [0x02, VOID], [0x02, VOID], [0x02, VOID],
    lget(0), [0x0e, 3, 0, 1, 2, 3],
    [END], i32(10), br(3), [END], i32(20), br(2), [END], i32(30), br(1), [END], i32(99), [END]]);
// block with result and a br_if that keeps its value on the stack
add('nest', 1, [], [[0x02, BLOCK_I32], [0x02, BLOCK_I32], i32(5), lget(0), brif(1), [0x1a], i32(7), [END], i32(1), [0x6a], [END]]);
// select / drop / tee
add('sel', 2, [], [i32(111), i32(222), lget(0), [0x1b], lget(1), ltee(1), [0x6a], [0x1a], lget(1)]);
// global counter (mutable global 0)
add('count', 3, [], [gget(0), i32(1), [0x6a], gset(0), gget(0)]);
// multi-value return (n+1, n*2)
add('pair', 4, [], [lget(0), i32(1), [0x6a], lget(0), i32(2), [0x6c]]);
// block with params (type 6): ( a b ) -> ( a-b , b ) then add
add('blockparams', 2, [[2, T.i32]], [lget(0), lget(1), [0x02, 6], lset(3), lset(2), lget(2), lget(3), [0x6b], lget(3), [END], [0x6a]]);
const ADD = add('addf', 2, [], [lget(0), lget(1), [0x6a]]);
const SUB = add('subf', 2, [], [lget(0), lget(1), [0x6b]]);
const MUL = add('mulf', 2, [], [lget(0), lget(1), [0x6c]]);
add('voidf', 5, [], []);
add('ci', 7, [], [lget(1), lget(2), lget(0), [0x11, 2, 0]]);
add('unr', 5, [], [[0x00]]);
const REC = funcs.length;
add('rec', 1, [], [lget(0), i32(1), [0x6a], call(REC)]);
add('early', 1, [], [lget(0), [0x04, 0x40], i32(5), [0x0f], [END], i32(6)]);
const bytes = build({
    types, funcs,
    tables: [[0x70, 0, 4]],
    globals: [[T.i32, 1, 0x41, 100, 0x0b]],
    exports: exports.concat([['tab', 1, 0]]),
    elems: [[0, 0x41, 0, 0x0b, 3, ...u(ADD), ...u(SUB), ...u(MUL)]],     // table[0..2] = add, sub, mul; [3] stays null
});
const e = new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports;
out('fac', e.fac(0n), e.fac(1n), e.fac(5n), e.fac(20n), e.fac(21n));
out('fib', [0, 1, 2, 3, 10, 20, 40].map(n => e.fib(n)));
out('sum', e.sum(1), e.sum(10), e.sum(100));
out('dispatch', [0, 1, 2, 3, 4, -1, 100].map(n => e.dispatch(n)));
out('nest', e.nest(0), e.nest(1), e.nest(-5));
out('sel', e.sel(1, 7), e.sel(0, 7));
out('count', e.count(), e.count(), e.count());
out('pair', e.pair(10), e.pair(-3));
out('blockparams', e.blockparams(10, 3), e.blockparams(0, 0));
out('ci', e.ci(0, 7, 3), e.ci(1, 7, 3), e.ci(2, 7, 3));
out('ci-traps', errName(() => e.ci(3, 1, 1)), errName(() => e.ci(4, 1, 1)), errName(() => e.ci(-1, 1, 1)));
out('table', e.tab.length, e.tab.get(0) === e.addf, e.tab.get(1)(10, 4), e.tab.get(3), errName(() => e.tab.get(4)));
out('unr', errName(() => e.unr()));
out('stack', errName(() => e.rec(0)));
out('early', e.early(0), e.early(1));
e.tab.set(3, e.voidf);
out('sigmismatch', errName(() => e.ci(3, 1, 1)));
e.tab.set(3, null);
out('afternull', errName(() => e.ci(3, 1, 1)));
out('exports', Object.keys(e).length, Object.isFrozen(e), Object.getPrototypeOf(e), typeof e.fac, e.fac.length, e.fac.name);
done();

// interpreter speed: recursive fib(n) and a tight loop (printed with timings; not compared)
const T = WB.T, u = WB.uleb;
const bytes = build({
    types: [[[T.i32], [T.i32]]],
    funcs: [
        { type: 0, locals: [], body: [[0x20, 0], [0x41, 2], [0x48], [0x04, 0x7f], [0x20, 0], [0x05], [0x20, 0], [0x41, 1], [0x6b], [0x10, 0], [0x20, 0], [0x41, 2], [0x6b], [0x10, 0], [0x6a], [0x0b]] },
        { type: 0, locals: [[2, T.i32]], body: [[0x03, 0x40], [0x20, 1], [0x20, 2], [0x6a], [0x21, 1], [0x20, 2], [0x41, 1], [0x6a], [0x22, 2], [0x20, 0], [0x48], [0x0d, 0], [0x0b], [0x20, 1]] },
    ],
    exports: [['fib', 0, 0], ['loop', 0, 1]],
});
const e = inst(bytes).exports;
const now = typeof performance !== 'undefined' ? () => performance.now() : Date.now;
let t = now();
const r = e.fib(27);
const dt = now() - t;
t = now();
const r2 = e.loop(5000000);
const dt2 = now() - t;
out('fib(27) =', r, 'ms', Math.round(dt));
out('loop(5e6) =', r2, 'ms', Math.round(dt2));
done();

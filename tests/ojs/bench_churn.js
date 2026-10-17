// Allocation churn on top of a big live heap (a web app: 100+ MB of retained data, lots of short-lived garbage): total wall time and the
// number of collections. OJS_MEMLIMIT=<MB> sets the realm limit like the kernel does.
//   OJS_MEMLIMIT=360 build-host/ojs_run tests/ojs/bench_churn.js
var keep = [];
for (var i = 0; i < 700000; i++) keep.push({ id: i, name: 'item' + i, tags: ['a' + (i % 7)], nested: { x: i, y: i * 2 } });
var t0 = Date.now();
var sink = 0;
for (var r = 0; r < 60; r++) {
    var tmp = [];
    for (var i = 0; i < 30000; i++) tmp.push({ a: i, b: 'x' + i, c: [i, i + 1] });
    sink += tmp.length;
}
print('churn: ' + (Date.now() - t0) + ' ms, live ' + keep.length + ' objects, sink ' + sink);

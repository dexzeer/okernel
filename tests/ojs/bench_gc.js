// GC benchmark: a live heap shaped like a web app's (many small objects with a few properties, arrays of them, strings, closures), then
// a number of full collections, timed with Date. Prints ms per collection.
//   build-host/ojs_run tests/ojs/bench_gc.js
var keep = [];
for (var i = 0; i < 400000; i++) {
    var o = { id: i, name: 'item' + i, tags: ['a' + (i % 7), 'b' + (i % 13)], nested: { x: i, y: i * 2, z: 'v' + i }, f: (function (k) { return function () { return k; }; })(i) };
    keep.push(o);
}
var strs = [];
for (var i = 0; i < 100000; i++) strs.push('s' + i + 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx');
var bufs = [];
for (var i = 0; i < 20; i++) bufs.push(new Uint8Array(512 * 1024));
$262.gc();
var t0 = Date.now();
var N = 10;
for (var i = 0; i < N; i++) $262.gc();
print('gc: ' + ((Date.now() - t0) / N).toFixed(1) + ' ms per full collection, live objects ' + keep.length);

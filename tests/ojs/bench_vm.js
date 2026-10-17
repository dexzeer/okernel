// Interpreter micro-benchmarks: what the inline caches / the JIT are supposed to speed up. Prints milliseconds per benchmark (lower is
// better) and a checksum per benchmark so a wrong result cannot hide behind a fast one.
//   build-host/ojs_run tests/ojs/bench_vm.js [name ...]
var results = [];
function bench(name, fn) {
    var only = typeof scriptArgs !== 'undefined' ? scriptArgs : [];
    var t0 = Date.now();
    var r = fn();
    var ms = Date.now() - t0;
    results.push(name + ' ' + ms + ' ms  (' + r + ')');
}

bench('prop_mono', function () {            // same shape every time: own data properties
    var pts = [];
    for (var i = 0; i < 1000; i++) pts.push({ x: i, y: i * 2, z: 1 });
    var s = 0;
    for (var r = 0; r < 5000; r++) for (var i = 0; i < 1000; i++) { var p = pts[i]; s += p.x + p.y + p.z; }
    return s;
});

bench('prop_poly4', function () {           // four shapes at one site
    var objs = [];
    for (var i = 0; i < 1000; i++) {
        var o = { x: i };
        if (i % 4 == 1) o.a = 1; else if (i % 4 == 2) o.b = 1; else if (i % 4 == 3) o.c = 1;
        objs.push(o);
    }
    var s = 0;
    for (var r = 0; r < 3000; r++) for (var i = 0; i < 1000; i++) s += objs[i].x;
    return s;
});

bench('proto_deep', function () {           // a method six prototypes up (Polymer-style hierarchy)
    function A() { this.v = 1; }
    A.prototype.get = function () { return this.v; };
    function mk(base) { function C() { base.call(this); } C.prototype = Object.create(base.prototype); return C; }
    var C1 = mk(A), C2 = mk(C1), C3 = mk(C2), C4 = mk(C3), C5 = mk(C4), C6 = mk(C5);
    var o = new C6();
    var s = 0;
    for (var i = 0; i < 3000000; i++) s += o.get();
    return s;
});

bench('class_chain', function () {
    class A { constructor() { this.n = 0; } inc() { this.n++; return this; } }
    class B extends A { }
    class C extends B { }
    class D extends C { bump() { return this.inc(); } }
    var d = new D();
    for (var i = 0; i < 3000000; i++) d.bump();
    return d.n;
});

bench('fib27', function () {
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    return fib(27);
});

bench('loop_arith', function () {
    var s = 0;
    for (var i = 0; i < 20000000; i++) s = (s + i * 3) % 1000003;
    return s;
});

bench('array_index', function () {
    var a = new Array(1000);
    for (var i = 0; i < 1000; i++) a[i] = i;
    var s = 0;
    for (var r = 0; r < 8000; r++) for (var i = 0; i < 1000; i++) s += a[i];
    return s;
});

bench('string_ops', function () {
    var s = '';
    for (var i = 0; i < 200000; i++) s += String.fromCharCode(97 + i % 26);
    var c = 0;
    for (var i = 0; i < s.length; i++) c += s.charCodeAt(i);
    return c;
});

bench('alloc_objects', function () {
    var last;
    for (var i = 0; i < 2000000; i++) last = { a: i, b: i + 1, c: i + 2 };
    return last.c;
});

bench('closures', function () {
    function counter() { var n = 0; return function () { return ++n; }; }
    var c = counter(), s = 0;
    for (var i = 0; i < 5000000; i++) s += c();
    return s;
});

bench('setters_getters', function () {      // accessor properties on a prototype (Polymer property effects)
    function T() { this._x = 0; }
    Object.defineProperty(T.prototype, 'x', { get: function () { return this._x; }, set: function (v) { this._x = v; } });
    var t = new T();
    for (var i = 0; i < 2000000; i++) t.x = t.x + 1;
    return t.x;
});

bench('method_calls_mixed', function () {
    var objs = [];
    for (var i = 0; i < 100; i++) objs.push({ id: i, f: function () { return this.id; } });
    var s = 0;
    for (var r = 0; r < 30000; r++) for (var i = 0; i < 100; i++) s += objs[i].f();
    return s;
});

print(results.join('\n'));

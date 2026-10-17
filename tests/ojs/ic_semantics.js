// Inline cache correctness: every scenario warms a property access site (so it is cached), changes something the cache must notice, and
// checks that the next access sees the change. Run plain and under OJS_GCSTRESS=N.
var fails = 0;
function eq(a, b, what) { if (a !== b) { fails++; print('FAIL ' + what + ': got ' + String(a) + ', expected ' + String(b)); } }
function warm(f) { var r; for (var i = 0; i < 6; i++) r = f(); return r; }

// ---- a prototype gains a shadowing property between the receiver and the holder
(function () {
    function A() {} A.prototype.m = function () { return 'A'; };
    function B() {} B.prototype = Object.create(A.prototype);
    function C() {} C.prototype = Object.create(B.prototype);
    var o = new C();
    function get() { return o.m(); }
    eq(warm(get), 'A', 'chain hit');
    B.prototype.m = function () { return 'B'; };
    eq(get(), 'B', 'shadow added on an intermediate prototype');
    o.m = function () { return 'own'; };
    eq(get(), 'own', 'own property added to the receiver');
    delete o.m;
    eq(get(), 'B', 'own property deleted again');
    delete B.prototype.m;
    eq(get(), 'A', 'intermediate property deleted');
})();

// ---- the receiver's or a prototype's prototype changes
(function () {
    var p1 = { v: 1 }, p2 = { v: 2 }, mid = Object.create(p1), o = Object.create(mid);
    function get() { return o.v; }
    eq(warm(get), 1, 'deep read');
    Object.setPrototypeOf(mid, p2);
    eq(get(), 2, 'setPrototypeOf of an intermediate prototype');
    Object.setPrototypeOf(o, p1);
    eq(get(), 1, 'setPrototypeOf of the receiver');
    o.__proto__ = p2;
    eq(get(), 2, '__proto__ assignment');
})();

// ---- a method / value on the holder is replaced (value only: the cache keeps the slot)
(function () {
    var P = { f: function () { return 1; } }, o = Object.create(P);
    function get() { return o.f(); }
    eq(warm(get), 1, 'method');
    P.f = function () { return 2; };
    eq(get(), 2, 'method replaced');
    Object.defineProperty(P, 'f', { value: function () { return 3; }, writable: true, configurable: true });
    eq(get(), 3, 'method redefined');
    Object.defineProperty(P, 'f', { get: function () { return function () { return 4; }; }, configurable: true });
    eq(get(), 4, 'data property turned into an accessor');
})();

// ---- accessors: cached getters follow redefinition
(function () {
    var calls = 0;
    var P = {}; Object.defineProperty(P, 'x', { get: function () { calls++; return 10; }, configurable: true });
    var o = Object.create(P);
    function get() { return o.x; }
    eq(warm(get), 10, 'inherited getter');
    eq(calls, 6, 'getter ran each time');
    Object.defineProperty(P, 'x', { get: function () { return 11; }, configurable: true });
    eq(get(), 11, 'getter replaced');
    Object.defineProperty(P, 'x', { value: 12, configurable: true });
    eq(get(), 12, 'accessor turned into data');
    var q = {}; Object.defineProperty(q, 'y', { get: function () { return 'own-getter'; }, configurable: true });
    function gy() { return q.y; }
    eq(warm(gy), 'own-getter', 'own getter');
    Object.defineProperty(q, 'y', { get: function () { return 'again'; }, configurable: true });
    eq(gy(), 'again', 'own getter replaced');
    var u = {}; Object.defineProperty(u, 'z', { set: function (v) {}, configurable: true });
    eq(warm(function () { return u.z; }), undefined, 'getter-less accessor reads undefined');
})();

// ---- setters
(function () {
    var log = [];
    function T() {}
    Object.defineProperty(T.prototype, 'v', { set: function (x) { log.push(x); }, get: function () { return log.length; }, configurable: true });
    var t = new T();
    function set(x) { t.v = x; }
    for (var i = 0; i < 6; i++) set(i);
    eq(log.join(), '0,1,2,3,4,5', 'cached setter calls');
    Object.defineProperty(T.prototype, 'v', { set: function (x) { log.push('new' + x); }, configurable: true });
    set(9);
    eq(log[log.length - 1], 'new9', 'setter replaced');
    Object.defineProperty(T.prototype, 'v', { set: undefined, get: function () { return 1; }, configurable: true });
    (function () { 'use strict'; var threw = false; try { t.v = 5; } catch (e) { threw = e instanceof TypeError; } eq(threw, true, 'strict assignment to a getter-only accessor throws'); })();
})();

// ---- adding a property: the cached transition must respect later setters, read-only inheritance and non-extensibility
(function () {
    function make() { return { a: 1 }; }
    function add(o) { o.b = 2; return o; }
    for (var i = 0; i < 6; i++) eq(add(make()).b, 2, 'add transition');
    Object.defineProperty(Object.prototype, 'b', { set: function (v) { this._seen = v; }, configurable: true });
    var o = add(make());
    eq(o.b, undefined, 'setter on Object.prototype now intercepts the add');
    eq(o._seen, 2, 'setter ran');
    delete Object.prototype.b;
    eq(add(make()).b, 2, 'setter removed: the add works again');
    Object.defineProperty(Object.prototype, 'b', { value: 7, writable: false, configurable: true });
    var o2 = add(make());
    eq(o2.b, 7, 'read-only inherited property blocks the add (sloppy mode)');
    delete Object.prototype.b;
    var f = Object.preventExtensions(make());
    add(f);
    eq(f.b, undefined, 'non-extensible object does not gain the property');
    var fr = Object.freeze(make());
    add(fr);
    eq(fr.b, undefined, 'frozen object does not gain the property');
    eq(add(make()).b, 2, 'ordinary objects still do');
})();

// ---- constructor pattern: many objects, one site
(function () {
    function P(x, y) { this.x = x; this.y = y; this.z = x + y; }
    var s = 0;
    for (var i = 0; i < 100; i++) { var p = new P(i, 1); s += p.x + p.y + p.z; }
    eq(s, 4950 + 100 + 5050, 'constructor adds');
    P.prototype = { __proto__: null };
    var q = new P(1, 2);
    eq(q.z, 3, 'constructor with another prototype');
    Object.defineProperty(Object.prototype, 'z', { set: function (v) { this._z = v; }, configurable: true });
    var r = new P(5, 6);
    eq(r._z, undefined, 'null-prototype objects ignore Object.prototype setters');
    var Q = function (x) { this.z = x; };
    new Q(1); new Q(2); new Q(3); new Q(4);
    var w = new Q(9);
    eq(w._z, 9, 'setter intercepts the add on a new site too');
    delete Object.prototype.z;
})();

// ---- polymorphic sites: three and four shapes at one place
(function () {
    var objs = [{ k: 1 }, { a: 0, k: 2 }, { a: 0, b: 0, k: 3 }, { c: 0, k: 4 }, { k: 5, d: 0 }];
    function rd(o) { return o.k; }
    var s = 0;
    for (var r = 0; r < 10; r++) for (var i = 0; i < objs.length; i++) s += rd(objs[i]);
    eq(s, 150, 'polymorphic reads');
    var p = []; for (var i = 0; i < objs.length; i++) p.push(Object.create(objs[i]));
    s = 0;
    for (var r = 0; r < 10; r++) for (var i = 0; i < p.length; i++) s += rd(p[i]);
    eq(s, 150, 'polymorphic chain reads');
})();

// ---- exotic receivers sharing shapes with plain objects
(function () {
    var plain = {};
    function ts(o) { return o.toString; }
    for (var i = 0; i < 6; i++) ts(plain);
    var px = new Proxy({}, { get: function (t, k) { return 'trapped:' + String(k); } });
    eq(ts(px), 'trapped:toString', 'proxy sharing the empty shape');
    var ta = new Uint8Array(2);
    function ln(o) { return o.length; }
    eq(warm(function () { return ln([1, 2, 3]); }), 3, 'array length');
    eq(ln(ta), 2, 'typed array length');
    eq(ln('abcd'), 4, 'string length');
    function cl(o) { return o.constructor; }
    for (var i = 0; i < 6; i++) cl({});
    eq(cl(new Proxy({}, { get: function () { return 'x'; } })), 'x', 'proxy constructor');
    function ar() { return arguments.length; }
    eq(ar(1, 2, 3), 3, 'arguments length');
})();

// ---- classes
(function () {
    class A { m() { return 'A.m'; } static s() { return 'A.s'; } get g() { return 'A.g'; } }
    class B extends A { }
    class C extends B { m() { return 'C>' + super.m(); } }
    var c = new C();
    function m() { return c.m(); }
    eq(warm(m), 'C>A.m', 'super call');
    B.prototype.m = function () { return 'B.m'; };
    eq(m(), 'C>B.m', 'method added to the middle class');
    function s() { return C.s(); }
    eq(warm(s), 'A.s', 'static inherited');
    B.s = function () { return 'B.s'; };
    eq(s(), 'B.s', 'static added to the middle class');
    function g() { return c.g; }
    eq(warm(g), 'A.g', 'inherited getter');
    Object.defineProperty(C.prototype, 'g', { get: function () { return 'C.g'; }, configurable: true });
    eq(g(), 'C.g', 'getter shadowed by the subclass');
    Object.setPrototypeOf(C.prototype, { g: 'plain', m: function () { return 'plain.m'; } });
    eq(g(), 'C.g', 'own accessor still wins');
    delete C.prototype.g;
    eq(g(), 'plain', 'accessor deleted, new prototype used');
})();

// ---- globals
(function () {
    globalThis.gv = 1;
    function rg() { return gv; }
    eq(warm(rg), 1, 'global');
    globalThis.gv = 2;
    eq(rg(), 2, 'global changed');
    delete globalThis.gv;
    var threw = false; try { rg(); } catch (e) { threw = e instanceof ReferenceError; }
    eq(threw, true, 'global deleted');
})();

print(fails ? fails + ' FAILED' : 'ALL OK');

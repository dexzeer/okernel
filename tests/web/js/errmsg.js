// Error-message battery: the exact strings V8 produces for common throw sites,
// which exception-driven code (BotGuard) reads via e.message. Diff ojs vs Node.
function t(name, fn) {
    try { fn(); console.log(name + ' = (no throw)'); }
    catch (e) { console.log(name + ' = ' + e.constructor.name + ': ' + e.message); }
}
t('null-prop', () => { var z = null; z.foo; });
t('undef-prop', () => { var z; z.foo; });
t('undef-deep', () => { var z; z.a.b; });
t('not-fn', () => { var z = 5; z(); });
t('not-fn-prop', () => { var z = {}; z.foo(); });
t('not-ctor', () => { new 5(); });
t('undef-ctor', () => { var z; new z(); });
t('const-assign', () => { const z = 1; z = 2; });
t('readonly-len', () => { var a = [1, 2]; a.length = -1; });
t('bad-array', () => { var a = new Array(-1); });
t('json-parse', () => { JSON.parse('{'); });
t('json-parse2', () => { JSON.parse('{"a":}'); });
t('decode-uri', () => { decodeURIComponent('%'); });
t('encode-bad', () => { encodeURIComponent('\uFFFE'); });
t('regexp', () => { new RegExp('('); });
t('regexp-flags', () => { new RegExp('a', 'gg'); });
t('tofixed', () => { (1).toFixed(101); });
t('toPrecision', () => { (1).toPrecision(0); });
t('string-repeat-neg', () => { 'a'.repeat(-1); });
t('array-from-len', () => { new ArrayBuffer(-1); });
t('promise-resolve-nonfn', () => { Promise.resolve(1, 2); });
t('map-bad-iter', () => { new Map(1); });
t('spread-noniter', () => { var a = [...1]; });
t('forof-noniter', () => { for (var x of 1) {} });
t('null-call', () => { var z = null; z(); });
t('typeof-call', () => { (void 0).x(); });
t('in-nonobj', () => { 'a' in 1; });
t('instanceof-nonfn', () => { 1 instanceof 2; });
t('delete-plain', () => { delete 1; });
t('getter-throw', () => { var o = { get x() { throw new RangeError('custom'); } }; o.x; });
t('bigint-str', () => { JSON.stringify(1n); });
t('bigint-conv', () => { Symbol() + ''; });
t('symbol-key-num', () => { var o = {}; o[Symbol()] = 1; Object.keys(o).length; });
t('weakref', () => { new WeakRef(1); });
t('reflect', () => { Reflect.get(1, 'a'); });
t('proxy-revoke', () => { var r = Proxy.revocable({}, {}); r.revoke(); r.proxy.a; });
t('daterange', () => { new Date('not a date').getTime(); });
t('date-invalid', () => { new Date(8.64e15 + 1).toISOString(); });
t('number-format', () => { (1.5).toLocaleString('zz'); });
t('uri-error', () => { encodeURI('\uD800'); });
t('typedarray-bad', () => { new Int8Array(new Array(2)); });
t('typedarray-oob', () => { var a = new Int8Array(2); a[5] = 1; a[5]; });
t('string-oob', () => { 'abc'.charCodeAt(10); });
t('array-oob', () => { [1, 2, 3][10]; });
t('call-null-this', () => { String.prototype.trim.call(null); });
t('obj-defineprop', () => { Object.defineProperty(1, 'a', { value: 2 }); });
t('object-assign-null', () => { Object.assign(null, {}); });
t('func-tostring-parsed', () => { eval('(function(){})').prototype.x; });
t('eval-referr', () => { 'use strict'; unevaled; });

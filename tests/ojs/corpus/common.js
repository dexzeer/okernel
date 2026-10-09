// common.js — helpers for the differential corpus (runs first, in node and in ojs).
// Every exercise prints lines through show(); the runner diffs node's output
// against ojs's.

// browser bundles look for `self` (window/worker global)
if (typeof self === 'undefined') globalThis.self = globalThis;

var __fnv = function (s) {
    s = String(s);
    var h = 2166136261;
    for (var i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619) >>> 0; }
    return h.toString(16) + ':' + s.length;
};

function show(label, v) {
    var s;
    try { s = typeof v === 'string' ? v : JSON.stringify(v); } catch (e) { s = 'unserializable: ' + e.message; }
    if (s === undefined) s = String(v);
    console.log(label + ' = ' + (s.length > 400 ? __fnv(s) + ' ' + s.slice(0, 160) + ' ... ' + s.slice(-80) : s));
}

function attempt(label, f) {
    try { show(label, f()); } catch (e) { console.log(label + ' THREW ' + (e && e.name) + ': ' + (e && e.message)); }
}

function attemptAsync(label, f) {
    var p;
    try { p = Promise.resolve(f()); } catch (e) { p = Promise.reject(e); }
    return p.then(function (v) { show(label, v); }, function (e) { console.log(label + ' REJECTED ' + (e && e.name) + ': ' + (e && e.message)); });
}

// a deterministic PRNG (exact in doubles: 32-bit state)
var __seed = 0x9e3779b9;
function rand() {
    __seed ^= __seed << 13; __seed >>>= 0;
    __seed ^= __seed >>> 17;
    __seed ^= __seed << 5; __seed >>>= 0;
    return __seed / 4294967296;
}

// source text with most of the language in it: parsers, compilers and formatters chew on this
var SAMPLE_SRC = String.raw`
'use strict';
import { a as b, c } from './mod.js';
export default class Shape extends Base {
  #secret = 1;
  static count = 0;
  static { Shape.count++; }
  constructor(x, y = 2, ...rest) { super(x); this.x = x; this.y = y; this.rest = rest; }
  get area() { return this.x * this.y; }
  set area(v) { this.x = v / this.y; }
  *points() { yield* [[this.x, 0], [0, this.y]]; }
  async load(url) { const r = await fetch(url); return r?.json?.() ?? null; }
  static #hidden() { return #secret in this; }
  [Symbol.iterator]() { return this.points(); }
}
export const fmt = (n, { digits = 2, unit = 'px' } = {}) => ${'`'}${'$'}{n.toFixed(digits)}${'$'}{unit}${'`'};
export async function* stream(xs) { for await (const x of xs) { if (x == null) continue; yield x ** 2; } }
function legacy(a, b) {
  var out = [], i, j;
  label: for (i = 0; i < a.length; i++) {
    for (j in b) { if (b[j] === a[i]) continue label; if (j > 3) break label; }
    switch (typeof a[i]) { case 'number': out.push(a[i] | 0); break; case 'string': out.push(a[i].trim()); default: out.push(null); }
  }
  try { JSON.parse(out); } catch { out.length = 0; } finally { i = -1; }
  do { i++; } while (i < 3);
  return out.concat([...new Set(a)], { ...b, extra: !0 }, 0x1F, 1e-7, 0b101, 0o17, 10n, /re[gx]+/giu, void 0, typeof x, delete b.z);
}
let { p, q: [r1, , r3 = 4] = [], ...others } = obj;
x ||= 1; y &&= 2; z ??= 3; w **= 2; v >>>= 1;
const tagged = String.raw${'`'}a${'$'}{1}b${'`'};
import.meta.url; await Promise.all([import('./lazy.js')]);
`;

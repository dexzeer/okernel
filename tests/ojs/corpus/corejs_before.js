// corejs_before.js — snapshot of the built-ins before core-js loads (see x_corejs.js)
var __before = new Map();
var __roots = { Object: Object, Array: Array, String: String, Number: Number, Math: Math, JSON: JSON, Promise: Promise, Symbol: Symbol, Map: Map, Set: Set, WeakMap: WeakMap, WeakSet: WeakSet,
    RegExp: RegExp, Date: Date, Reflect: Reflect, ArrayBuffer: ArrayBuffer, DataView: DataView, Uint8Array: Uint8Array, Function: Function, Error: Error, Iterator: typeof Iterator !== 'undefined' ? Iterator : undefined,
    'Array.prototype': Array.prototype, 'String.prototype': String.prototype, 'Number.prototype': Number.prototype, 'Promise.prototype': Promise.prototype, 'RegExp.prototype': RegExp.prototype, 'Object.prototype': Object.prototype,
    'Map.prototype': Map.prototype, 'Set.prototype': Set.prototype, 'Date.prototype': Date.prototype, 'Function.prototype': Function.prototype, 'Symbol.prototype': Symbol.prototype, 'Error.prototype': Error.prototype,
    'TypedArray.prototype': Object.getPrototypeOf(Uint8Array.prototype), 'TypedArray': Object.getPrototypeOf(Uint8Array), 'ArrayBuffer.prototype': ArrayBuffer.prototype, 'DataView.prototype': DataView.prototype,
    'Iterator.prototype': typeof Iterator !== 'undefined' ? Iterator.prototype : undefined, globalThis: globalThis };
var __globals = Object.getOwnPropertyNames(globalThis);
for (var __k in __roots) {
    var __o = __roots[__k];
    if (!__o) continue;
    Object.getOwnPropertyNames(__o).forEach(function (p) {
        var d = Object.getOwnPropertyDescriptor(__o, p);
        if (d) __before.set(__k + '.' + p, d.value !== undefined ? d.value : d.get);
    });
}
var __Promise = Promise;

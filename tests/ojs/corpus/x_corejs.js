// before: corejs_before.js
// libs: corejs.js
// core-js feature-detects hundreds of spec details and replaces whatever it judges
// broken: anything it replaces in ojs and not in node is a conformance bug.
var __changed = [];
for (var __k in __roots) {
    var __o = __roots[__k];
    if (!__o) continue;
    Object.getOwnPropertyNames(__o).forEach(function (p) {
        var d = Object.getOwnPropertyDescriptor(__o, p);
        var v = d && (d.value !== undefined ? d.value : d.get);
        var key = __k + '.' + p;
        if (!__before.has(key)) __changed.push('+' + key);
        else if (__before.get(key) !== v && typeof v === 'function') __changed.push('~' + key);
    });
}
Object.getOwnPropertyNames(globalThis).forEach(function (g) { if (__globals.indexOf(g) < 0 && g.indexOf('core-js') < 0 && g !== '__changed') __changed.push('+global.' + g); });
// the comparison keys on names, so the list is sorted; node's own extras (Intl,
// proposals it ships) show up as differences to review, not necessarily bugs
__changed.sort();
console.log(__changed.join('\n'));
show('promise replaced', Promise !== __Promise);

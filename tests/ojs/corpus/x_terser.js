// libs: terser.js
attemptAsync('minify', function () {
    return Terser.minify('function add(first, second) { return first + second; } var result = add(1 + 2, 3 * 4); console.log(result, [1,2,3].map(function (x) { return x * 2; }));', { toplevel: true }).then(function (r) { return r.code; });
});
attemptAsync('modern', function () {
    return Terser.minify('export class A { #x = 1; static y = 2; get x() { return this.#x; } async *g() { yield* [1]; } } export const f = (a, { b = 2, ...c } = {}) => a ?? b?.c ?? c;', { module: true, compress: { passes: 2 }, mangle: true }).then(function (r) { return r.code; });
});
attemptAsync('sample', function () {
    return Terser.minify({ 'a.js': SAMPLE_SRC.replace(/^import .*$/m, '').replace(/export default /, '').replace(/export /g, '').replace(/import\.meta\.url; await Promise\.all\(\[import\('\.\/lazy\.js'\)\]\);/, '') }, { compress: { unsafe: true }, mangle: { properties: false } }).then(function (r) { return r.code; });
});
attemptAsync('error', function () { return Terser.minify('var = ;').then(function (r) { return r.code; }, function (e) { return 'ERR ' + e.message; }); });

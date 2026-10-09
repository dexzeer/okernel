// libs: babel.js
// @babel/standalone: modern syntax down to ES5 (huge generated code, lots of runtime paths)
var B = Babel;
var opts = { presets: [['env', { targets: { ie: '11' }, modules: 'commonjs' }]], plugins: [], sourceType: 'module', filename: 'sample.js', compact: false, comments: true };
attempt('env', function () { return B.transform(SAMPLE_SRC, opts).code; });
attempt('react', function () {
    return B.transform('const App = ({ items }) => <ul className="x">{items.map(i => <li key={i}>{i}</li>)}</ul>;', { presets: ['react'] }).code;
});
attempt('typescript', function () {
    return B.transform('enum Color { Red = 1, Green } interface P { x: number } function f<T extends P>(p: T): number { return p.x as number; } let a: Color = Color.Red;', { presets: ['typescript'], filename: 'a.ts' }).code;
});
attempt('minify-ish', function () {
    return B.transform('async function main() { for await (const x of g()) console.log(x); } class A { static #p = 1; m() { return A.#p; } }', { presets: [['env', { targets: { chrome: '60' } }]] }).code;
});
attempt('error', function () { try { B.transform('let x = ;'); } catch (e) { return e.message.split('\n')[0]; } });
attempt('ast', function () { return B.transform('a?.b ?? c', { ast: true, code: false }).ast.program.body[0].expression.type; });

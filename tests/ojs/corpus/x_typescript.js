// libs: typescript.js
var TS = ts;
var src = 'enum Dir { Up = 1, Down } namespace NS { export const v: number = 2; }\n' +
    'abstract class Animal<T extends { name: string }> { constructor(protected readonly info: T) {} abstract speak(): string; @dec method(@inject() x?: number): void {} }\n' +
    'class Dog extends Animal<{ name: string; breed?: string }> { speak() { return `${this.info.name} barks`; } }\n' +
    'type Mapped<T> = { readonly [K in keyof T]?: T[K] extends Function ? never : T[K] };\n' +
    'function overload(a: string): string; function overload(a: number): number; function overload(a: any) { return a; }\n' +
    'const x = <const>["a", "b"]; let y = x satisfies readonly string[]; export default async function* gen() { yield await Promise.resolve(Dir.Up); }\n' +
    'declare global { interface Window { foo: string } } let opt = obj?.a!.b ?? 1; for (const [k, v] of Object.entries({})) {}\n';
attempt('transpile es5', function () { return TS.transpileModule(src, { compilerOptions: { target: TS.ScriptTarget.ES5, module: TS.ModuleKind.CommonJS, experimentalDecorators: true, downlevelIteration: true } }).outputText; });
attempt('transpile es2022', function () { return TS.transpileModule(src, { compilerOptions: { target: TS.ScriptTarget.ES2022, module: TS.ModuleKind.ESNext } }).outputText; });
attempt('typecheck', function () {
    var files = { 'a.ts': 'export function f(x: number): string { return x; }\nconst n: number = f(1);\ninterface P { a: string } const p: P = { a: 1, b: 2 };\nlet u = [1, 2].map(x => x.toFixed()).join() + undefinedName;\n' };
    var host = {
        getSourceFile: function (name, lang) { return files[name] !== undefined ? TS.createSourceFile(name, files[name], lang) : name === 'lib.d.ts' ? TS.createSourceFile(name, 'interface Array<T> { length: number; map<U>(f: (v: T) => U): U[]; join(s?: string): string; [n: number]: T } interface Number { toFixed(d?: number): string } interface String {} interface Boolean {} interface Object {} interface Function {} interface CallableFunction {} interface NewableFunction {} interface IArguments {} interface RegExp {}', lang) : undefined; },
        getDefaultLibFileName: function () { return 'lib.d.ts'; }, writeFile: function () {}, getCurrentDirectory: function () { return '/'; }, getDirectories: function () { return []; },
        fileExists: function (n) { return files[n] !== undefined || n === 'lib.d.ts'; }, readFile: function (n) { return files[n]; }, getCanonicalFileName: function (n) { return n; },
        useCaseSensitiveFileNames: function () { return true; }, getNewLine: function () { return '\n'; }
    };
    var prog = TS.createProgram(['a.ts'], { noEmit: true, strict: true, target: TS.ScriptTarget.ES2020 }, host);
    return TS.getPreEmitDiagnostics(prog).map(function (d) { return d.code + ' ' + TS.flattenDiagnosticMessageText(d.messageText, '\n'); });
});

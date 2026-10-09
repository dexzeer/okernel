var out = [];
function check(name, got, want) { if (got !== want) out.push("FAIL " + name + ": got " + got + " want " + want); }
// closures, classes, getters
function counter() { var n = 0; return { inc: () => ++n, get val() { return n; } }; }
var c = counter(); c.inc(); c.inc(); check("closure", c.val, 2);
class A { #x = 1; constructor(y) { this.y = y; } get x() { return this.#x; } static s() { return "s"; } m() { return this.y + this.#x; } }
class B extends A { constructor() { super(5); } m() { return super.m() * 2; } }
check("class", new B().m(), 12); check("static", A.s(), "s"); check("private", new A(1).x, 1);
// destructuring, spread, rest
var { a, b: [c1, ...rest], ...others } = { a: 1, b: [2, 3, 4], d: 5, e: 6 };
check("destr", a + c1 + rest.length + Object.keys(others).length, 1 + 2 + 2 + 2);
check("spread", Math.max(...[1, 9, 3]), 9);
// generators
function* g() { var x = yield 1; yield x * 2; return 7; }
var it = g(); it.next(); check("gen", it.next(21).value, 42); check("gen ret", it.next().value, 7);
check("gen spread", [...function* () { yield* [1, 2]; yield 3; }()].join(), "1,2,3");
// try/finally
function tf() { try { return 1; } finally { out.push2 = 1; } }
check("finally", tf(), 1);
var log = []; try { try { throw new Error("x"); } finally { log.push("f"); } } catch (e) { log.push(e.message); }
check("catch", log.join(), "f,x");
// labels, switch
outer: for (var i = 0; i < 3; i++) { for (var j = 0; j < 3; j++) { if (j == 1) continue outer; if (i == 2) break outer; } }
check("labels", i, 2);
switch (3) { case 1: check("sw", 1, 0); case 3: var sw = "three"; break; default: sw = "d"; }
check("switch", sw, "three");
// template literals, tagged
function tag(s, ...v) { return s.raw.join("|") + v.join(); }
check("tag", tag`a${1}b${2}c`, "a|b|c1,2");
check("tmpl", `x${1 + 1}y`, "x2y");
// regexp
check("re", "2024-05-06".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"), "06/05/2024");
check("re named", /(?<y>\d{4})/.exec("in 1999").groups.y, "1999");
check("re g", "a1b22c333".match(/\d+/g).join(), "1,22,333");
check("re split", "a, b,c".split(/\s*,\s*/).join("|"), "a|b|c");
check("re i", /HELLO/i.test("hello"), true);
check("re lookbehind", "$10 €20".match(/(?<=\$)\d+/)[0], "10");
// JSON
check("json", JSON.stringify({ a: [1, "x", null], b: { c: true } }), '{"a":[1,"x",null],"b":{"c":true}}');
check("json parse", JSON.parse('{"x":[1,2,{"y":"z"}]}').x[2].y, "z");
// Map/Set
var m = new Map([[1, "a"], [2, "b"]]); m.set(3, "c"); m.delete(1);
check("map", [...m.keys()].join(), "2,3");
check("set", new Set([1, 2, 2, 3]).size, 3);
// numbers
check("num", (0.1 + 0.2).toString(), "0.30000000000000004");
check("tofixed", (1.005).toFixed(2), "1.00");
check("int", parseInt("ff", 16), 255);
check("bigint", (2n ** 64n).toString(), "18446744073709551616");
// string methods
check("pad", "5".padStart(3, "0"), "005");
check("upper", "straße".toUpperCase(), "STRASSE");
check("at", "abc".at(-1), "c");
// symbols / iterators
var obj = { [Symbol.iterator]: function* () { yield 1; yield 2; } };
check("iter", Array.from(obj).join(), "1,2");
// typed arrays
var u8 = new Uint8Array([1, 2, 300]); check("u8", u8[2], 44);
var f64 = new Float64Array(2); f64[0] = 1.5; check("f64", new Uint8Array(f64.buffer)[7], 63);
// proxies
var p = new Proxy({}, { get: (t, k) => k + "!" }); check("proxy", p.hi, "hi!");
// Date
check("date", new Date(Date.UTC(2020, 1, 29)).toISOString(), "2020-02-29T00:00:00.000Z");
// getter/setter, defineProperty
var o2 = {}; Object.defineProperty(o2, "x", { get() { return 42; } }); check("defprop", o2.x, 42);
// arguments
function ar() { arguments[0] = 9; return arguments.length + arguments[0]; } check("args", ar(1, 2), 11);
function ar2(x) { arguments[0] = 9; return x; } check("mapped args", ar2(1), 9);
// eval
var ev = 5; check("eval", eval("ev + 1"), 6);
function fe() { var q = 3; return eval("q * 2"); } check("direct eval", fe(), 6);
check("indirect eval", (0, eval)("typeof ev"), "number");
// optional chaining / nullish
var n = null; check("optchain", n?.x?.y, undefined); check("nullish", n ?? "d", "d");
// async
var asyncLog = [];
async function af() { asyncLog.push(1); await null; asyncLog.push(3); return "done"; }
af().then(v => { asyncLog.push(v); });
asyncLog.push(2);
Promise.resolve().then(() => {}).then(() => {}).then(() => {
  check("async order", asyncLog.join(), "1,2,3,done");
  if (out.length) print(out.join("\n")); else print("ALL OK");
});

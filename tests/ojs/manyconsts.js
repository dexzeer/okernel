// A function with more than 65536 constants (the wrapper function of a big
// bundle, like YouTube's 10MB app script): constant indices past 0xFFFF used
// to wrap to 16 bits and load another constant - a function template, which
// leaked into JS as a value that was typeof "number" and String() "".
// Generated: the source is built here and run through eval / new Function.
var fails = 0;
function check(name, got, want) {
    if (got !== want) { fails++; console.log("FAIL " + name + ": got " + String(got) + ", want " + String(want)); }
}

var N = 70000;
var parts = ["var sink = 0;"];
for (var i = 0; i < N; i++) parts.push("sink += 's" + i + "'.length;");   // N distinct string constants
// every instruction kind with a constant operand, emitted after the first 65536 constants
parts.push(
    "var str = 'past-the-limit';",                                  // CONST(_W)
    "var num = 1234.5678;",                                         // CONST(_W) (double)
    "var big = 123456789012345678901234567890n;",                   // CONST(_W) (BigInt)
    "var re = /ab+c/gi;",                                           // REGEXP (pattern + flags)
    "var fn = function inner() { return str; };",                   // CLOSURE
    "class K { m() { return 'method'; } }",                         // CLASS
    "var tpl = (function (s) { return s; })`raw${1}text`;",         // template object
    "var tdz; try { tdzv; let tdzv = 1; } catch (e) { tdz = e.message; }",   // GET_LOC_CHK name
    "var tdz2; try { tdzw = 2; let tdzw; } catch (e) { tdz2 = e.message; }", // PUT_LOC_CHK name
    "var tdz3; try { (function () { return tdzu; })(); } catch (e) { tdz3 = e.message; } let tdzu = 3;", // GET_UPV_CHK
    "var ev = eval('str + \"/\" + num');",                          // EVAL env descriptor
    "var ev2 = eval(...['str.length']);",                           // EVAL_SPREAD (direct per spec and test262 eval-spread.js; V8 makes it indirect)
    "return [sink, str, num, String(big), re.source + '/' + re.flags, fn(), new K().m(), tpl.raw.join('|'), tdz, tdz2, tdz3, ev, ev2];"
);
var src = parts.join("\n");
var r = new Function(src)();
var sink = 0;
for (var j = 0; j < N; j++) sink += ("s" + j).length;
check("sink", r[0], sink);
check("string const", r[1], "past-the-limit");
check("number const", r[2], 1234.5678);
check("bigint const", r[3], "123456789012345678901234567890");
check("regexp", r[4], "ab+c/gi");
check("closure", r[5], "past-the-limit");
check("class", r[6], "method");
check("template", r[7], "raw|text");
check("tdz get name", r[8], "Cannot access 'tdzv' before initialization");
check("tdz put name", r[9], "Cannot access 'tdzw' before initialization");
check("tdz upvalue name", r[10], "Cannot access 'tdzu' before initialization");
check("direct eval", r[11], "past-the-limit/1234.5678");
check("eval spread", r[12], 14);

// same at script top level (global code: CHECK_GLOBAL_DECLS) through indirect eval
var g = parts.slice(0, N + 1).join("\n") + "\nvar topLevel = 'top-' + /x/.source; topLevel";
check("global code", (0, eval)(g), "top-x");

console.log(fails ? "FAILED " + fails : "ALL OK");

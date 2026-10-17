// upvals.js - closures capturing many variables of one frame (the slot -> upvalue table of find_upval) and block scopes that close
// them in the middle: every closure must see the same variable as every other closure that captured it, before and after the scope ends.
var fails = 0;
function check(name, cond) { if (!cond) { fails++; print("FAIL " + name); } }

var seed = 12345;
function rnd(n) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; }

// 1. one big function: N variables, closures made in a random order, each reading and writing 1-4 random variables
(function () {
    var N = 3000, M = 6000;
    var src = "var r = [];\n";
    for (var i = 0; i < N; i++) src += "var v" + i + " = " + i + ";\n";
    var expect = [];
    for (var i = 0; i < N; i++) expect.push(i);
    var plan = [];
    for (var m = 0; m < M; m++) {
        var k = 1 + rnd(4), vars = [];
        for (var j = 0; j < k; j++) vars.push(rnd(N));
        plan.push(vars);
        // the closure adds its index to each captured variable and returns their sum
        src += "r.push(function () { var s = 0;" + vars.map(function (v) { return " v" + v + " += " + (m % 7 + 1) + "; s += v" + v + ";"; }).join("") + " return s; });\n";
    }
    src += "return r;";
    var fns = new Function(src)();
    // run a sample of closures in a different order and mirror them
    var order = [];
    for (var m = 0; m < M; m++) order.push(m);
    for (var m = M - 1; m > 0; m--) { var j = rnd(m + 1), t = order[m]; order[m] = order[j]; order[j] = t; }
    var ok = true;
    for (var q = 0; q < order.length && ok; q++) {
        var m = order[q], s = 0;
        for (var j = 0; j < plan[m].length; j++) { expect[plan[m][j]] += m % 7 + 1; s += expect[plan[m][j]]; }
        if (fns[m]() !== s) ok = false;
    }
    check("many captured variables, shared state", ok);
})();

// 2. blocks that capture a few of many outer variables and close their own per-iteration bindings
(function () {
    var a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i1 = 9, j1 = 10, k1 = 11, l1 = 12, m1 = 13, n1 = 14, o1 = 15, p1 = 16, q1 = 17, r1 = 18;
    var s1 = 19, t1 = 20, u1 = 21, v1 = 22, w1 = 23, x1 = 24, y1 = 25, z1 = 26, a2 = 27, b2 = 28, c2 = 29, d2 = 30;
    var keep = [];
    keep.push(function () { return a + z1 + d2; });
    keep.push(function () { return b + y1 + c2; });
    keep.push(function () { return c + x1 + b2; });
    keep.push(function () { return d + w1 + a2; });
    for (var i = 0; i < 40; i++) {
        let inner = i * 2;
        keep.push(function () { inner++; return inner + a + (i1 || 0); });
        keep.push(function () { return inner; });
    }
    a = 100; z1 = 1000; d2 = 10000;
    check("outer vars changed after capture", keep[0]() === 11100);
    var got = [];
    for (var q = 4; q < keep.length; q += 2) { var x = keep[q](), y = keep[q + 1](); got.push(x - 100 - 9 === y); }
    check("per-iteration bindings stay separate and shared with their sibling", got.every(function (v) { return v; }));
    check("closures created in the loop saw distinct bindings", keep[5]() === 1 && keep[7]() === 3 && keep[83]() === 79);
})();

// 3. exceptions and returns out of a frame with a big table
(function () {
    function big(n, bail) {
        var vs = [], fs = [];
        var a0 = 0, a1 = 1, a2 = 2, a3 = 3, a4 = 4, a5 = 5, a6 = 6, a7 = 7, a8 = 8, a9 = 9, b0 = 10, b1 = 11, b2 = 12, b3 = 13, b4 = 14, b5 = 15,
            b6 = 16, b7 = 17, b8 = 18, b9 = 19, c0 = 20, c1 = 21, c2 = 22, c3 = 23, c4 = 24, c5 = 25, c6 = 26, c7 = 27, c8 = 28, c9 = 29, d0 = 30;
        fs.push(function () { return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9; });
        fs.push(function () { return b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8 + b9; });
        fs.push(function () { return c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8 + c9 + d0; });
        fs.push(function () { a9 = 90; b9 = 190; c9 = 290; d0 = 300; });
        if (bail) throw fs;
        return fs;
    }
    var fs = big(0, false);
    fs[3]();
    check("sums after frame exit", fs[0]() === 126 && fs[1]() === 316 && fs[2]() === 806);
    var thrown;
    try { big(0, true); } catch (e) { thrown = e; }
    thrown[3]();
    check("closures survive an exception leaving the frame", thrown[0]() === 126 && thrown[1]() === 316);
})();

print(fails ? "FAILED " + fails : "ALL OK");

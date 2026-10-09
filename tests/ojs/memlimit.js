// Run with OJS_MEMLIMIT=64: ArrayBuffer storage counts against the realm limit
// (it used to be taken from the system unchecked), and dead buffers are
// collected when the limit is reached.
//
// Not checked: that a buffer dropped a moment ago is free at once. The
// collector scans the C stack conservatively, and a stale copy of a value can
// sit in a dead slot of a live C frame until it is overwritten - so the test
// that fills the limit with live buffers comes last.
var fails = 0;
function check(name, ok) { if (!ok) { fails++; console.log("FAIL " + name); } }
var MB = 1024 * 1024;

var threw = "";
try { new ArrayBuffer(100 * MB); } catch (e) { threw = e.name; }
check("100MB buffer over a 64MB limit throws RangeError", threw === "RangeError");

threw = "";
try { new Uint8Array(90 * MB); } catch (e) { threw = e.name; }
check("90MB typed array throws", threw === "RangeError");

// 200 x 8MB, each dropped right away: needs collections to stay under the limit
var sum = 0;
for (var i = 0; i < 200; i++) { var b = new Uint8Array(8 * MB); b[i] = i; sum += b[i]; }
b = null;
check("churn of dropped 8MB buffers", sum === 199 * 200 / 2);

// garbage made by calls that returned is collected
function churn() { var k = []; for (var j = 0; j < 5; j++) k.push(new ArrayBuffer(8 * MB)); return k.length; }
var total = 0;
for (var c = 0; c < 10; c++) total += churn();
check("buffers of returned calls are collected", total === 50);

// transfer / detach / resize / slice keep the accounting right
function transfers() { var t = new ArrayBuffer(16 * MB); for (var k = 0; k < 20; k++) t = t.transfer(); return t.byteLength; }
check("transfer chain", transfers() === 16 * MB);
function transfers2() { var t = new ArrayBuffer(16 * MB); for (var k = 0; k < 20; k++) t = t.transfer(k & 1 ? 16 * MB : 8 * MB); return t.byteLength; }
check("transfer with new lengths", transfers2() === 16 * MB);
function resizable() { var r = new ArrayBuffer(4, { maxByteLength: 16 * MB }); r.resize(16 * MB); r.resize(1); return r.maxByteLength; }
for (var q = 0; q < 10; q++) check("resizable " + q, resizable() === 16 * MB);
function slices() { var s = new ArrayBuffer(12 * MB); return s.slice(0).byteLength + s.slice(MB).byteLength; }
for (var q = 0; q < 10; q++) check("slice " + q, slices() === 23 * MB);

// keep buffers alive: the limit is reached and throws, it does not crash
var keep = [];
threw = "";
try { for (var j = 0; j < 20; j++) keep.push(new ArrayBuffer(8 * MB)); } catch (e) { threw = e.name; }
check("live buffers hit the limit", threw === "RangeError" && keep.length >= 4 && keep.length < 8);

console.log(fails ? "FAILED " + fails : "ALL OK (kept " + keep.length + ")");

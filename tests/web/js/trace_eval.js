// Focused tracer (diagnostics only): what a page hands to eval / new Function / Trusted Types / dynamic script insertion, with
// the arguments in full (first 400 non-blank characters and the length) and what came back. Same entry points as trace_api.js:
//   WB_PRE=tests/web/js/trace_eval.js WB_DUMP=__traceDump WB_DUMP_FILE=out.json build-host/wbrowse URL ...
//   TRACER=tests/web/js/trace_eval.js node tests/web/trace_edge.mjs URL out.json
(function () {
    'use strict';
    var G = typeof globalThis !== 'undefined' ? globalThis : window;
    if (G.__traceDump) return;
    var log = [], t0 = Date.now(), busy = 0, nev = 0;
    var origToString = Function.prototype.toString, WM = new WeakMap();
    function clip(v, n) {
        var s;
        try { s = typeof v === 'string' ? v : String(v); } catch (e) { return '<unprintable>'; }
        var lead = /^\s*/.exec(s)[0].length;
        var t = s.slice(lead, lead + (n || 400));
        return '[' + s.length + (lead ? ' lead-ws=' + lead : '') + '] ' + t.replace(/\s+/g, ' ');
    }
    function res(v) {
        var t = typeof v;
        if (t === 'function') return 'fn';
        if (t === 'object') return v === null ? 'null' : 'obj:' + ((v.constructor && v.constructor.name) || '?');
        return t + ':' + String(v).slice(0, 60);
    }
    function caller() {
        var st;
        try { st = new Error().stack; } catch (e) { return '?'; }
        var lines = String(st || '').split('\n');
        for (var i = 0; i < lines.length; i++) {
            var l = lines[i];
            if (l.indexOf('trace_eval') >= 0 || l.indexOf('#inline-script-0:') >= 0) continue;
            var m = /(https?:\/\/[^\s)]+?):(\d+):(\d+)/.exec(l);
            if (m) return m[1].replace(/^https?:\/\/[^/]+/, '').replace(/\?.*$/, '').slice(-50) + ':' + m[2];
            if (/\beval\b|anonymous/.test(l)) return 'EVAL';
        }
        return '?';
    }
    function add(kind, arg, r) {
        if (busy) return;
        busy = 1;
        try {
            nev++;
            if (log.length < 400) log.push({ n: nev, t: Date.now() - t0, k: kind, a: arg, r: r, c: caller() });
            else if (!log.cut) { log.cut = 1; log.push({ n: nev, k: 'truncated' }); }
        } finally { busy = 0; }
    }
    function wrap(obj, name, label, argf) {
        var of = obj[name];
        if (typeof of !== 'function') return;
        var w = function () {
            var a = arguments, r, thrown = false, ex;
            try { r = new.target ? Reflect.construct(of, a, new.target) : of.apply(this, a); } catch (e) { thrown = true; ex = e; }
            add(label, argf ? argf(a) : clip(a[0]), thrown ? 'THROW ' + String(ex).slice(0, 120) : res(r));
            if (thrown) throw ex;
            return r;
        };
        try { Object.defineProperty(w, 'name', { value: of.name, configurable: true }); Object.defineProperty(w, 'length', { value: of.length, configurable: true }); } catch (e) { }
        if (of.prototype) { try { w.prototype = of.prototype; } catch (e) { } }
        WM.set(w, of);
        Object.defineProperty(obj, name, { value: w, writable: true, enumerable: false, configurable: true });
    }
    wrap(G, 'eval', 'eval');
    wrap(G, 'Function', 'Function', function (a) { return Array.prototype.map.call(a, function (x) { return clip(x, 200); }).join(' ;; '); });
    // Function.prototype.toString: wrapped functions answer with the original's text; calls are counted per (this name)
    var tsCount = Object.create(null);
    var ts = function () {
        var o = WM.get(this);
        var r = origToString.call(o || this);
        if (!busy) {
            var k = (typeof this === 'function' ? 'fn:' + (this.name || '') : typeof this);
            tsCount[k] = (tsCount[k] || 0) + 1;
            if (tsCount[k] <= 3) add('toString', k + ' ' + clip(r, 120), '');
        }
        return r;
    };
    WM.set(ts, origToString);
    Object.defineProperty(Function.prototype, 'toString', { value: ts, writable: true, enumerable: false, configurable: true });
    // Trusted Types (only where it exists)
    var TT = G.trustedTypes;
    add('trustedTypes', typeof TT, TT ? 'defined' : 'absent');
    if (TT && TT.createPolicy) {
        var ocp = TT.createPolicy;
        TT.createPolicy = function (name, rules) {
            var p = ocp.call(this, name, rules);
            add('createPolicy', String(name) + ' rules=' + (rules ? Object.keys(rules).join(',') : 'none'), 'policy');
            return p;
        };
    }
    // dynamic script elements and listener options probes
    var oae = EventTarget.prototype.addEventListener, ore = EventTarget.prototype.removeEventListener;
    EventTarget.prototype.addEventListener = function (t, f, o) {
        if (t === 'test') add('addEventListener', String(t) + ' opts=' + (o && typeof o === 'object' ? Object.keys(o).join(',') : String(o)), '');
        return oae.apply(this, arguments);
    };
    EventTarget.prototype.removeEventListener = function (t, f, o) {
        if (t === 'test') add('removeEventListener', String(t) + ' opts=' + (o && typeof o === 'object' ? Object.keys(o).join(',') : String(o)), '');
        return ore.apply(this, arguments);
    };
    var oat = G.atob, obt = G.btoa;
    G.atob = function (s) { var r = oat.apply(this, arguments); add('atob', clip(s, 60), 'len ' + r.length); return r; };
    G.btoa = function (s) { var r = obt.apply(this, arguments); add('btoa', clip(s, 40), 'len ' + r.length); return r; };
    WM.set(G.atob, oat); WM.set(G.btoa, obt);
    G.__traceDump = function () { busy = 1; var s = JSON.stringify({ total: nev, tsCount: tsCount, ua: (G.navigator && G.navigator.userAgent) || '', log: log }); busy = 0; return s; };
    Object.defineProperty(G, '__traceDump', { enumerable: false });
})();
//# sourceURL=trace_eval.js

// API access tracer (diagnostics only; never part of the kernel). Run it BEFORE a page's own scripts, in any engine:
//   host harness:  WB_PRE=tests/web/js/trace_api.js WB_DUMP=__traceDump WB_DUMP_FILE=out.json build-host/wbrowse URL ...
//   headless Edge: tests/web/trace_edge.mjs (Page.addScriptToEvaluateOnNewDocument)
// It wraps the getters, setters and methods of the usual browser API objects so that every access is recorded once per
// distinct (kind, name, arguments, result) with the script that made it, in the order first seen. Pages that probe a
// FRESH realm (a hidden same-origin iframe, as bot-detection code does to get unmodified built-ins) are followed: whenever
// the page obtains an iframe's contentWindow the tracer installs itself there, with the labels prefixed "f1:", "f2:"...
// Comparing the two outputs (tests/web/trace_compare.py) shows which browser features the page really looks at, and what
// our engine answers where a real browser answers differently.
(function () {
    'use strict';
    var TOP = typeof globalThis !== 'undefined' ? globalThis : window;
    if (TOP.__traceDump) return;
    var WM = new WeakMap();
    var seen = Object.create(null), perLabel = Object.create(null), log = [], total = 0, busy = 0;
    var dateNow = Date.now, seq = 0;
    var t0 = dateNow();
    var realms = [], nrealm = 0;
    var HOT = /^(Math|JSON|Reflect|Object|Array|Promise|Symbol|String|Number|Boolean|Date|RegExp|Map|Set|WeakMap|WeakSet|Error|Intl|console|WebAssembly|Atomics|undefined|NaN|Infinity|globalThis|isNaN|isFinite|parseInt|parseFloat|encodeURIComponent|decodeURIComponent|encodeURI|decodeURI|escape|unescape|queueMicrotask|clearTimeout|clearInterval|requestAnimationFrame|cancelAnimationFrame)$/;
    var NOISY = /(^|\.)(now|random|getTime|valueOf|getEntries|getEntriesByType|getEntriesByName)$|Math\.|Date\.now/;

    function mark(w, orig) { try { WM.set(w, orig); } catch (e) { } }

    function install(G, tag) {
        var O = G.Object, gopn = O.getOwnPropertyNames, gopd = O.getOwnPropertyDescriptor, dp = O.defineProperty;
        var FP = G.Function.prototype, apply = FP.apply, call = FP.call, origToString = FP.toString;
        var R = G.Reflect;

        function ctorName(v) {
            try { var c = v.constructor; return c && c.name ? c.name : 'Object'; } catch (e) { return 'Object'; }
        }
        function summ(v) {
            var t = typeof v;
            if (v === null) return 'null';
            if (t === 'string') return 's:' + (v.length > 48 ? v.slice(0, 48) + '..(' + v.length + ')' : v);
            if (t === 'number') return 'n:' + (v !== v ? 'NaN' : v);
            if (t === 'boolean') return 'b:' + v;
            if (t === 'undefined') return 'undef';
            if (t === 'function') return 'fn:' + (v.name || '');
            if (t === 'symbol') return 'sym';
            if (t === 'bigint') return 'big';
            if (v === G) return 'obj:window';
            return 'obj:' + ctorName(v);
        }
        function who(v) {
            if (v === G) return 'window';
            if (v === undefined || v === null) return String(v);
            var t = typeof v;
            if (t === 'function') return 'fn:' + (v.name || '');
            if (t !== 'object') return t;
            return ctorName(v);
        }
        // the script that made the call: file:line of the first frame outside this tracer, or "EVAL" for generated code
        function caller() {
            var st;
            try { st = new Error().stack; } catch (e) { return '?'; }
            if (!st) return '?';
            var lines = String(st).split('\n');
            for (var i = 0; i < lines.length; i++) {
                var l = lines[i];
                if (l.indexOf('trace_api') >= 0 || l.indexOf('__trace') >= 0 || l.indexOf('#inline-script-0:') >= 0) continue;
                var isEval = /\beval\b|anonymous[:)]/.test(l);
                var m = /(https?:\/\/[^\s)]+?):(\d+):(\d+)/.exec(l) || /(<anonymous>|eval|<eval>)[^\n]*/.exec(l);
                if (m) {
                    if (isEval) return 'EVAL';
                    var s = m[1] || m[0];
                    s = s.replace(/^https?:\/\/[^/]+/, '').replace(/\?.*$/, '');
                    if (s.length > 60) s = '..' + s.slice(-58);
                    return s + (m[2] ? ':' + m[2] : '');
                }
            }
            return '?';
        }
        function rec(kind, label, args, res) {
            if (busy) return;
            if (label === 'Object.defineProperty' && args === '') return;      // engine-internal bookkeeping properties (_name)
            busy = 1;
            try {
                total++;
                label = tag + label;
                var r = NOISY.test(label) ? 'v' : summ(res);
                var key = kind + ' ' + label + (args ? '(' + args + ')' : '') + ' -> ' + r;
                var e = seen[key];
                if (e) { e.n++; return; }
                var pl = perLabel[kind + label] = (perLabel[kind + label] || 0) + 1;
                if (pl > 24) { if (pl === 25) log.push({ s: seq++, t: dateNow() - t0, k: kind, l: label, a: '', r: '(many distinct results)', c: '', n: 1 }); return; }
                e = seen[key] = { s: seq++, t: dateNow() - t0, k: kind, l: label, a: args || '', r: r, c: caller(), n: 1 };
                log.push(e);
            } finally { busy = 0; }
        }
        function argSumm(a) {
            var out = [];
            for (var i = 0; i < a.length && i < 3; i++) out.push(summ(a[i]));
            return out.join(',');
        }

        // ---- wrap one property of an object
        function wrapProp(obj, name, label) {
            var d;
            try { d = gopd(obj, name); } catch (e) { return; }
            if (!d || !d.configurable) return;
            var full = label + '.' + (typeof name === 'symbol' ? 'sym' : name);
            if (d.get || d.set) {
                var nd = { enumerable: d.enumerable, configurable: true };
                if (d.get) {
                    var og = d.get;
                    nd.get = function () { var r = call.call(og, this); rec('get', full, '', r); return r; };
                    mark(nd.get, og);
                }
                if (d.set) {
                    var os = d.set;
                    nd.set = function (v) { rec('set', full, summ(v), undefined); return call.call(os, this, v); };
                    mark(nd.set, os);
                }
                try { dp(obj, name, nd); } catch (e) { }
            } else if (typeof d.value === 'function') {
                if (name === 'constructor') return;
                var of = d.value;
                var w = function () {
                    var r;
                    if (new.target) { r = R.construct(of, arguments, new.target); rec('new', full, argSumm(arguments), r); return r; }
                    r = apply.call(of, this, arguments);
                    rec('call', full, argSumm(arguments), r);
                    return r;
                };
                try { dp(w, 'name', { value: of.name, configurable: true }); dp(w, 'length', { value: of.length, configurable: true }); } catch (e) { }
                if (of.prototype) { try { w.prototype = of.prototype; } catch (e) { } }
                mark(w, of);
                try { dp(obj, name, { value: w, writable: d.writable, enumerable: d.enumerable, configurable: true }); } catch (e) { }
            }
        }
        function wrapAll(obj, label, skip) {
            if (!obj) return;
            var names;
            try { names = gopn(obj); } catch (e) { return; }
            for (var i = 0; i < names.length; i++) {
                var n = names[i];
                if (skip && skip.test(n)) continue;
                wrapProp(obj, n, label);
            }
        }

        // ---- window: data properties (document, location, chrome, ...) become logging accessors; plain functions are wrapped
        function wrapWindow() {
            var names = gopn(G);
            for (var i = 0; i < names.length; i++) {
                var n = names[i];
                if (n === '__traceDump' || HOT.test(n)) continue;
                var d = gopd(G, n);
                if (!d || !d.configurable) continue;
                if (d.get || d.set) { wrapProp(G, n, 'window'); continue; }
                if (typeof d.value === 'function') {
                    if (/^[A-Z]/.test(n)) continue;                // constructors / namespaces stay as they are
                    wrapProp(G, n, 'window');
                } else if (n !== 'window' && n !== 'self' && n !== 'top' && n !== 'parent' && n !== 'frames' && n !== 'document') {
                    (function (n, d) {
                        var v = d.value;
                        var g = function () { rec('get', 'window.' + n, '', v); return v; };
                        var s = d.writable ? function (x) { rec('set', 'window.' + n, summ(x), undefined); v = x; } : undefined;
                        mark(g, g);
                        try { dp(G, n, { get: g, set: s, enumerable: d.enumerable, configurable: true }); } catch (e) { }
                    })(n, d);
                }
            }
            var dd = gopd(G, 'document');
            if (dd && dd.configurable) {
                if (dd.get) wrapProp(G, 'document', 'window');
                else (function (v) { dp(G, 'document', { get: function () { rec('get', 'window.document', '', v); return v; }, enumerable: true, configurable: true }); })(dd.value);
            }
        }

        var PROTOS = ['Navigator', 'Screen', 'Performance', 'Document', 'HTMLDocument', 'Element', 'HTMLElement', 'Node', 'EventTarget', 'Window',
            'HTMLCanvasElement', 'CanvasRenderingContext2D', 'OffscreenCanvas', 'WebGLRenderingContext', 'WebGL2RenderingContext', 'AudioContext',
            'OfflineAudioContext', 'BaseAudioContext', 'HTMLIFrameElement', 'HTMLScriptElement', 'HTMLInputElement', 'HTMLAnchorElement',
            'HTMLImageElement', 'HTMLMediaElement', 'HTMLVideoElement', 'HTMLAudioElement', 'XMLHttpRequest', 'Storage', 'History', 'Crypto',
            'SubtleCrypto', 'MutationObserver', 'IntersectionObserver', 'ResizeObserver', 'PerformanceObserver', 'MediaSource', 'SourceBuffer',
            'MediaDevices', 'Permissions', 'Notification', 'ServiceWorkerContainer', 'Plugin', 'PluginArray', 'MimeTypeArray', 'NetworkInformation',
            'MediaCapabilities', 'Clipboard', 'Selection', 'Range', 'CSSStyleDeclaration', 'CSS', 'FontFaceSet', 'FontFace', 'Event', 'MouseEvent',
            'KeyboardEvent', 'TextEncoder', 'TextDecoder', 'URL', 'Blob', 'FileReader', 'Worker', 'MessagePort', 'WebSocket', 'RTCPeerConnection',
            'MediaRecorder', 'SpeechSynthesis', 'Gamepad', 'BatteryManager', 'DOMRect', 'DOMMatrix', 'Image', 'Option'];
        function wrapProtos() {
            for (var i = 0; i < PROTOS.length; i++) {
                var n = PROTOS[i], C = G[n];
                if (!C) continue;
                if (C.prototype) wrapAll(C.prototype, n + '.prototype', /^(constructor|on[a-z]+)$/);
                if (typeof C === 'object') wrapAll(C, n);
                else if (n === 'CSS') wrapAll(C, n);
            }
            wrapAll(G.Intl && G.Intl.DateTimeFormat && G.Intl.DateTimeFormat.prototype, 'Intl.DateTimeFormat.prototype');
            wrapAll(G.Intl && G.Intl.NumberFormat && G.Intl.NumberFormat.prototype, 'Intl.NumberFormat.prototype');
            wrapAll(G.Date && G.Date.prototype, 'Date.prototype', /^(constructor|get(Time|Full|Month|Date|Day|Hours|Minutes|Seconds|Milliseconds|UTC.*)|toISOString|valueOf|setTime|Symbol.*)$/);
            if (G.location) wrapAll(G.location, 'location');
            if (G.navigator) wrapAll(G.navigator, 'navigator');
            if (G.screen) wrapAll(G.screen, 'screen');
            if (G.performance) wrapAll(G.performance, 'performance');
        }

        // ---- reflection: how a script enumerates and inspects objects
        function wrapReflection() {
            function wrapFn(obj, name, label, fmt) {
                if (!obj) return;
                var of = obj[name];
                if (typeof of !== 'function') return;
                var w = function () {
                    var r = apply.call(of, this, arguments);
                    if (!busy) {
                        var a = arguments;
                        rec('call', label, fmt(this, a), (r && typeof r === 'object' && typeof r.length === 'number') ? 'arr:' + r.length : r);
                    }
                    return r;
                };
                try { dp(w, 'name', { value: of.name, configurable: true }); dp(w, 'length', { value: of.length, configurable: true }); } catch (e) { }
                mark(w, of);
                dp(obj, name, { value: w, writable: true, enumerable: false, configurable: true });
            }
            var two = function (t, a) { return who(a[0]) + (a.length > 1 ? ',' + summ(a[1]) : ''); };
            var one = function (t, a) { return who(a[0]); };
            wrapFn(O, 'getOwnPropertyNames', 'Object.getOwnPropertyNames', one);
            wrapFn(O, 'keys', 'Object.keys', one);
            wrapFn(O, 'getOwnPropertyDescriptor', 'Object.getOwnPropertyDescriptor', two);
            wrapFn(O, 'getOwnPropertyDescriptors', 'Object.getOwnPropertyDescriptors', one);
            wrapFn(O, 'getPrototypeOf', 'Object.getPrototypeOf', one);
            wrapFn(O, 'setPrototypeOf', 'Object.setPrototypeOf', one);
            wrapFn(O, 'defineProperty', 'Object.defineProperty', function (t, a) { return (typeof a[1] === 'string' && a[1].charAt(0) === '_') ? '' : two(t, a); });
            wrapFn(O, 'getOwnPropertySymbols', 'Object.getOwnPropertySymbols', one);
            wrapFn(R, 'ownKeys', 'Reflect.ownKeys', one);
            wrapFn(R, 'has', 'Reflect.has', two);
            wrapFn(R, 'getPrototypeOf', 'Reflect.getPrototypeOf', one);
            wrapFn(O.prototype, 'hasOwnProperty', 'Object.prototype.hasOwnProperty', function (t, a) { return who(t) + ',' + summ(a[0]); });
            wrapFn(O.prototype, 'isPrototypeOf', 'Object.prototype.isPrototypeOf', function (t, a) { return who(t); });
            wrapFn(O.prototype, 'propertyIsEnumerable', 'Object.prototype.propertyIsEnumerable', function (t, a) { return who(t) + ',' + summ(a[0]); });
            wrapFn(O.prototype, 'toString', 'Object.prototype.toString', function (t, a) { return who(t); });
            wrapFn(FP, 'bind', 'Function.prototype.bind', function (t, a) { return who(t); });
            wrapFn(G.Error, 'captureStackTrace', 'Error.captureStackTrace', one);
            var ts = function () {
                var o = WM.get(this);
                var r = call.call(origToString, o || this);
                if (!busy) rec('call', 'Function.prototype.toString', who(this) + ':' + (r.length > 60 ? r.slice(0, 60).replace(/\s+/g, ' ') + '..' : r.replace(/\s+/g, ' ')), undefined);
                return r;
            };
            mark(ts, origToString);
            try { dp(ts, 'name', { value: 'toString', configurable: true }); } catch (e) { }
            dp(FP, 'toString', { value: ts, writable: true, enumerable: false, configurable: true });
        }

        // ---- a page that wants a clean realm makes an iframe and reads its contentWindow: install the tracer there too
        function hookFrames() {
            var P = G.HTMLIFrameElement && G.HTMLIFrameElement.prototype;
            if (!P) return;
            ['contentWindow', 'contentDocument'].forEach(function (n) {
                var d = gopd(P, n);
                if (!d || !d.get) return;
                var og = d.get;
                var g = function () {
                    var r = call.call(og, this);
                    try {
                        var w = n === 'contentWindow' ? r : (r && r.defaultView);
                        if (w && realms.indexOf(w) < 0) {
                            realms.push(w);
                            var t = 'f' + (++nrealm) + ':';
                            rec('call', 'trace.newRealm', t, undefined);
                            try { install(w, t); } catch (e) { rec('call', 'trace.installFailed', t + String(e).slice(0, 60), undefined); }
                        }
                    } catch (e) { }
                    rec('get', 'HTMLIFrameElement.prototype.' + n, '', r);
                    return r;
                };
                mark(g, og);
                dp(P, n, { get: g, set: d.set, enumerable: d.enumerable, configurable: true });
            });
        }

        wrapReflection();
        wrapProtos();
        wrapWindow();
        if (tag === '') hookFrames();
        else hookFrames();
    }

    install(TOP, '');
    realms.push(TOP);

    void TOP.__traceReady;                  // a guaranteed miss: lines the engine logs before this are the tracer's own

    TOP.__traceDump = function () {
        busy = 1;
        var out = { total: total, distinct: log.length, realms: nrealm, ua: (TOP.navigator && TOP.navigator.userAgent) || '', log: log };
        var s = JSON.stringify(out);
        busy = 0;
        return s;
    };
    Object.defineProperty(TOP, '__traceDump', { enumerable: false });
})();
//# sourceURL=trace_api.js

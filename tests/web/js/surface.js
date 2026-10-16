// API surface dump: which names exist on the usual roots and what kind each is. Run in any browser/engine and diff the
// outputs (tests/web/surface_diff.py): the difference is the list of web APIs this engine lacks or shapes differently.
(function () {
    var lines = [];
    function kind(o, k) {
        var d;
        try { d = Object.getOwnPropertyDescriptor(o, k); } catch (e) { return 'err'; }
        if (!d) return 'none';
        if (d.get || d.set) return 'accessor' + (d.get ? '+g' : '') + (d.set ? '+s' : '');
        var t = typeof d.value;
        return t;
    }
    function dump(label, o) {
        if (o === undefined || o === null) { lines.push(label + ' <absent>'); return; }
        var names;
        try { names = Object.getOwnPropertyNames(o); } catch (e) { lines.push(label + ' <unreadable>'); return; }
        names.sort();
        for (var i = 0; i < names.length; i++) lines.push(label + '.' + names[i] + ' ' + kind(o, names[i]));
    }
    var g = typeof globalThis !== 'undefined' ? globalThis : window;
    function proto(n) { return g[n] && g[n].prototype; }
    dump('window', g);
    var protos = ['Navigator', 'Document', 'Element', 'HTMLElement', 'Node', 'EventTarget', 'Window', 'Screen', 'Performance',
        'History', 'Location', 'Crypto', 'SubtleCrypto', 'Storage', 'HTMLCanvasElement', 'CanvasRenderingContext2D', 'HTMLMediaElement',
        'HTMLVideoElement', 'MediaSource', 'SourceBuffer', 'Event', 'MouseEvent', 'KeyboardEvent', 'XMLHttpRequest', 'Response', 'Request',
        'Headers', 'URL', 'URLSearchParams', 'Blob', 'Promise', 'Array', 'Object', 'Function', 'String', 'Number', 'Date', 'RegExp',
        'Map', 'Set', 'WeakMap', 'ArrayBuffer', 'DataView', 'Uint8Array', 'Error', 'Symbol', 'Proxy', 'Reflect', 'JSON', 'Math', 'Intl',
        'HTMLIFrameElement', 'HTMLScriptElement', 'HTMLInputElement', 'ShadowRoot', 'CSSStyleDeclaration', 'MutationObserver',
        'IntersectionObserver', 'ResizeObserver', 'PerformanceObserver', 'TextEncoder', 'TextDecoder', 'WebSocket', 'MessagePort',
        'AudioContext', 'OfflineAudioContext', 'WebGLRenderingContext', 'RTCPeerConnection', 'Worker', 'Notification', 'ServiceWorkerContainer'];
    for (var i = 0; i < protos.length; i++) {
        var n = protos[i], c = g[n];
        if (c === undefined) { lines.push('ctor ' + n + ' <absent>'); continue; }
        lines.push('ctor ' + n + ' ' + typeof c);
        if (typeof c === 'function') dump(n + '.static', c);
        if (c && c.prototype) dump(n + '.prototype', c.prototype);
        else if (typeof c === 'object') dump(n, c);
    }
    dump('navigator', g.navigator);
    dump('screen', g.screen);
    dump('document', g.document);
    dump('performance', g.performance);
    dump('history', g.history);
    dump('location', g.location);
    dump('crypto', g.crypto);
    dump('console', g.console);
    dump('Intl', g.Intl);
    var out = lines.join('\n');
    if (typeof document !== 'undefined' && document.body) {
        var pre = document.createElement('pre');
        pre.id = 'surface';
        pre.textContent = '\n' + out + '\n';
        document.body.appendChild(pre);
    }
    g.__surface = out;
})();

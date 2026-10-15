// okai Web API prelude: the browser environment for page scripts, written in
// JS on top of the C natives in wjs.c / wjs_dom.c (the `W` object). Compiled
// once per document realm; called as (function (W, G) {...})(natives, global)
// and returns the hooks object the C side drives (event dispatch, timers,
// fetch completions, lifecycle events).
(function (W, G) {
'use strict';

const hidden = (o, name, value) =>
    Object.defineProperty(o, name, { value, writable: true, configurable: true, enumerable: false });
const accessor = (o, name, get, set) =>
    Object.defineProperty(o, name, { get, set, configurable: true, enumerable: true });
const methods = (o, table) => { for (const k of Object.keys(table)) hidden(o, k, table[k]); };
const str = v => (v === null || v === undefined) ? String(v) : String(v);
const illegal = () => { throw new TypeError('Illegal constructor'); };
function report(e, where) {
    try {
        let msg = (e && e.stack) ? (String(e) + '\n' + e.stack) : String(e);
        W.log(3, 'Uncaught ' + msg + (where ? ' [' + where + ']' : ''));
    } catch (_) { W.log(3, 'Uncaught (unprintable error)'); }
    try {
        const ev = new ErrorEvent('error', { message: String(e && e.message || e), error: e });
        dispatch(G, ev);
    } catch (_) {}
}

// ---------------------------------------------------------------- DOMException
class DOMException extends Error {
    constructor(message, name) { super(message === undefined ? '' : String(message)); this.name = name || 'Error'; }
    get code() { return { IndexSizeError: 1, HierarchyRequestError: 3, NotFoundError: 8, NotSupportedError: 9,
        InvalidStateError: 11, SyntaxError: 12, InvalidCharacterError: 5, NetworkError: 19, AbortError: 20,
        TimeoutError: 23, DataCloneError: 25, SecurityError: 18 }[this.name] || 0; }
}

// ---------------------------------------------------------------- events
const LISTENERS = new WeakMap();   // target -> Map(type -> [{cb, capture, once, passive}])
const HANDLERS = new WeakMap();    // target -> Map(type -> fn)  (onclick = ...)
const INLINE = new WeakMap();      // element -> Map(type -> {src, fn})

class Event {
    constructor(type, init) {
        if (arguments.length < 1) throw new TypeError('Event: type required');
        init = init || {};
        hidden(this, '_type', String(type));
        hidden(this, '_flags', 0);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.eventPhase = 0;
        this.target = null;
        this.currentTarget = null;
        this.isTrusted = false;
        this.timeStamp = W.now();
        hidden(this, '_stop', 0);
        hidden(this, '_passive', false);
    }
    get type() { return this._type; }
    get srcElement() { return this.target; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this._stop > 0; }
    set cancelBubble(v) { if (v) this._stop = Math.max(this._stop, 1); }
    preventDefault() { if (this.cancelable && !this._passive) this.defaultPrevented = true; }
    stopPropagation() { this._stop = Math.max(this._stop, 1); }
    stopImmediatePropagation() { this._stop = 2; }
    composedPath() { return this._path ? this._path.slice() : []; }
    initEvent(type, bubbles, cancelable) {
        this._type = String(type); this.bubbles = !!bubbles; this.cancelable = !!cancelable;
    }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;

class CustomEvent extends Event {
    constructor(type, init) { super(type, init); this.detail = init && init.detail !== undefined ? init.detail : null; }
    initCustomEvent(type, b, c, detail) { this.initEvent(type, b, c); this.detail = detail; }
}
class UIEvent extends Event {
    constructor(type, init) { super(type, init); init = init || {}; this.view = init.view || null; this.detail = init.detail | 0; }
}
class MouseEvent extends UIEvent {
    constructor(type, init) {
        super(type, init); init = init || {};
        for (const k of ['screenX', 'screenY', 'clientX', 'clientY', 'button', 'buttons', 'movementX', 'movementY'])
            this[k] = +init[k] || 0;
        for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!init[k];
        this.relatedTarget = init.relatedTarget || null;
    }
    get pageX() { return this.clientX + G.scrollX; }
    get pageY() { return this.clientY + G.scrollY; }
    get x() { return this.clientX; }
    get y() { return this.clientY; }
    get offsetX() { return this.clientX; }
    get offsetY() { return this.clientY; }
    get which() { return this.button + 1; }
    getModifierState() { return false; }
}
class PointerEvent extends MouseEvent {
    constructor(type, init) { super(type, init); init = init || {}; this.pointerId = init.pointerId || 1; this.pointerType = init.pointerType || 'mouse'; this.isPrimary = true; this.width = 1; this.height = 1; this.pressure = 0; }
}
class WheelEvent extends MouseEvent {
    constructor(type, init) { super(type, init); init = init || {}; this.deltaX = +init.deltaX || 0; this.deltaY = +init.deltaY || 0; this.deltaZ = 0; this.deltaMode = 0; }
}
class KeyboardEvent extends UIEvent {
    constructor(type, init) {
        super(type, init); init = init || {};
        this.key = init.key || ''; this.code = init.code || ''; this.location = 0; this.repeat = !!init.repeat;
        this.isComposing = false;
        for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!init[k];
        this.keyCode = init.keyCode | 0; this.charCode = init.charCode | 0;
    }
    get which() { return this.keyCode; }
    getModifierState() { return false; }
}
class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = (i && i.relatedTarget) || null; } }
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data === undefined ? null : i.data; this.inputType = i.inputType || ''; this.isComposing = false; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.filename = i.filename || ''; this.lineno = i.lineno | 0; this.colno = i.colno | 0; this.error = i.error; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.lengthComputable = !!i.lengthComputable; this.loaded = i.loaded || 0; this.total = i.total || 0; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.lastEventId = ''; this.source = i.source || null; this.ports = []; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && i.state !== undefined ? i.state : null; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
class PageTransitionEvent extends Event { constructor(t, i) { super(t, i); this.persisted = !!(i && i.persisted); } }
class SubmitEvent extends Event { constructor(t, i) { super(t, i); this.submitter = (i && i.submitter) || null; } }
class StorageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || null; this.oldValue = i.oldValue || null; this.newValue = i.newValue || null; this.url = i.url || ''; this.storageArea = i.storageArea || null; } }
class AnimationEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.animationName = i.animationName || ''; this.elapsedTime = 0; } }
class TransitionEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.propertyName = i.propertyName || ''; this.elapsedTime = 0; } }
class PromiseRejectionEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.promise = i.promise; this.reason = i.reason; } }
class TouchEvent extends UIEvent { constructor(t, i) { super(t, i); this.touches = []; this.targetTouches = []; this.changedTouches = []; } }
class CompositionEvent extends UIEvent { constructor(t, i) { super(t, i); this.data = (i && i.data) || ''; } }
class ClipboardEvent extends Event { constructor(t, i) { super(t, i); this.clipboardData = null; } }

function EventTarget() {}
function listenerOpts(opts) {
    if (typeof opts === 'boolean') return { capture: opts, once: false, passive: false, signal: null };
    opts = opts || {};
    return { capture: !!opts.capture, once: !!opts.once, passive: !!opts.passive, signal: opts.signal || null };
}
// WebIDL: a null/undefined `this` (bare `addEventListener(...)` in strict or
// module code) means the global object.
methods(EventTarget.prototype, {
    addEventListener(type, cb, opts) {
        if (this == null) return G.addEventListener(type, cb, opts);
        if (cb === null || cb === undefined) return;
        const o = listenerOpts(opts);
        if (o.signal && o.signal.aborted) return;
        type = String(type);
        let m = LISTENERS.get(this);
        if (!m) { m = new Map(); LISTENERS.set(this, m); }
        let list = m.get(type);
        if (!list) { list = []; m.set(type, list); }
        for (const l of list) if (l.cb === cb && l.capture === o.capture) return;
        const rec = { cb, capture: o.capture, once: o.once, passive: o.passive, removed: false };
        list.push(rec);
        if (o.signal) o.signal.addEventListener('abort', () => this.removeEventListener(type, cb, o.capture));
        if (this === G && (type === 'load' || type === 'DOMContentLoaded')) lateLifecycle(type, rec);
    },
    removeEventListener(type, cb, opts) {
        if (this == null) return G.removeEventListener(type, cb, opts);
        const capture = listenerOpts(opts).capture;
        const m = LISTENERS.get(this);
        const list = m && m.get(String(type));
        if (!list) return;
        for (let i = 0; i < list.length; i++)
            if (list[i].cb === cb && list[i].capture === capture) { list[i].removed = true; list.splice(i, 1); return; }
    },
    dispatchEvent(ev) {
        if (!(ev instanceof Event)) throw new TypeError('dispatchEvent: not an Event');
        return dispatch(this == null ? G : this, ev);
    }
});

function parentForEvent(t, ev) {
    if (t === G) return null;
    if (t === document) return ev._type === 'load' ? null : G;
    if (t instanceof Node) {
        const p = W.parent(t);
        if (p) return p;
        return null;
    }
    return null;
}

function inlineHandler(t, type) {
    if (!(t instanceof Element) && t !== G && t !== document) return null;
    let src;
    if (t === G || t === document) {
        // <body onload=...> handlers are window handlers
        const b = document.body;
        if (t === G && b && /^(load|unload|beforeunload|resize|scroll|hashchange|popstate|message|pageshow|pagehide|error)$/.test(type))
            src = W.getAttr(b, 'on' + type);
        if (src === null || src === undefined) return null;
    } else {
        src = W.getAttr(t, 'on' + type);
        if (src === null) return null;
    }
    let m = INLINE.get(t);
    if (!m) { m = new Map(); INLINE.set(t, m); }
    let rec = m.get(type);
    if (!rec || rec.src !== src) {
        let fn = null;
        try { fn = new Function('event', src); } catch (e) { report(e, 'on' + type + ' attribute'); }
        rec = { src, fn };
        m.set(type, rec);
    }
    return rec.fn;
}

function invoke(t, ev, phase) {
    ev.currentTarget = t;
    ev.eventPhase = phase;
    const m = LISTENERS.get(t);
    const list = m && m.get(ev._type);
    if (list && list.length) {
        for (const l of list.slice()) {
            if (l.removed) continue;
            if (phase === 1 && !l.capture) continue;
            if (phase === 3 && l.capture) continue;
            if (l.once) t.removeEventListener(ev._type, l.cb, l.capture);
            ev._passive = l.passive;
            try {
                if (typeof l.cb === 'function') l.cb.call(t, ev);
                else if (l.cb && typeof l.cb.handleEvent === 'function') l.cb.handleEvent(ev);
            } catch (e) { report(e, ev._type + ' listener'); }
            ev._passive = false;
            if (ev._stop === 2) return;
        }
    }
    if (phase !== 1) {
        const hm = HANDLERS.get(t);
        let h = hm && hm.get(ev._type);
        if (h === undefined) h = inlineHandler(t, ev._type);
        if (typeof h === 'function') {
            let r;
            try {
                r = (ev._type === 'error' && t === G && ev instanceof ErrorEvent)
                    ? h.call(t, ev.message, ev.filename, ev.lineno, ev.colno, ev.error)
                    : h.call(t, ev);
            } catch (e) { report(e, 'on' + ev._type); }
            if (r === false && ev._type !== 'mouseover' && ev._type !== 'error') ev.preventDefault();
            if (ev._type === 'beforeunload' && typeof r === 'string') ev.preventDefault();
        }
    }
}

function dispatch(target, ev) {
    ev.target = target;
    const path = [target];
    for (let n = parentForEvent(target, ev); n; n = parentForEvent(n, ev)) path.push(n);
    hidden(ev, '_path', path);
    ev._stop = 0;
    for (let i = path.length - 1; i > 0 && !ev._stop; i--) invoke(path[i], ev, 1);
    if (!ev._stop) invoke(target, ev, 2);
    if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) invoke(path[i], ev, 3);
    ev.eventPhase = 0;
    ev.currentTarget = null;
    return !ev.defaultPrevented;
}

// on<event> handler properties on elements, document and window
const EVENT_NAMES = ['abort', 'animationend', 'animationiteration', 'animationstart', 'auxclick', 'beforeinput',
    'beforeunload', 'blur', 'cancel', 'change', 'click', 'close', 'contextmenu', 'copy', 'cut', 'dblclick', 'drag',
    'dragend', 'dragenter', 'dragleave', 'dragover', 'dragstart', 'drop', 'error', 'focus', 'focusin', 'focusout',
    'hashchange', 'input', 'invalid', 'keydown', 'keypress', 'keyup', 'load', 'loadeddata', 'loadedmetadata',
    'loadend', 'loadstart', 'message', 'mousedown', 'mouseenter', 'mouseleave', 'mousemove', 'mouseout',
    'mouseover', 'mouseup', 'offline', 'online', 'pagehide', 'pageshow', 'paste', 'pause', 'play', 'playing',
    'pointercancel', 'pointerdown', 'pointerenter', 'pointerleave', 'pointermove', 'pointerout', 'pointerover',
    'pointerup', 'popstate', 'progress', 'readystatechange', 'reset', 'resize', 'scroll', 'scrollend', 'select',
    'selectionchange', 'storage', 'canplay', 'canplaythrough', 'durationchange', 'emptied', 'ended', 'ratechange', 'seeked',
    'seeking', 'stalled', 'suspend', 'volumechange', 'waiting', 'sourceopen', 'sourceended', 'sourceclose', 'updatestart',
    'update', 'updateend', 'addsourcebuffer', 'removesourcebuffer', 'submit', 'timeupdate', 'toggle', 'touchcancel', 'touchend', 'touchmove',
    'touchstart', 'transitionend', 'unload', 'visibilitychange', 'wheel'];
function defineHandlers(proto) {
    for (const name of EVENT_NAMES) {
        Object.defineProperty(proto, 'on' + name, {
            configurable: true, enumerable: true,
            get() { const m = HANDLERS.get(this); const h = m && m.get(name); return h === undefined ? (inlineHandler(this, name) || null) : h; },
            set(fn) {
                let m = HANDLERS.get(this);
                if (!m) { m = new Map(); HANDLERS.set(this, m); }
                m.set(name, typeof fn === 'function' ? fn : null);
                if (this === G && name === 'load' && typeof fn === 'function') lateLifecycle('load', { cb: fn, handler: true });
            }
        });
    }
}

// ---------------------------------------------------------------- DOM classes
function Node() { illegal(); }
Node.prototype = Object.create(EventTarget.prototype, { constructor: { value: Node, writable: true, configurable: true } });
Object.setPrototypeOf(Node, EventTarget);
const NODE_CONSTS = { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4,
    PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10,
    DOCUMENT_FRAGMENT_NODE: 11, DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2,
    DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16,
    DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32 };
for (const k of Object.keys(NODE_CONSTS)) { Node[k] = NODE_CONSTS[k]; Node.prototype[k] = NODE_CONSTS[k]; }

function sub(name, parent, ctor) {
    const C = ctor || function () { illegal(); };
    Object.defineProperty(C, 'name', { value: name });
    C.prototype = Object.create(parent.prototype, { constructor: { value: C, writable: true, configurable: true } });
    Object.setPrototypeOf(C, parent);
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
    G[name] = C;
    return C;
}
const CharacterData = sub('CharacterData', Node);
const Text = sub('Text', CharacterData, function Text(data) {
    if (!new.target) illegal();
    return W.createText(data === undefined ? '' : String(data));
});
const Comment = sub('Comment', CharacterData, function Comment(data) {
    if (!new.target) illegal();
    return W.createComment(data === undefined ? '' : String(data));
});
const CDATASection = sub('CDATASection', Text);
sub('ProcessingInstruction', CharacterData);   // never created from HTML; feature checks read its prototype
const Element = sub('Element', Node);
const DocumentFragment = sub('DocumentFragment', Node, function DocumentFragment() {
    if (!new.target) illegal();
    return W.createFragment();
});
const ShadowRoot = sub('ShadowRoot', DocumentFragment);
const Document = sub('Document', Node);
const HTMLDocument = sub('HTMLDocument', Document);
const DocumentType = sub('DocumentType', Node);
const Attr = sub('Attr', Node);

// custom elements state (used by the HTMLElement constructor)
const CE_BY_NAME = new Map(), CE_BY_CTOR = new Map(), CE_WAIT = new Map();
let ceUpgrading = null;

const HTMLElement = sub('HTMLElement', Element, function HTMLElement() {
    if (!new.target) illegal();
    if (ceUpgrading) {
        const el = ceUpgrading;
        ceUpgrading = null;
        Object.setPrototypeOf(el, new.target.prototype);
        return el;
    }
    const name = CE_BY_CTOR.get(new.target);
    if (!name) illegal();
    const el = W.create(name, 0);
    Object.setPrototypeOf(el, new.target.prototype);
    return el;
});
const SVGElement = sub('SVGElement', Element);
const SVGGraphicsElement = sub('SVGGraphicsElement', SVGElement);
const SVGSVGElement = sub('SVGSVGElement', SVGGraphicsElement);
const MathMLElement = sub('MathMLElement', Element);
const HTMLUnknownElement = sub('HTMLUnknownElement', HTMLElement);

const PROTO_BY_TAG = Object.create(null);
function htmlSub(name, tags) {
    const C = sub(name, HTMLElement);
    for (const t of tags) PROTO_BY_TAG[t] = C.prototype;
    return C;
}
const HTMLAnchorElement = htmlSub('HTMLAnchorElement', ['a']);
const HTMLAreaElement = htmlSub('HTMLAreaElement', ['area']);
const HTMLBodyElement = htmlSub('HTMLBodyElement', ['body']);
const HTMLBRElement = htmlSub('HTMLBRElement', ['br']);
const HTMLButtonElement = htmlSub('HTMLButtonElement', ['button']);
const HTMLCanvasElement = htmlSub('HTMLCanvasElement', ['canvas']);
const HTMLDivElement = htmlSub('HTMLDivElement', ['div']);
const HTMLDListElement = htmlSub('HTMLDListElement', ['dl']);
const HTMLDetailsElement = htmlSub('HTMLDetailsElement', ['details']);
const HTMLDialogElement = htmlSub('HTMLDialogElement', ['dialog']);
const HTMLFieldSetElement = htmlSub('HTMLFieldSetElement', ['fieldset']);
const HTMLFormElement = htmlSub('HTMLFormElement', ['form']);
const HTMLHeadElement = htmlSub('HTMLHeadElement', ['head']);
const HTMLHeadingElement = htmlSub('HTMLHeadingElement', ['h1', 'h2', 'h3', 'h4', 'h5', 'h6']);
const HTMLHRElement = htmlSub('HTMLHRElement', ['hr']);
const HTMLHtmlElement = htmlSub('HTMLHtmlElement', ['html']);
const HTMLIFrameElement = htmlSub('HTMLIFrameElement', ['iframe']);
const HTMLImageElement = htmlSub('HTMLImageElement', ['img']);
const HTMLInputElement = htmlSub('HTMLInputElement', ['input']);
const HTMLLabelElement = htmlSub('HTMLLabelElement', ['label']);
const HTMLLegendElement = htmlSub('HTMLLegendElement', ['legend']);
const HTMLLIElement = htmlSub('HTMLLIElement', ['li']);
const HTMLLinkElement = htmlSub('HTMLLinkElement', ['link']);
const HTMLMapElement = htmlSub('HTMLMapElement', ['map']);
const HTMLMetaElement = htmlSub('HTMLMetaElement', ['meta']);
const HTMLMediaElement = sub('HTMLMediaElement', HTMLElement);
const HTMLMeterElement = htmlSub('HTMLMeterElement', ['meter']);
const HTMLModElement = htmlSub('HTMLModElement', ['ins', 'del']);
const HTMLOListElement = htmlSub('HTMLOListElement', ['ol']);
const HTMLOptGroupElement = htmlSub('HTMLOptGroupElement', ['optgroup']);
const HTMLOptionElement = htmlSub('HTMLOptionElement', ['option']);
const HTMLOutputElement = htmlSub('HTMLOutputElement', ['output']);
const HTMLParagraphElement = htmlSub('HTMLParagraphElement', ['p']);
const HTMLPictureElement = htmlSub('HTMLPictureElement', ['picture']);
const HTMLPreElement = htmlSub('HTMLPreElement', ['pre', 'listing', 'xmp']);
const HTMLProgressElement = htmlSub('HTMLProgressElement', ['progress']);
const HTMLQuoteElement = htmlSub('HTMLQuoteElement', ['q', 'blockquote']);
const HTMLScriptElement = htmlSub('HTMLScriptElement', ['script']);
const HTMLSelectElement = htmlSub('HTMLSelectElement', ['select']);
const HTMLSlotElement = htmlSub('HTMLSlotElement', ['slot']);
const HTMLSourceElement = htmlSub('HTMLSourceElement', ['source']);
const HTMLSpanElement = htmlSub('HTMLSpanElement', ['span']);
const HTMLStyleElement = htmlSub('HTMLStyleElement', ['style']);
const HTMLTableElement = htmlSub('HTMLTableElement', ['table']);
const HTMLTableCaptionElement = htmlSub('HTMLTableCaptionElement', ['caption']);
const HTMLTableCellElement = htmlSub('HTMLTableCellElement', ['td', 'th']);
const HTMLTableColElement = htmlSub('HTMLTableColElement', ['col', 'colgroup']);
const HTMLTableRowElement = htmlSub('HTMLTableRowElement', ['tr']);
const HTMLTableSectionElement = htmlSub('HTMLTableSectionElement', ['tbody', 'thead', 'tfoot']);
const HTMLTemplateElement = htmlSub('HTMLTemplateElement', ['template']);
const HTMLTextAreaElement = htmlSub('HTMLTextAreaElement', ['textarea']);
const HTMLTimeElement = htmlSub('HTMLTimeElement', ['time']);
const HTMLTitleElement = htmlSub('HTMLTitleElement', ['title']);
const HTMLUListElement = htmlSub('HTMLUListElement', ['ul']);
const HTMLVideoElement = sub('HTMLVideoElement', HTMLMediaElement);
const HTMLAudioElement = sub('HTMLAudioElement', HTMLMediaElement);
PROTO_BY_TAG.video = HTMLVideoElement.prototype;
PROTO_BY_TAG.audio = HTMLAudioElement.prototype;
const HTMLEmbedElement = htmlSub('HTMLEmbedElement', ['embed']);
const HTMLObjectElement = htmlSub('HTMLObjectElement', ['object']);
const HTMLBaseElement = htmlSub('HTMLBaseElement', ['base']);
const HTMLDataElement = htmlSub('HTMLDataElement', ['data']);
const HTMLDataListElement = htmlSub('HTMLDataListElement', ['datalist']);
const HTMLMenuElement = htmlSub('HTMLMenuElement', ['menu']);
const HTMLTrackElement = htmlSub('HTMLTrackElement', ['track']);
const HTMLFrameSetElement = htmlSub('HTMLFrameSetElement', ['frameset']);
const KNOWN_HTML = new Set(('abbr address article aside b bdi bdo cite code dd dfn dt em figcaption figure footer ' +
    'header hgroup i kbd main mark nav noscript rp rt ruby s samp search section small strong sub summary sup u var ' +
    'wbr center big tt strike font nobr acronym').split(' '));

// C asks for the prototype of a fresh wrapper
function protoFor(name, ns, type) {
    if (type === 1) {
        if (ns === 1) return name === 'svg' ? SVGSVGElement.prototype : SVGGraphicsElement.prototype;
        if (ns === 2) return MathMLElement.prototype;
        const p = PROTO_BY_TAG[name];
        if (p) return p;
        // custom elements start as HTMLElement: the upgrade runs the
        // constructor and switches the prototype (never skip construction)
        if (name.indexOf('-') > 0 || KNOWN_HTML.has(name)) return HTMLElement.prototype;
        return HTMLUnknownElement.prototype;
    }
    if (type === 3) return Text.prototype;
    if (type === 8) return Comment.prototype;
    if (type === 9) return HTMLDocument.prototype;
    if (type === 11) return DocumentFragment.prototype;
    return Node.prototype;
}

// ---------------------------------------------------------------- collections
function makeList(arr, Cls) {
    Object.setPrototypeOf(arr, Cls.prototype);
    return arr;
}
class NodeList extends Array {
    item(i) { return this[i] === undefined ? null : this[i]; }
    static get [Symbol.species]() { return Array; }
}
class HTMLCollection extends Array {
    item(i) { return this[i] === undefined ? null : this[i]; }
    namedItem(n) { for (const e of this) if (e.id === n || e.getAttribute('name') === n) return e; return null; }
    static get [Symbol.species]() { return Array; }
}
G.NodeList = NodeList;
G.HTMLCollection = HTMLCollection;
const nodeList = a => makeList(a, NodeList);
const htmlCollection = a => makeList(a, HTMLCollection);

// ---------------------------------------------------------------- Node
function toNode(v) {
    if (v instanceof Node) return v;
    return W.createText(String(v));
}
function nodesToNode(args) {
    if (args.length === 1) return toNode(args[0]);
    const f = W.createFragment();
    for (const a of args) W.insert(f, toNode(a), null);
    return f;
}
function isAncestor(a, b) {   // a is an inclusive ancestor of b
    for (let n = b; n; n = W.parent(n)) if (n === a) return true;
    return false;
}
function preInsert(parent, node, ref) {
    if (!(node instanceof Node)) throw new TypeError("Failed to execute 'insert': parameter is not of type 'Node'.");
    if (isAncestor(node, parent)) throw new DOMException('The new child element contains the parent.', 'HierarchyRequestError');
    if (ref && W.parent(ref) !== parent) throw new DOMException('The node before which the new node is to be inserted is not a child of this node.', 'NotFoundError');
    if (ref === node) ref = W.next(node);
    const frag = W.ntype(node) === 11;
    const moved = frag ? W.children(node) : [node];
    const wasConnected = frag ? [] : (ceActive && W.connected(node) ? [node] : []);
    if (wasConnected.length) ceDisconnect(node);
    W.insert(parent, node, ref || null);
    if (ceActive && W.connected(parent)) for (const n of moved) ceConnect(n);
    return node;
}
methods(Node.prototype, {
    appendChild(c) { return preInsert(this, c, null); },
    insertBefore(c, ref) { return preInsert(this, c, ref || null); },
    removeChild(c) {
        if (!(c instanceof Node) || W.parent(c) !== this) throw new DOMException('The node to be removed is not a child of this node.', 'NotFoundError');
        const wasConnected = ceActive && W.connected(c);
        W.remove(c);
        if (wasConnected) ceDisconnect(c);
        return c;
    },
    replaceChild(nc, old) {
        if (!(old instanceof Node) || W.parent(old) !== this) throw new DOMException('The node to be replaced is not a child of this node.', 'NotFoundError');
        if (nc === old) return old;
        const ref = W.next(old);
        this.removeChild(old);
        preInsert(this, nc, ref === nc ? W.next(nc) : ref);
        return old;
    },
    hasChildNodes() { return W.first(this) !== null; },
    contains(o) { return o instanceof Node ? isAncestor(this, o) : false; },
    cloneNode(deep) {
        const c = W.clone(this, !!deep);
        if (ceActive && c && W.ntype(c) === 1) ceUpgradeTree(c);
        return c;
    },
    isEqualNode(o) { return o instanceof Node && W.html(this, 1) === W.html(o, 1); },
    isSameNode(o) { return this === o; },
    getRootNode() { let n = this; for (let p; (p = W.parent(n)); n = p); return n; },
    normalize() {
        let c = W.first(this);
        while (c) {
            const next = W.next(c);
            if (W.ntype(c) === 3) {
                let t = W.data(c), n = next;
                while (n && W.ntype(n) === 3) { t += W.data(n); const nn = W.next(n); W.remove(n); n = nn; }
                if (t === '') W.remove(c); else W.setData(c, t);
                c = n;
                continue;
            }
            if (W.ntype(c) === 1) c.normalize();
            c = next;
        }
    },
    compareDocumentPosition(o) {
        if (o === this) return 0;
        const chain = n => { const a = []; for (; n; n = W.parent(n)) a.unshift(n); return a; };
        const a = chain(this), b = chain(o);
        if (a[0] !== b[0]) return 1 | 32 | 2;
        let i = 0;
        while (i < a.length && i < b.length && a[i] === b[i]) i++;
        if (i === a.length) return 4 | 16;           // o is a descendant
        if (i === b.length) return 2 | 8;            // o is an ancestor
        // siblings under a[i-1]: which comes first?
        for (let s = W.next(a[i]); s; s = W.next(s)) if (s === b[i]) return 4;
        return 2;
    },
    lookupNamespaceURI() { return null; },
    isDefaultNamespace() { return false; }
});
accessor(Node.prototype, 'nodeType', function () { return W.ntype(this); });
accessor(Node.prototype, 'nodeName', function () {
    const t = W.ntype(this);
    if (t === 1) { const n = W.name(this); return W.ns(this) === 0 ? n.toUpperCase() : n; }
    return t === 3 ? '#text' : t === 8 ? '#comment' : t === 9 ? '#document' : '#document-fragment';
});
accessor(Node.prototype, 'nodeValue', function () { const t = W.ntype(this); return t === 3 || t === 8 ? W.data(this) : null; },
    function (v) { const t = W.ntype(this); if (t === 3 || t === 8) W.setData(this, v === null ? '' : String(v)); });
accessor(Node.prototype, 'textContent', function () {
    const t = W.ntype(this);
    if (t === 9) return null;
    return t === 3 || t === 8 ? W.data(this) : W.text(this);
}, function (v) {
    const t = W.ntype(this);
    v = v === null || v === undefined ? '' : String(v);
    if (t === 3 || t === 8) W.setData(this, v);
    else if (t === 1 || t === 11) W.setText(this, v);
});
accessor(Node.prototype, 'parentNode', function () { return W.parent(this); });
accessor(Node.prototype, 'parentElement', function () { const p = W.parent(this); return p && W.ntype(p) === 1 ? p : null; });
accessor(Node.prototype, 'childNodes', function () { return nodeList(W.children(this)); });
accessor(Node.prototype, 'firstChild', function () { return W.first(this); });
accessor(Node.prototype, 'lastChild', function () { return W.last(this); });
accessor(Node.prototype, 'nextSibling', function () { return W.next(this); });
accessor(Node.prototype, 'previousSibling', function () { return W.prev(this); });
accessor(Node.prototype, 'ownerDocument', function () { return W.ntype(this) === 9 ? null : document; });
accessor(Node.prototype, 'isConnected', function () { return W.connected(this); });
accessor(Node.prototype, 'baseURI', function () { return W.baseURL(); });

// ParentNode / ChildNode mixins
const ParentNode = {
    append(...n) { if (n.length) preInsert(this, nodesToNode(n), null); },
    prepend(...n) { if (n.length) preInsert(this, nodesToNode(n), W.first(this)); },
    replaceChildren(...n) {
        for (let c; (c = W.first(this));) this.removeChild(c);
        if (n.length) preInsert(this, nodesToNode(n), null);
    },
    querySelector(sel) { return W.query(this, String(sel), false); },
    querySelectorAll(sel) { return nodeList(W.query(this, String(sel), true)); },
    getElementsByTagName(tag) {
        tag = String(tag);
        return htmlCollection(tag === '*' ? W.query(this, '*', true) : W.byTag(this, tag.toLowerCase()));
    },
    getElementsByClassName(cls) {
        const parts = String(cls).trim().split(/\s+/).filter(Boolean);
        if (!parts.length) return htmlCollection([]);
        return htmlCollection(W.query(this, parts.map(p => '.' + cssEscape(p)).join(''), true));
    },
    getElementsByTagNameNS(ns, tag) { return this.getElementsByTagName(tag); }
};
const ChildNode = {
    remove() { const p = W.parent(this); if (p) p.removeChild(this); },
    before(...n) { const p = W.parent(this); if (p && n.length) preInsert(p, nodesToNode(n), this); },
    after(...n) { const p = W.parent(this); if (p && n.length) preInsert(p, nodesToNode(n), W.next(this)); },
    replaceWith(...n) {
        const p = W.parent(this);
        if (!p) return;
        const next = W.next(this);
        p.removeChild(this);
        if (n.length) preInsert(p, nodesToNode(n), next);
    }
};
for (const P of [Element.prototype, Document.prototype, DocumentFragment.prototype]) {
    methods(P, ParentNode);
    accessor(P, 'children', function () { return htmlCollection(W.children(this).filter(c => W.ntype(c) === 1)); });
    accessor(P, 'childElementCount', function () { let n = 0; for (let c = W.first(this); c; c = W.next(c)) if (W.ntype(c) === 1) n++; return n; });
    accessor(P, 'firstElementChild', function () { for (let c = W.first(this); c; c = W.next(c)) if (W.ntype(c) === 1) return c; return null; });
    accessor(P, 'lastElementChild', function () { for (let c = W.last(this); c; c = W.prev(c)) if (W.ntype(c) === 1) return c; return null; });
}
methods(DocumentFragment.prototype, { getElementById(id) { return W.query(this, '#' + cssEscape(String(id)), false); } });
for (const P of [Element.prototype, CharacterData.prototype]) {
    methods(P, ChildNode);
    accessor(P, 'nextElementSibling', function () { for (let c = W.next(this); c; c = W.next(c)) if (W.ntype(c) === 1) return c; return null; });
    accessor(P, 'previousElementSibling', function () { for (let c = W.prev(this); c; c = W.prev(c)) if (W.ntype(c) === 1) return c; return null; });
}

// CharacterData / Text
accessor(CharacterData.prototype, 'data', function () { return W.data(this); }, function (v) { W.setData(this, String(v)); });
accessor(CharacterData.prototype, 'length', function () { return W.data(this).length; });
methods(CharacterData.prototype, {
    appendData(s) { W.setData(this, W.data(this) + s); },
    insertData(o, s) { const d = W.data(this); W.setData(this, d.slice(0, o) + s + d.slice(o)); },
    deleteData(o, n) { const d = W.data(this); W.setData(this, d.slice(0, o) + d.slice(o + n)); },
    replaceData(o, n, s) { const d = W.data(this); W.setData(this, d.slice(0, o) + s + d.slice(o + n)); },
    substringData(o, n) { return W.data(this).substr(o, n); }
});
accessor(Text.prototype, 'wholeText', function () { return W.data(this); });
methods(Text.prototype, {
    splitText(off) {
        const d = W.data(this);
        W.setData(this, d.slice(0, off));
        const t = W.createText(d.slice(off));
        const p = W.parent(this);
        if (p) W.insert(p, t, W.next(this));
        return t;
    }
});

// ---------------------------------------------------------------- Element
function cssEscape(s) {
    s = String(s);
    let out = '';
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c === 0) { out += '�'; continue; }
        if ((c >= 1 && c <= 31) || c === 127 || (i === 0 && c >= 48 && c <= 57) ||
            (i === 1 && c >= 48 && c <= 57 && s.charCodeAt(0) === 45)) { out += '\\' + c.toString(16) + ' '; continue; }
        if (i === 0 && c === 45 && s.length === 1) { out += '\\-'; continue; }
        if (c >= 128 || c === 45 || c === 95 || (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)) { out += s[i]; continue; }
        out += '\\' + s[i];
    }
    return out;
}
const lname = (el, n) => W.ns(el) === 0 ? String(n).toLowerCase() : String(n);
methods(Element.prototype, {
    getAttribute(n) { return W.getAttr(this, lname(this, n)); },
    getAttributeNS(ns, n) { return W.getAttr(this, lname(this, n)); },
    setAttribute(n, v) { setAttr(this, lname(this, n), String(v)); },
    setAttributeNS(ns, n, v) { const i = String(n).indexOf(':'); setAttr(this, lname(this, i >= 0 ? String(n).slice(i + 1) : n), String(v)); },
    removeAttribute(n) { setAttr(this, lname(this, n), null); },
    removeAttributeNS(ns, n) { setAttr(this, lname(this, n), null); },
    hasAttribute(n) { return W.getAttr(this, lname(this, n)) !== null; },
    hasAttributeNS(ns, n) { return W.getAttr(this, lname(this, n)) !== null; },
    hasAttributes() { return W.attrNames(this).length > 0; },
    getAttributeNames() { return W.attrNames(this); },
    toggleAttribute(n, force) {
        n = lname(this, n);
        const has = W.getAttr(this, n) !== null;
        if (force === undefined ? has : !force) { if (has) setAttr(this, n, null); return false; }
        if (!has) setAttr(this, n, '');
        return true;
    },
    getAttributeNode(n) { const v = this.getAttribute(n); return v === null ? null : makeAttr(this, lname(this, n)); },
    setAttributeNode(a) { this.setAttribute(a.name, a.value); return null; },
    matches(sel) { return W.matches(this, String(sel)); },
    webkitMatchesSelector(sel) { return W.matches(this, String(sel)); },
    closest(sel) {
        sel = String(sel);
        for (let n = this; n && W.ntype(n) === 1; n = W.parent(n)) if (W.matches(n, sel)) return n;
        return null;
    },
    insertAdjacentHTML(pos, html) {
        pos = String(pos).toLowerCase();
        const ctx = (pos === 'beforebegin' || pos === 'afterend') ? W.parent(this) : this;
        if (!ctx) throw new DOMException('no parent', 'NoModificationAllowedError');
        const f = W.parseHTML(String(html), ctx);
        insertAdjacent(this, pos, f);
    },
    insertAdjacentElement(pos, el) { return insertAdjacent(this, String(pos).toLowerCase(), el) ? el : null; },
    insertAdjacentText(pos, text) { insertAdjacent(this, String(pos).toLowerCase(), W.createText(String(text))); },
    getBoundingClientRect() { return rectOf(this); },
    getClientRects() { const r = W.rect(this); return r ? [rectOf(this)] : []; },
    scrollIntoView(arg) {
        const r = W.rect(this);
        if (!r) return;
        const v = W.view();
        let y = r[1];
        if (arg && typeof arg === 'object' && arg.block === 'center') y = r[1] - (v[1] - r[3]) / 2;
        else if (arg === false || (arg && arg.block === 'end')) y = r[1] + r[3] - v[1];
        W.scroll(Math.max(0, y | 0));
    },
    scrollIntoViewIfNeeded() { this.scrollIntoView(); },
    scrollTo() {}, scroll() {}, scrollBy() {},
    focus() { W.focus(this, 1); },
    blur() { W.focus(this, 0); },
    click() { dispatchClick(this); },
    attachShadow(init) {
        // no shadow DOM: content renders in the light tree (good enough for
        // most progressive-enhancement components)
        const root = this;
        hidden(this, '_shadow', root);
        return root;
    },
    animate() { return { finished: Promise.resolve(), cancel() {}, play() {}, pause() {}, finish() {}, addEventListener() {}, onfinish: null }; },
    getAnimations() { return []; },
    requestFullscreen() { return Promise.reject(new DOMException('not supported', 'NotSupportedError')); },
    setPointerCapture() {}, releasePointerCapture() {}, hasPointerCapture() { return false; },
    checkVisibility() { return !!W.rect(this); },
    computedStyleMap() { return new Map(); }
});
function insertAdjacent(el, pos, node) {
    const p = W.parent(el);
    if (pos === 'beforebegin') { if (!p) return false; preInsert(p, node, el); }
    else if (pos === 'afterbegin') preInsert(el, node, W.first(el));
    else if (pos === 'beforeend') preInsert(el, node, null);
    else if (pos === 'afterend') { if (!p) return false; preInsert(p, node, W.next(el)); }
    else throw new DOMException('bad position', 'SyntaxError');
    return true;
}
function setAttr(el, name, value) {
    const old = W.getAttr(el, name);
    if (value === null) { if (old === null) return; W.removeAttr(el, name); }
    else W.setAttr(el, name, value);
    if (ceActive) ceAttrChanged(el, name, old, value);
    if (moActive) moRecord('attributes', el, name, old);
}
class DOMRectReadOnly {
    constructor(x, y, w, h) { this.x = +x || 0; this.y = +y || 0; this.width = +w || 0; this.height = +h || 0; }
    get top() { return Math.min(this.y, this.y + this.height); }
    get left() { return Math.min(this.x, this.x + this.width); }
    get right() { return Math.max(this.x, this.x + this.width); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; }
    static fromRect(r) { r = r || {}; return new this(r.x, r.y, r.width, r.height); }
}
class DOMRect extends DOMRectReadOnly {}
G.DOMRect = DOMRect; G.DOMRectReadOnly = DOMRectReadOnly;
function rectOf(el) {
    const r = W.rect(el);
    if (!r) return new DOMRect(0, 0, 0, 0);
    return new DOMRect(r[0], r[1] - W.view()[2], r[2], r[3]);
}
accessor(Element.prototype, 'tagName', function () { return this.nodeName; });
accessor(Element.prototype, 'localName', function () { return W.name(this); });
accessor(Element.prototype, 'prefix', function () { return null; });
accessor(Element.prototype, 'namespaceURI', function () {
    return ['http://www.w3.org/1999/xhtml', 'http://www.w3.org/2000/svg', 'http://www.w3.org/1998/Math/MathML'][W.ns(this)];
});
const reflect = (P, prop, attr) => accessor(P, prop,
    function () { const v = W.getAttr(this, attr); return v === null ? '' : v; },
    function (v) { setAttr(this, attr, String(v)); });
const reflectBool = (P, prop, attr) => accessor(P, prop,
    function () { return W.getAttr(this, attr) !== null; },
    function (v) { setAttr(this, attr, v ? '' : null); });
const reflectURL = (P, prop, attr) => accessor(P, prop,
    function () { const v = W.getAttr(this, attr); if (v === null) return ''; const r = W.resolve(v); return r === null ? v : r; },
    function (v) { setAttr(this, attr, String(v)); });
const reflectInt = (P, prop, attr, dflt) => accessor(P, prop,
    function () { const v = parseInt(W.getAttr(this, attr), 10); return isNaN(v) ? dflt : v; },
    function (v) { setAttr(this, attr, String(v | 0)); });
reflect(Element.prototype, 'id', 'id');
reflect(Element.prototype, 'className', 'class');
reflect(Element.prototype, 'slot', 'slot');
accessor(Element.prototype, 'attributes', function () {
    const a = W.attrNames(this).map(n => makeAttr(this, n));
    a.getNamedItem = n => { n = lname(this, n); return a.find(x => x.name === n) || null; };
    a.item = i => a[i] || null;
    for (const at of a) if (!(at.name in a)) Object.defineProperty(a, at.name, { value: at, enumerable: false });
    return a;
});
function makeAttr(el, name) {
    const a = Object.create(Attr.prototype);
    Object.defineProperties(a, {
        name: { value: name, enumerable: true }, localName: { value: name }, nodeName: { value: name },
        ownerElement: { value: el }, specified: { value: true }, namespaceURI: { value: null }, prefix: { value: null },
        value: { get() { const v = W.getAttr(el, name); return v === null ? '' : v; }, set(v) { setAttr(el, name, String(v)); }, enumerable: true },
        nodeValue: { get() { return W.getAttr(el, name); } }
    });
    return a;
}
accessor(Element.prototype, 'innerHTML', function () { return W.html(this, 0); }, function (v) {
    const s = v === null ? '' : String(v);
    if (ceActive || moActive) for (let c; (c = W.first(this));) this.removeChild(c);
    W.setHTML(this, s);
    if (ceActive) for (let c = W.first(this); c; c = W.next(c)) ceUpgradeTree(c, W.connected(this));
});
accessor(Element.prototype, 'outerHTML', function () { return W.html(this, 1); }, function (v) {
    const p = W.parent(this);
    if (!p) return;
    const f = W.parseHTML(String(v), p);
    preInsert(p, f, this);
    p.removeChild(this);
});
accessor(Element.prototype, 'classList', function () { return tokenList(this, 'class'); });
accessor(Element.prototype, 'part', function () { return tokenList(this, 'part'); });
accessor(Element.prototype, 'shadowRoot', function () { return null; });
accessor(Element.prototype, 'assignedSlot', function () { return null; });
for (const [p, i] of [['clientWidth', 2], ['clientHeight', 3], ['offsetWidth', 2], ['offsetHeight', 3], ['scrollWidth', 2], ['scrollHeight', 3]])
    accessor(Element.prototype, p, function () {
        if (this === document.documentElement && (p === 'clientWidth' || p === 'clientHeight')) return W.view()[i - 2];
        if (this === document.documentElement && (p === 'scrollHeight')) return W.view()[4];
        const r = W.rect(this); return r ? Math.round(r[i]) : 0;
    });
accessor(Element.prototype, 'clientTop', function () { return 0; });
accessor(Element.prototype, 'clientLeft', function () { return 0; });
accessor(Element.prototype, 'offsetTop', function () { const r = W.rect(this); return r ? Math.round(r[1]) : 0; });
accessor(Element.prototype, 'offsetLeft', function () { const r = W.rect(this); return r ? Math.round(r[0]) : 0; });
accessor(Element.prototype, 'offsetParent', function () { return W.rect(this) ? document.body : null; });
accessor(Element.prototype, 'scrollTop', function () {
    return (this === document.documentElement || this === document.body) ? W.view()[2] : 0;
}, function (v) { if (this === document.documentElement || this === document.body) W.scroll(Math.max(0, +v | 0)); });
accessor(Element.prototype, 'scrollLeft', function () { return 0; }, function () {});
defineHandlers(HTMLElement.prototype);
defineHandlers(SVGElement.prototype);

// DOMTokenList over an attribute
function tokenList(el, attr) {
    const get = () => { const v = W.getAttr(el, attr); return v ? v.split(/[ \t\n\f\r]+/).filter(Boolean) : []; };
    const set = a => setAttr(el, attr, a.join(' '));
    const tl = {
        get length() { return get().length; },
        get value() { const v = W.getAttr(el, attr); return v === null ? '' : v; },
        set value(v) { setAttr(el, attr, String(v)); },
        item(i) { const a = get(); return i < a.length ? a[i] : null; },
        contains(t) { return get().includes(String(t)); },
        add(...t) { const a = get(); let ch = false; for (let x of t) { x = String(x); if (!x) throw new DOMException('empty token', 'SyntaxError'); if (!a.includes(x)) { a.push(x); ch = true; } } if (ch || W.getAttr(el, attr) === null) set(a); },
        remove(...t) { const a = get(); const b = a.filter(x => !t.map(String).includes(x)); if (b.length !== a.length) set(b); },
        toggle(t, force) {
            t = String(t);
            const a = get(), has = a.includes(t);
            if (has && force !== true) { set(a.filter(x => x !== t)); return false; }
            if (!has && force !== false) { a.push(t); set(a); return true; }
            return has;
        },
        replace(o, n) { const a = get(); const i = a.indexOf(String(o)); if (i < 0) return false; a[i] = String(n); set(a); return true; },
        supports() { return true; },
        forEach(cb, thisArg) { get().forEach((v, i) => cb.call(thisArg, v, i, tl)); },
        entries() { return get().entries(); }, keys() { return get().keys(); }, values() { return get().values(); },
        toString() { return this.value; },
        [Symbol.iterator]() { return get()[Symbol.iterator](); }
    };
    return new Proxy(tl, { get(t, k) { if (typeof k === 'string' && /^\d+$/.test(k)) return t.item(+k); return t[k]; } });
}
G.DOMTokenList = function DOMTokenList() { illegal(); };

// element.style: CSSStyleDeclaration over the style attribute
const camelToDash = p => p.startsWith('--') ? p : p.replace(/^(webkit|moz|ms)(?=[A-Z])/, '-$1').replace(/[A-Z]/g, m => '-' + m.toLowerCase()).replace(/^css-?float$/, 'float');
function parseDecls(text) {
    const out = [];
    if (!text) return out;
    let depth = 0, q = 0, start = 0;
    const push = seg => {
        const i = seg.indexOf(':');
        if (i <= 0) return;
        const name = seg.slice(0, i).trim().toLowerCase();
        let value = seg.slice(i + 1).trim(), important = '';
        const m = /!\s*important\s*$/i.exec(value);
        if (m) { value = value.slice(0, m.index).trim(); important = 'important'; }
        if (!name) return;
        const k = out.findIndex(d => d[0] === name);
        if (k >= 0) out.splice(k, 1);
        out.push([name.startsWith('--') ? seg.slice(0, i).trim() : name, value, important]);
    };
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (q) { if (c === q) q = 0; else if (c === '\\') i++; continue; }
        if (c === '"' || c === "'") q = c;
        else if (c === '(') depth++;
        else if (c === ')') depth--;
        else if (c === ';' && depth <= 0) { push(text.slice(start, i)); start = i + 1; }
    }
    push(text.slice(start));
    return out;
}
const serializeDecls = a => a.map(d => d[0] + ': ' + d[1] + (d[2] ? ' !important' : '') + ';').join(' ');
class CSSStyleDeclaration {
    constructor(el) { hidden(this, '_el', el); }
    get cssText() { const v = W.getAttr(this._el, 'style'); return v === null ? '' : v; }
    set cssText(v) { setAttr(this._el, 'style', String(v)); }
    get length() { return parseDecls(W.getAttr(this._el, 'style')).length; }
    item(i) { const d = parseDecls(W.getAttr(this._el, 'style'))[i]; return d ? d[0] : ''; }
    getPropertyValue(p) {
        p = String(p);
        const n = p.startsWith('--') ? p : p.toLowerCase();
        const d = parseDecls(W.getAttr(this._el, 'style')).find(x => x[0] === n);
        return d ? d[1] : '';
    }
    getPropertyPriority(p) { const d = parseDecls(W.getAttr(this._el, 'style')).find(x => x[0] === String(p).toLowerCase()); return d ? d[2] : ''; }
    setProperty(p, v, prio) {
        p = String(p);
        const name = p.startsWith('--') ? p : p.toLowerCase();
        const a = parseDecls(W.getAttr(this._el, 'style'));
        const k = a.findIndex(x => x[0] === name);
        if (v === null || v === undefined || String(v) === '') { if (k >= 0) { a.splice(k, 1); setAttr(this._el, 'style', serializeDecls(a)); } return; }
        const d = [name, String(v).trim(), prio === 'important' ? 'important' : ''];
        if (k >= 0) a[k] = d; else a.push(d);
        setAttr(this._el, 'style', serializeDecls(a));
    }
    removeProperty(p) { const old = this.getPropertyValue(p); this.setProperty(p, ''); return old; }
}
const styleProxyHandler = {
    get(t, k) {
        if (typeof k !== 'string' || k in t) { const v = t[k]; return typeof v === 'function' ? v.bind(t) : v; }
        if (/^\d+$/.test(k)) return t.item(+k);
        return t.getPropertyValue(camelToDash(k));
    },
    set(t, k, v) {
        if (typeof k !== 'string') return true;
        if (k === 'cssText') { t.cssText = v; return true; }
        t.setProperty(camelToDash(k), v === null || v === undefined ? '' : (typeof v === 'number' && v !== 0 && !/opacity|z-index|zIndex|flex|order|line-?height|font-?weight|zoom/i.test(k) ? v + 'px' : String(v)));
        return true;
    },
    has(t, k) { return typeof k === 'string'; }
};
const STYLES = new WeakMap();
accessor(HTMLElement.prototype, 'style', function () {
    let s = STYLES.get(this);
    if (!s) { s = new Proxy(new CSSStyleDeclaration(this), styleProxyHandler); STYLES.set(this, s); }
    return s;
}, function (v) { setAttr(this, 'style', String(v)); });
Object.defineProperty(SVGElement.prototype, 'style', Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'style'));
G.CSSStyleDeclaration = CSSStyleDeclaration;

// element.dataset
const dataKey = k => 'data-' + k.replace(/[A-Z]/g, m => '-' + m.toLowerCase());
accessor(HTMLElement.prototype, 'dataset', function () {
    const el = this;
    return new Proxy({}, {
        get(t, k) { if (typeof k !== 'string') return undefined; const v = W.getAttr(el, dataKey(k)); return v === null ? undefined : v; },
        set(t, k, v) { if (typeof k === 'string') setAttr(el, dataKey(k), String(v)); return true; },
        deleteProperty(t, k) { if (typeof k === 'string') setAttr(el, dataKey(k), null); return true; },
        has(t, k) { return typeof k === 'string' && W.getAttr(el, dataKey(k)) !== null; },
        ownKeys() { return W.attrNames(el).filter(n => n.startsWith('data-')).map(n => n.slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase())); },
        getOwnPropertyDescriptor(t, k) { const v = W.getAttr(el, dataKey(String(k))); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; }
    });
});
Object.defineProperty(SVGElement.prototype, 'dataset', Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'dataset'));

// HTMLElement reflected attributes and text
const HE = HTMLElement.prototype;
for (const a of ['title', 'lang', 'dir', 'accessKey', 'autocapitalize', 'enterKeyHint', 'inputMode', 'nonce', 'popover'])
    reflect(HE, a, a.toLowerCase());
reflectBool(HE, 'hidden', 'hidden');
reflectBool(HE, 'inert', 'inert');
reflectBool(HE, 'autofocus', 'autofocus');
accessor(HE, 'tabIndex', function () { const v = parseInt(W.getAttr(this, 'tabindex'), 10); return isNaN(v) ? (/^(a|button|input|select|textarea)$/.test(W.name(this)) ? 0 : -1) : v; },
    function (v) { setAttr(this, 'tabindex', String(v | 0)); });
accessor(HE, 'contentEditable', function () { const v = W.getAttr(this, 'contenteditable'); return v === null ? 'inherit' : v; }, function (v) { setAttr(this, 'contenteditable', String(v)); });
accessor(HE, 'isContentEditable', function () { return W.getAttr(this, 'contenteditable') !== null; });
accessor(HE, 'draggable', function () { return W.getAttr(this, 'draggable') === 'true'; }, function (v) { setAttr(this, 'draggable', v ? 'true' : 'false'); });
accessor(HE, 'spellcheck', function () { return W.getAttr(this, 'spellcheck') !== 'false'; }, function (v) { setAttr(this, 'spellcheck', v ? 'true' : 'false'); });
accessor(HE, 'translate', function () { return W.getAttr(this, 'translate') !== 'no'; }, function (v) { setAttr(this, 'translate', v ? 'yes' : 'no'); });
function innerTextOf(el) {
    // approximation: textContent with block boundaries as newlines
    const out = [];
    const walk = n => {
        for (let c = W.first(n); c; c = W.next(c)) {
            const t = W.ntype(c);
            if (t === 3) out.push(W.data(c).replace(/[ \t\n\r\f]+/g, ' '));
            else if (t === 1) {
                const nm = W.name(c);
                if (nm === 'script' || nm === 'style' || nm === 'template' || nm === 'noscript') continue;
                if (nm === 'br') { out.push('\n'); continue; }
                const block = /^(p|div|li|tr|h[1-6]|section|article|header|footer|ul|ol|table|form|blockquote|pre|dd|dt|nav|main|aside|figure|figcaption)$/.test(nm);
                if (block) out.push('\n');
                walk(c);
                if (block) out.push('\n');
            }
        }
    };
    walk(el);
    return out.join('').replace(/ *\n */g, '\n').replace(/\n{2,}/g, '\n').trim();
}
accessor(HE, 'innerText', function () { return innerTextOf(this); }, function (v) { W.setText(this, v === null ? '' : String(v)); });
accessor(HE, 'outerText', function () { return innerTextOf(this); });
accessor(HE, 'offsetParent', function () { return W.rect(this) ? document.body : null; });
methods(HE, {
    showPopover() {}, hidePopover() {}, togglePopover() { return false; },
    attachInternals() { return { setFormValue() {}, setValidity() {}, states: new Set(), form: null, labels: [] }; }
});

// links
for (const P of [HTMLAnchorElement.prototype, HTMLAreaElement.prototype]) {
    reflectURL(P, 'href', 'href');
    for (const a of ['target', 'download', 'rel', 'hreflang', 'type', 'referrerPolicy', 'ping'])
        reflect(P, a, a.toLowerCase());
    accessor(P, 'relList', function () { return tokenList(this, 'rel'); });
    for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password'])
        accessor(P, part, function () { try { return new URL(this.href)[part]; } catch (e) { return ''; } },
            function (v) { try { const u = new URL(this.href); u[part] = v; this.href = u.href; } catch (e) {} });
    methods(P, { toString() { return this.href; } });
}
accessor(HTMLAnchorElement.prototype, 'text', function () { return W.text(this); }, function (v) { W.setText(this, String(v)); });

// images
const HI = HTMLImageElement.prototype;
reflectURL(HI, 'src', 'src');
for (const a of ['alt', 'srcset', 'sizes', 'crossOrigin', 'useMap', 'loading', 'decoding', 'referrerPolicy', 'fetchPriority'])
    reflect(HI, a, a.toLowerCase());
reflectBool(HI, 'isMap', 'ismap');
accessor(HI, 'width', function () { const r = W.rect(this); if (r) return Math.round(r[2]); const v = parseInt(W.getAttr(this, 'width'), 10); return isNaN(v) ? 0 : v; }, function (v) { setAttr(this, 'width', String(v | 0)); });
accessor(HI, 'height', function () { const r = W.rect(this); if (r) return Math.round(r[3]); const v = parseInt(W.getAttr(this, 'height'), 10); return isNaN(v) ? 0 : v; }, function (v) { setAttr(this, 'height', String(v | 0)); });
accessor(HI, 'naturalWidth', function () { const s = W.imgSize(this); return s ? s[0] : 0; });
accessor(HI, 'naturalHeight', function () { const s = W.imgSize(this); return s ? s[1] : 0; });
accessor(HI, 'complete', function () { const s = W.imgSize(this); return s === null || s[2] !== 0; });
accessor(HI, 'currentSrc', function () { return this.src; });
methods(HI, { decode() { return Promise.resolve(); } });
G.Image = function Image(w, h) {
    const img = document.createElement('img');
    if (w !== undefined) img.width = w;
    if (h !== undefined) img.height = h;
    return img;
};
G.Image.prototype = HI;
G.Audio = function Audio(src) {
    if (!new.target) illegal();
    const a = document.createElement('audio');
    a.setAttribute('preload', 'auto');
    if (src !== undefined) a.src = src;
    return a;
};
G.Audio.prototype = HTMLAudioElement.prototype;

// forms
function controlValue(el) {
    const nm = W.name(el);
    if (nm === 'textarea') { const v = W.getAttr(el, 'value'); return v === null ? W.text(el) : v; }
    if (nm === 'select') { const o = selectedOption(el); return o ? o.value : ''; }
    if (nm === 'option') { const v = W.getAttr(el, 'value'); return v === null ? W.text(el).replace(/\s+/g, ' ').trim() : v; }
    const v = W.getAttr(el, 'value');
    if (v === null) { const t = (W.getAttr(el, 'type') || '').toLowerCase(); return t === 'checkbox' || t === 'radio' ? 'on' : ''; }
    return v;
}
function optionsOf(sel) { return W.query(sel, 'option', true); }
function selectedOption(sel) {
    const opts = optionsOf(sel);
    for (const o of opts) if (W.getAttr(o, 'selected') !== null) return o;
    return opts[0] || null;
}
const FORM_PROTOS = [HTMLInputElement.prototype, HTMLTextAreaElement.prototype, HTMLSelectElement.prototype,
    HTMLButtonElement.prototype, HTMLOutputElement.prototype, HTMLFieldSetElement.prototype, HTMLOptionElement.prototype];
for (const P of FORM_PROTOS) {
    accessor(P, 'value', function () { return controlValue(this); }, function (v) {
        v = v === null || v === undefined ? '' : String(v);
        if (W.name(this) === 'select') {
            for (const o of optionsOf(this)) { if (controlValue(o) === v) W.setAttr(o, 'selected', ''); else W.removeAttr(o, 'selected'); }
            return;
        }
        W.setAttr(this, 'value', v);
    });
    accessor(P, 'form', function () { return this.closest('form'); });
    reflect(P, 'name', 'name');
    reflectBool(P, 'disabled', 'disabled');
    accessor(P, 'labels', function () { const id = this.id; return nodeList(id ? W.query(document, 'label[for="' + cssEscape(id) + '"]', true) : []); });
    accessor(P, 'validity', function () { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; });
    accessor(P, 'validationMessage', function () { return ''; });
    accessor(P, 'willValidate', function () { return true; });
    methods(P, { checkValidity() { return true; }, reportValidity() { return true; }, setCustomValidity() {} });
}
const HIN = HTMLInputElement.prototype;
accessor(HIN, 'type', function () { const t = (W.getAttr(this, 'type') || 'text').toLowerCase(); return t; }, function (v) { setAttr(this, 'type', String(v)); });
for (const a of ['placeholder', 'autocomplete', 'pattern', 'min', 'max', 'step', 'accept', 'alt', 'dirName', 'list'])
    reflect(HIN, a, a.toLowerCase());
reflectURL(HIN, 'src', 'src');
for (const a of ['required', 'readOnly', 'multiple', 'formNoValidate'])
    reflectBool(HIN, a, a.toLowerCase());
reflectInt(HIN, 'maxLength', 'maxlength', -1);
reflectInt(HIN, 'minLength', 'minlength', -1);
reflectInt(HIN, 'size', 'size', 20);
accessor(HIN, 'defaultValue', function () { const v = W.getAttr(this, 'value'); return v === null ? '' : v; }, function (v) { W.setAttr(this, 'value', String(v)); });
accessor(HIN, 'checked', function () { return W.getAttr(this, 'checked') !== null; }, function (v) {
    if (v) {
        if (this.type === 'radio' && this.name) for (const r of W.query(document, 'input[type=radio][name="' + cssEscape(this.name) + '"]', true)) if (r !== this) W.removeAttr(r, 'checked');
        W.setAttr(this, 'checked', '');
    } else W.removeAttr(this, 'checked');
});
accessor(HIN, 'defaultChecked', function () { return W.getAttr(this, 'checked') !== null; }, function (v) { setAttr(this, 'checked', v ? '' : null); });
accessor(HIN, 'indeterminate', function () { return false; }, function () {});
accessor(HIN, 'files', function () { return []; });
accessor(HIN, 'valueAsNumber', function () { return parseFloat(this.value); }, function (v) { this.value = String(v); });
accessor(HIN, 'selectionStart', function () { return this.value.length; }, function () {});
accessor(HIN, 'selectionEnd', function () { return this.value.length; }, function () {});
methods(HIN, { select() {}, setSelectionRange() {}, setRangeText() {}, stepUp() {}, stepDown() {}, showPicker() {} });
const HTA = HTMLTextAreaElement.prototype;
for (const a of ['placeholder', 'autocomplete', 'wrap']) reflect(HTA, a, a);
for (const a of ['required', 'readOnly']) reflectBool(HTA, a, a.toLowerCase());
reflectInt(HTA, 'rows', 'rows', 2);
reflectInt(HTA, 'cols', 'cols', 20);
reflectInt(HTA, 'maxLength', 'maxlength', -1);
accessor(HTA, 'type', function () { return 'textarea'; });
accessor(HTA, 'defaultValue', function () { return W.text(this); }, function (v) { W.setText(this, String(v)); });
accessor(HTA, 'textLength', function () { return this.value.length; });
accessor(HTA, 'selectionStart', function () { return this.value.length; }, function () {});
accessor(HTA, 'selectionEnd', function () { return this.value.length; }, function () {});
methods(HTA, { select() {}, setSelectionRange() {}, setRangeText() {} });
const HS = HTMLSelectElement.prototype;
accessor(HS, 'options', function () { const o = htmlCollection(optionsOf(this)); const sel = this; Object.defineProperty(o, 'selectedIndex', { get() { return sel.selectedIndex; }, set(v) { sel.selectedIndex = v; } }); o.add = (opt, before) => sel.add(opt, before); o.remove = i => sel.remove(i); return o; });
accessor(HS, 'length', function () { return optionsOf(this).length; });
accessor(HS, 'type', function () { return W.getAttr(this, 'multiple') !== null ? 'select-multiple' : 'select-one'; });
reflectBool(HS, 'multiple', 'multiple');
reflectBool(HS, 'required', 'required');
reflectInt(HS, 'size', 'size', 0);
accessor(HS, 'selectedIndex', function () { const o = optionsOf(this); const s = selectedOption(this); return s ? o.indexOf(s) : -1; }, function (i) {
    optionsOf(this).forEach((o, k) => { if (k === (i | 0)) W.setAttr(o, 'selected', ''); else W.removeAttr(o, 'selected'); });
});
accessor(HS, 'selectedOptions', function () { const s = selectedOption(this); return htmlCollection(s ? [s] : []); });
methods(HS, {
    item(i) { return optionsOf(this)[i] || null; },
    namedItem(n) { return optionsOf(this).find(o => o.id === n || o.getAttribute('name') === n) || null; },
    add(opt, before) { const ref = typeof before === 'number' ? optionsOf(this)[before] : before; if (ref) ref.before(opt); else this.appendChild(opt); },
    remove(i) { if (i === undefined) { ChildNode.remove.call(this); return; } const o = optionsOf(this)[i]; if (o) o.remove(); }
});
const HO = HTMLOptionElement.prototype;
accessor(HO, 'selected', function () { const s = W.parent(this) && this.closest('select'); return s ? selectedOption(s) === this : W.getAttr(this, 'selected') !== null; },
    function (v) { const s = this.closest('select'); if (v && s) for (const o of optionsOf(s)) W.removeAttr(o, 'selected'); if (v) W.setAttr(this, 'selected', ''); else W.removeAttr(this, 'selected'); });
reflectBool(HO, 'defaultSelected', 'selected');
reflect(HO, 'label', 'label');
accessor(HO, 'text', function () { return W.text(this).replace(/\s+/g, ' ').trim(); }, function (v) { W.setText(this, String(v)); });
accessor(HO, 'index', function () { const s = this.closest('select'); return s ? optionsOf(s).indexOf(this) : 0; });
G.Option = function Option(text, value, defSel, sel) {
    const o = document.createElement('option');
    if (text !== undefined) o.text = text;
    if (value !== undefined) o.value = value;
    if (defSel || sel) o.setAttribute('selected', '');
    return o;
};
G.Option.prototype = HO;
const HB = HTMLButtonElement.prototype;
accessor(HB, 'type', function () { const t = (W.getAttr(this, 'type') || 'submit').toLowerCase(); return t === 'button' || t === 'reset' ? t : 'submit'; }, function (v) { setAttr(this, 'type', String(v)); });
const HF = HTMLFormElement.prototype;
for (const a of ['name', 'target', 'enctype', 'autocomplete', 'acceptCharset']) reflect(HF, a, a === 'acceptCharset' ? 'accept-charset' : a.toLowerCase());
reflectURL(HF, 'action', 'action');
accessor(HF, 'method', function () { return (W.getAttr(this, 'method') || 'get').toLowerCase() === 'post' ? 'post' : 'get'; }, function (v) { setAttr(this, 'method', String(v)); });
reflectBool(HF, 'noValidate', 'novalidate');
accessor(HF, 'elements', function () {
    const els = htmlCollection(W.query(this, 'input,select,textarea,button,fieldset,output,object', true));
    for (const e of els) { const n = e.getAttribute('name') || e.id; if (n && !(n in els)) Object.defineProperty(els, n, { value: e, enumerable: false }); }
    return els;
});
accessor(HF, 'length', function () { return this.elements.length; });
methods(HF, {
    submit() { W.submit(this, null); },
    requestSubmit(submitter) {
        const ev = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: submitter || null });
        if (dispatch(this, ev)) W.submit(this, submitter || null);
    },
    reset() { dispatch(this, new Event('reset', { bubbles: true, cancelable: true })); },
    checkValidity() { return true; }, reportValidity() { return true; }
});
const HL = HTMLLabelElement.prototype;
accessor(HL, 'htmlFor', function () { const v = W.getAttr(this, 'for'); return v === null ? '' : v; }, function (v) { setAttr(this, 'for', String(v)); });
accessor(HL, 'control', function () { const f = W.getAttr(this, 'for'); return f ? document.getElementById(f) : W.query(this, 'input,select,textarea,button', false); });

// other reflected bits
reflectURL(HTMLLinkElement.prototype, 'href', 'href');
for (const a of ['rel', 'media', 'type', 'as', 'crossOrigin', 'integrity', 'hreflang', 'sizes', 'referrerPolicy', 'fetchPriority'])
    reflect(HTMLLinkElement.prototype, a, a.toLowerCase());
reflectBool(HTMLLinkElement.prototype, 'disabled', 'disabled');
accessor(HTMLLinkElement.prototype, 'relList', function () { return tokenList(this, 'rel'); });
accessor(HTMLLinkElement.prototype, 'sheet', function () {
    const rel = W.getAttr(this, 'rel');
    return rel && /stylesheet/i.test(rel) ? linkSheet(this) : null;
});
const HSC = HTMLScriptElement.prototype;
reflectURL(HSC, 'src', 'src');
for (const a of ['type', 'charset', 'crossOrigin', 'integrity', 'referrerPolicy', 'fetchPriority']) reflect(HSC, a, a.toLowerCase());
reflectBool(HSC, 'defer', 'defer');
reflectBool(HSC, 'noModule', 'nomodule');
accessor(HSC, 'async', function () { return W.getAttr(this, 'async') !== null || !W.parserInserted(this); }, function (v) { setAttr(this, 'async', v ? '' : null); });
accessor(HSC, 'text', function () { return W.text(this); }, function (v) { W.setText(this, String(v)); });
HTMLScriptElement.supports = t => t === 'classic' || t === 'module';
reflect(HTMLStyleElement.prototype, 'media', 'media');
reflectBool(HTMLStyleElement.prototype, 'disabled', 'disabled');
reflect(HTMLMetaElement.prototype, 'name', 'name');
reflect(HTMLMetaElement.prototype, 'content', 'content');
reflect(HTMLMetaElement.prototype, 'httpEquiv', 'http-equiv');
reflectURL(HTMLIFrameElement.prototype, 'src', 'src');
reflect(HTMLIFrameElement.prototype, 'name', 'name');
accessor(HTMLIFrameElement.prototype, 'contentWindow', function () { return null; });
accessor(HTMLIFrameElement.prototype, 'contentDocument', function () { return null; });
reflectURL(HTMLSourceElement.prototype, 'src', 'src');
reflect(HTMLSourceElement.prototype, 'srcset', 'srcset');
reflect(HTMLSourceElement.prototype, 'type', 'type');
reflect(HTMLSourceElement.prototype, 'media', 'media');
reflectInt(HTMLTableCellElement.prototype, 'colSpan', 'colspan', 1);
reflectInt(HTMLTableCellElement.prototype, 'rowSpan', 'rowspan', 1);
accessor(HTMLTableElement.prototype, 'rows', function () { return htmlCollection(W.query(this, 'tr', true)); });
accessor(HTMLTableElement.prototype, 'tBodies', function () { return htmlCollection(W.query(this, 'tbody', true)); });
accessor(HTMLTableSectionElement.prototype, 'rows', function () { return htmlCollection(W.query(this, 'tr', true)); });
accessor(HTMLTableRowElement.prototype, 'cells', function () { return htmlCollection(W.children(this).filter(c => /^t[dh]$/.test(W.name(c)))); });
accessor(HTMLTableRowElement.prototype, 'rowIndex', function () { const t = this.closest('table'); return t ? t.rows.indexOf(this) : -1; });
reflectBool(HTMLDetailsElement.prototype, 'open', 'open');
reflectBool(HTMLDialogElement.prototype, 'open', 'open');
methods(HTMLDialogElement.prototype, {
    show() { setAttr(this, 'open', ''); }, showModal() { setAttr(this, 'open', ''); },
    close(rv) { setAttr(this, 'open', null); this.returnValue = rv === undefined ? '' : rv; dispatch(this, new Event('close')); }
});
accessor(HTMLTemplateElement.prototype, 'content', function () { return W.templateContent(this); });
accessor(HTMLTitleElement.prototype, 'text', function () { return W.text(this); }, function (v) { W.setText(this, String(v)); });
// ---------------------------------------------------------------- media (<video>, <audio>, MediaSource)
// State lives in the C media engine (one session per element, W.media*); this is the DOM-facing
// shell: element properties, the event sequence, MediaSource/SourceBuffer, and a ticker that turns
// engine state changes into events while something is playing or loading.
const HM = HTMLMediaElement.prototype;
reflectURL(HM, 'src', 'src');
let userActivated = false;   // a trusted click / key press / pointer press reached the page (autoplay policy, see HM.play)
for (const a of ['autoplay', 'loop', 'controls', 'playsInline', 'disableRemotePlayback']) reflectBool(HM, a, a.toLowerCase());
reflect(HM, 'crossOrigin', 'crossorigin');
reflect(HM, 'preload', 'preload');
reflectBool(HM, 'defaultMuted', 'muted');
reflectURL(HTMLVideoElement.prototype, 'poster', 'poster');
reflectInt(HTMLVideoElement.prototype, 'width', 'width', 0);
reflectInt(HTMLVideoElement.prototype, 'height', 'height', 0);
for (const [k, v] of [['NETWORK_EMPTY', 0], ['NETWORK_IDLE', 1], ['NETWORK_LOADING', 2], ['NETWORK_NO_SOURCE', 3],
        ['HAVE_NOTHING', 0], ['HAVE_METADATA', 1], ['HAVE_CURRENT_DATA', 2], ['HAVE_FUTURE_DATA', 3], ['HAVE_ENOUGH_DATA', 4]]) {
    Object.defineProperty(HTMLMediaElement, k, { value: v });
    Object.defineProperty(HM, k, { value: v });
}

class TimeRanges {
    constructor(flat) { hidden(this, '_f', flat || []); }
    get length() { return this._f.length >> 1; }
    start(i) { i = i >>> 0; if (i >= this.length) throw new DOMException('The index provided is greater than or equal to the maximum bound.', 'IndexSizeError'); return this._f[2 * i]; }
    end(i) { i = i >>> 0; if (i >= this.length) throw new DOMException('The index provided is greater than or equal to the maximum bound.', 'IndexSizeError'); return this._f[2 * i + 1]; }
}
class MediaError {
    constructor(code, message) { this.code = code; this.message = message || ''; }
}
for (const [k, v] of [['MEDIA_ERR_ABORTED', 1], ['MEDIA_ERR_NETWORK', 2], ['MEDIA_ERR_DECODE', 3], ['MEDIA_ERR_SRC_NOT_SUPPORTED', 4]]) {
    MediaError[k] = v;
    MediaError.prototype[k] = v;
}

// what the engine can decode: Opus and AAC-LC audio, VP9 and H.264 video, in WebM (Opus, VP9) and MP4
function parseMime(mime) {
    const m = /^\s*([a-z]+)\/([a-z0-9.+-]+)\s*(?:;\s*codecs\s*=\s*(?:"([^"]*)"|'([^']*)'|([^;\s]*)))?\s*(?:;.*)?$/i.exec(String(mime));
    if (!m) return null;
    const codecs = (m[3] !== undefined ? m[3] : m[4] !== undefined ? m[4] : m[5] !== undefined ? m[5] : null);
    return { type: m[1].toLowerCase(), sub: m[2].toLowerCase(), codecs: codecs === null ? null : codecs.split(',').map(c => c.trim().toLowerCase()).filter(Boolean) };
}
// A slow CPU cannot decode big pictures in real time: codec strings above a level cap are reported as unsupported, which makes a
// player (YouTube) pick a smaller format on its own. Level = decimal level x 10 (VP9 "vp09.00.31.08" and H.264 "avc1.64001f" are 3.1 = 720p).
function levelCap() { const s = W.slow(); return s >= 8 ? 21 : s >= 4 ? 30 : s >= 2 ? 31 : 99; }
function mimeKinds(mime) {      // bit 0 audio, bit 1 video; 0 = unsupported
    const p = parseMime(mime);
    if (!p || (p.type !== 'video' && p.type !== 'audio')) return 0;
    const webm = p.sub === 'webm', mp4 = p.sub === 'mp4' || (p.type === 'audio' && p.sub === 'x-m4a');
    if (!webm && !mp4) return 0;
    if (p.codecs === null) return p.type === 'audio' ? 1 : 3;
    let k = 0;
    for (const c of p.codecs) {
        if (c === 'opus') k |= 1;
        else if (c === 'vp9' || /^vp09(\.|$)/.test(c)) { if (p.type === 'audio') return 0; const lv = parseInt(c.split('.')[2], 10); if (lv > levelCap()) return 0; k |= 2; }
        else if (mp4 && /^mp4a\.40\.0?(2|5)$/.test(c)) k |= 1;                           // AAC-LC (HE-AAC plays without SBR)
        else if (mp4 && /^avc[13]\.[0-9a-f]{6}$/.test(c)) {                               // H.264: Baseline, Main, High (8-bit 4:2:0)
            const prof = parseInt(c.substr(5, 2), 16);
            if (prof !== 66 && prof !== 77 && prof !== 100) return 0;
            if (parseInt(c.substr(9, 2), 16) > levelCap()) return 0;
            if (p.type === 'audio') return 0;
            k |= 2;
        }
        else return 0;
    }
    return k;
}
methods(HM, {
    canPlayType(t) {
        const p = parseMime(t);
        if (!p) return '';
        const k = mimeKinds(t);
        if (!k) return '';
        return p.codecs === null ? 'maybe' : 'probably';
    }
});

const MS = new WeakMap();            // media element -> state
const MEDIA_ACTIVE = new Set();      // elements with an engine session
const OBJURLS = new Map();           // blob: URL -> MediaSource
let mediaTimer = 0, objSeq = 1;
function mst(el) {
    let s = MS.get(el);
    if (!s) {
        s = { id: 0, ms: null, mseUrl: '', gen: 0, currentSrc: '', netState: 0, rs: 0, paused: true, ended: false, seeking: false,
              seekTo: 0, error: null, vol: 1, muted: false, rate: 1, defRate: 1, dur: NaN, ct: 0, vw: 0, vh: 0, bound: false,
              fMeta: false, fData: false, fCan: false, fThrough: false, playing: false, waiting: false, lastTU: 0,
              lastProg: 0, lastBuf: 0, promises: [], frames: 0, dropped: 0, started: false, eosSent: false };
        MS.set(el, s);
    }
    return s;
}
function fire(el, type) {
    if (type !== 'timeupdate' && type !== 'progress' && el instanceof HTMLMediaElement) mlog('event ' + type + ' t=' + (+el.currentTime || 0).toFixed(2) + ' rs=' + el.readyState + ' size=' + el.videoWidth + 'x' + el.videoHeight);
    try { dispatch(el, new Event(type)); } catch (e) { report(e, type); }
}
function mediaKick() {
    if (!mediaTimer) mediaTimer = addTimer(mediaTick, 40, [], true);
}
function mediaBusy(s) { return !s.paused || s.seeking || (s.id && !s.error && !s.fCan); }
function mediaStopTimer() {
    if (!mediaTimer) return;
    for (const el of MEDIA_ACTIVE) { const s = MS.get(el); if (s && mediaBusy(s)) return; }
    TIMERS.delete(mediaTimer);
    mediaTimer = 0;
}
function mediaTeardown(el, s) {
    if (s.id) { try { W.mediaFree(s.id); } catch (e) {} }
    s.id = 0;
    s.bound = false;
    MEDIA_ACTIVE.delete(el);
    if (s.ms && s.ms._el === el) s.ms._detach();
    s.ms = null;
    s.gen++;
}
function mediaSession(el, s) {
    if (s.id) return s.id;
    s.id = W.mediaNew();
    if (!s.id) return 0;
    MEDIA_ACTIVE.add(el);
    W.mediaVolume(s.id, s.vol, (s.mutedSet ? s.muted : W.getAttr(el, 'muted') !== null) ? 1 : 0);
    W.mediaRate(s.id, s.rate);
    if (el instanceof HTMLVideoElement) { W.mediaBind(s.id, el); s.bound = true; }
    mediaKick();
    return s.id;
}
let mlogN = 0;
function mlog(m) { if (mlogN++ < 1500) W.log(4, '[media] ' + m); }
function mediaFail(el, s, code, msg) {
    mlog('ERROR ' + code + ' ' + msg);
    s.error = new MediaError(code, msg);
    s.netState = 3;
    fire(el, 'error');
}
// Decorative media on a slow CPU: a muted or autoplay element is not fetched (a whole mp4 is downloaded by fetch()) until the
// user clicks or presses a key, the same moment play() stops being refused. Controls-driven players are not affected.
function mediaDecorative(el) {
    if (/^blob:/i.test(W.getAttr(el, 'src') || '')) return false;     // MediaSource players need their events either way
    return !userActivated && W.slow() >= 3 && (W.getAttr(el, 'autoplay') !== null || W.getAttr(el, 'muted') !== null || el.muted);
}
function mediaLoad(el) {
    const s = mst(el);
    if (mediaDecorative(el)) { s.deferred = true; mlog('load deferred until user activation: ' + (W.getAttr(el, 'src') || '(sources)')); return; }
    s.deferred = false;
    mlog('load ' + (W.getAttr(el, 'src') || '(sources)'));
    const hadSession = s.id !== 0 || s.currentSrc !== '';
    mediaTeardown(el, s);
    for (const p of s.promises.splice(0)) p.reject(new DOMException('The play() request was interrupted by a new load request.', 'AbortError'));
    if (hadSession) fire(el, 'emptied');
    Object.assign(s, { rs: 0, ended: false, seeking: false, error: null, dur: NaN, ct: 0, vw: 0, vh: 0, fMeta: false, fData: false,
        fCan: false, fThrough: false, playing: false, waiting: false, lastTU: 0, lastProg: 0, lastBuf: 0, started: false, eosSent: false,
        currentSrc: '', netState: 0 });
    if (!hadSession && W.getAttr(el, 'src') === null && !W.query(el, 'source', true).length) return;
    let src = W.getAttr(el, 'src');
    if (src !== null) src = W.resolve(src) || src;
    else {
        for (const so of W.query(el, 'source', true)) {
            const t = W.getAttr(so, 'type');
            if (t && !mimeKinds(t)) continue;
            const u = W.getAttr(so, 'src');
            if (u !== null) { src = W.resolve(u) || u; break; }
        }
    }
    if (src === null || src === '') { s.netState = 3; return; }
    s.currentSrc = src;
    s.netState = 2;
    fire(el, 'loadstart');
    const gen = s.gen;
    if (/^blob:/i.test(src)) {
        const ms = OBJURLS.get(src);
        if (!ms) { setTimeout(() => { if (s.gen === gen) mediaFail(el, s, 4, 'MEDIA_ELEMENT_ERROR: Media load rejected by URL safety check'); }, 0); return; }
        if (!mediaSession(el, s)) { mediaFail(el, s, 4, 'no media session available'); return; }
        s.ms = ms;
        ms._attach(el, s);
        return;
    }
    if (!mediaSession(el, s)) { mediaFail(el, s, 4, 'no media session available'); return; }
    const sb = W.mediaAddSb(s.id, 3);
    fetch(src).then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.arrayBuffer();
    }).then(buf => {
        if (s.gen !== gen) return;
        if (W.mediaAppend(s.id, sb, buf) < 0) { mediaFail(el, s, 4, 'DEMUXER_ERROR_COULD_NOT_OPEN: not a playable WebM/MP4 file (Opus, AAC, VP9, H.264)'); return; }
        W.mediaEos(s.id);
        s.netState = 1;
        fire(el, 'progress');
        fire(el, 'suspend');
        mediaApplyState(el, s);
    }, e => {
        if (s.gen !== gen) return;
        mediaFail(el, s, 2, 'network error: ' + (e && e.message));
    });
    if (W.getAttr(el, 'autoplay') !== null) setTimeout(() => { if (s.gen === gen && s.paused) el.play().catch(() => {}); }, 0);
}
// engine state -> events
function mediaApplyState(el, s) {
    if (!s.id) return;
    const st = W.mediaState(s.id);
    const now = W.now();
    const rs = st[2];
    if (st[6] && !s.error) { mediaFail(el, s, st[6] === 2 ? 4 : 3, 'media decode error'); }
    if (!s.fMeta && rs >= 1) {
        s.fMeta = true;
        s.dur = st[1];
        s.vw = st[7]; s.vh = st[8];
        fire(el, 'durationchange');
        fire(el, 'loadedmetadata');
        if (s.vw) fire(el, 'resize');
    } else if (s.fMeta) {
        if (st[1] !== s.dur && !(st[1] !== st[1] && s.dur !== s.dur)) { s.dur = st[1]; fire(el, 'durationchange'); }
        if (st[7] !== s.vw || st[8] !== s.vh) { s.vw = st[7]; s.vh = st[8]; fire(el, 'resize'); }
    }
    const prev = s.rs;
    s.rs = rs;
    if (rs >= 2 && !s.fData) { s.fData = true; s.netState = 1; fire(el, 'loadeddata'); }
    const hadCan = s.fCan;
    if (rs >= 3 && !s.fCan) { s.fCan = true; fire(el, 'canplay'); }
    else if (rs >= 3 && prev < 3 && hadCan && s.waiting) { s.waiting = false; fire(el, 'canplay'); }
    if (rs >= 4 && !s.fThrough) { s.fThrough = true; fire(el, 'canplaythrough'); }
    if (!s.paused && !s.ended && !st[4]) {
        if (rs >= 3 && !s.playing) {
            s.playing = true;
            s.waiting = false;
            fire(el, 'playing');
            for (const p of s.promises.splice(0)) p.resolve();
        } else if (rs < 3 && s.playing) {
            s.playing = false;
            s.waiting = true;
            fire(el, 'waiting');
        } else if (rs < 3 && !s.playing && !s.waiting && s.fMeta) {
            s.waiting = true;
            fire(el, 'waiting');
        }
    }
    if (s.seeking && !st[5]) {
        s.seeking = false;
        fire(el, 'timeupdate');
        fire(el, 'seeked');
        s.lastTU = now;
    }
    if (st[4] && !s.ended) {
        s.ended = true;
        s.playing = false;
        fire(el, 'timeupdate');
        if (el.loop) {
            s.ended = false;
            mediaSeek(el, s, 0);
            W.mediaPlay(s.id);
        } else {
            if (!s.paused) { s.paused = true; fire(el, 'pause'); }
            fire(el, 'ended');
        }
    } else if (!s.paused && !st[4] && now - s.lastTU >= 250) {
        s.lastTU = now;
        fire(el, 'timeupdate');
    }
    s.frames = st[11]; s.dropped = st[13];
    const buf = W.mediaBytes(s.id);
    if (buf !== s.lastBuf && now - s.lastProg >= 350) { s.lastBuf = buf; s.lastProg = now; fire(el, 'progress'); }
}
function mediaTick() {
    for (const el of Array.from(MEDIA_ACTIVE)) {
        const s = MS.get(el);
        if (!s || !s.id) { MEDIA_ACTIVE.delete(el); continue; }
        try { mediaApplyState(el, s); } catch (e) { report(e, 'media'); }
    }
    mediaStopTimer();
}
function mediaSeek(el, s, t) {
    mlog('seek ' + t);
    if (!s.id) { s.ct = t; return; }
    if (!(t === t)) return;
    const d = s.dur;
    if (d === d && t > d) t = d;
    if (t < 0) t = 0;
    s.seeking = true;
    s.seekTo = t;
    s.ended = false;
    fire(el, 'seeking');
    W.mediaSeek(s.id, t);
    mediaKick();
}
function playbackQuality(s) { return { creationTime: W.now(), totalVideoFrames: s.frames, droppedVideoFrames: s.dropped, corruptedVideoFrames: 0, totalFrameDelay: 0 }; }

accessor(HM, 'currentSrc', function () { return mst(this).currentSrc; });
accessor(HM, 'src', function () {
    const v = W.getAttr(this, 'src');
    if (v === null) return '';
    const r = W.resolve(v);
    return r === null ? v : r;
}, function (v) { setAttr(this, 'src', String(v)); mediaLoad(this); });
accessor(HM, 'srcObject', function () { return mst(this).srcObject || null; }, function (v) {
    const s = mst(this);
    s.srcObject = v || null;
    if (v instanceof MediaSource) { const u = v._objUrl(); setAttr(this, 'src', u); mediaLoad(this); }
});
accessor(HM, 'networkState', function () { return mst(this).netState; });
accessor(HM, 'readyState', function () { return mst(this).rs; });
accessor(HM, 'paused', function () { return mst(this).paused; });
accessor(HM, 'ended', function () { return mst(this).ended; });
accessor(HM, 'seeking', function () { return mst(this).seeking; });
accessor(HM, 'error', function () { return mst(this).error; });
accessor(HM, 'duration', function () { const s = mst(this); if (s.id) { const d = W.mediaState(s.id)[1]; return d; } return NaN; });
accessor(HM, 'currentTime', function () {
    const s = mst(this);
    if (!s.id) return s.ct;
    if (s.seeking) return s.seekTo;
    return W.mediaTime(s.id);
}, function (v) { v = +v; if (v !== v) throw new TypeError("Failed to set the 'currentTime' property: The provided double value is non-finite."); mediaSeek(this, mst(this), v); });
accessor(HM, 'volume', function () { return mst(this).vol; }, function (v) {
    v = +v;
    if (!(v >= 0 && v <= 1)) throw new DOMException('The volume provided (' + v + ') is outside the range [0, 1].', 'IndexSizeError');
    const s = mst(this);
    if (s.vol === v) return;
    s.vol = v;
    if (s.id) W.mediaVolume(s.id, s.vol, s.muted ? 1 : 0);
    fire(this, 'volumechange');
});
accessor(HM, 'muted', function () { const s = mst(this); return s.mutedSet === undefined ? W.getAttr(this, 'muted') !== null : s.muted; }, function (v) {
    const s = mst(this);
    v = !!v;
    if (s.muted === v && s.mutedSet) return;
    s.muted = v;
    s.mutedSet = true;
    if (s.id) W.mediaVolume(s.id, s.vol, v ? 1 : 0);
    fire(this, 'volumechange');
});
accessor(HM, 'playbackRate', function () { return mst(this).rate; }, function (v) {
    v = +v;
    if (!(v > 0)) return;
    const s = mst(this);
    if (s.rate === v) return;
    s.rate = v;
    if (s.id) W.mediaRate(s.id, v);
    fire(this, 'ratechange');
});
accessor(HM, 'defaultPlaybackRate', function () { return mst(this).defRate; }, function (v) { mst(this).defRate = +v || 1; });
accessor(HM, 'preservesPitch', function () { return true; }, function () {});
accessor(HM, 'buffered', function () { const s = mst(this); return new TimeRanges(s.id ? W.mediaBuffered(s.id) : []); });
accessor(HM, 'seekable', function () {
    const s = mst(this);
    if (!s.id) return new TimeRanges([]);
    const b = W.mediaBuffered(s.id), d = W.mediaState(s.id)[1];
    return new TimeRanges(d === d && d !== Infinity ? [0, d] : b);
});
accessor(HM, 'played', function () { return new TimeRanges([]); });
accessor(HM, 'textTracks', function () { const a = []; a.addEventListener = () => {}; a.removeEventListener = () => {}; a.getTrackById = () => null; return a; });
accessor(HM, 'mediaKeys', function () { return null; });
accessor(HM, 'webkitDecodedFrameCount', function () { return mst(this).frames; });
accessor(HM, 'webkitDroppedFrameCount', function () { return mst(this).dropped; });
accessor(HM, 'webkitAudioDecodedByteCount', function () { return 0; });
accessor(HM, 'webkitVideoDecodedByteCount', function () { return 0; });
accessor(HTMLVideoElement.prototype, 'videoWidth', function () { return mst(this).vw; });
accessor(HTMLVideoElement.prototype, 'videoHeight', function () { return mst(this).vh; });
methods(HTMLVideoElement.prototype, {
    getVideoPlaybackQuality() { return playbackQuality(mst(this)); },
    requestVideoFrameCallback(cb) { return setTimeout(() => cb(W.now(), { presentationTime: W.now(), width: mst(this).vw, height: mst(this).vh }), 33); },
    cancelVideoFrameCallback(id) { clearTimeout(id); },
    requestPictureInPicture() { return Promise.reject(new DOMException('not supported', 'NotSupportedError')); }
});
methods(HM, {
    load() { mediaLoad(this); },
    play() {
        const s = mst(this);
        mlog('play()');
        if (!userActivated && W.slow() >= 3 && (this.muted || W.getAttr(this, 'muted') !== null)) {
            mlog('play() blocked: slow CPU, no user activation');
            return Promise.reject(new DOMException("play() failed because the user didn't interact with the document first.", 'NotAllowedError'));
        }
        if (!s.id && !s.currentSrc) {
            const hasSrc = W.getAttr(this, 'src') !== null || W.query(this, 'source', true).length;
            if (hasSrc) mediaLoad(this);
            if (!s.id && !s.currentSrc) return Promise.reject(new DOMException('The element has no supported sources.', 'NotSupportedError'));
        }
        if (s.error) return Promise.reject(new DOMException('The media resource indicated by the src attribute was not suitable.', 'NotSupportedError'));
        // A slow CPU cannot afford decorative background video: muted autoplay waits for the first click or key press
        // (the same "autoplay blocked" path a browser in low-power mode takes).
        if (!userActivated && W.slow() >= 3 && (this.muted || W.getAttr(this, 'muted') !== null)) {
            mlog('play() blocked: slow CPU, no user activation');
            return Promise.reject(new DOMException("play() failed because the user didn't interact with the document first.", 'NotAllowedError'));
        }
        const wasPaused = s.paused;
        s.paused = false;
        s.started = true;
        if (s.ended) { s.ended = false; mediaSeek(this, s, 0); }
        if (s.id) W.mediaPlay(s.id);
        mediaKick();
        if (wasPaused) {
            fire(this, 'play');
            if (s.rs >= 3) { s.playing = true; fire(this, 'playing'); return Promise.resolve(); }
            fire(this, 'waiting');
            s.waiting = true;
        } else if (s.rs >= 3) return Promise.resolve();
        return new Promise((resolve, reject) => { s.promises.push({ resolve, reject }); });
    },
    pause() {
        const s = mst(this);
        if (s.paused) return;
        s.paused = true;
        s.playing = false;
        s.waiting = false;
        if (s.id) W.mediaPause(s.id);
        for (const p of s.promises.splice(0)) p.reject(new DOMException('The play() request was interrupted by a call to pause().', 'AbortError'));
        fire(this, 'timeupdate');
        fire(this, 'pause');
    },
    fastSeek(t) { mediaSeek(this, mst(this), +t); },
    addTextTrack() { return { mode: 'disabled', cues: [], addCue() {}, removeCue() {}, addEventListener() {} }; },
    setMediaKeys() { return Promise.reject(new DOMException('encrypted media is not supported', 'NotSupportedError')); },
    setSinkId() { return Promise.resolve(); },
    captureStream() { throw new DOMException('not supported', 'NotSupportedError'); }
});
// <video src> / <audio src> / <source> children parsed from HTML start loading once the document is ready
function mediaScan() {
    for (const el of W.query(document, 'video,audio', true)) {
        const s = mst(el);
        if (!s.id && !s.currentSrc && (W.getAttr(el, 'src') !== null || W.query(el, 'source', true).length) && W.getAttr(el, 'preload') !== 'none') mediaLoad(el);
    }
}

// ---- MediaSource
class SourceBufferList extends EventTarget {
    constructor() { super(); hidden(this, '_a', []); }
    get length() { return this._a.length; }
    item(i) { return this._a[i] || null; }
    [Symbol.iterator]() { return this._a[Symbol.iterator](); }
}
class SourceBuffer extends EventTarget {
    constructor(ms, idx, mime) {
        super();
        hidden(this, '_ms', ms);
        hidden(this, '_idx', idx);
        hidden(this, '_q', []);
        hidden(this, '_busy', false);
        hidden(this, '_removed', false);
        this.mode = 'segments';
        this.updating = false;
        this.timestampOffset = 0;
        this.appendWindowStart = 0;
        this.appendWindowEnd = Infinity;
        this.type = mime;
    }
    get buffered() {
        if (this._removed) throw new DOMException('This SourceBuffer has been removed from the parent media source.', 'InvalidStateError');
        const el = this._ms._el, s = el && MS.get(el);
        return new TimeRanges(s && s.id ? W.mediaBuffered(s.id, this._idx) : []);
    }
    _check() {
        if (this._removed) throw new DOMException('This SourceBuffer has been removed from the parent media source.', 'InvalidStateError');
        if (this.updating) throw new DOMException('The SourceBuffer is still processing an \'appendBuffer\' or \'remove\' operation.', 'InvalidStateError');
        if (this._ms.readyState === 'ended') { this._ms.readyState = 'open'; fire(this._ms, 'sourceopen'); }
    }
    _op(fn) {
        this.updating = true;
        fire(this, 'updatestart');
        setTimeout(() => {
            if (this._removed) return;
            let ok = true;
            try { ok = fn() !== false; } catch (e) { report(e, 'SourceBuffer'); ok = false; }
            this.updating = false;
            if (ok) fire(this, 'update'); else { fire(this, 'error'); }
            fire(this, 'updateend');
        }, 0);
    }
    appendBuffer(data) {
        this._check();
        if (!(data instanceof ArrayBuffer || ArrayBuffer.isView(data))) throw new TypeError("Failed to execute 'appendBuffer' on 'SourceBuffer': The provided value is not of type 'ArrayBuffer'.");
        const ms = this._ms;
        mlog('appendBuffer sb ' + this._idx + ' ' + data.byteLength + ' bytes');
        this._op(() => {
            const el = ms._el, s = el && MS.get(el);
            if (!s || !s.id) return false;
            const rc = W.mediaAppend(s.id, this._idx, data);
            if (rc < 0) { mediaFail(el, s, 3, 'CHUNK_DEMUXER_ERROR_APPEND_FAILED'); return false; }
            mediaApplyState(el, s);
            return true;
        });
    }
    appendBufferAsync(d) { this.appendBuffer(d); return Promise.resolve(); }
    remove(start, end) {
        this._check();
        start = +start; end = +end;
        if (!(start >= 0) || !(end > start)) throw new TypeError("Failed to execute 'remove' on 'SourceBuffer': The start and end must be valid.");
        const ms = this._ms;
        this._op(() => {
            const el = ms._el, s = el && MS.get(el);
            if (s && s.id) W.mediaRemove(s.id, start, end);
            return true;
        });
    }
    abort() {
        if (this._removed) throw new DOMException('This SourceBuffer has been removed from the parent media source.', 'InvalidStateError');
        const el = this._ms._el, s = el && MS.get(el);
        if (s && s.id) W.mediaAbort(s.id, this._idx);
        if (this.updating) { this.updating = false; fire(this, 'abort'); fire(this, 'updateend'); }
    }
    changeType(mime) { if (!mimeKinds(mime)) throw new DOMException('Unsupported type', 'NotSupportedError'); this.type = mime; }
}
class MediaSource extends EventTarget {
    constructor() {
        super();
        this.readyState = 'closed';
        hidden(this, '_dur', NaN);
        hidden(this, '_el', null);
        hidden(this, '_url', '');
        this.sourceBuffers = new SourceBufferList();
        this.activeSourceBuffers = new SourceBufferList();
    }
    static isTypeSupported(t) { return mimeKinds(t) !== 0; }
    _objUrl() {
        if (!this._url) { this._url = 'blob:' + location.origin + '/' + crypto.randomUUID(); OBJURLS.set(this._url, this); }
        return this._url;
    }
    _attach(el, s) {
        this._el = el;
        setTimeout(() => {
            if (this._el !== el || s.ms !== this) return;
            this.readyState = 'open';
            fire(this, 'sourceopen');
        }, 0);
    }
    _detach() {
        const was = this.readyState;
        this._el = null;
        this.readyState = 'closed';
        for (const sb of this.sourceBuffers._a) sb._removed = true;
        this.sourceBuffers._a.length = 0;
        this.activeSourceBuffers._a.length = 0;
        if (was !== 'closed') fire(this, 'sourceclose');
    }
    get duration() { return this._dur; }
    set duration(d) {
        d = +d;
        if (this.readyState !== 'open') throw new DOMException("Failed to set the 'duration' property on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
        if (d !== d || d < 0) throw new TypeError('The value provided is not a valid duration.');
        this._dur = d;
        const el = this._el, s = el && MS.get(el);
        if (s && s.id) W.mediaDuration(s.id, d);
    }
    get canConstructInDedicatedWorker() { return false; }
    addSourceBuffer(mime) {
        if (this.readyState !== 'open') throw new DOMException("Failed to execute 'addSourceBuffer' on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
        const kinds = mimeKinds(mime);
        if (!kinds) throw new DOMException("Failed to execute 'addSourceBuffer' on 'MediaSource': The type provided ('" + mime + "') is unsupported.", 'NotSupportedError');
        const el = this._el, s = el && MS.get(el);
        if (!s || !s.id) throw new DOMException('The MediaSource is not attached.', 'InvalidStateError');
        const idx = W.mediaAddSb(s.id, kinds);
        if (idx < 0) throw new DOMException('Cannot add another SourceBuffer.', 'QuotaExceededError');
        mlog('addSourceBuffer ' + mime + ' -> sb ' + idx);
        const sb = new SourceBuffer(this, idx, String(mime));
        this.sourceBuffers._a.push(sb);
        this.activeSourceBuffers._a.push(sb);
        fire(this.sourceBuffers, 'addsourcebuffer');
        return sb;
    }
    removeSourceBuffer(sb) {
        const i = this.sourceBuffers._a.indexOf(sb);
        if (i < 0) throw new DOMException('The SourceBuffer provided is not in this MediaSource.', 'NotFoundError');
        sb._removed = true;
        this.sourceBuffers._a.splice(i, 1);
        const j = this.activeSourceBuffers._a.indexOf(sb);
        if (j >= 0) this.activeSourceBuffers._a.splice(j, 1);
        fire(this.sourceBuffers, 'removesourcebuffer');
    }
    endOfStream(err) {
        if (this.readyState !== 'open') throw new DOMException("Failed to execute 'endOfStream' on 'MediaSource': The MediaSource's readyState is not 'open'.", 'InvalidStateError');
        for (const sb of this.sourceBuffers._a) if (sb.updating) throw new DOMException('A SourceBuffer is still updating.', 'InvalidStateError');
        this.readyState = 'ended';
        const el = this._el, s = el && MS.get(el);
        if (s && s.id) {
            if (this._dur !== this._dur) { /* duration = buffered end */ const b = W.mediaBuffered(s.id); if (b.length) { this._dur = b[b.length - 1]; W.mediaDuration(s.id, this._dur); } }
            W.mediaEos(s.id);
            s.eosSent = true;
        }
        fire(this, 'sourceended');
    }
    setLiveSeekableRange() {}
    clearLiveSeekableRange() {}
}
function mediaObjURL(obj) {
    if (obj instanceof MediaSource) return obj._objUrl();
    return 'blob:' + location.origin + '/' + crypto.randomUUID();
}
function mediaDecodingInfo(cfg) {
    let ok = false, smooth = false;
    const v = cfg && cfg.video, a = cfg && cfg.audio;
    if ((v || a) && (!v || mimeKinds(v.contentType) & 2) && (!a || mimeKinds(a.contentType) & 1)) {
        ok = true;
        smooth = !v || (v.width * v.height <= (levelCap() >= 99 ? 1280 * 720 : levelCap() >= 31 ? 1280 * 720 : levelCap() >= 30 ? 854 * 480 : 480 * 360) && (v.framerate || 30) <= 30);
    }
    return Promise.resolve({ supported: ok, smooth, powerEfficient: false });
}
for (const C of [MediaSource, SourceBuffer, SourceBufferList]) defineHandlers(C.prototype);
const HC = HTMLCanvasElement.prototype;
// ---------------------------------------------------------------- canvas 2D
// CanvasRenderingContext2D over the software canvas natives (wjs_canvas.c / wcanvas.c): this side keeps
// the drawing state, builds paths in device space (the CTM is applied when a segment is added) and
// passes plain number arrays to the natives.
const CV_OPS = ['source-over', 'source-in', 'source-out', 'source-atop', 'destination-over', 'destination-in', 'destination-out',
    'destination-atop', 'lighter', 'copy', 'xor', 'multiply', 'screen', 'overlay', 'darken', 'lighten', 'color-dodge', 'color-burn',
    'hard-light', 'soft-light', 'difference', 'exclusion', 'hue', 'saturation', 'color', 'luminosity'];
const CV_CLEAR = 26;
const IDENT = [1, 0, 0, 1, 0, 0];
const mmul = (m, n) => [m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
    m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]];
function minv(m) {
    const det = m[0] * m[3] - m[1] * m[2];
    if (!det || !isFinite(det)) return null;
    const id = 1 / det;
    return [m[3] * id, -m[1] * id, -m[2] * id, m[0] * id, (m[2] * m[5] - m[3] * m[4]) * id, (m[1] * m[4] - m[0] * m[5]) * id];
}
const finite = (...v) => { for (const x of v) if (!isFinite(x)) return false; return true; };
const num = v => +v;

class DOMPointReadOnly {
    constructor(x = 0, y = 0, z = 0, w = 1) { this.x = +x; this.y = +y; this.z = +z; this.w = +w; }
    static fromPoint(p) { return new DOMPoint(p && p.x, p && p.y, p && p.z, p && p.w === undefined ? 1 : p.w); }
}
class DOMPoint extends DOMPointReadOnly {}
class DOMMatrixReadOnly {
    constructor(init) {
        let m = IDENT.slice();
        if (Array.isArray(init) || (init && typeof init === 'object' && init.length !== undefined && typeof init !== 'string')) {
            if (init.length === 6) m = Array.from(init, Number);
            else if (init.length === 16) m = [init[0], init[1], init[4], init[5], init[12], init[13]];
        } else if (typeof init === 'string') {
            const mm = /matrix\(([^)]*)\)/.exec(init);
            if (mm) m = mm[1].split(',').map(Number);
        }
        hidden(this, '_m', m);
    }
    get a() { return this._m[0]; } get b() { return this._m[1]; } get c() { return this._m[2]; }
    get d() { return this._m[3]; } get e() { return this._m[4]; } get f() { return this._m[5]; }
    get m11() { return this._m[0]; } get m12() { return this._m[1]; } get m21() { return this._m[2]; } get m22() { return this._m[3]; }
    get m41() { return this._m[4]; } get m42() { return this._m[5]; }
    get m13() { return 0; } get m14() { return 0; } get m23() { return 0; } get m24() { return 0; } get m31() { return 0; } get m32() { return 0; }
    get m33() { return 1; } get m34() { return 0; } get m43() { return 0; } get m44() { return 1; }
    get is2D() { return true; }
    get isIdentity() { const m = this._m; return m[0] === 1 && m[1] === 0 && m[2] === 0 && m[3] === 1 && m[4] === 0 && m[5] === 0; }
    _new(m) { const r = new DOMMatrix(); r._m = m; return r; }
    multiply(o) { return this._new(mmul(this._m, new DOMMatrix(o)._m)); }
    translate(x = 0, y = 0) { return this._new(mmul(this._m, [1, 0, 0, 1, +x, +y])); }
    scale(sx = 1, sy = sx, _sz, ox = 0, oy = 0) { return this._new(mmul(mmul(mmul(this._m, [1, 0, 0, 1, ox, oy]), [sx, 0, 0, sy, 0, 0]), [1, 0, 0, 1, -ox, -oy])); }
    rotate(deg = 0) { const a = deg * Math.PI / 180, c = Math.cos(a), s = Math.sin(a); return this._new(mmul(this._m, [c, s, -s, c, 0, 0])); }
    inverse() { const i = minv(this._m); return this._new(i || [NaN, NaN, NaN, NaN, NaN, NaN]); }
    transformPoint(p) { const m = this._m, x = p && p.x || 0, y = p && p.y || 0; return new DOMPoint(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]); }
    toString() { return 'matrix(' + this._m.join(', ') + ')'; }
    toJSON() { const m = this._m; return { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5] }; }
}
class DOMMatrix extends DOMMatrixReadOnly {
    set a(v) { this._m[0] = +v; } set b(v) { this._m[1] = +v; } set c(v) { this._m[2] = +v; }
    set d(v) { this._m[3] = +v; } set e(v) { this._m[4] = +v; } set f(v) { this._m[5] = +v; }
    get a() { return this._m[0]; } get b() { return this._m[1]; } get c() { return this._m[2]; }
    get d() { return this._m[3]; } get e() { return this._m[4]; } get f() { return this._m[5]; }
    multiplySelf(o) { this._m = mmul(this._m, new DOMMatrix(o)._m); return this; }
    translateSelf(x, y) { this._m = mmul(this._m, [1, 0, 0, 1, +x || 0, +y || 0]); return this; }
    scaleSelf(sx = 1, sy = sx) { this._m = mmul(this._m, [sx, 0, 0, sy, 0, 0]); return this; }
    rotateSelf(deg = 0) { const a = deg * Math.PI / 180, c = Math.cos(a), s = Math.sin(a); this._m = mmul(this._m, [c, s, -s, c, 0, 0]); return this; }
    invertSelf() { this._m = minv(this._m) || [NaN, NaN, NaN, NaN, NaN, NaN]; return this; }
    static fromMatrix(o) { return new DOMMatrix(o && o._m ? o._m : o && o.a !== undefined ? [o.a, o.b, o.c, o.d, o.e, o.f] : undefined); }
}

// ---- colours
function colorString(argb) {
    const a = (argb >>> 24) & 255, r = (argb >>> 16) & 255, g = (argb >>> 8) & 255, b = argb & 255;
    if (a === 255) return '#' + ((1 << 24) | (r << 16) | (g << 8) | b).toString(16).slice(1);
    return 'rgba(' + r + ', ' + g + ', ' + b + ', ' + (Math.round(a / 255 * 1000) / 1000) + ')';
}
function parseColor(s, el) {
    const v = W.cssColor(String(s));
    if (v === -1) return null;
    if (v === -2) {                                       // currentcolor: the element's colour
        try { const c = el && getComputedStyle(el).color; const w = c && W.cssColor(c); if (w >= 0) return w; } catch (_) {}
        return 0xFF000000;
    }
    return v;
}

class CanvasGradient {
    constructor(kind, g) { hidden(this, '_kind', kind); hidden(this, '_g', g); hidden(this, '_stops', []); }
    addColorStop(offset, color) {
        offset = +offset;
        if (!(offset >= 0 && offset <= 1)) throw new DOMException("Failed to execute 'addColorStop' on 'CanvasGradient': The provided value (" + offset + ") is outside the range (0.0, 1.0).", 'IndexSizeError');
        const c = parseColor(color);
        if (c === null) throw new DOMException("Failed to execute 'addColorStop' on 'CanvasGradient': The value provided ('" + color + "') could not be parsed as a color.", 'SyntaxError');
        let i = this._stops.length;
        while (i > 0 && this._stops[i - 1][0] > offset) i--;
        this._stops.splice(i, 0, [offset, c]);
    }
}
class CanvasPattern {
    constructor(src, rx, ry) { hidden(this, '_src', src); hidden(this, '_rx', rx); hidden(this, '_ry', ry); }
    setTransform(m) { hidden(this, '_m', m ? new DOMMatrix(m)._m : null); }
}

// ---- paths
class PathBuilder {
    constructor() { this.c = []; this.m = null; this.cx = 0; this.cy = 0; this.sx = 0; this.sy = 0; this.open = false; this.hasSub = false; }
    _x(x, y) { const m = this.m; return m ? [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]] : [x, y]; }
    _ensure(x, y) {                       // start a subpath for a drawing command at (x, y) when none is open
        if (this.open) return;
        if (this.hasSub) this.moveTo(this.cx, this.cy); else this.moveTo(x, y);
    }
    moveTo(x, y) {
        x = +x; y = +y;
        if (!finite(x, y)) return;
        const p = this._x(x, y);
        this.c.push(0, p[0], p[1]);
        this.cx = this.sx = x; this.cy = this.sy = y; this.open = true; this.hasSub = true;
    }
    lineTo(x, y) {
        x = +x; y = +y;
        if (!finite(x, y)) return;
        if (!this.hasSub) { this.moveTo(x, y); return; }
        this._ensure(x, y);
        const p = this._x(x, y);
        this.c.push(1, p[0], p[1]);
        this.cx = x; this.cy = y;
    }
    closePath() {
        if (!this.open) return;
        this.c.push(4);
        this.cx = this.sx; this.cy = this.sy; this.open = false;
    }
    quadraticCurveTo(cpx, cpy, x, y) {
        cpx = +cpx; cpy = +cpy; x = +x; y = +y;
        if (!finite(cpx, cpy, x, y)) return;
        if (!this.hasSub) this.moveTo(cpx, cpy); else this._ensure(cpx, cpy);
        const a = this._x(cpx, cpy), b = this._x(x, y);
        this.c.push(2, a[0], a[1], b[0], b[1]);
        this.cx = x; this.cy = y;
    }
    bezierCurveTo(x1, y1, x2, y2, x, y) {
        x1 = +x1; y1 = +y1; x2 = +x2; y2 = +y2; x = +x; y = +y;
        if (!finite(x1, y1, x2, y2, x, y)) return;
        if (!this.hasSub) this.moveTo(x1, y1); else this._ensure(x1, y1);
        const a = this._x(x1, y1), b = this._x(x2, y2), c = this._x(x, y);
        this.c.push(3, a[0], a[1], b[0], b[1], c[0], c[1]);
        this.cx = x; this.cy = y;
    }
    rect(x, y, w, h) {
        x = +x; y = +y; w = +w; h = +h;
        if (!finite(x, y, w, h)) return;
        this.moveTo(x, y); this.lineTo(x + w, y); this.lineTo(x + w, y + h); this.lineTo(x, y + h); this.closePath();
        this.moveTo(x, y);
    }
    // arc of an ellipse as cubic segments; the current point is assumed to be at the start already
    _arcSeg(cx, cy, rx, ry, rot, a0, sweep) {
        const n = Math.max(1, Math.ceil(Math.abs(sweep) / (Math.PI / 2) - 1e-9));
        const d = sweep / n, k = 4 / 3 * Math.tan(d / 4);
        const cr = Math.cos(rot), sr = Math.sin(rot);
        const P = (ux, uy) => { const px = ux * rx, py = uy * ry; return [cx + px * cr - py * sr, cy + px * sr + py * cr]; };
        let a = a0;
        for (let i = 0; i < n; i++) {
            const b = a + d, ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(b), sb = Math.sin(b);
            const p1 = P(ca - k * sa, sa + k * ca), p2 = P(cb + k * sb, sb - k * cb), p3 = P(cb, sb);
            this.bezierCurveTo(p1[0], p1[1], p2[0], p2[1], p3[0], p3[1]);
            a = b;
        }
    }
    ellipse(x, y, rx, ry, rot, a0, a1, ccw) {
        x = +x; y = +y; rx = +rx; ry = +ry; rot = +rot; a0 = +a0; a1 = +a1; ccw = !!ccw;
        if (!finite(x, y, rx, ry, rot, a0, a1)) return;
        if (rx < 0 || ry < 0) throw new DOMException("Failed to execute 'ellipse' on 'CanvasRenderingContext2D': The major-axis radius provided (" + (rx < 0 ? rx : ry) + ') is negative.', 'IndexSizeError');
        const TAU = Math.PI * 2;
        let sweep;
        if (!ccw) { sweep = a1 - a0; if (sweep >= TAU) sweep = TAU; else if (sweep < 0) { sweep = sweep % TAU; if (sweep < 0) sweep += TAU; } }
        else { sweep = a1 - a0; if (sweep <= -TAU) sweep = -TAU; else if (sweep > 0) { sweep = sweep % TAU; if (sweep > 0) sweep -= TAU; } }
        const cr = Math.cos(rot), sr = Math.sin(rot);
        const sx = x + rx * Math.cos(a0) * cr - ry * Math.sin(a0) * sr, sy = y + rx * Math.cos(a0) * sr + ry * Math.sin(a0) * cr;
        if (this.hasSub) this.lineTo(sx, sy); else this.moveTo(sx, sy);
        if (rx === 0 || ry === 0) { const ex = x + rx * Math.cos(a0 + sweep) * cr - ry * Math.sin(a0 + sweep) * sr, ey = y + rx * Math.cos(a0 + sweep) * sr + ry * Math.sin(a0 + sweep) * cr; this.lineTo(ex, ey); return; }
        this._arcSeg(x, y, rx, ry, rot, a0, sweep);
    }
    arc(x, y, r, a0, a1, ccw) {
        if (+r < 0) throw new DOMException("Failed to execute 'arc' on 'CanvasRenderingContext2D': The radius provided (" + r + ') is negative.', 'IndexSizeError');
        this.ellipse(x, y, r, r, 0, a0, a1, ccw);
    }
    arcTo(x1, y1, x2, y2, r) {
        x1 = +x1; y1 = +y1; x2 = +x2; y2 = +y2; r = +r;
        if (!finite(x1, y1, x2, y2, r)) return;
        if (r < 0) throw new DOMException("Failed to execute 'arcTo' on 'CanvasRenderingContext2D': The radius provided (" + r + ') is negative.', 'IndexSizeError');
        if (!this.hasSub) this.moveTo(x1, y1);
        this._ensure(x1, y1);
        const x0 = this.cx, y0 = this.cy;
        if ((x0 === x1 && y0 === y1) || (x1 === x2 && y1 === y2) || r === 0) { this.lineTo(x1, y1); return; }
        let ax = x0 - x1, ay = y0 - y1, bx = x2 - x1, by = y2 - y1;
        const la = Math.hypot(ax, ay), lb = Math.hypot(bx, by);
        ax /= la; ay /= la; bx /= lb; by /= lb;
        const cross = ax * by - ay * bx, dot = ax * bx + ay * by;
        if (Math.abs(cross) < 1e-9) { this.lineTo(x1, y1); return; }
        const half = Math.acos(Math.max(-1, Math.min(1, dot))) / 2;
        const dist = r / Math.tan(half);
        const t1x = x1 + ax * dist, t1y = y1 + ay * dist, t2x = x1 + bx * dist, t2y = y1 + by * dist;
        let mx = ax + bx, my = ay + by;
        const ml = Math.hypot(mx, my);
        mx /= ml; my /= ml;
        const cd = r / Math.sin(half), ccx = x1 + mx * cd, ccy = y1 + my * cd;
        const s0 = Math.atan2(t1y - ccy, t1x - ccx), s1 = Math.atan2(t2y - ccy, t2x - ccx);
        let sweep = s1 - s0;
        while (sweep > Math.PI) sweep -= 2 * Math.PI;
        while (sweep < -Math.PI) sweep += 2 * Math.PI;
        this.lineTo(t1x, t1y);
        this._arcSeg(ccx, ccy, r, r, 0, s0, sweep);
    }
    roundRect(x, y, w, h, radii) {
        x = +x; y = +y; w = +w; h = +h;
        if (!finite(x, y, w, h)) return;
        let rs = radii === undefined ? [0] : Array.isArray(radii) ? radii : [radii];
        if (rs.length < 1 || rs.length > 4) throw new RangeError("Failed to execute 'roundRect' on 'CanvasRenderingContext2D': " + rs.length + ' radii provided. Between one and four radii are necessary.');
        rs = rs.map(r => { const v = (r && typeof r === 'object') ? { x: +r.x, y: r.y === undefined ? +r.x : +r.y } : { x: +r, y: +r }; return v; });
        for (const r of rs) if (!finite(r.x, r.y)) return; else if (r.x < 0 || r.y < 0) throw new RangeError("Failed to execute 'roundRect' on 'CanvasRenderingContext2D': Radius value " + Math.min(r.x, r.y) + ' is negative.');
        const q = n => rs[rs.length === 1 ? 0 : rs.length === 2 ? n % 2 : rs.length === 3 ? (n === 3 ? 1 : n) : n];
        let tl = q(0), tr = q(1), br = q(2), bl = q(3);
        if (w < 0) { x += w; w = -w; [tl, tr, br, bl] = [tr, tl, bl, br]; }
        if (h < 0) { y += h; h = -h; [tl, tr, br, bl] = [bl, br, tr, tl]; }
        let k = 1;
        for (const [a, b, len] of [[tl.x, tr.x, w], [bl.x, br.x, w], [tl.y, bl.y, h], [tr.y, br.y, h]]) if (a + b > len) k = Math.min(k, len / (a + b));
        const sc = r => ({ x: r.x * k, y: r.y * k });
        tl = sc(tl); tr = sc(tr); br = sc(br); bl = sc(bl);
        this.moveTo(x + tl.x, y);
        this.lineTo(x + w - tr.x, y);
        this._arcSeg(x + w - tr.x, y + tr.y, tr.x, tr.y, 0, -Math.PI / 2, Math.PI / 2);
        this.lineTo(x + w, y + h - br.y);
        this._arcSeg(x + w - br.x, y + h - br.y, br.x, br.y, 0, 0, Math.PI / 2);
        this.lineTo(x + bl.x, y + h);
        this._arcSeg(x + bl.x, y + h - bl.y, bl.x, bl.y, 0, Math.PI / 2, Math.PI / 2);
        this.lineTo(x, y + tl.y);
        this._arcSeg(x + tl.x, y + tl.y, tl.x, tl.y, 0, Math.PI, Math.PI / 2);
        this.closePath();
        this.moveTo(x, y);
    }
}
function pathMethods(proto) {
    for (const k of ['moveTo', 'lineTo', 'closePath', 'quadraticCurveTo', 'bezierCurveTo', 'rect', 'arc', 'arcTo', 'ellipse', 'roundRect'])
        hidden(proto, k, function (...a) { this._p.m = this._mat ? this._mat() : null; return this._p[k](...a); });
}

// SVG path data -> builder calls
function svgPath(p, d) {
    const re = /([MmLlHhVvCcSsQqTtAaZz])|(-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)/g;
    const toks = [];
    let m;
    while ((m = re.exec(d))) toks.push(m[1] || +m[2]);
    let i = 0, cmd = '', x = 0, y = 0, sx = 0, sy = 0, lcx = 0, lcy = 0, last = '';
    const has = n => i + n <= toks.length && toks.slice(i, i + n).every(t => typeof t === 'number');
    const arcTo = (rx, ry, rot, large, sweep, ex, ey) => {            // endpoint arc -> centre parameterisation (SVG 1.1 F.6)
        if (rx === 0 || ry === 0) { p.lineTo(ex, ey); return; }
        rx = Math.abs(rx); ry = Math.abs(ry);
        const phi = rot * Math.PI / 180, c = Math.cos(phi), s = Math.sin(phi);
        const dx = (x - ex) / 2, dy = (y - ey) / 2, x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
        const lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
        if (lam > 1) { const k = Math.sqrt(lam); rx *= k; ry *= k; }
        const num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        let co = Math.sqrt(Math.max(0, num / den)) * (large === sweep ? -1 : 1);
        const cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
        const cx = c * cxp - s * cyp + (x + ex) / 2, cy = s * cxp + c * cyp + (y + ey) / 2;
        const ang = (ux, uy, vx, vy) => { const a = Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy); return a; };
        const th1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        let dth = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && dth > 0) dth -= 2 * Math.PI; else if (sweep && dth < 0) dth += 2 * Math.PI;
        p._arcSeg(cx, cy, rx, ry, phi, th1, dth);
    };
    while (i < toks.length) {
        if (typeof toks[i] === 'string') cmd = toks[i++]; else if (!cmd) return;
        const rel = cmd === cmd.toLowerCase(), C = cmd.toUpperCase();
        const ox = rel ? x : 0, oy = rel ? y : 0;
        if (C === 'Z') { p.closePath(); x = sx; y = sy; last = 'Z'; continue; }
        const need = { M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, Q: 4, T: 2, A: 7 }[C];
        if (!need || !has(need)) return;
        const a = toks.slice(i, i + need);
        i += need;
        if (C === 'M') { x = ox + a[0]; y = oy + a[1]; sx = x; sy = y; p.moveTo(x, y); cmd = rel ? 'l' : 'L'; }
        else if (C === 'L') { x = ox + a[0]; y = oy + a[1]; p.lineTo(x, y); }
        else if (C === 'H') { x = ox + a[0]; p.lineTo(x, y); }
        else if (C === 'V') { y = oy + a[0]; p.lineTo(x, y); }
        else if (C === 'C') { p.bezierCurveTo(ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]); lcx = ox + a[2]; lcy = oy + a[3]; x = ox + a[4]; y = oy + a[5]; }
        else if (C === 'S') {
            const c1x = last === 'C' || last === 'S' ? 2 * x - lcx : x, c1y = last === 'C' || last === 'S' ? 2 * y - lcy : y;
            p.bezierCurveTo(c1x, c1y, ox + a[0], oy + a[1], ox + a[2], oy + a[3]); lcx = ox + a[0]; lcy = oy + a[1]; x = ox + a[2]; y = oy + a[3];
        }
        else if (C === 'Q') { p.quadraticCurveTo(ox + a[0], oy + a[1], ox + a[2], oy + a[3]); lcx = ox + a[0]; lcy = oy + a[1]; x = ox + a[2]; y = oy + a[3]; }
        else if (C === 'T') {
            const qx = last === 'Q' || last === 'T' ? 2 * x - lcx : x, qy = last === 'Q' || last === 'T' ? 2 * y - lcy : y;
            p.quadraticCurveTo(qx, qy, ox + a[0], oy + a[1]); lcx = qx; lcy = qy; x = ox + a[0]; y = oy + a[1];
        }
        else if (C === 'A') { const ex = ox + a[5], ey = oy + a[6]; arcTo(a[0], a[1], a[2], a[3] !== 0, a[4] !== 0, ex, ey); x = ex; y = ey; }
        last = C;
    }
}

class Path2D {
    constructor(arg) {
        hidden(this, '_p', new PathBuilder());
        if (arg instanceof Path2D) this.addPath(arg);
        else if (typeof arg === 'string') svgPath(this._p, arg);
    }
    addPath(path, m) {
        const mm = m ? new DOMMatrix(m)._m : null;
        const src = path._p.c;
        for (let i = 0; i < src.length;) {
            const cmd = src[i], n = cmd === 0 || cmd === 1 ? 2 : cmd === 2 ? 4 : cmd === 3 ? 6 : 0;
            this._p.c.push(cmd);
            for (let k = 0; k < n; k += 2) {
                const x = src[i + 1 + k], y = src[i + 2 + k];
                this._p.c.push(mm ? mm[0] * x + mm[2] * y + mm[4] : x, mm ? mm[1] * x + mm[3] * y + mm[5] : y);
            }
            i += 1 + n;
        }
        this._p.hasSub = this._p.hasSub || path._p.hasSub;
    }
}
pathMethods(Path2D.prototype);

// ---- fonts
const FONT_SIZES = { 'xx-small': 9, 'x-small': 10, small: 13, medium: 16, large: 18, 'x-large': 24, 'xx-large': 32, 'xxx-large': 48, smaller: 13, larger: 19 };
function parseFont(s) {
    s = String(s).trim();
    const m = /^((?:(?:italic|oblique|normal|small-caps|bold|bolder|lighter|\d{3}|\d+(?:\.\d+)?%?)\s+)*)(xx-small|x-small|small|medium|large|x-large|xx-large|xxx-large|smaller|larger|-?\d*\.?\d+(?:px|pt|em|rem|%|pc|in|cm|mm|ex|vh|vw))(?:\s*\/\s*[^\s]+)?\s+(.+)$/i.exec(s);
    if (!m) return null;
    const pre = m[1].toLowerCase().split(/\s+/).filter(Boolean);
    let bold = false, italic = false;
    for (const t of pre) {
        if (t === 'italic' || t === 'oblique') italic = true;
        else if (t === 'bold' || t === 'bolder') bold = true;
        else if (/^\d{3}$/.test(t) && +t >= 600) bold = true;
    }
    let px;
    const sz = m[2].toLowerCase();
    if (FONT_SIZES[sz] !== undefined) px = FONT_SIZES[sz];
    else {
        const v = parseFloat(sz);
        px = /pt$/.test(sz) ? v * 4 / 3 : /pc$/.test(sz) ? v * 16 : /in$/.test(sz) ? v * 96 : /cm$/.test(sz) ? v * 96 / 2.54 : /mm$/.test(sz) ? v * 96 / 25.4 :
            /rem$|em$/.test(sz) ? v * 16 : /ex$/.test(sz) ? v * 8 : /%$/.test(sz) ? v * 0.16 : v;
    }
    if (!(px > 0)) return null;
    const fam = m[3].toLowerCase();
    const family = /mono|courier|consolas|menlo|lucida console/.test(fam) ? 2 : (/serif/.test(fam) && !/sans/.test(fam)) || /times|georgia|garamond|palatino|cambria|book/.test(fam) ? 1 : 0;
    return { text: s, family, bold, italic, px };
}

class TextMetrics {
    constructor(o) { Object.assign(this, o); }
}

// ---- the context
const CTX_BY_CANVAS = new WeakMap();
const CV_IDS = new WeakMap();           // canvas / OffscreenCanvas / ImageBitmap -> native canvas id

function newState() {
    return { m: IDENT.slice(), fill: { c: 0xFF000000, s: '#000000' }, stroke: { c: 0xFF000000, s: '#000000' }, lw: 1, cap: 0, join: 0, miter: 10,
        dash: [], dashOff: 0, alpha: 1, op: 0, opName: 'source-over', sc: 0, sblur: 0, sx: 0, sy: 0, font: parseFont('10px sans-serif'),
        align: 'start', baseline: 'alphabetic', dir: 'ltr', smooth: true, quality: 'low', filter: 'none', fontKerning: 'auto', letterSpacing: '0px',
        textRendering: 'auto', wordSpacing: '0px', fontStretch: 'normal', fontVariantCaps: 'normal', textAlign2: '' };
}

class CanvasRenderingContext2D {
    constructor(canvas, id, attrs) {
        hidden(this, '_id', id);
        hidden(this, '_cv', canvas);
        hidden(this, '_s', newState());
        hidden(this, '_stack', []);
        hidden(this, '_p', new PathBuilder());
        hidden(this, '_attrs', attrs || {});
        hidden(this, '_comp', new Float64Array(6));
    }
    _mat() { return this._s.m; }
    get canvas() { return this._cv; }
    getContextAttributes() { return { alpha: true, colorSpace: 'srgb', desynchronized: false, willReadFrequently: !!this._attrs.willReadFrequently }; }
    // ---- state
    save() { this._stack.push(Object.assign({}, this._s, { m: this._s.m.slice(), dash: this._s.dash.slice() })); W.cvSave(this._id); }
    restore() { if (this._stack.length) { this._s = this._stack.pop(); W.cvRestore(this._id); } }
    reset() { this._stack.length = 0; this._s = newState(); this._p = new PathBuilder(); W.cvResize(this._id, W.cvSize(this._id, 0), W.cvSize(this._id, 1)); }
    isContextLost() { return false; }
    // ---- transform
    getTransform() { return new DOMMatrix(this._s.m); }
    setTransform(a, b, c, d, e, f) {
        let m;
        if (a !== null && typeof a === 'object') m = new DOMMatrix(a)._m;
        else { if (arguments.length < 6) throw new TypeError("Failed to execute 'setTransform' on 'CanvasRenderingContext2D': 6 arguments required, but only " + arguments.length + ' present.'); m = [+a, +b, +c, +d, +e, +f]; }
        if (finite(...m)) this._s.m = m;
    }
    resetTransform() { this._s.m = IDENT.slice(); }
    transform(a, b, c, d, e, f) {
        const m = [+a, +b, +c, +d, +e, +f];
        if (finite(...m)) this._s.m = mmul(this._s.m, m);
    }
    translate(x, y) { if (finite(+x, +y)) this._s.m = mmul(this._s.m, [1, 0, 0, 1, +x, +y]); }
    scale(x, y) { if (finite(+x, +y)) this._s.m = mmul(this._s.m, [+x, 0, 0, +y, 0, 0]); }
    rotate(a) { a = +a; if (finite(a)) { const c = Math.cos(a), s = Math.sin(a); this._s.m = mmul(this._s.m, [c, s, -s, c, 0, 0]); } }
    // ---- styles
    get fillStyle() { const f = this._s.fill; return f.obj || f.s; }
    set fillStyle(v) { this._style(this._s, 'fill', v); }
    get strokeStyle() { const f = this._s.stroke; return f.obj || f.s; }
    set strokeStyle(v) { this._style(this._s, 'stroke', v); }
    _style(s, which, v) {
        if (v instanceof CanvasGradient || v instanceof CanvasPattern) { s[which] = { obj: v }; return; }
        const c = parseColor(typeof v === 'string' ? v : String(v), this._cv);
        if (c === null) return;
        s[which] = { c, s: colorString(c) };
    }
    get lineWidth() { return this._s.lw; }
    set lineWidth(v) { v = +v; if (v > 0 && isFinite(v)) this._s.lw = v; }
    get lineCap() { return ['butt', 'round', 'square'][this._s.cap]; }
    set lineCap(v) { const i = ['butt', 'round', 'square'].indexOf(String(v)); if (i >= 0) this._s.cap = i; }
    get lineJoin() { return ['miter', 'round', 'bevel'][this._s.join]; }
    set lineJoin(v) { const i = ['miter', 'round', 'bevel'].indexOf(String(v)); if (i >= 0) this._s.join = i; }
    get miterLimit() { return this._s.miter; }
    set miterLimit(v) { v = +v; if (v > 0 && isFinite(v)) this._s.miter = v; }
    getLineDash() { return this._s.dash.slice(); }
    setLineDash(a) {
        a = Array.from(a || [], Number);
        for (const v of a) if (!(v >= 0) || !isFinite(v)) return;
        if (a.length & 1) a = a.concat(a);
        this._s.dash = a;
    }
    get lineDashOffset() { return this._s.dashOff; }
    set lineDashOffset(v) { v = +v; if (isFinite(v)) this._s.dashOff = v; }
    get globalAlpha() { return this._s.alpha; }
    set globalAlpha(v) { v = +v; if (v >= 0 && v <= 1) this._s.alpha = v; }
    get globalCompositeOperation() { return this._s.opName; }
    set globalCompositeOperation(v) { const i = CV_OPS.indexOf(String(v)); if (i >= 0) { this._s.op = i; this._s.opName = CV_OPS[i]; } }
    get shadowColor() { return this._s.sc ? colorString(this._s.sc) : 'rgba(0, 0, 0, 0)'; }
    set shadowColor(v) { const c = parseColor(String(v), this._cv); if (c !== null) this._s.sc = c; }
    get shadowBlur() { return this._s.sblur; }
    set shadowBlur(v) { v = +v; if (v >= 0 && isFinite(v)) this._s.sblur = v; }
    get shadowOffsetX() { return this._s.sx; }
    set shadowOffsetX(v) { v = +v; if (isFinite(v)) this._s.sx = v; }
    get shadowOffsetY() { return this._s.sy; }
    set shadowOffsetY(v) { v = +v; if (isFinite(v)) this._s.sy = v; }
    get imageSmoothingEnabled() { return this._s.smooth; }
    set imageSmoothingEnabled(v) { this._s.smooth = !!v; }
    get imageSmoothingQuality() { return this._s.quality; }
    set imageSmoothingQuality(v) { if (['low', 'medium', 'high'].includes(String(v))) this._s.quality = String(v); }
    get font() { return this._s.font.text; }
    set font(v) { const f = parseFont(v); if (f) this._s.font = f; }
    get textAlign() { return this._s.align; }
    set textAlign(v) { if (['start', 'end', 'left', 'right', 'center'].includes(String(v))) this._s.align = String(v); }
    get textBaseline() { return this._s.baseline; }
    set textBaseline(v) { if (['top', 'hanging', 'middle', 'alphabetic', 'ideographic', 'bottom'].includes(String(v))) this._s.baseline = String(v); }
    get direction() { return this._s.dir; }
    set direction(v) { if (['ltr', 'rtl', 'inherit'].includes(String(v))) this._s.dir = String(v); }
    get filter() { return this._s.filter; }
    set filter(v) { this._s.filter = String(v); }
    get fontKerning() { return this._s.fontKerning; } set fontKerning(v) { this._s.fontKerning = String(v); }
    get letterSpacing() { return this._s.letterSpacing; } set letterSpacing(v) { this._s.letterSpacing = String(v); }
    get wordSpacing() { return this._s.wordSpacing; } set wordSpacing(v) { this._s.wordSpacing = String(v); }
    get textRendering() { return this._s.textRendering; } set textRendering(v) { this._s.textRendering = String(v); }
    get fontStretch() { return this._s.fontStretch; } set fontStretch(v) { this._s.fontStretch = String(v); }
    get fontVariantCaps() { return this._s.fontVariantCaps; } set fontVariantCaps(v) { this._s.fontVariantCaps = String(v); }
    createLinearGradient(x0, y0, x1, y1) { const v = [+x0, +y0, +x1, +y1]; if (!finite(...v)) throw new TypeError('The provided double value is non-finite.'); return new CanvasGradient(1, v); }
    createRadialGradient(x0, y0, r0, x1, y1, r1) {
        const v = [+x0, +y0, +r0, +x1, +y1, +r1];
        if (!finite(...v)) throw new TypeError('The provided double value is non-finite.');
        if (v[2] < 0 || v[5] < 0) throw new DOMException("Failed to execute 'createRadialGradient' on 'CanvasRenderingContext2D': The radius provided is negative.", 'IndexSizeError');
        return new CanvasGradient(2, v);
    }
    createConicGradient(a, x, y) { const v = [+a, +x, +y]; if (!finite(...v)) throw new TypeError('The provided double value is non-finite.'); return new CanvasGradient(3, v); }
    createPattern(image, rep) {
        const src = canvasSource(image);
        if (!src) throw new TypeError("Failed to execute 'createPattern' on 'CanvasRenderingContext2D': The provided value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement or HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or VideoFrame)'.");
        rep = rep === null || rep === undefined || rep === '' ? 'repeat' : String(rep);
        if (!['repeat', 'repeat-x', 'repeat-y', 'no-repeat'].includes(rep)) throw new DOMException("Failed to execute 'createPattern' on 'CanvasRenderingContext2D': The provided type ('" + rep + "') is not recognized.", 'SyntaxError');
        // a canvas source is snapshotted: the pattern keeps showing what it showed now
        let snap = src;
        if (src.kind === 2) { const id = W.cvNew(src.w, src.h); if (id) { W.cvDrawImage(id, 2, src.id, 0, 0, src.w, src.h, 0, 0, src.w, src.h, new Float64Array(IDENT), new Float64Array([9, 1, 0, 0, 0, 0]), 0); snap = { kind: 2, id, w: src.w, h: src.h, own: true }; } }
        return new CanvasPattern(snap, rep === 'repeat' || rep === 'repeat-x', rep === 'repeat' || rep === 'repeat-y');
    }
    // ---- drawing helpers
    _paintArr(st, inv) {
        const o = st.obj;
        if (!o) { const a = new Float64Array(14); a[0] = 0; a[1] = st.c; a[8] = 1; a[11] = 1; return a; }
        if (o instanceof CanvasPattern) {
            const a = new Float64Array(14);
            let m = inv;
            if (o._m) { const pm = minv(o._m); if (pm) m = mmul(pm, inv); }
            a[0] = 4; a[2] = o._src.kind; a[3] = o._src.id; a[4] = o._rx ? 1 : 0; a[5] = o._ry ? 1 : 0; a[6] = this._s.smooth ? 1 : 0;
            for (let i = 0; i < 6; i++) a[8 + i] = m[i];
            return a;
        }
        const g = o._g, stops = o._stops;
        const a = new Float64Array(15 + stops.length * 2);
        a[0] = o._kind;
        for (let i = 0; i < g.length; i++) a[2 + i] = g[i];
        for (let i = 0; i < 6; i++) a[8 + i] = inv[i];
        a[14] = stops.length;
        for (let i = 0; i < stops.length; i++) { a[15 + 2 * i] = stops[i][0]; a[16 + 2 * i] = stops[i][1]; }
        return a;
    }
    _compArr(opOverride) {
        const s = this._s, c = this._comp;
        c[0] = opOverride === undefined ? s.op : opOverride; c[1] = s.alpha;
        const shadow = (s.sc >>> 24) !== 0 && (s.sx || s.sy || s.sblur);
        c[2] = shadow ? s.sc : 0; c[3] = s.sblur; c[4] = s.sx; c[5] = s.sy;
        return c;
    }
    _fillPath(pathArr, rule, styleKey) {
        const s = this._s, inv = minv(s.m);
        if (!inv || !pathArr.length) return;
        W.cvFill(this._id, new Float64Array(pathArr), rule === 'evenodd' ? 1 : 0, this._paintArr(s[styleKey], inv), this._compArr());
    }
    _strokeArr() {
        const s = this._s, m = s.m, sc = Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2]));
        const a = [s.lw * sc, s.cap, s.join, s.miter, s.dashOff * sc];
        for (const d of s.dash) a.push(d * sc);
        return new Float64Array(a);
    }
    _strokePath(pathArr) {
        const s = this._s, inv = minv(s.m);
        if (!inv || !pathArr.length) return;
        W.cvStroke(this._id, new Float64Array(pathArr), this._strokeArr(), this._paintArr(s.stroke, inv), this._compArr());
    }
    _rectPath(x, y, w, h) {
        const m = this._s.m, P = (px, py) => [m[0] * px + m[2] * py + m[4], m[1] * px + m[3] * py + m[5]];
        const a = P(x, y), b = P(x + w, y), c = P(x + w, y + h), d = P(x, y + h);
        return [0, a[0], a[1], 1, b[0], b[1], 1, c[0], c[1], 1, d[0], d[1], 4];
    }
    _userPath(p) {               // a Path2D transformed by the current matrix
        const m = this._s.m, src = p._p.c, out = new Array(src.length);
        for (let i = 0; i < src.length;) {
            const cmd = src[i], n = cmd === 0 || cmd === 1 ? 2 : cmd === 2 ? 4 : cmd === 3 ? 6 : 0;
            out[i] = cmd;
            for (let k = 0; k < n; k += 2) { const x = src[i + 1 + k], y = src[i + 2 + k]; out[i + 1 + k] = m[0] * x + m[2] * y + m[4]; out[i + 2 + k] = m[1] * x + m[3] * y + m[5]; }
            i += 1 + n;
        }
        return out;
    }
    // ---- drawing
    fillRect(x, y, w, h) { x = +x; y = +y; w = +w; h = +h; if (!finite(x, y, w, h) || !w || !h) return; this._fillPath(this._rectPath(x, y, w, h), 'nonzero', 'fill'); }
    strokeRect(x, y, w, h) {
        x = +x; y = +y; w = +w; h = +h;
        if (!finite(x, y, w, h) || (!w && !h)) return;
        const p = this._rectPath(x, y, w, h);
        this._strokePath(p);
    }
    clearRect(x, y, w, h) {
        x = +x; y = +y; w = +w; h = +h;
        if (!finite(x, y, w, h) || !w || !h) return;
        const s = this._s, inv = minv(s.m);
        if (!inv) return;
        const c = this._comp; c[0] = CV_CLEAR; c[1] = 1; c[2] = 0; c[3] = 0; c[4] = 0; c[5] = 0;
        const a = new Float64Array(14); a[1] = 0xFF000000; a[8] = 1; a[11] = 1;
        W.cvFill(this._id, new Float64Array(this._rectPath(x, y, w, h)), 0, a, c);
    }
    beginPath() { this._p = new PathBuilder(); }
    fill(a, b) {
        if (a instanceof Path2D) this._fillPath(this._userPath(a), b === 'evenodd' ? 'evenodd' : 'nonzero', 'fill');
        else this._fillPath(this._p.c, a === 'evenodd' ? 'evenodd' : 'nonzero', 'fill');
    }
    stroke(p) { this._strokePath(p instanceof Path2D ? this._userPath(p) : this._p.c); }
    clip(a, b) {
        let path, rule;
        if (a instanceof Path2D) { path = this._userPath(a); rule = b; } else { path = this._p.c; rule = a; }
        W.cvClip(this._id, new Float64Array(path), rule === 'evenodd' ? 1 : 0);
    }
    isPointInPath(a, b, c, d) {
        let path, x, y, rule;
        if (a instanceof Path2D) { path = this._userPath(a); x = b; y = c; rule = d; } else { path = this._p.c; x = a; y = b; rule = c; }
        const m = this._s.m;
        x = +x; y = +y;
        const X = m[0] * x + m[2] * y + m[4], Y = m[1] * x + m[3] * y + m[5];
        return !!W.cvPointInPath(new Float64Array(path), X, Y, rule === 'evenodd' ? 1 : 0);
    }
    isPointInStroke(a, b, c) {
        let path, x, y;
        if (a instanceof Path2D) { path = this._userPath(a); x = b; y = c; } else { path = this._p.c; x = a; y = b; }
        const m = this._s.m;
        x = +x; y = +y;
        const X = m[0] * x + m[2] * y + m[4], Y = m[1] * x + m[3] * y + m[5];
        return !!W.cvPointInStroke(new Float64Array(path), X, Y, this._strokeArr());
    }
    drawFocusIfNeeded() {}
    scrollPathIntoView() {}
    // ---- images
    drawImage(image, a, b, c, d, e, f, g, h) {
        const src = canvasSource(image);
        if (!src) throw new TypeError("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': The provided value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement or HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or VideoFrame)'.");
        if (src.w === 0 || src.h === 0) { if (src.broken) return; throw new DOMException("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': The image argument is a canvas element with a width or height of 0.", 'InvalidStateError'); }
        let sx = 0, sy = 0, sw = src.w, sh = src.h, dx, dy, dw, dh;
        const n = arguments.length;
        if (n === 3) { dx = +a; dy = +b; dw = sw; dh = sh; }
        else if (n === 5) { dx = +a; dy = +b; dw = +c; dh = +d; }
        else if (n === 9) { sx = +a; sy = +b; sw = +c; sh = +d; dx = +e; dy = +f; dw = +g; dh = +h; }
        else throw new TypeError("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': Valid arities are: [ 3, 5, 9 ], but " + n + ' arguments provided.');
        if (!finite(sx, sy, sw, sh, dx, dy, dw, dh)) return;
        if (!minv(this._s.m)) return;
        W.cvDrawImage(this._id, src.kind, src.id, sx, sy, sw, sh, dx, dy, dw, dh, new Float64Array(this._s.m), this._compArr(), this._s.smooth ? 1 : 0);
    }
    createImageData(a, b, c) {
        if (a instanceof ImageData) return new ImageData(a.width, a.height);
        a = Math.abs(Math.trunc(+a)); b = Math.abs(Math.trunc(+b));
        if (!a || !b) throw new DOMException("Failed to execute 'createImageData' on 'CanvasRenderingContext2D': The source width is 0.", 'IndexSizeError');
        return new ImageData(a, b);
    }
    getImageData(x, y, w, h) {
        x = Math.trunc(+x); y = Math.trunc(+y); w = Math.trunc(+w); h = Math.trunc(+h);
        if (!finite(x, y, w, h)) throw new TypeError("Failed to execute 'getImageData' on 'CanvasRenderingContext2D': The provided double value is non-finite.");
        if (!w || !h) throw new DOMException("Failed to execute 'getImageData' on 'CanvasRenderingContext2D': The source width is 0.", 'IndexSizeError');
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        const img = new ImageData(w, h);
        W.cvGet(this._id, x, y, w, h, img.data);
        return img;
    }
    putImageData(img, dx, dy, ix, iy, iw, ih) {
        if (!(img instanceof ImageData)) throw new TypeError("Failed to execute 'putImageData' on 'CanvasRenderingContext2D': parameter 1 is not of type 'ImageData'.");
        dx = Math.trunc(+dx); dy = Math.trunc(+dy);
        if (!finite(dx, dy)) throw new TypeError('The provided double value is non-finite.');
        let x0 = 0, y0 = 0, w0 = img.width, h0 = img.height;
        if (arguments.length >= 7) {
            x0 = Math.trunc(+ix); y0 = Math.trunc(+iy); w0 = Math.trunc(+iw); h0 = Math.trunc(+ih);
            if (w0 < 0) { x0 += w0; w0 = -w0; }
            if (h0 < 0) { y0 += h0; h0 = -h0; }
        }
        W.cvPut(this._id, dx, dy, img.width, img.height, img.data, x0, y0, w0, h0);
    }
    // ---- text
    _fontArr() { const f = this._s.font; return new Float64Array([f.family, f.bold ? 1 : 0, f.italic ? 1 : 0, f.px]); }
    measureText(text) {
        text = String(text);
        const f = this._s.font, r = W.cvMeasure(text, this._fontArr()) || [0, f.px * 0.8, f.px * 0.2];
        const w = r[0], asc = r[1], desc = r[2];
        const al = this._s.align, off = al === 'center' ? w / 2 : al === 'right' || (al === 'end' && this._s.dir === 'ltr') || (al === 'start' && this._s.dir === 'rtl') ? w : 0;
        const bo = this._baselineOffset(asc, desc);
        return new TextMetrics({ width: w, actualBoundingBoxLeft: off, actualBoundingBoxRight: w - off, actualBoundingBoxAscent: asc * 0.9 + bo, actualBoundingBoxDescent: desc * 0.5 - bo,
            fontBoundingBoxAscent: asc + bo, fontBoundingBoxDescent: desc - bo, emHeightAscent: asc + bo, emHeightDescent: desc - bo, hangingBaseline: asc * 0.8 + bo, alphabeticBaseline: bo, ideographicBaseline: -desc + bo });
    }
    _baselineOffset(asc, desc) {
        switch (this._s.baseline) {
            case 'top': return asc;
            case 'hanging': return asc * 0.8;
            case 'middle': return (asc - desc) / 2;
            case 'bottom': case 'ideographic': return -desc;
            default: return 0;
        }
    }
    _text(text, x, y, maxWidth, styleKey) {
        text = String(text);
        x = +x; y = +y;
        if (!finite(x, y) || !text) return;
        const s = this._s, inv = minv(s.m);
        if (!inv) return;
        if (maxWidth !== undefined) { maxWidth = +maxWidth; if (!(maxWidth > 0)) return; }
        const fa = this._fontArr(), r = W.cvMeasure(text, fa);
        if (!r) return;
        let w = r[0];
        let m = s.m;
        const al = s.align, left = al === 'left' || (al === 'start' && s.dir !== 'rtl') || (al === 'end' && s.dir === 'rtl');
        const center = al === 'center';
        let sc = 1;
        if (maxWidth !== undefined && w > maxWidth) sc = maxWidth / w;
        const ox = center ? -w * sc / 2 : left ? 0 : -w * sc;
        const oy = this._baselineOffset(r[1], r[2]);
        // text lives in the plane scaled horizontally by sc and translated so (x, y) + (ox, oy) is the origin
        const t = mmul(m, [sc, 0, 0, 1, x + ox, y + oy]);
        W.cvText(this._id, text, fa, new Float64Array(t), 0, 0, this._paintArr(s[styleKey], inv), this._compArr());
    }
    fillText(text, x, y, maxWidth) { this._text(text, x, y, maxWidth, 'fill'); }
    strokeText(text, x, y, maxWidth) { this._text(text, x, y, maxWidth, 'stroke'); }
}
pathMethods(CanvasRenderingContext2D.prototype);
hidden(CanvasRenderingContext2D.prototype, 'createPath', undefined);

class ImageData {
    constructor(a, b, c, d) {
        let w, h, data;
        if (a instanceof Uint8ClampedArray) {
            data = a; w = Math.trunc(+b);
            if (!(w > 0)) throw new DOMException("Failed to construct 'ImageData': The source width is zero or not a number.", 'IndexSizeError');
            if (data.length % 4) throw new DOMException("Failed to construct 'ImageData': The input data length is not a multiple of 4.", 'InvalidStateError');
            if ((data.length / 4) % w) throw new DOMException("Failed to construct 'ImageData': The input data length is not a multiple of (4 * width).", 'IndexSizeError');
            h = c === undefined ? data.length / 4 / w : Math.trunc(+c);
            if (h * w * 4 !== data.length) throw new DOMException("Failed to construct 'ImageData': The input data byte length is not equal to (4 * width * height).", 'IndexSizeError');
        } else {
            w = Math.trunc(+a); h = Math.trunc(+b);
            if (!(w > 0)) throw new DOMException("Failed to construct 'ImageData': The source width is zero or not a number.", 'IndexSizeError');
            if (!(h > 0)) throw new DOMException("Failed to construct 'ImageData': The source height is zero or not a number.", 'IndexSizeError');
            if (w * h > 64 * 1024 * 1024) throw new RangeError("Failed to construct 'ImageData': The requested image size exceeds the supported range.");
            data = new Uint8ClampedArray(w * h * 4);
        }
        Object.defineProperty(this, 'width', { value: w, enumerable: true });
        Object.defineProperty(this, 'height', { value: h, enumerable: true });
        Object.defineProperty(this, 'data', { value: data, enumerable: true });
        Object.defineProperty(this, 'colorSpace', { value: 'srgb', enumerable: true });
    }
}

// ---- sources of drawImage / createPattern
function canvasSource(v) {
    if (v instanceof HTMLImageElement) {
        const s = W.imgSize(v);
        if (!s || !s[0] || s[2] === 2) return { kind: 1, id: -1, w: 0, h: 0, broken: true };
        return { kind: 1, id: W.nodeIndex(v), w: s[0], h: s[1] };
    }
    if (v instanceof HTMLCanvasElement) {
        let id = CV_IDS.get(v);
        if (!id) {                          // never drawn on: an empty bitmap of the element's size
            const w = parseInt(W.getAttr(v, 'width'), 10), h = parseInt(W.getAttr(v, 'height'), 10);
            id = W.cvNew(isNaN(w) ? 300 : w, isNaN(h) ? 150 : h);
            if (!id) return null;
            CV_IDS.set(v, id);
        }
        return { kind: 2, id, w: W.cvSize(id, 0), h: W.cvSize(id, 1) };
    }
    if (v instanceof OffscreenCanvas || v instanceof ImageBitmap) {
        const id = CV_IDS.get(v);
        return id ? { kind: 2, id, w: W.cvSize(id, 0), h: W.cvSize(id, 1) } : { kind: 2, id: 0, w: 0, h: 0, broken: true };
    }
    if (v instanceof HTMLVideoElement) {
        const st = MS.get(v);
        if (!st || !st.id || !st.vw) return { kind: 3, id: 0, w: 0, h: 0, broken: true };
        return { kind: 3, id: st.id, w: st.vw, h: st.vh };
    }
    return null;
}

class ImageBitmap {
    constructor() { illegal(); }
    close() { const id = CV_IDS.get(this); if (id) { W.cvFree(id); CV_IDS.delete(this); } hidden(this, 'width', 0); hidden(this, 'height', 0); }
}
function makeBitmap(w, h) {
    const b = Object.create(ImageBitmap.prototype);
    const id = W.cvNew(w, h);
    if (!id) return null;
    CV_IDS.set(b, id);
    Object.defineProperty(b, 'width', { value: w, configurable: true, enumerable: true });
    Object.defineProperty(b, 'height', { value: h, configurable: true, enumerable: true });
    return b;
}
function createImageBitmap(src, a, b, c, d) {
    return new Promise((resolve, reject) => {
        let sx = 0, sy = 0, sw, sh;
        if (src instanceof ImageData) {
            sw = src.width; sh = src.height;
            if (typeof a === 'number' && arguments.length >= 5) { sx = a; sy = b; sw = c; sh = d; }
            const bm = makeBitmap(Math.abs(sw), Math.abs(sh));
            if (!bm) return reject(new DOMException('Out of memory', 'InvalidStateError'));
            W.cvPut(CV_IDS.get(bm), -sx, -sy, src.width, src.height, src.data, 0, 0, src.width, src.height);
            return resolve(bm);
        }
        const cs = canvasSource(src);
        if (!cs || !cs.w || !cs.h) return reject(new DOMException('The source image could not be decoded.', 'InvalidStateError'));
        sw = cs.w; sh = cs.h;
        if (typeof a === 'number' && arguments.length >= 5) { sx = a; sy = b; sw = c; sh = d; }
        const bm = makeBitmap(Math.abs(sw), Math.abs(sh));
        if (!bm) return reject(new DOMException('Out of memory', 'InvalidStateError'));
        W.cvDrawImage(CV_IDS.get(bm), cs.kind, cs.id, sx, sy, sw, sh, 0, 0, Math.abs(sw), Math.abs(sh), new Float64Array(IDENT), new Float64Array([0, 1, 0, 0, 0, 0]), 1);
        resolve(bm);
    });
}

function dataUrlToBlob(url, type) {
    const bin = atob(url.slice(url.indexOf(',') + 1));
    return new Blob([bin], { type: type || 'image/png' });
}

// ---- the canvas element
function canvasId(el) { return CV_IDS.get(el) || 0; }
function attrSize(el, name, dflt) { const v = parseInt(W.getAttr(el, name), 10); return v >= 0 ? v : dflt; }
function resizeCanvas(el, w, h) {
    const id = CV_IDS.get(el);
    if (id) {
        W.cvResize(id, w, h);
        const ctx = CTX_BY_CANVAS.get(el);
        if (ctx) { ctx._s = newState(); ctx._stack.length = 0; ctx._p = new PathBuilder(); }
    }
}
accessor(HC, 'width', function () { return attrSize(this, 'width', 300); }, function (v) {
    v = Math.trunc(+v) || 0; if (v < 0) v = 0;
    setAttr(this, 'width', String(v));
    resizeCanvas(this, v, attrSize(this, 'height', 150));
});
accessor(HC, 'height', function () { return attrSize(this, 'height', 150); }, function (v) {
    v = Math.trunc(+v) || 0; if (v < 0) v = 0;
    setAttr(this, 'height', String(v));
    resizeCanvas(this, attrSize(this, 'width', 300), v);
});
methods(HC, {
    getContext(type, attrs) {
        type = String(type);
        if (type !== '2d') return null;
        let ctx = CTX_BY_CANVAS.get(this);
        if (ctx) return ctx;
        let id = CV_IDS.get(this);
        const w = attrSize(this, 'width', 300), h = attrSize(this, 'height', 150);
        if (!id) { id = W.cvNew(w, h); if (!id) return null; CV_IDS.set(this, id); }
        else W.cvResize(id, w, h);
        W.cvBind(id, this);
        ctx = new CanvasRenderingContext2D(this, id, attrs);
        CTX_BY_CANVAS.set(this, ctx);
        return ctx;
    },
    toDataURL(type) { const id = CV_IDS.get(this); if (!id) { const w = attrSize(this, 'width', 300), h = attrSize(this, 'height', 150); if (!w || !h) return 'data:,'; const t = W.cvNew(w, h); const u = W.cvPng(t); W.cvFree(t); return u; } return W.cvSize(id, 0) && W.cvSize(id, 1) ? W.cvPng(id) : 'data:,'; },
    toBlob(cb, type) { const url = this.toDataURL(type); setTimeout(() => cb(url === 'data:,' ? null : dataUrlToBlob(url, 'image/png')), 0); },
    captureStream() { throw new DOMException('captureStream is not supported', 'NotSupportedError'); },
    transferControlToOffscreen() { throw new DOMException('Cannot transfer control', 'InvalidStateError'); }
});

class OffscreenCanvas extends EventTarget {
    constructor(w, h) {
        super();
        w = Math.trunc(+w) || 0; h = Math.trunc(+h) || 0;
        const id = W.cvNew(w, h);
        if (!id) throw new DOMException('Out of memory', 'InvalidStateError');
        CV_IDS.set(this, id);
        hidden(this, '_w', w); hidden(this, '_h', h);
    }
    get width() { return this._w; }
    set width(v) { this._w = Math.trunc(+v) || 0; W.cvResize(CV_IDS.get(this), this._w, this._h); const c = CTX_BY_CANVAS.get(this); if (c) { c._s = newState(); c._stack.length = 0; c._p = new PathBuilder(); } }
    get height() { return this._h; }
    set height(v) { this._h = Math.trunc(+v) || 0; W.cvResize(CV_IDS.get(this), this._w, this._h); const c = CTX_BY_CANVAS.get(this); if (c) { c._s = newState(); c._stack.length = 0; c._p = new PathBuilder(); } }
    getContext(type, attrs) {
        if (String(type) !== '2d') return null;
        let ctx = CTX_BY_CANVAS.get(this);
        if (!ctx) { ctx = new CanvasRenderingContext2D(this, CV_IDS.get(this), attrs); CTX_BY_CANVAS.set(this, ctx); }
        return ctx;
    }
    convertToBlob(opts) { return Promise.resolve(dataUrlToBlob(W.cvPng(CV_IDS.get(this)), 'image/png')); }
    transferToImageBitmap() {
        const bm = makeBitmap(this._w, this._h);
        W.cvDrawImage(CV_IDS.get(bm), 2, CV_IDS.get(this), 0, 0, this._w, this._h, 0, 0, this._w, this._h, new Float64Array(IDENT), new Float64Array([0, 1, 0, 0, 0, 0]), 0);
        return bm;
    }
}
reflectInt(HTMLOListElement.prototype, 'start', 'start', 1);
reflectBool(HTMLOListElement.prototype, 'reversed', 'reversed');
reflectInt(HTMLLIElement.prototype, 'value', 'value', 0);
reflectURL(HTMLEmbedElement.prototype, 'src', 'src');
reflect(HTMLObjectElement.prototype, 'data', 'data');
reflectURL(HTMLBaseElement.prototype, 'href', 'href');
accessor(HTMLProgressElement.prototype, 'value', function () { return parseFloat(W.getAttr(this, 'value')) || 0; }, function (v) { setAttr(this, 'value', String(v)); });
accessor(HTMLProgressElement.prototype, 'max', function () { return parseFloat(W.getAttr(this, 'max')) || 1; }, function (v) { setAttr(this, 'max', String(v)); });
reflect(HTMLTimeElement.prototype, 'dateTime', 'datetime');
reflect(HTMLDataElement.prototype, 'value', 'value');

// ---------------------------------------------------------------- custom elements
let ceActive = false;
function ceConnect(n) {
    if (W.ntype(n) !== 1) return;
    const walk = el => {
        const def = CE_BY_NAME.get(W.name(el));
        if (def) {
            if (Object.getPrototypeOf(el) !== def.ctor.prototype) ceUpgrade(el, def);
            else if (def.ctor.prototype.connectedCallback) { try { el.connectedCallback(); } catch (e) { report(e, 'connectedCallback'); } }
        }
        for (let c = W.first(el); c; c = W.next(c)) if (W.ntype(c) === 1) walk(c);
    };
    walk(n);
}
function ceDisconnect(n) {
    if (W.ntype(n) !== 1) return;
    const walk = el => {
        const def = CE_BY_NAME.get(W.name(el));
        if (def && Object.getPrototypeOf(el) === def.ctor.prototype && def.ctor.prototype.disconnectedCallback)
            try { el.disconnectedCallback(); } catch (e) { report(e, 'disconnectedCallback'); }
        for (let c = W.first(el); c; c = W.next(c)) if (W.ntype(c) === 1) walk(c);
    };
    walk(n);
}
function ceUpgrade(el, def) {
    if (Object.getPrototypeOf(el) === def.ctor.prototype) return;
    ceUpgrading = el;
    try { Reflect.construct(def.ctor, [], def.ctor); }
    catch (e) { report(e, 'custom element constructor <' + def.name + '>'); Object.setPrototypeOf(el, def.ctor.prototype); }
    ceUpgrading = null;
    const P = def.ctor.prototype;
    if (P.attributeChangedCallback && def.observed.length)
        for (const a of def.observed) { const v = W.getAttr(el, a); if (v !== null) try { el.attributeChangedCallback(a, null, v); } catch (e) { report(e, 'attributeChangedCallback'); } }
    if (W.connected(el) && P.connectedCallback) try { el.connectedCallback(); } catch (e) { report(e, 'connectedCallback'); }
}
function ceUpgradeTree(n, connected) {
    if (W.ntype(n) !== 1) return;
    const def = CE_BY_NAME.get(W.name(n));
    if (def) ceUpgrade(n, def);
    for (let c = W.first(n); c; c = W.next(c)) ceUpgradeTree(c, connected);
}
function ceAttrChanged(el, name, old, value) {
    const def = CE_BY_NAME.get(W.name(el));
    if (!def || Object.getPrototypeOf(el) !== def.ctor.prototype || !def.ctor.prototype.attributeChangedCallback) return;
    if (!def.observed.includes(name)) return;
    try { el.attributeChangedCallback(name, old, value); } catch (e) { report(e, 'attributeChangedCallback'); }
}
class CustomElementRegistry {
    define(name, ctor, opts) {
        name = String(name);
        if (!/^[a-z][a-z0-9._·À-￿]*-[a-z0-9._\-·À-￿]*$/.test(name)) throw new DOMException('"' + name + '" is not a valid custom element name', 'SyntaxError');
        if (CE_BY_NAME.has(name)) throw new DOMException('"' + name + '" has already been defined', 'NotSupportedError');
        if (typeof ctor !== 'function') throw new TypeError('constructor required');
        let observed = [];
        try { observed = Array.from(ctor.observedAttributes || [], String); } catch (e) {}
        const def = { name, ctor, observed };
        CE_BY_NAME.set(name, def);
        CE_BY_CTOR.set(ctor, name);
        ceActive = true;
        for (const el of W.query(document, cssEscape(name), true)) ceUpgrade(el, def);
        const w = CE_WAIT.get(name);
        if (w) { CE_WAIT.delete(name); w.resolve(ctor); }
    }
    get(name) { const d = CE_BY_NAME.get(String(name)); return d ? d.ctor : undefined; }
    getName(ctor) { return CE_BY_CTOR.get(ctor) || null; }
    whenDefined(name) {
        name = String(name);
        const d = CE_BY_NAME.get(name);
        if (d) return Promise.resolve(d.ctor);
        let w = CE_WAIT.get(name);
        if (!w) { let resolve; const p = new Promise(r => { resolve = r; }); w = { p, resolve }; CE_WAIT.set(name, w); }
        return w.p;
    }
    upgrade(root) { if (root instanceof Node) ceUpgradeTree(root, W.connected(root)); }
}

// ---------------------------------------------------------------- MutationObserver
let moActive = false;
const OBSERVERS = [];
let moQueued = false;
class MutationRecord {}
function moRecord(type, target, attr, old) {
    for (const o of OBSERVERS) {
        for (const reg of o._targets) {
            const opt = reg.opts;
            if (!(reg.node === target || (opt.subtree && isAncestor(reg.node, target)))) continue;
            if (type === 'attributes' && !opt.attributes) continue;
            if (type === 'attributes' && opt.attributeFilter && !opt.attributeFilter.includes(attr)) continue;
            if (type === 'childList' && !opt.childList) continue;
            if (type === 'characterData' && !opt.characterData) continue;
            const r = Object.assign(new MutationRecord(), { type, target, attributeName: type === 'attributes' ? attr : null,
                attributeNamespace: null, oldValue: (opt.attributeOldValue || opt.characterDataOldValue) ? old : null,
                addedNodes: nodeList([]), removedNodes: nodeList([]), previousSibling: null, nextSibling: null });
            if (type === 'childList') { r.addedNodes = nodeList(attr.added || []); r.removedNodes = nodeList(attr.removed || []); }
            o._records.push(r);
            break;
        }
    }
    if (!moQueued) {
        moQueued = true;
        queueMicrotask(() => {
            moQueued = false;
            for (const o of OBSERVERS.slice()) {
                if (!o._records.length) continue;
                const recs = o._records.splice(0);
                try { o._cb.call(o, recs, o); } catch (e) { report(e, 'MutationObserver callback'); }
            }
        });
    }
}
class MutationObserver {
    constructor(cb) { if (typeof cb !== 'function') throw new TypeError('callback required'); hidden(this, '_cb', cb); hidden(this, '_targets', []); hidden(this, '_records', []); }
    observe(node, opts) {
        opts = Object.assign({}, opts || {});
        if (opts.attributeOldValue || opts.attributeFilter) opts.attributes = true;
        if (opts.characterDataOldValue) opts.characterData = true;
        const ex = this._targets.find(t => t.node === node);
        if (ex) ex.opts = opts; else this._targets.push({ node, opts });
        if (!OBSERVERS.includes(this)) OBSERVERS.push(this);
        moActive = true;
        if (opts.childList) W.setMutationHook(true);
    }
    disconnect() { this._targets.length = 0; const i = OBSERVERS.indexOf(this); if (i >= 0) OBSERVERS.splice(i, 1); }
    takeRecords() { return this._records.splice(0); }
}
// childList records come from the C mutation hook (W.setMutationHook)
function moChildList(parent, added, removed) {
    if (moActive) moRecord('childList', parent, { added, removed }, null);
}

// IntersectionObserverEntry: a real class, as in browsers (pages feature-detect it: YouTube's polyfill replaces the
// observer unless 'intersectionRatio' is on IntersectionObserverEntry.prototype)
class IntersectionObserverEntry {
    constructor(init) { hidden(this, '_e', init || {}); }
}
for (const k of ['time', 'rootBounds', 'boundingClientRect', 'intersectionRect', 'isIntersecting', 'intersectionRatio', 'target'])
    accessor(IntersectionObserverEntry.prototype, k, function () { return this._e[k]; });

// ResizeObserver / IntersectionObserver. The shell wakes observersCheck() whenever a layout finished, the page scrolled or the
// viewport changed (W.obsActive tells it there is something to measure), so targets are re-measured like a browser's
// "update the rendering" step: resize observations first, then intersections.
const RO_LIST = [];
const IO_LIST = [];
function obsSync() { W.obsActive(RO_LIST.length + IO_LIST.length > 0); }
function observersCheck() {
    let ran = false;
    for (const o of RO_LIST.slice()) if (o._check()) ran = true;
    for (const o of IO_LIST.slice()) if (o._check()) ran = true;
    return ran;
}
const pxOf = v => { const f = parseFloat(v); return f === f ? f : 0; };

class ResizeObserver {
    constructor(cb) {
        if (typeof cb !== 'function') throw new TypeError("Failed to construct 'ResizeObserver': The callback provided as parameter 1 is not a function.");
        hidden(this, '_cb', cb); hidden(this, '_t', []);
    }
    observe(el) {
        if (this._t.some(t => t.el === el)) return;
        this._t.push({ el, w: 0, h: 0 });                 // the size last reported starts at 0x0: an element that has a size is reported once
        if (RO_LIST.indexOf(this) < 0) { RO_LIST.push(this); obsSync(); }
    }
    unobserve(el) {
        const i = this._t.findIndex(t => t.el === el);
        if (i >= 0) this._t.splice(i, 1);
        if (!this._t.length) this.disconnect();
    }
    disconnect() { this._t.length = 0; const i = RO_LIST.indexOf(this); if (i >= 0) { RO_LIST.splice(i, 1); obsSync(); } }
    _check() {
        const entries = [];
        for (const t of this._t) {
            const r = rectOf(t.el);
            let cw = r.width, ch = r.height, pl = 0, pt = 0;
            if (r.width > 0 || r.height > 0) {                  // the content box: the border box minus padding and border
                const cs = getComputedStyle(t.el);
                pl = pxOf(cs.paddingLeft) + pxOf(cs.borderLeftWidth); pt = pxOf(cs.paddingTop) + pxOf(cs.borderTopWidth);
                cw = Math.max(0, r.width - pl - pxOf(cs.paddingRight) - pxOf(cs.borderRightWidth));
                ch = Math.max(0, r.height - pt - pxOf(cs.paddingBottom) - pxOf(cs.borderBottomWidth));
            }
            if (cw === t.w && ch === t.h) continue;
            t.w = cw; t.h = ch;
            const cbox = [{ inlineSize: cw, blockSize: ch }], bbox = [{ inlineSize: r.width, blockSize: r.height }];
            entries.push({ target: t.el, contentRect: new DOMRect(pl, pt, cw, ch), borderBoxSize: bbox, contentBoxSize: cbox, devicePixelContentBoxSize: cbox });
        }
        if (!entries.length) return false;
        try { this._cb(entries, this); } catch (e) { report(e, 'ResizeObserver callback'); }
        return true;
    }
}

class IntersectionObserver {
    constructor(cb, opts) {
        if (typeof cb !== 'function') throw new TypeError("Failed to construct 'IntersectionObserver': The callback provided as parameter 1 is not a function.");
        opts = opts || {};
        hidden(this, '_cb', cb); hidden(this, '_t', []); hidden(this, '_q', []);
        this.root = opts.root || null;
        this.rootMargin = opts.rootMargin === undefined ? '0px 0px 0px 0px' : String(opts.rootMargin);
        const th = opts.threshold === undefined ? [0] : Array.isArray(opts.threshold) ? opts.threshold.map(Number) : [Number(opts.threshold)];
        this.thresholds = th.length ? th.sort((a, b) => a - b) : [0];
        const m = this.rootMargin.trim().split(/\s+/).map(pxOf);       // top right bottom left (px only; % is treated as 0)
        hidden(this, '_m', m.length === 1 ? [m[0], m[0], m[0], m[0]] : m.length === 2 ? [m[0], m[1], m[0], m[1]] : m.length === 3 ? [m[0], m[1], m[2], m[1]] : m.slice(0, 4));
    }
    observe(el) {
        if (this._t.some(t => t.el === el)) return;
        this._t.push({ el, idx: -2, inter: false });      // -2: nothing reported yet (the first observation is always delivered)
        if (IO_LIST.indexOf(this) < 0) { IO_LIST.push(this); obsSync(); }
    }
    unobserve(el) {
        const i = this._t.findIndex(t => t.el === el);
        if (i >= 0) this._t.splice(i, 1);
        if (!this._t.length) this.disconnect();
    }
    disconnect() { this._t.length = 0; const i = IO_LIST.indexOf(this); if (i >= 0) { IO_LIST.splice(i, 1); obsSync(); } }
    takeRecords() { return this._q.splice(0); }
    _check() {
        const v = W.view();
        let rx = 0, ry = 0, rw = v[0], rh = v[1];
        if (this.root && this.root.getBoundingClientRect) { const rr = rectOf(this.root); rx = rr.x; ry = rr.y; rw = rr.width; rh = rr.height; }
        const m = this._m;
        const bx = rx - m[3], by = ry - m[0], bw = rw + m[1] + m[3], bh = rh + m[0] + m[2];       // the root rectangle grown by rootMargin
        const entries = [];
        for (const t of this._t) {
            const has = W.rect(t.el) !== null;               // no box (display:none, detached) = never intersecting
            const r = has ? rectOf(t.el) : new DOMRect(0, 0, 0, 0);
            const x0 = Math.max(r.x, bx), y0 = Math.max(r.y, by), x1 = Math.min(r.x + r.width, bx + bw), y1 = Math.min(r.y + r.height, by + bh);
            const inter = has && x0 <= x1 && y0 <= y1;       // touching edges counts, as in the spec
            const area = r.width * r.height;
            const ratio = !inter ? 0 : area > 0 ? Math.min(1, ((x1 - x0) * (y1 - y0)) / area) : 1;
            let idx = -1;
            if (inter) { idx = 0; for (let i = 0; i < this.thresholds.length; i++) if (ratio >= this.thresholds[i]) idx = i + 1; }
            if (idx === t.idx && inter === t.inter) continue;
            t.idx = idx; t.inter = inter;
            entries.push(new IntersectionObserverEntry({ target: t.el, isIntersecting: inter, intersectionRatio: ratio, boundingClientRect: r,
                intersectionRect: inter ? new DOMRect(x0, y0, Math.max(0, x1 - x0), Math.max(0, y1 - y0)) : new DOMRect(0, 0, 0, 0),
                rootBounds: new DOMRect(rx, ry, rw, rh), time: W.now() }));
        }
        if (!entries.length) return false;
        try { this._cb(entries, this); } catch (e) { report(e, 'IntersectionObserver callback'); }
        return true;
    }
}

// ---------------------------------------------------------------- URL
const DEFAULT_PORTS = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21' };
function parseURL(input, base) {
    const abs = W.resolve(String(input), base === undefined ? null : String(base));
    if (abs === null) return null;
    const m = /^([a-zA-Z][a-zA-Z0-9+.\-]*:)(\/\/(?:([^:@\/]*)(?::([^@\/]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(abs);
    if (!m) return null;
    const u = { protocol: m[1].toLowerCase(), username: m[3] || '', password: m[4] || '', hostname: (m[5] || '').toLowerCase(),
        port: m[6] || '', pathname: m[7] || '', search: m[8] || '', hash: m[9] || '', special: !!m[2] };
    if (u.port === DEFAULT_PORTS[u.protocol]) u.port = '';
    if (u.special && !u.pathname) u.pathname = '/';
    if (u.search === '?') u.search = '';
    if (u.hash === '#') u.hash = '';
    return u;
}
const URL_PRIV = new WeakMap();
class URLSearchParams {
    constructor(init) {
        hidden(this, '_list', []);
        hidden(this, '_url', null);
        if (init === undefined || init === null) return;
        if (typeof init === 'object') {
            if (init instanceof URLSearchParams) { for (const [k, v] of init._list) this._list.push([k, v]); return; }
            if (typeof init[Symbol.iterator] === 'function') { for (const p of init) this._list.push([String(p[0]), String(p[1])]); return; }
            for (const k of Object.keys(init)) this._list.push([k, String(init[k])]);
            return;
        }
        this._parse(String(init));
    }
    _parse(s) {
        this._list.length = 0;
        if (s[0] === '?') s = s.slice(1);
        for (const part of s.split('&')) {
            if (!part) continue;
            const i = part.indexOf('=');
            const k = i < 0 ? part : part.slice(0, i), v = i < 0 ? '' : part.slice(i + 1);
            this._list.push([formDecode(k), formDecode(v)]);
        }
    }
    _update() { if (this._url) { const s = this.toString(); URL_PRIV.get(this._url).search = s ? '?' + s : ''; } }
    append(k, v) { this._list.push([String(k), String(v)]); this._update(); }
    delete(k, v) { k = String(k); this._list = this._list.filter(p => p[0] !== k || (v !== undefined && p[1] !== String(v))); this._update(); }
    get(k) { k = String(k); const p = this._list.find(p => p[0] === k); return p ? p[1] : null; }
    getAll(k) { k = String(k); return this._list.filter(p => p[0] === k).map(p => p[1]); }
    has(k, v) { k = String(k); return this._list.some(p => p[0] === k && (v === undefined || p[1] === String(v))); }
    set(k, v) {
        k = String(k); v = String(v);
        const i = this._list.findIndex(p => p[0] === k);
        if (i < 0) this._list.push([k, v]);
        else { this._list[i][1] = v; this._list = this._list.filter((p, j) => j <= i || p[0] !== k); }
        this._update();
    }
    sort() { this._list.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); this._update(); }
    forEach(cb, t) { for (const [k, v] of this._list.slice()) cb.call(t, v, k, this); }
    keys() { return this._list.map(p => p[0])[Symbol.iterator](); }
    values() { return this._list.map(p => p[1])[Symbol.iterator](); }
    entries() { return this._list.map(p => [p[0], p[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get size() { return this._list.length; }
    toString() { return this._list.map(p => formEncode(p[0]) + '=' + formEncode(p[1])).join('&'); }
}
function formEncode(s) { return encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase()); }
function formDecode(s) { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } }
class URL {
    constructor(url, base) {
        if (base !== undefined && base !== null) {
            const b = parseURL(base);
            if (!b) throw new TypeError("Failed to construct 'URL': Invalid base URL");
        }
        const u = parseURL(url, base === null ? undefined : base);
        if (!u) throw new TypeError("Failed to construct 'URL': Invalid URL");
        URL_PRIV.set(this, u);
        const sp = new URLSearchParams(u.search);
        sp._url = this;
        hidden(this, '_sp', sp);
    }
    static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
    static parse(u, b) { try { return new URL(u, b); } catch (e) { return null; } }
    static createObjectURL(obj) { return mediaObjURL(obj); }
    static revokeObjectURL(u) { OBJURLS.delete(String(u)); }
    get href() {
        const u = URL_PRIV.get(this);
        if (!u.special) return u.protocol + u.pathname + u.search + u.hash;
        const auth = u.username ? u.username + (u.password ? ':' + u.password : '') + '@' : '';
        return u.protocol + '//' + auth + this.host + u.pathname + u.search + u.hash;
    }
    set href(v) { const u = parseURL(v); if (!u) throw new TypeError('Invalid URL'); URL_PRIV.set(this, u); this._sp._parse(u.search); }
    get origin() { const u = URL_PRIV.get(this); return u.special && /^(https?|wss?|ftp):$/.test(u.protocol) ? u.protocol + '//' + this.host : 'null'; }
    get protocol() { return URL_PRIV.get(this).protocol; }
    set protocol(v) { v = String(v).replace(/:.*$/, '').toLowerCase() + ':'; URL_PRIV.get(this).protocol = v; }
    get username() { return URL_PRIV.get(this).username; }
    set username(v) { URL_PRIV.get(this).username = String(v); }
    get password() { return URL_PRIV.get(this).password; }
    set password(v) { URL_PRIV.get(this).password = String(v); }
    get host() { const u = URL_PRIV.get(this); return u.hostname + (u.port ? ':' + u.port : ''); }
    set host(v) { const m = /^([^:\/]*)(?::(\d*))?/.exec(String(v)); const u = URL_PRIV.get(this); u.hostname = m[1].toLowerCase(); u.port = m[2] && m[2] !== DEFAULT_PORTS[u.protocol] ? m[2] : ''; }
    get hostname() { return URL_PRIV.get(this).hostname; }
    set hostname(v) { URL_PRIV.get(this).hostname = String(v).toLowerCase(); }
    get port() { return URL_PRIV.get(this).port; }
    set port(v) { const u = URL_PRIV.get(this); v = String(v); u.port = v === DEFAULT_PORTS[u.protocol] ? '' : v.replace(/\D.*$/, ''); }
    get pathname() { return URL_PRIV.get(this).pathname; }
    set pathname(v) { v = String(v); URL_PRIV.get(this).pathname = (v[0] === '/' ? '' : '/') + v.replace(/[ "#<>?`{}]/g, encodeURIComponent); }
    get search() { return URL_PRIV.get(this).search; }
    set search(v) { v = String(v); if (v && v[0] !== '?') v = '?' + v; if (v === '?') v = ''; URL_PRIV.get(this).search = v; this._sp._parse(v); }
    get searchParams() { return this._sp; }
    get hash() { return URL_PRIV.get(this).hash; }
    set hash(v) { v = String(v); if (v && v[0] !== '#') v = '#' + v; if (v === '#') v = ''; URL_PRIV.get(this).hash = v; }
    toString() { return this.href; }
    toJSON() { return this.href; }
}

// ---------------------------------------------------------------- Location / History
function navigate(url, replace) {
    const abs = W.resolve(String(url));
    if (abs === null) throw new DOMException('Invalid URL', 'SyntaxError');
    const cur = W.docURL();
    const strip = s => s.replace(/#.*$/, '');
    if (strip(abs) === strip(cur) && abs.indexOf('#') >= 0) {   // fragment navigation
        W.pushURL(abs, !!replace);
        const frag = abs.slice(abs.indexOf('#') + 1);
        const target = frag && (document.getElementById(decodeURIComponent(frag)) || W.query(document, 'a[name="' + cssEscape(frag) + '"]', false));
        if (target) target.scrollIntoView();
        if (abs !== cur) dispatch(G, new HashChangeEvent('hashchange', { oldURL: cur, newURL: abs }));
        return;
    }
    if (/^javascript:/i.test(abs)) {
        try { (0, eval)(decodeURIComponent(abs.slice(11))); } catch (e) { report(e, 'javascript: URL'); }
        return;
    }
    W.navigate(abs, !!replace);
}
class Location {
    get href() { return W.docURL(); }
    set href(v) { navigate(v, false); }
    get origin() { return new URL(W.docURL()).origin; }
    get ancestorOrigins() { return []; }
    assign(u) { navigate(u, false); }
    replace(u) { navigate(u, true); }
    reload() { W.navigate(W.docURL(), true); }
    toString() { return W.docURL(); }
}
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'])
    accessor(Location.prototype, part, function () { return new URL(W.docURL())[part]; },
        function (v) { const u = new URL(W.docURL()); u[part] = v; navigate(u.href, false); });
const location = new Location();
let historyState = null, historyLen = 1;
class History {
    get length() { return historyLen; }
    get state() { return historyState; }
    get scrollRestoration() { return 'auto'; }
    set scrollRestoration(v) {}
    pushState(state, title, url) {
        historyState = state === undefined ? null : structuredCloneLite(state);
        if (url !== undefined && url !== null) { const abs = W.resolve(String(url)); if (abs !== null) W.pushURL(abs, false); }
        historyLen++;
    }
    replaceState(state, title, url) {
        historyState = state === undefined ? null : structuredCloneLite(state);
        if (url !== undefined && url !== null) { const abs = W.resolve(String(url)); if (abs !== null) W.pushURL(abs, true); }
    }
    back() { W.historyGo(-1); }
    forward() { W.historyGo(1); }
    go(n) { n = n | 0; if (n === 0) location.reload(); else W.historyGo(n); }
}
const history = new History();
function structuredCloneLite(v) {
    if (v === null || typeof v !== 'object') return v;
    try { return JSON.parse(JSON.stringify(v)); } catch (e) { return v; }
}

// ---------------------------------------------------------------- timers
let timerSeq = 1;
const TIMERS = new Map();     // id -> {fn, args, due, interval}
let timerNest = 0;   // nesting level of the timer callback being run
function addTimer(fn, ms, args, repeat) {
    if (typeof fn !== 'function') {
        const code = String(fn);
        fn = () => (0, eval)(code);
    }
    ms = +ms || 0;
    const nest = timerNest + 1;
    if (ms < 4) ms = repeat ? 10 : (nest > 5 ? 4 : 0);      // HTML: timers nested more than 5 deep wait at least 4 ms (a setTimeout(0) chain would spin the CPU)
    const id = timerSeq++;
    TIMERS.set(id, { fn, args, due: W.now() + ms, interval: repeat ? Math.max(ms, 10) : -1, nest });
    return id;
}
function setTimeout(fn, ms, ...args) { return addTimer(fn, ms, args, false); }
function setInterval(fn, ms, ...args) { return addTimer(fn, ms, args, true); }
function clearTimeout(id) { TIMERS.delete(+id); }
let rafSeq = 1;
const RAF = new Map();
function requestAnimationFrame(cb) { const id = rafSeq++; RAF.set(id, cb); return id; }
function cancelAnimationFrame(id) { RAF.delete(+id); }
function requestIdleCallback(cb, opts) {
    return setTimeout(() => cb({ didTimeout: false, timeRemaining: () => 10 }), Math.min(50, (opts && opts.timeout) || 50));
}
// C: next due time (ms) or -1
function nextDue() {
    let due = -1;
    for (const t of TIMERS.values()) if (due < 0 || t.due < due) due = t.due;
    if (RAF.size && (due < 0 || lastFrame + 33 < due)) due = lastFrame + 33;
    return due;
}
// C: run ONE due timer (microtasks drain in between); returns true if one ran
function runDue(now) {
    let best = null, bestId = 0;
    for (const [id, t] of TIMERS) if (t.due <= now && (!best || t.due < best.due)) { best = t; bestId = id; }
    if (!best) return false;
    if (best.interval >= 0) best.due = now + best.interval; else TIMERS.delete(bestId);
    const prevNest = timerNest;
    timerNest = best.nest || 0;
    try { best.fn.apply(G, best.args); } catch (e) { report(e, 'timer'); }
    timerNest = prevNest;
    return true;
}
let lastFrame = 0;
// C: run animation frame callbacks (at most ~30/s)
function runFrame(now) {
    if (!RAF.size || now - lastFrame < 33) return false;
    lastFrame = now;
    const cbs = Array.from(RAF.values());
    RAF.clear();
    for (const cb of cbs) try { cb(now); } catch (e) { report(e, 'requestAnimationFrame'); }
    return true;
}

// ---------------------------------------------------------------- fetch / XHR
const PENDING = new Map();    // request id -> handler(status, statusText, headersRaw, body:ArrayBuffer, url)
function startRequest(method, url, headers, body, handler) {
    let bodyStr = null, ctype = null;      // bodyStr: a string, or the bytes of an ArrayBuffer / view
    if (body !== null && body !== undefined) {
        if (typeof body === 'string') { bodyStr = body; ctype = 'text/plain;charset=UTF-8'; }
        else if (body instanceof URLSearchParams) { bodyStr = body.toString(); ctype = 'application/x-www-form-urlencoded;charset=UTF-8'; }
        else if (body instanceof FormData) { bodyStr = body._urlencoded(); ctype = 'application/x-www-form-urlencoded;charset=UTF-8'; }
        else if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) bodyStr = body;
        else if (body instanceof Blob) { bodyStr = body._text(); if (body.type) ctype = body.type; }
        else { bodyStr = String(body); ctype = 'text/plain;charset=UTF-8'; }
    }
    const hs = [];
    let hasType = false;
    for (const [k, v] of headers) { hs.push(k + ': ' + v); if (k.toLowerCase() === 'content-type') hasType = true; }
    if (ctype && !hasType) hs.push('Content-Type: ' + ctype);
    const id = W.fetch(String(url), String(method).toUpperCase(), hs.join('\r\n'), bodyStr);
    if (id < 0) { setTimeout(() => handler(0, '', '', null, url), 0); return; }
    PENDING.set(id, handler);
    if (/\/videoplayback/.test(url)) { PTIME.set(id, W.now()); mlog('request ' + id + ' ' + method + ' videoplayback' + (bodyStr && bodyStr.byteLength !== undefined ? ' body=' + bodyStr.byteLength : '')); }
}
const PTIME = new Map();
// C: a request finished
function onFetch(id, status, statusText, headersRaw, body, url) {
    const h = PENDING.get(id);
    if (PTIME.has(id)) { mlog('response ' + id + ' status=' + status + ' bytes=' + (body ? body.byteLength : -1) + ' in ' + (W.now() - PTIME.get(id)) + 'ms' + (h ? '' : ' (nobody waiting)')); PTIME.delete(id); }
    if (!h) return;
    PENDING.delete(id);
    h(status, statusText, headersRaw, body, url);
}
function parseHeaders(raw) {
    const h = new Headers();
    for (const line of String(raw || '').split(/\r?\n/)) {
        const i = line.indexOf(':');
        if (i > 0) h.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
    }
    return h;
}
class Headers {
    constructor(init) {
        hidden(this, '_m', new Map());
        if (!init) return;
        if (init instanceof Headers) { for (const [k, v] of init) this.append(k, v); return; }
        if (typeof init[Symbol.iterator] === 'function') { for (const p of init) this.append(p[0], p[1]); return; }
        for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { k = String(k).toLowerCase(); v = String(v); const o = this._m.get(k); this._m.set(k, o === undefined ? v : o + ', ' + v); }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
    get(k) { const v = this._m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    delete(k) { this._m.delete(String(k).toLowerCase()); }
    forEach(cb, t) { for (const [k, v] of this) cb.call(t, v, k, this); }
    getSetCookie() { return []; }
    keys() { return Array.from(this._m.keys()).sort()[Symbol.iterator](); }
    values() { return Array.from(this).map(p => p[1])[Symbol.iterator](); }
    entries() { return Array.from(this._m.entries()).sort((a, b) => a[0] < b[0] ? -1 : 1)[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
}
class Blob {
    constructor(parts, opts) {
        let s = '';
        for (const p of parts || []) {
            if (typeof p === 'string') s += p;
            else if (p instanceof Blob) s += p._s;
            else if (p instanceof ArrayBuffer || ArrayBuffer.isView(p)) s += new TextDecoder().decode(p);
            else s += String(p);
        }
        hidden(this, '_s', s);
        this.type = (opts && opts.type) || '';
    }
    get size() { return new TextEncoder().encode(this._s).length; }
    _text() { return this._s; }
    text() { return Promise.resolve(this._s); }
    arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._s).buffer); }
    slice(a, b, t) { return new Blob([this._s.slice(a, b)], { type: t }); }
    stream() { throw new DOMException('streams are not supported', 'NotSupportedError'); }
}
class File extends Blob { constructor(parts, name, opts) { super(parts, opts); this.name = String(name); this.lastModified = Date.now(); } }
class FormData {
    constructor(form) {
        hidden(this, '_l', []);
        if (form instanceof HTMLFormElement) {
            for (const el of W.query(form, 'input,select,textarea', true)) {
                const name = el.getAttribute('name');
                if (!name || el.disabled) continue;
                const t = el.type;
                if ((t === 'checkbox' || t === 'radio') && !el.checked) continue;
                if (t === 'submit' || t === 'button' || t === 'reset' || t === 'image' || t === 'file') continue;
                this._l.push([name, el.value]);
            }
        }
    }
    append(k, v) { this._l.push([String(k), v instanceof Blob ? v : String(v)]); }
    delete(k) { this._l = this._l.filter(p => p[0] !== String(k)); }
    get(k) { const p = this._l.find(p => p[0] === String(k)); return p ? p[1] : null; }
    getAll(k) { return this._l.filter(p => p[0] === String(k)).map(p => p[1]); }
    has(k) { return this._l.some(p => p[0] === String(k)); }
    set(k, v) { this.delete(k); this.append(k, v); }
    forEach(cb, t) { for (const [k, v] of this._l) cb.call(t, v, k, this); }
    entries() { return this._l.map(p => [p[0], p[1]])[Symbol.iterator](); }
    keys() { return this._l.map(p => p[0])[Symbol.iterator](); }
    values() { return this._l.map(p => p[1])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    _urlencoded() { return this._l.map(p => formEncode(p[0]) + '=' + formEncode(typeof p[1] === 'string' ? p[1] : '')).join('&'); }
}
class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(r) { const c = new AbortController(); c.abort(r); return c.signal; }
    static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('signal timed out', 'TimeoutError')), ms); return c.signal; }
    static any(signals) { const c = new AbortController(); for (const s of signals) { if (s.aborted) { c.abort(s.reason); break; } s.addEventListener('abort', () => c.abort(s.reason)); } return c.signal; }
}
class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) {
        const s = this.signal;
        if (s.aborted) return;
        s.aborted = true;
        s.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
        const ev = new Event('abort');
        if (typeof s.onabort === 'function') try { s.onabort(ev); } catch (e) { report(e); }
        dispatch(s, ev);
    }
}
class ReadableStreamDefaultReader {
    constructor(st) {
        if (st._reader) throw new TypeError('ReadableStream is locked');
        hidden(this, '_st', st);
        st._reader = this;
        hidden(this, 'closed', new Promise((res, rej) => { this._cres = res; this._crej = rej; }));
        this.closed.catch(() => {});
        if (st._state === 'closed') this._cres(); else if (st._state === 'errored') this._crej(st._err);
    }
    read() {
        const st = this._st;
        if (!st) return Promise.reject(new TypeError('This readable stream reader has been released'));
        return st._read();
    }
    cancel(reason) { return this._st ? this._st._cancel(reason) : Promise.reject(new TypeError('released')); }
    releaseLock() { if (this._st) { this._st._reader = null; this._st = null; } }
}
class ReadableStream {
    constructor(src, strategy) {
        src = src || {};
        hidden(this, '_q', []); hidden(this, '_waiters', []); hidden(this, '_state', 'readable'); hidden(this, '_err', undefined);
        hidden(this, '_reader', null); hidden(this, '_src', src); hidden(this, '_pulling', false); hidden(this, '_again', false);
        hidden(this, '_hwm', strategy && strategy.highWaterMark !== undefined ? +strategy.highWaterMark : 1);
        const st = this;
        hidden(this, '_ctl', {
            enqueue(chunk) {
                if (st._state !== 'readable') throw new TypeError('The stream is not in a state that permits enqueue');
                const w = st._waiters.shift();
                if (w) w.res({ value: chunk, done: false }); else st._q.push(chunk);
            },
            close() {
                if (st._state !== 'readable') return;
                st._state = 'closed';
                for (const w of st._waiters.splice(0)) w.res({ value: undefined, done: true });
                if (st._reader) st._reader._cres();
            },
            error(e) { st._fail(e); },
            get desiredSize() { return st._state === 'readable' ? st._hwm - st._q.length : st._state === 'closed' ? 0 : null; },
        });
        let started;
        try { started = src.start ? src.start(this._ctl) : undefined; } catch (e) { this._fail(e); return; }
        Promise.resolve(started).then(() => this._pull(), e => this._fail(e));
    }
    _fail(e) {
        if (this._state !== 'readable') return;
        this._state = 'errored'; this._err = e; this._q.length = 0;
        for (const w of this._waiters.splice(0)) w.rej(e);
        if (this._reader) this._reader._crej(e);
    }
    _pull() {
        if (this._state !== 'readable' || !this._src.pull) return;
        if (this._pulling) { this._again = true; return; }
        if (this._q.length >= this._hwm && !this._waiters.length) return;
        this._pulling = true;
        let r;
        try { r = this._src.pull(this._ctl); } catch (e) { this._pulling = false; this._fail(e); return; }
        Promise.resolve(r).then(() => {
            this._pulling = false;
            if (this._again) { this._again = false; this._pull(); }
            else if (this._waiters.length || this._q.length < this._hwm) this._pull();
        }, e => { this._pulling = false; this._fail(e); });
    }
    _read() {
        if (this._q.length) {
            const v = this._q.shift();
            this._pull();
            return Promise.resolve({ value: v, done: false });
        }
        if (this._state === 'closed') return Promise.resolve({ value: undefined, done: true });
        if (this._state === 'errored') return Promise.reject(this._err);
        return new Promise((res, rej) => { this._waiters.push({ res, rej }); this._pull(); });
    }
    get locked() { return !!this._reader; }
    getReader() { return new ReadableStreamDefaultReader(this); }
    cancel(reason) {
        if (this._reader) return Promise.reject(new TypeError('Cannot cancel a locked stream'));
        return this._cancel(reason);
    }
    _cancel(reason) {
        if (this._state === 'closed') return Promise.resolve();
        if (this._state === 'errored') return Promise.reject(this._err);
        this._state = 'closed'; this._q.length = 0;
        for (const w of this._waiters.splice(0)) w.res({ value: undefined, done: true });
        if (this._reader) this._reader._cres();
        try { return Promise.resolve(this._src.cancel ? this._src.cancel(reason) : undefined).then(() => undefined); }
        catch (e) { return Promise.reject(e); }
    }
    tee() {
        const r = this.getReader();
        const ctls = [];
        const cancelled = [false, false];
        const mk = i => new ReadableStream({
            start(c) { ctls[i] = c; },
            pull() {
                return r.read().then(x => {
                    if (x.done) { for (let j = 0; j < 2; j++) if (!cancelled[j]) ctls[j].close(); return; }
                    for (let j = 0; j < 2; j++) if (!cancelled[j]) ctls[j].enqueue(x.value);
                });
            },
            cancel() { cancelled[i] = true; if (cancelled[0] && cancelled[1]) return r.cancel(); },
        });
        return [mk(0), mk(1)];
    }
    [Symbol.asyncIterator]() {
        const r = this.getReader();
        return { next: () => r.read(), return: () => { r.releaseLock(); return Promise.resolve({ done: true, value: undefined }); }, [Symbol.asyncIterator]() { return this; } };
    }
    static from(it) {
        const i = it[Symbol.asyncIterator] ? it[Symbol.asyncIterator]() : it[Symbol.iterator]();
        return new ReadableStream({ pull(c) { return Promise.resolve(i.next()).then(x => x.done ? c.close() : c.enqueue(x.value)); } });
    }
}
class Response {
    constructor(body, init) {
        init = init || {};
        hidden(this, '_body', body === undefined || body === null ? null : body);
        this.status = init.status === undefined ? 200 : init.status;
        this.statusText = init.statusText || '';
        this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
        this.url = init.url || '';
        this.type = 'basic';
        this.redirected = !!init.redirected;
        this.bodyUsed = false;
    }
    get ok() { return this.status >= 200 && this.status < 300; }
    get body() {
        if (this._body === null) return null;
        if (!this._stream) {
            const self = this;
            const b = this._body;
            const bytes = typeof b === 'string' ? new TextEncoder().encode(b)
                : b instanceof Blob ? new TextEncoder().encode(b._s)
                : b instanceof ArrayBuffer ? new Uint8Array(b)
                : ArrayBuffer.isView(b) ? new Uint8Array(b.buffer, b.byteOffset, b.byteLength)
                : new TextEncoder().encode(String(b));
            let pos = 0;
            hidden(this, '_stream', new ReadableStream({
                pull(c) {
                    self.bodyUsed = true;
                    if (pos >= bytes.length) { c.close(); return; }
                    const n = Math.min(65536, bytes.length - pos);
                    c.enqueue(bytes.slice(pos, pos + n));
                    pos += n;
                    if (pos >= bytes.length) c.close();
                },
            }, { highWaterMark: 0 }));
        }
        return this._stream;
    }
    _consume() {
        if (this.bodyUsed) return Promise.reject(new TypeError('body stream already read'));
        this.bodyUsed = true;
        return Promise.resolve(this._body);
    }
    text() { return this._consume().then(b => b === null ? '' : typeof b === 'string' ? b : b instanceof Blob ? b._s : new TextDecoder().decode(b)); }
    json() { return this.text().then(t => JSON.parse(t)); }
    arrayBuffer() { return this._consume().then(b => b === null ? new ArrayBuffer(0) : typeof b === 'string' ? new TextEncoder().encode(b).buffer : b instanceof ArrayBuffer ? b : new TextEncoder().encode(String(b)).buffer); }
    blob() { return this.text().then(t => new Blob([t], { type: this.headers.get('content-type') || '' })); }
    formData() { return this.text().then(t => { const f = new FormData(); for (const [k, v] of new URLSearchParams(t)) f.append(k, v); return f; }); }
    clone() { return new Response(this._body, { status: this.status, statusText: this.statusText, headers: new Headers(this.headers), url: this.url }); }
    static json(data, init) { const r = new Response(JSON.stringify(data), init); r.headers.set('content-type', 'application/json'); return r; }
    static error() { const r = new Response(null, { status: 0 }); r.type = 'error'; return r; }
    static redirect(url, status) { return new Response(null, { status: status || 302, headers: { location: String(url) } }); }
}
class Request {
    constructor(input, init) {
        init = init || {};
        if (input instanceof Request) { this.url = input.url; this.method = input.method; this.headers = new Headers(input.headers); hidden(this, '_body', input._body); }
        else { const u = parseURL(input); if (!u) throw new TypeError('Invalid URL'); this.url = W.resolve(String(input)); this.method = 'GET'; this.headers = new Headers(); hidden(this, '_body', null); }
        if (init.method) this.method = String(init.method).toUpperCase();
        if (init.headers) this.headers = new Headers(init.headers);
        if (init.body !== undefined) this._body = init.body;
        this.signal = init.signal || new AbortController().signal;
        this.credentials = init.credentials || 'same-origin';
        this.mode = init.mode || 'cors';
        this.cache = init.cache || 'default';
        this.redirect = init.redirect || 'follow';
        this.referrer = 'about:client';
    }
    clone() { return new Request(this); }
    text() { return Promise.resolve(this._body === null ? '' : String(this._body)); }
    json() { return this.text().then(JSON.parse); }
}
function fetch(input, init) {
    return new Promise((resolve, reject) => {
        let req;
        try { req = new Request(input, init); } catch (e) { reject(e); return; }
        if (req.signal.aborted) { reject(req.signal.reason); return; }
        let done = false;
        req.signal.addEventListener('abort', () => { if (!done) { done = true; if (/videoplayback/.test(req.url)) mlog('fetch aborted'); reject(req.signal.reason); } });
        if (/^data:/i.test(req.url)) {
            const m = /^data:([^,]*?)(;base64)?,(.*)$/is.exec(req.url);
            if (!m) { reject(new TypeError('Failed to fetch')); return; }
            const body = m[2] ? atob(m[3]) : decodeURIComponent(m[3]);
            resolve(new Response(body, { status: 200, headers: { 'content-type': m[1] || 'text/plain' }, url: req.url }));
            return;
        }
        startRequest(req.method, req.url, req.headers, req._body, (status, statusText, raw, body, url) => {
            if (done) return;
            done = true;
            if (!status) { reject(new TypeError('Failed to fetch')); return; }
            resolve(new Response(body, { status, statusText, headers: parseHeaders(raw), url: url || req.url }));
        });
    });
}
class XMLHttpRequest extends EventTarget {
    constructor() {
        super();
        this.readyState = 0; this.status = 0; this.statusText = ''; this.response = null; this.responseText = '';
        this.responseURL = ''; this.responseType = ''; this.timeout = 0; this.withCredentials = false; this.responseXML = null;
        this.upload = new EventTarget();
        hidden(this, '_h', new Headers()); hidden(this, '_rh', new Headers()); hidden(this, '_raw', '');
        hidden(this, '_aborted', false); hidden(this, '_gen', 0);
        for (const n of ['onreadystatechange', 'onload', 'onerror', 'onabort', 'onloadend', 'onloadstart', 'onprogress', 'ontimeout']) this[n] = null;
    }
    _fire(type) {
        const ev = type === 'readystatechange' ? new Event(type) : new ProgressEvent(type, { loaded: this.responseText.length, total: this.responseText.length });
        const h = this['on' + type];
        if (typeof h === 'function') try { h.call(this, ev); } catch (e) { report(e, 'XHR on' + type); }
        dispatch(this, ev);
    }
    _state(s) { this.readyState = s; this._fire('readystatechange'); }
    open(method, url, async) {
        this._method = String(method).toUpperCase();
        this._url = W.resolve(String(url));
        if (this._url === null) throw new DOMException('Invalid URL', 'SyntaxError');
        this._async = async !== false;
        this._h = new Headers();
        this._aborted = false;
        this._gen++;
        this.status = 0; this.responseText = ''; this.response = null;
        this._state(1);
    }
    setRequestHeader(k, v) { this._h.append(k, v); }
    getResponseHeader(k) { return this._rh.get(k); }
    getAllResponseHeaders() { let s = ''; for (const [k, v] of this._rh) s += k + ': ' + v + '\r\n'; return s; }
    overrideMimeType() {}
    abort() { this._aborted = true; if (this.readyState > 0 && this.readyState < 4) { this.readyState = 4; this._fire('readystatechange'); this._fire('abort'); this._fire('loadend'); } this.readyState = 0; }
    send(body) {
        if (!this._async) W.log(2, 'synchronous XMLHttpRequest is not supported (' + this._url + ')');
        const gen = this._gen;
        this._fire('loadstart');
        startRequest(this._method, this._url, this._h, body === undefined ? null : body, (status, statusText, raw, buf, url) => {
            if (this._aborted || gen !== this._gen) return;
            if (!status) { this.readyState = 4; this._fire('readystatechange'); this._fire('error'); this._fire('loadend'); return; }
            this.status = status; this.statusText = statusText; this.responseURL = url || this._url;
            this._rh = parseHeaders(raw);
            this._state(2);
            this._state(3);
            const text = buf === null ? '' : new TextDecoder().decode(buf);
            this.responseText = text;
            const rt = this.responseType;
            if (rt === 'json') { try { this.response = JSON.parse(text); } catch (e) { this.response = null; } }
            else if (rt === 'arraybuffer') this.response = buf || new ArrayBuffer(0);
            else if (rt === 'blob') this.response = new Blob([text], { type: this._rh.get('content-type') || '' });
            else if (rt === 'document') this.response = null;
            else this.response = text;
            this._state(4);
            this._fire('load');
            this._fire('loadend');
        });
    }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;

// ---------------------------------------------------------------- encoding
class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s) { return W.utf8enc(s === undefined ? '' : String(s)); }
    encodeInto(s, dest) { const b = this.encode(s); const n = Math.min(b.length, dest.length); dest.set(b.subarray(0, n)); return { read: s.length, written: n }; }
}
class TextDecoder {
    constructor(label, opts) { this.encoding = String(label || 'utf-8').toLowerCase(); this.fatal = !!(opts && opts.fatal); this.ignoreBOM = !!(opts && opts.ignoreBOM); }
    decode(buf) {
        if (buf === undefined || buf === null) return '';
        let u8;
        if (buf instanceof ArrayBuffer) u8 = new Uint8Array(buf);
        else if (ArrayBuffer.isView(buf)) u8 = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
        else throw new TypeError('TextDecoder.decode: not a buffer');
        if (/^(iso-8859-1|latin1|windows-1252|ascii|us-ascii)$/.test(this.encoding)) { let s = ''; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
        return W.utf8dec(u8);
    }
}
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function btoa(s) {
    s = String(s);
    let out = '';
    for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
        if (a > 255 || b > 255 || c > 255) throw new DOMException('The string to be encoded contains characters outside of the Latin1 range.', 'InvalidCharacterError');
        out += B64[a >> 2] + B64[((a & 3) << 4) | (i + 1 < s.length ? b >> 4 : 0)] +
            (i + 1 < s.length ? B64[((b & 15) << 2) | (i + 2 < s.length ? c >> 6 : 0)] : '=') +
            (i + 2 < s.length ? B64[c & 63] : '=');
    }
    return out;
}
function atob(s) {
    s = String(s).replace(/[\t\n\f\r ]/g, '');
    if (s.length % 4 === 0) s = s.replace(/==?$/, '');
    if (s.length % 4 === 1 || /[^A-Za-z0-9+\/]/.test(s)) throw new DOMException('The string to be decoded is not correctly encoded.', 'InvalidCharacterError');
    let out = '', buf = 0, bits = 0;
    for (let i = 0; i < s.length; i++) {
        buf = (buf << 6) | B64.indexOf(s[i]);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += String.fromCharCode((buf >> bits) & 255); }
    }
    return out;
}

// ---------------------------------------------------------------- storage, cookies
function makeStorage(area) {
    const st = Object.create(Storage.prototype);
    hidden(st, '_a', area);
    return new Proxy(st, {
        get(t, k) {
            if (typeof k !== 'string' || k in Storage.prototype || k === '_a') { const v = t[k]; return typeof v === 'function' ? v.bind(t) : v; }
            const v = W.store(area, 0, k);
            return v === null ? undefined : v;
        },
        set(t, k, v) { if (typeof k === 'string') W.store(area, 1, k, String(v)); return true; },
        deleteProperty(t, k) { if (typeof k === 'string') W.store(area, 2, k); return true; },
        has(t, k) { return typeof k === 'string' && (k in Storage.prototype || W.store(area, 0, k) !== null); },
        ownKeys() { const n = W.store(area, 5); const a = []; for (let i = 0; i < n; i++) a.push(W.store(area, 4, i)); return a; },
        getOwnPropertyDescriptor(t, k) { const v = W.store(area, 0, String(k)); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; }
    });
}
function Storage() { illegal(); }
methods(Storage.prototype, {
    getItem(k) { return W.store(this._a, 0, String(k)); },
    setItem(k, v) { W.store(this._a, 1, String(k), String(v)); },
    removeItem(k) { W.store(this._a, 2, String(k)); },
    clear() { W.store(this._a, 3); },
    key(i) { return W.store(this._a, 4, i | 0); }
});
accessor(Storage.prototype, 'length', function () { return W.store(this._a, 5); });
const localStorage = makeStorage(0), sessionStorage = makeStorage(1);

// ---------------------------------------------------------------- document
W.setProtoFor(protoFor);   // C needs it before creating the first wrapper
const document = W.doc();
const D = Document.prototype;
methods(D, {
    getElementById(id) { return W.byId(String(id)); },
    getElementsByName(n) { return nodeList(W.query(document, '[name="' + cssEscape(String(n)) + '"]', true)); },
    createElement(name, opts) {
        name = String(name);
        if (!/^[A-Za-z][^\s\/>\0]*$/.test(name)) throw new DOMException('The tag name provided (\'' + name + '\') is not a valid name.', 'InvalidCharacterError');
        name = name.toLowerCase();
        const is = opts && typeof opts === 'object' ? opts.is : null;
        const def = CE_BY_NAME.get(name);
        if (def) { try { return new def.ctor(); } catch (e) { report(e, 'custom element constructor'); } }
        const el = W.create(name, 0);
        if (is) W.setAttr(el, 'is', String(is));
        return el;
    },
    createElementNS(ns, qname) {
        qname = String(qname);
        const local = qname.indexOf(':') >= 0 ? qname.slice(qname.indexOf(':') + 1) : qname;
        if (ns === 'http://www.w3.org/2000/svg') return W.create(local, 1);
        if (ns === 'http://www.w3.org/1998/Math/MathML') return W.create(local, 2);
        return this.createElement(local);
    },
    createTextNode(s) { return W.createText(String(s)); },
    createComment(s) { return W.createComment(String(s)); },
    createCDATASection(s) { return W.createText(String(s)); },
    createDocumentFragment() { return W.createFragment(); },
    createProcessingInstruction() { return W.createComment(''); },
    createAttribute(n) { const holder = W.create('span', 0); return makeAttr(holder, String(n).toLowerCase()); },
    createEvent(kind) {
        const k = String(kind).toLowerCase();
        const C = /mouse/.test(k) ? MouseEvent : /keyboard/.test(k) ? KeyboardEvent : /custom/.test(k) ? CustomEvent : /ui/.test(k) ? UIEvent : Event;
        const ev = new C('');
        return ev;
    },
    createRange() { return new Range(); },
    createTreeWalker(root, what, filter) { return new TreeWalker(root, what === undefined ? 0xFFFFFFFF : what, filter); },
    createNodeIterator(root, what, filter) { return new TreeWalker(root, what === undefined ? 0xFFFFFFFF : what, filter); },
    importNode(n, deep) { return n.cloneNode(!!deep); },
    adoptNode(n) { if (W.parent(n)) W.parent(n).removeChild(n); return n; },
    write(...s) { W.write(s.join('')); },
    writeln(...s) { W.write(s.join('') + '\n'); },
    open() { return this; }, close() {},
    hasFocus() { return true; },
    getSelection() { return G.getSelection(); },
    elementFromPoint(x, y) { return W.hit(x | 0, y | 0); },
    elementsFromPoint(x, y) { const e = W.hit(x | 0, y | 0); const a = []; for (let n = e; n && W.ntype(n) === 1; n = W.parent(n)) a.push(n); return a; },
    execCommand() { return false; },
    queryCommandSupported() { return false; },
    exitFullscreen() { return Promise.resolve(); },
    startViewTransition(cb) { const p = Promise.resolve().then(() => cb && cb()); return { finished: p, ready: p, updateCallbackDone: p, skipTransition() {} }; }
});
accessor(D, 'documentElement', function () { return W.root(); });
accessor(D, 'head', function () { return W.query(document, 'head', false); });
accessor(D, 'body', function () { return W.query(document, 'body', false) || W.query(document, 'frameset', false); },
    function (b) { const old = this.body; const html = W.root(); if (!html || !b) return; if (old) html.replaceChild(b, old); else html.appendChild(b); });
accessor(D, 'title', function () { return W.title(); }, function (v) { W.title(String(v)); });
accessor(D, 'URL', function () { return W.docURL(); });
accessor(D, 'documentURI', function () { return W.docURL(); });
accessor(D, 'baseURI', function () { return W.baseURL(); });
accessor(D, 'location', function () { return location; }, function (v) { location.href = v; });
accessor(D, 'domain', function () { return location.hostname; }, function () {});
accessor(D, 'origin', function () { return location.origin; });
accessor(D, 'referrer', function () { return W.referrer(); });
accessor(D, 'cookie', function () { return W.cookie(); }, function (v) { W.cookie(String(v)); });
accessor(D, 'readyState', function () { return readyState; });
accessor(D, 'defaultView', function () { return G; });
accessor(D, 'activeElement', function () { return W.activeElement() || this.body; });
accessor(D, 'characterSet', function () { return 'UTF-8'; });
accessor(D, 'charset', function () { return 'UTF-8'; });
accessor(D, 'inputEncoding', function () { return 'UTF-8'; });
accessor(D, 'contentType', function () { return 'text/html'; });
accessor(D, 'compatMode', function () { return W.quirks() ? 'BackCompat' : 'CSS1Compat'; });
accessor(D, 'doctype', function () { return null; });
accessor(D, 'visibilityState', function () { return 'visible'; });
accessor(D, 'hidden', function () { return false; });
accessor(D, 'currentScript', function () { return W.currentScript(); });
accessor(D, 'scripts', function () { return htmlCollection(W.query(document, 'script', true)); });
accessor(D, 'images', function () { return htmlCollection(W.query(document, 'img', true)); });
accessor(D, 'links', function () { return htmlCollection(W.query(document, 'a[href],area[href]', true)); });
accessor(D, 'forms', function () { return htmlCollection(W.query(document, 'form', true)); });
accessor(D, 'anchors', function () { return htmlCollection(W.query(document, 'a[name]', true)); });
accessor(D, 'embeds', function () { return htmlCollection(W.query(document, 'embed', true)); });
accessor(D, 'plugins', function () { return htmlCollection([]); });

accessor(D, 'fonts', function () { return FONTS; });
accessor(D, 'scrollingElement', function () { return W.root(); });
accessor(D, 'fullscreenElement', function () { return null; });
accessor(D, 'fullscreenEnabled', function () { return false; });
accessor(D, 'pointerLockElement', function () { return null; });
accessor(D, 'implementation', function () { return IMPL; });
accessor(D, 'lastModified', function () { return new Date().toUTCString(); });
accessor(D, 'designMode', function () { return 'off'; }, function () {});
accessor(D, 'dir', function () { const h = W.root(); return h ? h.dir : ''; }, function (v) { const h = W.root(); if (h) h.dir = v; });
accessor(D, 'timeline', function () { return { currentTime: W.now() }; });
defineHandlers(D);
const FONTS = Object.assign(new EventTarget(), { ready: Promise.resolve(), status: 'loaded', size: 0,
    check() { return true; }, load() { return Promise.resolve([]); }, add() {}, delete() {}, has() { return false }, forEach() {} });
FONTS.ready = Promise.resolve(FONTS);
class FontFace { constructor(family) { this.family = family; this.status = 'loaded'; this.loaded = Promise.resolve(this); } load() { return Promise.resolve(this); } }
const IMPL = { hasFeature() { return true; },
    createHTMLDocument(title) {
        const doc = new DOMParser().parseFromString('<!doctype html><html><head></head><body></body></html>', 'text/html');
        if (title !== undefined) { const t = W.create('title', 0); W.setText(t, String(title)); W.insert(doc.head, t, null); }
        return doc;
    },
    createDocument() { return this.createHTMLDocument(''); }, createDocumentType() { return null; } };
let readyState = 'loading';

class Range {
    constructor() { this.startContainer = document; this.startOffset = 0; this.endContainer = document; this.endOffset = 0; this.collapsed = true; this.commonAncestorContainer = document; }
    setStart(n, o) { this.startContainer = n; this.startOffset = o; }
    setEnd(n, o) { this.endContainer = n; this.endOffset = o; }
    setStartBefore(n) {} setStartAfter(n) {} setEndBefore(n) {} setEndAfter(n) {}
    selectNode(n) { this.startContainer = this.endContainer = n; } selectNodeContents(n) { this.startContainer = this.endContainer = n; }
    collapse() {} detach() {} cloneRange() { return new Range(); }
    getBoundingClientRect() { return new DOMRect(0, 0, 0, 0); } getClientRects() { return []; }
    createContextualFragment(html) { return W.parseHTML(String(html), document.body || W.root()); }
    deleteContents() {} extractContents() { return W.createFragment(); } insertNode() {} surroundContents() {}
    toString() { return ''; }
}
class TreeWalker {
    constructor(root, what, filter) { this.root = root; this.whatToShow = what; this.filter = filter || null; this.currentNode = root; }
    _ok(n) {
        const t = W.ntype(n);
        if (!(this.whatToShow & (1 << (t - 1)))) return false;
        if (!this.filter) return true;
        const f = typeof this.filter === 'function' ? this.filter : this.filter.acceptNode;
        return f.call(this.filter, n) === 1;
    }
    _next(n) {
        let c = W.first(n);
        if (c) return c;
        for (; n && n !== this.root; n = W.parent(n)) { const s = W.next(n); if (s) return s; }
        return null;
    }
    nextNode() { for (let n = this._next(this.currentNode); n; n = this._next(n)) if (this._ok(n)) return (this.currentNode = n); return null; }
    previousNode() { return null; }
    parentNode() { for (let n = W.parent(this.currentNode); n && n !== W.parent(this.root); n = W.parent(n)) if (this._ok(n)) return (this.currentNode = n); return null; }
    firstChild() { for (let c = W.first(this.currentNode); c; c = W.next(c)) if (this._ok(c)) return (this.currentNode = c); return null; }
    lastChild() { for (let c = W.last(this.currentNode); c; c = W.prev(c)) if (this._ok(c)) return (this.currentNode = c); return null; }
    nextSibling() { for (let c = W.next(this.currentNode); c; c = W.next(c)) if (this._ok(c)) return (this.currentNode = c); return null; }
    previousSibling() { for (let c = W.prev(this.currentNode); c; c = W.prev(c)) if (this._ok(c)) return (this.currentNode = c); return null; }
    get referenceNode() { return this.currentNode; }
    detach() {}
}
const NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1,
    SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_PROCESSING_INSTRUCTION: 64, SHOW_COMMENT: 128,
    SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024 };

class DOMParser {
    parseFromString(s, type) {
        // A detached <html> subtree standing in for a separate document
        // (shares the page's node store; ownerDocument stays the page).
        s = String(s);
        const html = W.create('html', 0);
        const head = W.create('head', 0), body = W.create('body', 0);
        W.insert(html, head, null);
        W.insert(html, body, null);
        const hm = /<head[^>]*>([\s\S]*?)<\/head>/i.exec(s);
        const bm = /<body[^>]*>([\s\S]*?)(<\/body>|$)/i.exec(s);
        if (hm) W.insert(head, W.parseHTML(hm[1], head), null);
        const rest = bm ? bm[1] : s.replace(/<!doctype[^>]*>/i, '').replace(/<\/?(html|head|body)[^>]*>/gi, '').replace(hm ? hm[0] : '', '');
        W.insert(body, W.parseHTML(rest, body), null);
        const doc = Object.create(Document.prototype);
        const q = sel => W.query(html, sel, false);
        Object.defineProperties(doc, {
            nodeType: { value: 9 }, nodeName: { value: '#document' },
            documentElement: { value: html }, body: { get: () => body }, head: { get: () => head },
            title: { get: () => { const t = q('title'); return t ? W.text(t) : ''; } },
            URL: { value: 'about:blank' }, location: { value: null }, readyState: { value: 'complete' },
            childNodes: { get: () => nodeList([html]) }, firstChild: { get: () => html }, lastChild: { get: () => html },
            children: { get: () => htmlCollection([html]) }, firstElementChild: { get: () => html },
            querySelector: { value: sel => W.query(html, String(sel), false) },
            querySelectorAll: { value: sel => nodeList(W.query(html, String(sel), true)) },
            getElementById: { value: id => W.query(html, '#' + cssEscape(String(id)), false) },
            getElementsByTagName: { value: t => htmlCollection(W.byTag(html, String(t).toLowerCase())) },
            getElementsByClassName: { value: c => ParentNode.getElementsByClassName.call(html, c) },
            createElement: { value: n => document.createElement(n) },
            createElementNS: { value: (ns, n) => document.createElementNS(ns, n) },
            createTextNode: { value: t => document.createTextNode(t) },
            createComment: { value: t => document.createComment(t) },
            createDocumentFragment: { value: () => document.createDocumentFragment() },
            importNode: { value: (n, d) => n.cloneNode(!!d) },
            adoptNode: { value: n => n },
            implementation: { value: IMPL },
            defaultView: { value: null }
        });
        return doc;
    }
}
class XMLSerializer { serializeToString(n) { return W.html(n, 1); } }

// ---------------------------------------------------------------- CSSOM (minimal)
// Rules are kept as text; a sheet owned by a <style> element writes its
// rules back into the element (the engine restyles from <style> text), so
// CSS-in-JS insertRule() and constructable sheets actually apply.
function splitRules(text) {
    const out = [];
    let depth = 0, start = 0, q = 0;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (q) { if (c === q) q = 0; else if (c === '\\') i++; continue; }
        if (c === '/' && text[i + 1] === '*') { const e = text.indexOf('*/', i + 2); i = e < 0 ? text.length : e + 1; continue; }
        if (c === '"' || c === "'") q = c;
        else if (c === '{') depth++;
        else if (c === '}') { depth--; if (depth <= 0) { const r = text.slice(start, i + 1).trim(); if (r) out.push(r); start = i + 1; depth = 0; } }
        else if (c === ';' && depth === 0) { const r = text.slice(start, i + 1).trim(); if (r) out.push(r); start = i + 1; }
    }
    const tail = text.slice(start).trim();
    if (tail) out.push(tail);
    return out;
}
class CSSRule {
    constructor(text, sheet) {
        this.cssText = text; this.parentStyleSheet = sheet || null; this.parentRule = null;
        const b = text.indexOf('{');
        this.selectorText = b > 0 ? text.slice(0, b).trim() : '';
        this.type = text[0] === '@' ? (/^@media/i.test(text) ? 4 : /^@font-face/i.test(text) ? 5 : /^@import/i.test(text) ? 3 : /^@keyframes/i.test(text) ? 7 : 0) : 1;
        const decls = b > 0 ? text.slice(b + 1, text.lastIndexOf('}')) : '';
        const list = parseDecls(decls);
        this.style = { cssText: decls.trim(), getPropertyValue: p => { const d = list.find(x => x[0] === String(p).toLowerCase()); return d ? d[1] : ''; } };
    }
}
CSSRule.STYLE_RULE = 1; CSSRule.IMPORT_RULE = 3; CSSRule.MEDIA_RULE = 4; CSSRule.FONT_FACE_RULE = 5; CSSRule.KEYFRAMES_RULE = 7;
class StyleSheet {}
class CSSStyleSheet extends StyleSheet {
    constructor(opts) {
        super();
        hidden(this, '_rules', []);
        hidden(this, '_owners', new Set());
        this.disabled = false;
        this.href = null;
        this.ownerNode = null;
        this.title = null;
        this.type = 'text/css';
        const m = opts && opts.media ? String(opts.media) : '';
        this.media = Object.assign(m ? m.split(',').map(s => s.trim()) : [], { mediaText: m, appendMedium() {}, deleteMedium() {} });
    }
    get cssRules() { return this._rules.map(t => new CSSRule(t, this)); }
    get rules() { return this.cssRules; }
    get ownerRule() { return null; }
    replaceSync(text) { this._rules = splitRules(String(text)); this._sync(); }
    replace(text) { try { this.replaceSync(text); return Promise.resolve(this); } catch (e) { return Promise.reject(e); } }
    insertRule(rule, index) {
        index = index === undefined ? 0 : index | 0;
        if (index < 0 || index > this._rules.length) throw new DOMException('index out of range', 'IndexSizeError');
        this._rules.splice(index, 0, String(rule));
        this._sync();
        return index;
    }
    deleteRule(i) { this._rules.splice(i | 0, 1); this._sync(); }
    addRule(sel, style, idx) { return this.insertRule(sel + '{' + style + '}', idx === undefined ? this._rules.length : idx); }
    removeRule(i) { this.deleteRule(i || 0); }
    _text() { return this._rules.join('\n'); }
    _sync() { for (const el of this._owners) W.setText(el, this._text()); }
}
const STYLE_SHEETS = new WeakMap();
accessor(HTMLStyleElement.prototype, 'sheet', function () {
    if (!W.connected(this)) return null;
    let s = STYLE_SHEETS.get(this);
    if (!s) {
        s = new CSSStyleSheet({ media: W.getAttr(this, 'media') || '' });
        s._rules = splitRules(W.text(this));
        s.ownerNode = this;
        s._owners.add(this);
        STYLE_SHEETS.set(this, s);
    }
    return s;
});
function linkSheet(el) {
    let s = STYLE_SHEETS.get(el);
    if (!s) { s = new CSSStyleSheet(); s.ownerNode = el; s.href = el.href; STYLE_SHEETS.set(el, s); }
    return s;
}
// document.adoptedStyleSheets -> one <style data-okai-adopted> per sheet in <head>
let adopted = [];
const ADOPT_EL = new WeakMap();
accessor(Document.prototype, 'adoptedStyleSheets', function () { return adopted.slice(); }, function (list) {
    list = Array.from(list || []);
    for (const s of adopted) if (!list.includes(s)) { const el = ADOPT_EL.get(s); if (el) { s._owners.delete(el); ChildNode.remove.call(el); ADOPT_EL.delete(s); } }
    const head = document.head || W.root();
    for (const s of list) {
        if (!(s instanceof CSSStyleSheet) || ADOPT_EL.has(s)) continue;
        const el = W.create('style', 0);
        W.setAttr(el, 'data-okai-adopted', '');
        W.setText(el, s._text());
        if (head) W.insert(head, el, null);
        s._owners.add(el);
        ADOPT_EL.set(s, el);
    }
    adopted = list;
});
accessor(D, 'styleSheets', function () {
    const a = W.query(document, 'style,link[rel~=stylesheet]', true).map(el => el.sheet).filter(Boolean);
    a.item = i => a[i] || null;
    return a;
});

// ---------------------------------------------------------------- window
const navigator = {
    userAgent: 'Mozilla/5.0 (X11; okernel i686) AppleWebKit/537.36 (KHTML, like Gecko) okai/0.6 Safari/537.36',
    appName: 'Netscape', appCodeName: 'Mozilla', appVersion: '5.0 (X11; okernel i686)', product: 'Gecko', productSub: '20030107',
    vendor: 'okernel', vendorSub: '', platform: 'Linux i686', language: 'en-US', languages: ['en-US', 'en'],
    cookieEnabled: true, onLine: true, doNotTrack: '1', hardwareConcurrency: 1, maxTouchPoints: 0, deviceMemory: 1,
    pdfViewerEnabled: false, webdriver: false, plugins: [], mimeTypes: [],
    javaEnabled() { return false; },
    sendBeacon() { return true; },
    vibrate() { return false; },
    registerProtocolHandler() {},
    clipboard: { writeText() { return Promise.resolve(); }, readText() { return Promise.resolve(''); }, write() { return Promise.resolve(); } },
    permissions: { query() { return Promise.resolve({ state: 'denied', onchange: null, addEventListener() {} }); } },
    mediaCapabilities: { decodingInfo: mediaDecodingInfo, encodingInfo() { return Promise.resolve({ supported: false, smooth: false, powerEfficient: false }); } },
    mediaDevices: { enumerateDevices() { return Promise.resolve([]); }, getUserMedia() { return Promise.reject(new DOMException('not supported', 'NotSupportedError')); } },
    storage: { estimate() { return Promise.resolve({ quota: 0, usage: 0 }); }, persist() { return Promise.resolve(false); }, persisted() { return Promise.resolve(false); } },
    connection: { effectiveType: '3g', downlink: 1, rtt: 300, saveData: false, addEventListener() {}, removeEventListener() {} },
    userActivation: { hasBeenActive: true, isActive: false },
    locks: { request(name, opts, cb) { cb = typeof opts === 'function' ? opts : cb; return Promise.resolve().then(() => cb({ name })); } },
    getBattery() { return Promise.resolve(battery); },   // a desktop: on mains power
    getGamepads() { return []; }
};
const BatteryManager = sub('BatteryManager', EventTarget);
const battery = Object.create(BatteryManager.prototype);
for (const [k, v] of [['charging', true], ['chargingTime', 0], ['dischargingTime', Infinity], ['level', 1]])
    accessor(BatteryManager.prototype, k, function () { return v; });
for (const k of ['onchargingchange', 'onchargingtimechange', 'ondischargingtimechange', 'onlevelchange']) battery[k] = null;
const screen = { width: 1920, height: 1080, availWidth: 1920, availHeight: 1040, availLeft: 0, availTop: 0,
    colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {}, removeEventListener() {} } };
const performance = Object.assign(new EventTarget(), {
    now() { return W.now(); },
    timeOrigin: Date.now() - W.now(),
    timing: { navigationStart: Date.now() - W.now(), fetchStart: 0, domLoading: 0, domInteractive: 0, domComplete: 0, loadEventStart: 0, loadEventEnd: 0, responseStart: 0, responseEnd: 0, connectStart: 0, connectEnd: 0, requestStart: 0, domainLookupStart: 0, domainLookupEnd: 0, redirectStart: 0, redirectEnd: 0, unloadEventStart: 0, unloadEventEnd: 0, secureConnectionStart: 0, domContentLoadedEventStart: 0, domContentLoadedEventEnd: 0 },
    navigation: { type: 0, redirectCount: 0 },
    mark(n) { return { name: n, entryType: 'mark', startTime: W.now(), duration: 0 }; },
    measure(n) { return { name: n, entryType: 'measure', startTime: 0, duration: 0 }; },
    clearMarks() {}, clearMeasures() {}, clearResourceTimings() {}, setResourceTimingBufferSize() {},
    getEntries() { return []; }, getEntriesByType(t) { return t === 'navigation' ? [{ type: 'navigate', name: W.docURL(), entryType: 'navigation', startTime: 0, duration: 0 }] : []; },
    getEntriesByName() { return []; },
    toJSON() { return {}; },
    memory: { jsHeapSizeLimit: 67108864, totalJSHeapSize: 0, usedJSHeapSize: 0 }
});
class PerformanceObserver { constructor() {} observe() {} disconnect() {} takeRecords() { return []; } }
PerformanceObserver.supportedEntryTypes = [];
const console = {};
for (const [name, lvl] of [['log', 1], ['info', 1], ['debug', 0], ['warn', 2], ['error', 3], ['trace', 0]])
    console[name] = (...a) => W.log(lvl, a.map(fmt).join(' '));
console.assert = (c, ...a) => { if (!c) W.log(3, 'Assertion failed: ' + a.map(fmt).join(' ')); };
console.dir = console.dirxml = console.table = console.log;
for (const n of ['group', 'groupCollapsed', 'groupEnd', 'time', 'timeEnd', 'timeLog', 'count', 'countReset', 'clear', 'profile', 'profileEnd', 'timeStamp'])
    console[n] = () => {};
function fmt(v) {
    if (typeof v === 'string') return v;
    if (v instanceof Error) return String(v);
    if (v instanceof Node) return '<' + v.nodeName.toLowerCase() + '>';
    if (typeof v === 'object' && v !== null) { try { const s = JSON.stringify(v); return s && s.length > 300 ? s.slice(0, 300) + '...' : s; } catch (e) { return Object.prototype.toString.call(v); } }
    return String(v);
}
const crypto = {
    getRandomValues(a) { if (!ArrayBuffer.isView(a)) throw new TypeError('getRandomValues: not a typed array'); W.random(new Uint8Array(a.buffer, a.byteOffset, a.byteLength)); return a; },
    randomUUID() {
        const b = new Uint8Array(16); W.random(b);
        b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
        const h = Array.from(b, x => (x + 256).toString(16).slice(1)).join('');
        return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
    },
    subtle: { digest() { return Promise.reject(new DOMException('not supported', 'NotSupportedError')); } }
};
class MediaQueryList extends EventTarget {
    constructor(q) { super(); this.media = q; this.onchange = null; }
    get matches() { return W.mq(this.media); }
    addListener(cb) { this.addEventListener('change', cb); }
    removeListener(cb) { this.removeEventListener('change', cb); }
}
function matchMedia(q) { return new MediaQueryList(String(q)); }
function getComputedStyle(el, pseudo) {
    const m = (el instanceof Element && !pseudo) ? W.cstyle(el) : {};
    const decl = Object.create(CSSStyleDeclaration.prototype);
    const get = p => { p = String(p); if (p in m) return m[p]; if (p.startsWith('--')) return W.cssVar(el, p) || ''; return ''; };
    return new Proxy(decl, {
        get(t, k) {
            if (k === 'getPropertyValue') return get;
            if (k === 'getPropertyPriority') return () => '';
            if (k === 'length') return Object.keys(m).length;
            if (k === 'item') return i => Object.keys(m)[i] || '';
            if (k === 'cssText') return '';
            if (k === 'setProperty' || k === 'removeProperty') return () => { throw new DOMException('read-only', 'NoModificationAllowedError'); };
            if (typeof k !== 'string') return undefined;
            return get(camelToDash(k));
        }
    });
}
function queueMicrotask(cb) { Promise.resolve().then(() => { try { cb(); } catch (e) { report(e, 'microtask'); } }); }
function structuredClone(v) {
    const seen = new Map();
    const c = x => {
        if (x === null || typeof x !== 'object') { if (typeof x === 'function' || typeof x === 'symbol') throw new DOMException('could not be cloned', 'DataCloneError'); return x; }
        if (seen.has(x)) return seen.get(x);
        let r;
        if (x instanceof Date) r = new Date(x.getTime());
        else if (x instanceof RegExp) r = new RegExp(x.source, x.flags);
        else if (x instanceof Map) { r = new Map(); seen.set(x, r); for (const [k, v] of x) r.set(c(k), c(v)); return r; }
        else if (x instanceof Set) { r = new Set(); seen.set(x, r); for (const v of x) r.add(c(v)); return r; }
        else if (x instanceof ArrayBuffer) r = x.slice(0);
        else if (ArrayBuffer.isView(x)) r = new x.constructor(x);
        else if (Array.isArray(x)) { r = []; seen.set(x, r); for (let i = 0; i < x.length; i++) r[i] = c(x[i]); return r; }
        else if (x instanceof Node) throw new DOMException('could not be cloned', 'DataCloneError');
        else { r = {}; seen.set(x, r); for (const k of Object.keys(x)) r[k] = c(x[k]); return r; }
        seen.set(x, r);
        return r;
    };
    return c(v);
}
function getSelection() {
    return { rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null, anchorOffset: 0, focusOffset: 0,
        toString() { return ''; }, getRangeAt() { return new Range(); }, removeAllRanges() {}, addRange() {}, collapse() {}, empty() {},
        selectAllChildren() {}, containsNode() { return false; }, extend() {}, setBaseAndExtent() {} };
}
class BroadcastChannel extends EventTarget { constructor(n) { super(); this.name = n; this.onmessage = null; } postMessage() {} close() {} }
class MessageChannel { constructor() { const mk = () => Object.assign(new EventTarget(), { onmessage: null, postMessage(d) { const o = this._other; setTimeout(() => { const ev = new MessageEvent('message', { data: d }); if (typeof o.onmessage === 'function') o.onmessage(ev); dispatch(o, ev); }, 0); }, start() {}, close() {} }); this.port1 = mk(); this.port2 = mk(); this.port1._other = this.port2; this.port2._other = this.port1; } }
class WebSocket extends EventTarget {
    constructor(url) { super(); this.url = String(url); this.readyState = 3; this.protocol = ''; this.extensions = ''; this.bufferedAmount = 0; this.binaryType = 'blob'; this.onopen = this.onclose = this.onerror = this.onmessage = null;
        setTimeout(() => { const e = new Event('error'); if (this.onerror) try { this.onerror(e); } catch (x) {} dispatch(this, e); const c = Object.assign(new Event('close'), { code: 1006, reason: '', wasClean: false }); if (this.onclose) try { this.onclose(c); } catch (x) {} dispatch(this, c); }, 0); }
    send() { throw new DOMException('WebSocket is not open', 'InvalidStateError'); }
    close() {}
}
WebSocket.CONNECTING = 0; WebSocket.OPEN = 1; WebSocket.CLOSING = 2; WebSocket.CLOSED = 3;
function postMessage(data, origin) {
    setTimeout(() => dispatch(G, new MessageEvent('message', { data, origin: location.origin, source: G })), 0);
}

// window properties
function Window() { illegal(); }
Window.prototype = Object.create(EventTarget.prototype, { constructor: { value: Window, writable: true, configurable: true } });
Object.setPrototypeOf(Window, EventTarget);
const win = {
    window: G, self: G, globalThis: G, top: G, parent: G, frames: G, opener: null, frameElement: null, closed: false,
    document, location, history, navigator, screen, performance, console, crypto, localStorage, sessionStorage,
    customElements: new CustomElementRegistry(), length: 0, name: '', status: '', origin: location.origin,
    isSecureContext: /^https:/.test(W.docURL()), crossOriginIsolated: false, devicePixelRatio: 1,
    visualViewport: null, speechSynthesis: undefined, trustedTypes: undefined,
    setTimeout, setInterval, clearTimeout, clearInterval: clearTimeout, requestAnimationFrame, cancelAnimationFrame,
    requestIdleCallback, cancelIdleCallback: clearTimeout, queueMicrotask, structuredClone, matchMedia, getComputedStyle,
    getSelection, postMessage, fetch, atob, btoa,
    alert(m) { W.log(1, 'alert: ' + m); }, confirm(m) { W.log(1, 'confirm: ' + m); return false; }, prompt(m) { W.log(1, 'prompt: ' + m); return null; },
    print() {}, focus() {}, blur() {}, stop() {}, close() {}, moveTo() {}, resizeTo() {}, moveBy() {}, resizeBy() {},
    open(url) { if (url) navigate(url, false); return null; },
    scrollTo(x, y) { if (typeof x === 'object' && x) y = x.top; if (y !== undefined) W.scroll(Math.max(0, +y | 0)); },
    scroll(x, y) { G.scrollTo(x, y); },
    scrollBy(x, y) { if (typeof x === 'object' && x) y = x.top; W.scroll(Math.max(0, (W.view()[2] + (+y || 0)) | 0)); },
    captureEvents() {}, releaseEvents() {},
    Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent, ErrorEvent,
    ProgressEvent, MessageEvent, PopStateEvent, HashChangeEvent, PageTransitionEvent, SubmitEvent, StorageEvent,
    AnimationEvent, TransitionEvent, PromiseRejectionEvent, TouchEvent, CompositionEvent, ClipboardEvent,
    EventTarget, DOMException, Node, Element, CharacterData, Text, Comment, Document, HTMLDocument, DocumentFragment,
    URL, URLSearchParams, Location, History, Headers, Request, Response, Blob, File, FormData, AbortController, AbortSignal,
    CanvasRenderingContext2D, CanvasGradient, CanvasPattern, ImageData, Path2D, OffscreenCanvas, ImageBitmap, createImageBitmap, DOMMatrix, DOMMatrixReadOnly,
    DOMPoint, DOMPointReadOnly, TextMetrics,
    MediaSource, SourceBuffer, SourceBufferList, TimeRanges, MediaError, ReadableStream, ReadableStreamDefaultReader,
    XMLHttpRequest, TextEncoder, TextDecoder, Storage, MutationObserver, MutationRecord, IntersectionObserver, IntersectionObserverEntry, ResizeObserver,
    CustomElementRegistry, MediaQueryList, PerformanceObserver, Range, TreeWalker, NodeIterator: TreeWalker, NodeFilter,
    DOMParser, XMLSerializer, FontFace, BroadcastChannel, MessageChannel, WebSocket,
    CSSStyleSheet, StyleSheet, CSSRule, Window, Navigator: function Navigator() { illegal(); },
    Screen: function Screen() { illegal(); }, Performance: function Performance() { illegal(); },
    Crypto: function Crypto() { illegal(); }, Selection: function Selection() { illegal(); },
    NamedNodeMap: function NamedNodeMap() { illegal(); }, DOMStringMap: function DOMStringMap() { illegal(); },
    StyleSheetList: function StyleSheetList() { illegal(); }, MediaList: function MediaList() { illegal(); },
    ValidityState: function ValidityState() { illegal(); }, HTMLOptionsCollection: HTMLCollection,
    VisualViewport: function VisualViewport() { illegal(); }, CSSStyleRule: CSSRule, CSSMediaRule: CSSRule,
    CSS: { supports(a, b) { return W.supports(b === undefined ? String(a) : '(' + a + ': ' + b + ')'); }, escape: cssEscape },
    Intl: G.Intl
};
for (const k of Object.keys(win)) {
    if (win[k] === undefined) continue;
    Object.defineProperty(G, k, { value: win[k], writable: true, configurable: true, enumerable: false });
}
accessor(G, 'innerWidth', () => W.view()[0]);
accessor(G, 'innerHeight', () => W.view()[1]);
accessor(G, 'outerWidth', () => W.view()[0]);
accessor(G, 'outerHeight', () => W.view()[1] + 96);
accessor(G, 'scrollX', () => 0);
accessor(G, 'scrollY', () => W.view()[2]);
accessor(G, 'pageXOffset', () => 0);
accessor(G, 'pageYOffset', () => W.view()[2]);
accessor(G, 'screenX', () => 0);
accessor(G, 'screenY', () => 0);
accessor(G, 'screenLeft', () => 0);
accessor(G, 'screenTop', () => 0);
accessor(G, 'event', () => undefined);
Object.setPrototypeOf(G, Window.prototype);
defineHandlers(G);
// ---------------------------------------------------------------- Intl (en-US, UTC)
// ojs ships no Intl: a compact en-US implementation of the parts pages
// use (dates, relative times, numbers, plurals, lists).
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const WDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const pad2 = n => (n < 10 ? '0' : '') + n;
const ilocale = () => ({ locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' });
class DateTimeFormat {
    constructor(loc, o) {
        o = Object.assign({}, o || {});
        const ds = o.dateStyle, ts = o.timeStyle;
        if (ds) {
            if (ds === 'full') Object.assign(o, { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' });
            else if (ds === 'long') Object.assign(o, { year: 'numeric', month: 'long', day: 'numeric' });
            else if (ds === 'medium') Object.assign(o, { year: 'numeric', month: 'short', day: 'numeric' });
            else Object.assign(o, { year: '2-digit', month: 'numeric', day: 'numeric' });
        }
        if (ts) Object.assign(o, { hour: 'numeric', minute: '2-digit' }, ts === 'short' ? {} : { second: '2-digit' });
        if (!o.weekday && !o.year && !o.month && !o.day && !o.hour && !o.minute && !o.second)
            Object.assign(o, { year: 'numeric', month: 'numeric', day: 'numeric' });
        hidden(this, '_o', o);
    }
    formatToParts(d) {
        const t = new Date(d === undefined ? Date.now() : +d);
        if (isNaN(t)) throw new RangeError('Invalid time value');
        const o = this._o, P = [];
        const lit = v => P.push({ type: 'literal', value: v });
        const textMonth = o.month === 'short' || o.month === 'long' || o.month === 'narrow';
        if (o.weekday) { const w = WDAYS[t.getUTCDay()]; P.push({ type: 'weekday', value: o.weekday === 'long' ? w : o.weekday === 'narrow' ? w[0] : w.slice(0, 3) }); if (o.month || o.day || o.year) lit(', '); }
        if (textMonth) {
            const m = MONTHS[t.getUTCMonth()];
            P.push({ type: 'month', value: o.month === 'long' ? m : o.month === 'narrow' ? m[0] : m.slice(0, 3) });
            if (o.day) { lit(' '); P.push({ type: 'day', value: o.day === '2-digit' ? pad2(t.getUTCDate()) : String(t.getUTCDate()) }); }
            if (o.year) { lit(o.day ? ', ' : ' '); P.push({ type: 'year', value: o.year === '2-digit' ? pad2(t.getUTCFullYear() % 100) : String(t.getUTCFullYear()) }); }
        } else if (o.month || o.day || o.year) {
            const parts = [];
            if (o.month) parts.push({ type: 'month', value: o.month === '2-digit' ? pad2(t.getUTCMonth() + 1) : String(t.getUTCMonth() + 1) });
            if (o.day) parts.push({ type: 'day', value: o.day === '2-digit' ? pad2(t.getUTCDate()) : String(t.getUTCDate()) });
            if (o.year) parts.push({ type: 'year', value: o.year === '2-digit' ? pad2(t.getUTCFullYear() % 100) : String(t.getUTCFullYear()) });
            parts.forEach((p, i) => { if (i) lit('/'); P.push(p); });
        }
        if (o.hour || o.minute || o.second) {
            if (P.length) lit(', ');
            const h24 = o.hour12 === false || o.hourCycle === 'h23';
            let h = t.getUTCHours();
            const pm = h >= 12;
            if (!h24) { h = h % 12; if (h === 0) h = 12; }
            if (o.hour) P.push({ type: 'hour', value: o.hour === '2-digit' || h24 ? pad2(h) : String(h) });
            if (o.minute) { if (o.hour) lit(':'); P.push({ type: 'minute', value: pad2(t.getUTCMinutes()) }); }
            if (o.second) { lit(':'); P.push({ type: 'second', value: pad2(t.getUTCSeconds()) }); }
            if (o.hour && !h24) { lit(' '); P.push({ type: 'dayPeriod', value: pm ? 'PM' : 'AM' }); }
            if (o.timeZoneName) { lit(' '); P.push({ type: 'timeZoneName', value: 'UTC' }); }
        }
        return P;
    }
    format(d) { return this.formatToParts(d).map(p => p.value).join(''); }
    formatRange(a, b) { return this.format(a) + ' – ' + this.format(b); }
    resolvedOptions() { return Object.assign(ilocale(), this._o); }
    static supportedLocalesOf() { return ['en-US']; }
}
const RT_UNITS = { second: 1, minute: 1, hour: 1, day: 1, week: 1, month: 1, quarter: 1, year: 1 };
const RT_AUTO = { day: ['yesterday', 'today', 'tomorrow'], week: ['last week', 'this week', 'next week'],
    month: ['last month', 'this month', 'next month'], year: ['last year', 'this year', 'next year'],
    quarter: ['last quarter', 'this quarter', 'next quarter'], hour: [null, 'this hour', null],
    minute: [null, 'this minute', null], second: [null, 'now', null] };
class RelativeTimeFormat {
    constructor(loc, o) { hidden(this, '_o', Object.assign({ numeric: 'always', style: 'long' }, o || {})); }
    format(v, unit) {
        v = Number(v);
        unit = String(unit).replace(/s$/, '');
        if (!RT_UNITS[unit]) throw new RangeError('Invalid unit argument for format() \'' + unit + '\'');
        if (this._o.numeric === 'auto' && (v === -1 || v === 0 || v === 1)) {
            const w = RT_AUTO[unit][v + 1];
            if (w) return w;
        }
        const n = Math.abs(v);
        let u = unit;
        if (this._o.style !== 'long') u = { second: 'sec.', minute: 'min.', hour: 'hr.', day: 'day', week: 'wk.', month: 'mo.', quarter: 'qtr.', year: 'yr.' }[unit];
        const label = new NumberFormat().format(n) + ' ' + u + (n === 1 || this._o.style !== 'long' ? '' : 's');
        return (v < 0 || Object.is(v, -0)) ? label + ' ago' : 'in ' + label;
    }
    formatToParts(v, unit) { return [{ type: 'literal', value: this.format(v, unit) }]; }
    resolvedOptions() { return Object.assign(ilocale(), this._o); }
    static supportedLocalesOf() { return ['en-US']; }
}
class NumberFormat {
    constructor(loc, o) { hidden(this, '_o', Object.assign({}, o || {})); }
    format(n) {
        const o = this._o;
        n = Number(n);
        if (!isFinite(n)) return isNaN(n) ? 'NaN' : (n < 0 ? '-∞' : '∞');
        let suffix = '', prefix = '';
        if (o.style === 'percent') { n *= 100; suffix = '%'; }
        if (o.notation === 'compact') {
            const a = Math.abs(n);
            const tiers = [[1e12, 'T'], [1e9, 'B'], [1e6, 'M'], [1e3, 'K']];
            for (const [v, s] of tiers) if (a >= v) {
                const x = n / v;
                return (Math.abs(x) < 100 ? +x.toFixed(Math.abs(x) < 10 ? 1 : 0) : Math.round(x)) + (o.compactDisplay === 'long' ? ' ' + { T: 'trillion', B: 'billion', M: 'million', K: 'thousand' }[s] : s);
            }
        }
        let min = o.minimumFractionDigits, max = o.maximumFractionDigits;
        if (o.style === 'currency') {
            prefix = { USD: '$', EUR: '€', GBP: '£', JPY: '¥' }[String(o.currency).toUpperCase()] || (String(o.currency) + ' ');
            if (min === undefined) min = 2;
            if (max === undefined) max = Math.max(2, min);
        }
        if (min === undefined) min = 0;
        if (max === undefined) max = Math.max(min, o.style === 'percent' ? 0 : 3);
        let s = Math.abs(n).toFixed(max);
        if (s.indexOf('.') >= 0) { s = s.replace(/0+$/, ''); const fl = s.length - s.indexOf('.') - 1; if (fl < min) s += '0'.repeat(min - fl); s = s.replace(/\.$/, ''); }
        else if (min > 0) s += '.' + '0'.repeat(min);
        const parts = s.split('.');
        if (o.useGrouping !== false) parts[0] = parts[0].replace(/\B(?=(\d{3})+(?!\d))/g, ',');
        return (n < 0 ? '-' : '') + prefix + parts.join('.') + suffix;
    }
    formatToParts(n) { return [{ type: 'integer', value: this.format(n) }]; }
    formatRange(a, b) { return this.format(a) + '–' + this.format(b); }
    resolvedOptions() { return Object.assign(ilocale(), this._o); }
    static supportedLocalesOf() { return ['en-US']; }
}
class PluralRules {
    constructor(loc, o) { hidden(this, '_o', Object.assign({ type: 'cardinal' }, o || {})); }
    select(n) {
        n = Number(n);
        if (this._o.type === 'ordinal') {
            const a = n % 10, b = n % 100;
            return a === 1 && b !== 11 ? 'one' : a === 2 && b !== 12 ? 'two' : a === 3 && b !== 13 ? 'few' : 'other';
        }
        return n === 1 ? 'one' : 'other';
    }
    resolvedOptions() { return Object.assign(ilocale(), this._o, { pluralCategories: ['one', 'other'] }); }
    static supportedLocalesOf() { return ['en-US']; }
}
class Collator {
    constructor(loc, o) { hidden(this, '_o', Object.assign({}, o || {})); }
    compare(a, b) {
        a = String(a); b = String(b);
        if (this._o.sensitivity === 'base' || this._o.sensitivity === 'accent') { a = a.toLowerCase(); b = b.toLowerCase(); }
        if (this._o.numeric) {
            const re = /(\d+)|(\D+)/g, x = a.match(re) || [], y = b.match(re) || [];
            for (let i = 0; i < Math.min(x.length, y.length); i++) {
                const p = x[i], q = y[i];
                if (/^\d/.test(p) && /^\d/.test(q)) { if (+p !== +q) return +p < +q ? -1 : 1; }
                else if (p !== q) return p < q ? -1 : 1;
            }
            return x.length - y.length;
        }
        return a < b ? -1 : a > b ? 1 : 0;
    }
    resolvedOptions() { return Object.assign(ilocale(), this._o); }
    static supportedLocalesOf() { return ['en-US']; }
}
class ListFormat {
    constructor(loc, o) { hidden(this, '_o', Object.assign({ type: 'conjunction' }, o || {})); }
    format(list) {
        const a = Array.from(list, String);
        const w = this._o.type === 'disjunction' ? 'or' : 'and';
        if (a.length <= 1) return a.join('');
        if (a.length === 2) return a[0] + ' ' + w + ' ' + a[1];
        return a.slice(0, -1).join(', ') + ', ' + w + ' ' + a[a.length - 1];
    }
    formatToParts(list) { return [{ type: 'literal', value: this.format(list) }]; }
}
class Segmenter {
    constructor(loc, o) { hidden(this, '_g', (o && o.granularity) || 'grapheme'); }
    segment(s) {
        s = String(s);
        const out = [];
        if (this._g === 'word') { let i = 0; for (const m of s.split(/(\s+)/)) { if (m) out.push({ segment: m, index: i, input: s, isWordLike: !/^\s+$/.test(m) }); i += m.length; } }
        else { let i = 0; for (const c of s) { out.push({ segment: c, index: i, input: s }); i += c.length; } }
        out.containing = idx => out.find(x => idx >= x.index && idx < x.index + x.segment.length);
        return out;
    }
}
class DisplayNames { constructor() {} of(code) { return String(code); } }
class Locale {
    constructor(tag) { tag = String(tag || 'en-US'); this.baseName = tag; this.language = tag.split('-')[0]; this.region = tag.split('-')[1]; }
    toString() { return this.baseName; }
    maximize() { return this; } minimize() { return this; }
}
// ECMA-402: DateTimeFormat, NumberFormat and Collator also work when called without new
const callableCtor = C => {
    const F = function (locales, options) { return new C(locales, options); };
    F.prototype = C.prototype;
    hidden(C.prototype, 'constructor', F);
    Object.defineProperty(F, 'name', { value: C.name });
    hidden(F, 'supportedLocalesOf', l => [].concat(l || []));
    return F;
};
G.Intl = { DateTimeFormat: callableCtor(DateTimeFormat), RelativeTimeFormat, NumberFormat: callableCtor(NumberFormat), PluralRules,
    Collator: callableCtor(Collator), ListFormat, Segmenter, DisplayNames, Locale,
    getCanonicalLocales: l => [].concat(l || []), supportedValuesOf: () => [] };
Date.prototype.toLocaleDateString = function (l, o) {
    o = o || {};
    if (!o.year && !o.month && !o.day && !o.weekday && !o.dateStyle) o = Object.assign({ year: 'numeric', month: 'numeric', day: 'numeric' }, o);
    return new DateTimeFormat(l, o).format(this);
};
Date.prototype.toLocaleTimeString = function (l, o) {
    o = o || {};
    if (!o.hour && !o.minute && !o.second && !o.timeStyle) o = Object.assign({ hour: 'numeric', minute: '2-digit', second: '2-digit' }, o);
    return new DateTimeFormat(l, o).format(this);
};
Date.prototype.toLocaleString = function (l, o) {
    if (o && Object.keys(o).length) return new DateTimeFormat(l, o).format(this);
    return this.toLocaleDateString() + ', ' + this.toLocaleTimeString();
};
Number.prototype.toLocaleString = function (l, o) { return new NumberFormat(l, o).format(this); };

// ---------------------------------------------------------------- ES additions
// Array.fromAsync (ES2026): async iterables, sync iterables (values awaited) and
// array-likes; `this` may be a constructor, like Array.from
if (typeof Array.fromAsync !== 'function') {
    const isCtor = C => { try { Reflect.construct(String, [], C); return true; } catch (_) { return false; } };
    const put = (A, k, v) => Object.defineProperty(A, k, { value: v, writable: true, enumerable: true, configurable: true });
    const fromAsync = async function fromAsync(items, mapfn = undefined, thisArg = undefined) {
        const C = this, mapping = mapfn !== undefined;
        if (mapping && typeof mapfn !== 'function') throw new TypeError('Array.fromAsync: mapper is not a function');
        const asyncIt = items == null ? undefined : items[Symbol.asyncIterator];
        const syncIt = asyncIt == null && items != null ? items[Symbol.iterator] : undefined;
        if (asyncIt != null || syncIt != null) {
            const A = isCtor(C) ? new C() : [];
            let k = 0;
            const it = asyncIt != null ? asyncIt.call(items) : syncIt.call(items);
            const source = asyncIt != null ? { [Symbol.asyncIterator]: () => it } : { [Symbol.iterator]: () => it };
            for await (const v of source) {
                put(A, k, mapping ? await mapfn.call(thisArg, v, k) : v);
                k++;
            }
            A.length = k;
            return A;
        }
        const arrayLike = Object(items);
        const len = Math.min(Math.max(Math.trunc(Number(arrayLike.length)) || 0, 0), Number.MAX_SAFE_INTEGER);
        const A = isCtor(C) ? new C(len) : new Array(len);
        for (let k = 0; k < len; k++) {
            const v = await arrayLike[k];
            put(A, k, mapping ? await mapfn.call(thisArg, v, k) : v);
        }
        A.length = len;
        return A;
    };
    hidden(Array, 'fromAsync', fromAsync);
}

// lifecycle: listeners added after the event already fired still run once
let dclFired = false, loadFired = false;
function lateLifecycle(type, rec) {
    if ((type === 'DOMContentLoaded' && dclFired) || (type === 'load' && loadFired)) {
        // spec: no; reality: many loaders check readyState first. Only
        // listeners added *during* the dispatch would be missed: ignore.
    }
}
function setReady(s) {
    readyState = s;
    dispatch(document, new Event('readystatechange'));
}
function fireDCL() {
    if (readyState === 'loading') setReady('interactive');
    dclFired = true;
    dispatch(document, new Event('DOMContentLoaded', { bubbles: true }));
    try { mediaScan(); } catch (e) { report(e, 'media'); }
}
function fireLoad() {
    setReady('complete');
    loadFired = true;
    dispatch(G, new Event('load'));
    dispatch(G, new PageTransitionEvent('pageshow', { persisted: false }));
}
// C: an external/dynamic <script> finished (fires load/error on the element)
function scriptEvent(el, ok) { dispatch(el, new Event(ok ? 'load' : 'error')); }
// C: an <img> finished loading (or failed)
function imageEvent(el, ok) { dispatch(el, new Event(ok ? 'load' : 'error')); }
// C: UI events from the shell
function uiEvent(target, type, x, y, button, key) {
    let ev;
    const base = { bubbles: true, cancelable: true, view: G, clientX: x, clientY: y, button, buttons: button ? 0 : 1 };
    if (type === 'click' || type === 'mousedown' || type === 'mouseup' || type === 'mousemove' || type === 'mouseover' || type === 'mouseout' || type === 'dblclick' || type === 'contextmenu' || type === 'auxclick')
        ev = new MouseEvent(type, base);
    else if (type === 'pointerdown' || type === 'pointerup' || type === 'pointermove')
        ev = new PointerEvent(type, base);
    else if (type === 'keydown' || type === 'keyup' || type === 'keypress') {
        const k = key === 13 ? 'Enter' : key === 8 ? 'Backspace' : key === 9 ? 'Tab' : key === 27 ? 'Escape' : String.fromCharCode(key);
        ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key: k, code: k.length === 1 ? 'Key' + k.toUpperCase() : k, keyCode: key === 13 ? 13 : key, charCode: type === 'keypress' ? key : 0 });
    } else if (type === 'input') ev = new InputEvent('input', { bubbles: true, data: key ? String.fromCharCode(key) : null, inputType: key === 8 ? 'deleteContentBackward' : 'insertText' });
    else if (type === 'change') ev = new Event('change', { bubbles: true });
    else if (type === 'focus' || type === 'blur') ev = new FocusEvent(type, {});
    else if (type === 'focusin' || type === 'focusout') ev = new FocusEvent(type, { bubbles: true });
    else if (type === 'submit') ev = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: null });
    else if (type === 'scroll') ev = new Event('scroll', { bubbles: target === document });
    else if (type === 'resize') ev = new UIEvent('resize', {});
    else if (type === 'wheel') ev = new WheelEvent('wheel', Object.assign(base, { deltaY: key }));
    else ev = new Event(type, { bubbles: true, cancelable: true });
    hidden(ev, 'isTrusted', true);
    if (type === 'click' || type === 'mousedown' || type === 'keydown' || type === 'pointerdown') {
        if (!userActivated) {
            userActivated = true;
            for (const m of W.query(document, 'video,audio', true)) if (mst(m).deferred) mediaLoad(m);
        }
    }
    return dispatch(target || G, ev);
}
function dispatchClick(el) {
    if (el.disabled) return;
    const ev = new MouseEvent('click', { bubbles: true, cancelable: true, view: G, button: 0 });
    if (!dispatch(el, ev)) return;
    activate(el);   // default action: links, submit buttons, checkboxes
}
// default activation behaviour of a script-initiated click()
function activate(el) {
    const a = el.closest('a[href],area[href]');
    if (a) { navigate(a.href, false); return; }
    const name = W.name(el);
    const type = name === 'input' ? el.type : name === 'button' ? el.type : '';
    if (name === 'input' && (type === 'checkbox' || type === 'radio')) {
        if (type === 'checkbox') el.checked = !el.checked; else el.checked = true;
        dispatch(el, new InputEvent('input', { bubbles: true }));
        dispatch(el, new Event('change', { bubbles: true }));
        return;
    }
    if ((name === 'button' || name === 'input') && (type === 'submit' || type === 'image')) {
        const f = el.form;
        if (f) f.requestSubmit(el);
        return;
    }
    if (name === 'label') { const c = el.control; if (c && c !== el) dispatchClick(c); return; }
    if (name === 'summary') { const d = W.parent(el); if (d && W.name(d) === 'details') d.open = !d.open; }
}

// hide the natives; freeze what pages must not break
return { dispatch, uiEvent, onFetch, nextDue, runDue, runFrame, fireDCL, fireLoad, setReady, scriptEvent, imageEvent,
         protoFor, report, moChildList, ceConnect, ceUpgradeTree, dispatchClick, observersCheck };
})

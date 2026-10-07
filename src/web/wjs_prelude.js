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
    'selectionchange', 'storage', 'submit', 'timeupdate', 'toggle', 'touchcancel', 'touchend', 'touchmove',
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
const HM = HTMLMediaElement.prototype;
reflectURL(HM, 'src', 'src');
for (const a of ['autoplay', 'loop', 'muted', 'controls', 'playsInline']) reflectBool(HM, a, a.toLowerCase());
Object.assign(HM, { paused: true, currentTime: 0, duration: NaN, volume: 1, readyState: 0, networkState: 3, ended: false, playbackRate: 1 });
methods(HM, { play() { return Promise.reject(new DOMException('media playback is not supported', 'NotSupportedError')); }, pause() {}, load() {}, canPlayType() { return ''; } });
reflectURL(HTMLVideoElement.prototype, 'poster', 'poster');
const HC = HTMLCanvasElement.prototype;
reflectInt(HC, 'width', 'width', 300);
reflectInt(HC, 'height', 'height', 150);
methods(HC, { getContext() { return null; }, toDataURL() { return 'data:,'; }, toBlob(cb) { setTimeout(() => cb(null), 0); } });
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

// IntersectionObserver / ResizeObserver: report everything as visible once
class IntersectionObserver {
    constructor(cb, opts) { hidden(this, '_cb', cb); this.root = (opts && opts.root) || null; this.rootMargin = (opts && opts.rootMargin) || '0px'; this.thresholds = [0]; hidden(this, '_els', []); }
    observe(el) {
        if (this._els.includes(el)) return;
        this._els.push(el);
        setTimeout(() => {
            if (!this._els.includes(el)) return;
            const r = rectOf(el);
            const v = W.view();
            const inter = r.width > 0 || r.height > 0;
            try {
                this._cb([{ target: el, isIntersecting: inter, intersectionRatio: inter ? 1 : 0, boundingClientRect: r,
                    intersectionRect: r, rootBounds: new DOMRect(0, 0, v[0], v[1]), time: W.now() }], this);
            } catch (e) { report(e, 'IntersectionObserver callback'); }
        }, 0);
    }
    unobserve(el) { const i = this._els.indexOf(el); if (i >= 0) this._els.splice(i, 1); }
    disconnect() { this._els.length = 0; }
    takeRecords() { return []; }
}
class ResizeObserver {
    constructor(cb) { hidden(this, '_cb', cb); hidden(this, '_els', []); }
    observe(el) {
        this._els.push(el);
        setTimeout(() => {
            if (!this._els.includes(el)) return;
            const r = rectOf(el);
            const box = [{ inlineSize: r.width, blockSize: r.height }];
            try { this._cb([{ target: el, contentRect: r, borderBoxSize: box, contentBoxSize: box, devicePixelContentBoxSize: box }], this); }
            catch (e) { report(e, 'ResizeObserver callback'); }
        }, 0);
    }
    unobserve(el) { const i = this._els.indexOf(el); if (i >= 0) this._els.splice(i, 1); }
    disconnect() { this._els.length = 0; }
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
    static createObjectURL() { return 'blob:' + location.origin + '/' + crypto.randomUUID(); }
    static revokeObjectURL() {}
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
function addTimer(fn, ms, args, repeat) {
    if (typeof fn !== 'function') {
        const code = String(fn);
        fn = () => (0, eval)(code);
    }
    ms = +ms || 0;
    if (ms < 4) ms = repeat ? 10 : 0;
    const id = timerSeq++;
    TIMERS.set(id, { fn, args, due: W.now() + ms, interval: repeat ? Math.max(ms, 10) : -1 });
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
    try { best.fn.apply(G, best.args); } catch (e) { report(e, 'timer'); }
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
    let bodyStr = null;
    if (body !== null && body !== undefined) {
        if (typeof body === 'string') bodyStr = body;
        else if (body instanceof URLSearchParams) bodyStr = body.toString();
        else if (body instanceof FormData) bodyStr = body._urlencoded();
        else if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) bodyStr = new TextDecoder().decode(body);
        else if (body instanceof Blob) bodyStr = body._text();
        else bodyStr = String(body);
    }
    const hs = [];
    for (const [k, v] of headers) hs.push(k + ': ' + v);
    const id = W.fetch(String(url), String(method).toUpperCase(), hs.join('\r\n'), bodyStr);
    if (id < 0) { setTimeout(() => handler(0, '', '', null, url), 0); return; }
    PENDING.set(id, handler);
}
// C: a request finished
function onFetch(id, status, statusText, headersRaw, body, url) {
    const h = PENDING.get(id);
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
    get body() { return null; }
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
        req.signal.addEventListener('abort', () => { if (!done) { done = true; reject(req.signal.reason); } });
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
    mediaDevices: { enumerateDevices() { return Promise.resolve([]); }, getUserMedia() { return Promise.reject(new DOMException('not supported', 'NotSupportedError')); } },
    storage: { estimate() { return Promise.resolve({ quota: 0, usage: 0 }); }, persist() { return Promise.resolve(false); }, persisted() { return Promise.resolve(false); } },
    connection: { effectiveType: '3g', downlink: 1, rtt: 300, saveData: false, addEventListener() {}, removeEventListener() {} },
    userActivation: { hasBeenActive: true, isActive: false },
    locks: { request(name, opts, cb) { cb = typeof opts === 'function' ? opts : cb; return Promise.resolve().then(() => cb({ name })); } },
    getBattery() { return Promise.reject(new DOMException('not supported', 'NotSupportedError')); },
    getGamepads() { return []; }
};
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
    XMLHttpRequest, TextEncoder, TextDecoder, Storage, MutationObserver, MutationRecord, IntersectionObserver, ResizeObserver,
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
// QuickJS ships no Intl: a compact en-US implementation of the parts pages
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
G.Intl = { DateTimeFormat, RelativeTimeFormat, NumberFormat, PluralRules, Collator, ListFormat, Segmenter, DisplayNames, Locale,
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
         protoFor, report, moChildList, ceConnect, ceUpgradeTree, dispatchClick };
})

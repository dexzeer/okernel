// WB_EVAL probe for the YouTube home page: did the app build its shell and the video grid? (one console.log per fact: a log line is short)
(function () {
    const log = (s) => console.log('PROBE ' + s);
    for (const sel of ['ytd-app', 'ytd-masthead', '#masthead-container', 'ytd-page-manager', 'ytd-browse', 'ytd-two-column-browse-results-renderer',
                       'ytd-rich-grid-renderer', 'ytd-rich-item-renderer', 'ytd-guide-renderer', 'ytd-mini-guide-renderer', '#search-input', '#logo']) {
        const e = document.querySelector(sel);
        if (!e) { log(sel + ': none'); continue; }
        const r = e.getBoundingClientRect(), cs = getComputedStyle(e);
        log(sel + ' (x' + document.querySelectorAll(sel).length + '): ' + Math.round(r.x) + ',' + Math.round(r.y) + ' ' + Math.round(r.width) + 'x' + Math.round(r.height) +
            ' ' + cs.display + ' ' + cs.visibility + ' op=' + cs.opacity + ' children=' + e.children.length);
    }
    const d = window.ytInitialData;
    log('ytInitialData: ' + (typeof d) + (d ? ' keys=' + Object.keys(d).join(',') : ''));
    if (d && d.contents) log('contents keys: ' + Object.keys(d.contents).join(','));
    log('ytd-app defined: ' + (customElements.get('ytd-app') ? 'yes' : 'no') + ', ytd-rich-grid-renderer: ' + (customElements.get('ytd-rich-grid-renderer') ? 'yes' : 'no') +
        ', ytd-rich-item-renderer: ' + (customElements.get('ytd-rich-item-renderer') ? 'yes' : 'no'));
    log('body children: ' + Array.from(document.body.children).map(e => e.tagName.toLowerCase() + '(' + e.children.length + ')').join(' '));
    log('innerText ' + document.body.innerText.length + ': ' + document.body.innerText.slice(0, 200).replace(/\s+/g, ' '));
    const g = document.querySelector('ytd-rich-grid-renderer');
    if (g) {
        log('grid children: ' + Array.from(g.children).map(e => e.tagName.toLowerCase() + '#' + e.id + '[' + e.children.length + ']').join(' '));
        const c = g.querySelector('#contents');
        if (c) log('grid #contents children=' + c.children.length + ' ' + Array.from(c.children).slice(0, 4).map(e => e.tagName.toLowerCase()).join(' '));
        log('grid data: ' + (g.data ? Object.keys(g.data).join(',') : 'none'));
        if (g.data && g.data.contents) log('grid data.contents length ' + g.data.contents.length + ' first: ' + Object.keys(g.data.contents[0] || {}).join(','));
    }
    const m = document.querySelector('ytd-masthead');
    if (m) log('masthead children: ' + Array.from(m.children).map(e => e.tagName.toLowerCase() + '#' + e.id).join(' ') + ' hidden=' + m.hidden + ' attrs=' + Array.from(m.attributes).map(a => a.name).join(','));
    const b = document.querySelector('ytd-browse');
    if (b) log('browse attrs=' + Array.from(b.attributes).map(a => a.name).join(',') + ' data=' + (b.data ? Object.keys(b.data).join(',') : 'none'));
    return 'done';
})()

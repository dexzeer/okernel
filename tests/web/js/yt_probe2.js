(function () {
    const out = [];
    const bodies = document.querySelectorAll('body');
    out.push('bodies: ' + bodies.length + ' document.body is first: ' + (bodies[0] === document.body));
    function path(e) { const p = []; let n = 0; while (e && n++ < 30) { p.push(e.localName + (e.id ? '#' + e.id : '')); e = e.parentElement; } return p.join(' < '); }
    out.push('path(body): ' + path(document.body));
    out.push('path(ytd-watch-flexy): ' + path(document.querySelector('ytd-watch-flexy')));
    out.push('html kids: ' + Array.from(document.documentElement.children).map(c => c.localName).join(','));
    out.push('document.body.parentNode === html: ' + (document.body.parentNode === document.documentElement));
    return out.join('\n');
})()

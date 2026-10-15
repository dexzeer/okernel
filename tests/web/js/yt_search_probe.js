(function () {
    const log = (s) => console.log('PROBE ' + s);
    const s = document.querySelector('ytd-search'), r = document.querySelector('ytd-two-column-search-results-renderer');
    log('data.hideContents=' + s.data.hideContents + ' typeof=' + typeof s.data.hideContents + ' active=' + s.active);
    const d = Object.getOwnPropertyDescriptor(r, 'hidden');
    log('own hidden descriptor: ' + (d ? Object.keys(d).join(',') + (d.get ? ' accessor' : ' value=' + d.value) : 'none'));
    let p = Object.getPrototypeOf(r), n = 0;
    while (p && n++ < 6) { const dd = Object.getOwnPropertyDescriptor(p, 'hidden'); if (dd) { log('proto level ' + n + ' has hidden: ' + (dd.get ? 'accessor ' + String(dd.set).slice(0, 120) : 'value')); } p = Object.getPrototypeOf(p); }
    r.hidden = undefined; log('after r.hidden=undefined: ' + r.hidden + ' attr=' + r.hasAttribute('hidden'));
    return 'done';
})()

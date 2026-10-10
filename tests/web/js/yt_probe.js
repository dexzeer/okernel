// WB_EVAL probe for the YouTube watch page: what the player and the layout look like.
(function () {
    const out = [];
    const v = document.querySelector('video');
    out.push('video: ' + (v ? v.videoWidth + 'x' + v.videoHeight + ' t=' + v.currentTime.toFixed(2) + ' rs=' + v.readyState + ' paused=' + v.paused : 'none'));
    for (const sel of ['video', '#movie_player', '#player', 'ytd-masthead', '#masthead-container', '#primary', '#secondary', 'ytd-watch-flexy', '#search-input', '#logo', 'ytd-page-manager']) {
        const e = document.querySelector(sel);
        if (!e) { out.push(sel + ': none'); continue; }
        const r = e.getBoundingClientRect(), cs = getComputedStyle(e);
        out.push(sel + ': ' + Math.round(r.x) + ',' + Math.round(r.y) + ' ' + Math.round(r.width) + 'x' + Math.round(r.height) + ' ' + cs.display + ' ' + cs.visibility + ' op=' + cs.opacity + ' pos=' + cs.position);
    }
    let vis = 0, big = 0, hid = 0;
    for (const e of document.body.querySelectorAll('*')) {
        const r = e.getBoundingClientRect();
        if (r.width > 0 && r.height > 0) { vis++; if (r.width > 100 && r.height > 20) big++; }
        if (getComputedStyle(e).visibility === 'hidden') hid++;
    }
    out.push('elements with area: ' + vis + ' (big ' + big + '), visibility:hidden: ' + hid + ', innerText length ' + document.body.innerText.length);
    out.push('html bg=' + getComputedStyle(document.documentElement).backgroundColor + ' body bg=' + getComputedStyle(document.body).backgroundColor);
    return out.join('\n');
})()

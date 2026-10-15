(function () {
    const log = (s) => console.log('PROBE ' + s);
    const f = document.querySelector('ytd-watch-flexy'), cs = getComputedStyle(f);
    for (const v of ['--ytd-watch-flexy-width-ratio', '--ytd-watch-flexy-height-ratio', '--ytd-watch-flexy-sidebar-width', '--ytd-watch-flexy-min-player-width', '--ytd-watch-flexy-max-player-width', '--ytd-watch-flexy-min-player-height', '--ytd-watch-flexy-max-player-height', '--ytd-watch-flexy-max-player-width-wide-screen']) log(v + '=[' + cs.getPropertyValue(v) + ']');
    const o = document.querySelector('#player-container-outer'); const co = getComputedStyle(o);
    log('outer maxw=' + co.maxWidth + ' minw=' + co.minWidth + ' w=' + co.width);
    const p = document.querySelector('ytd-player#player, #player'); log('player tag=' + p.tagName + ' class=' + p.className + ' attrs=' + Array.from(p.attributes).map(a => a.name).join(','));
    return 'x';
})()

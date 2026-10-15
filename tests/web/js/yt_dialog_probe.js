(function () {
    const log = (s) => console.log('PROBE ' + s);
    const C = customElements.get('yt-formatted-string');
    log('defined=' + !!C);
    const all = Array.from(document.querySelectorAll('yt-formatted-string'));
    let up = 0, notup = 0, inDlg = 0, inDlgUp = 0;
    const dlg = document.querySelector('ytd-consent-bump-v2-lightbox');
    for (const e of all) { const u = C && e instanceof C; if (u) up++; else notup++; if (dlg.contains(e)) { inDlg++; if (u) inDlgUp++; } }
    log('total=' + all.length + ' upgraded=' + up + ' not=' + notup + ' in dialog=' + inDlg + ' upgraded there=' + inDlgUp);
    const e = Array.from(dlg.querySelectorAll('#content yt-formatted-string'))[0];
    log('first: instanceof=' + (e instanceof C) + ' isConnected=' + e.isConnected + ' own props=' + Object.getOwnPropertyNames(e).slice(0, 12).join(',') + ' proto=' + Object.getPrototypeOf(e).constructor.name);
    // try upgrading by hand
    try { customElements.upgrade(e); log('after upgrade: instanceof=' + (e instanceof C) + ' is-empty=' + e.hasAttribute('is-empty') + ' text=' + JSON.stringify(e.text).slice(0, 60)); } catch (x) { log('upgrade threw ' + x); }
    return 'x';
})()

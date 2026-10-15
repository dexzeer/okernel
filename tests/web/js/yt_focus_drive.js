// WB_MID_EVAL: click-focus the YouTube search box and watch who moves the focus away.
(function () {
    const log = (s) => console.log('DRIVE ' + s);
    const inp = document.querySelector('input#search, input[name=search_query]');
    if (!inp) { log('no input'); return 'x'; }
    const orig = HTMLElement.prototype.focus;
    HTMLElement.prototype.focus = function () {
        log('focus() on ' + this.tagName + '#' + this.id + '.' + String(this.className).slice(0, 40) + ' from ' + new Error().stack.split('\n').slice(2, 5).map(l => l.trim().slice(-80)).join(' <- '));
        return orig.apply(this, arguments);
    };
    inp.focus();
    log('active after focus: ' + document.activeElement.tagName);
    setTimeout(() => log('active 2s later: ' + document.activeElement.tagName + '#' + document.activeElement.id + '.' + String(document.activeElement.className).slice(0, 40)), 2000);
    return 'ok';
})()

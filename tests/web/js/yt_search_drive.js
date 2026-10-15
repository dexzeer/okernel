// WB_MID_EVAL: drive the YouTube search box from script (what a real click/typing sequence does), to see which step fails.
(function () {
    const log = (s) => console.log('DRIVE ' + s);
    const inp = document.querySelector('input#search, input[name=search_query]');
    if (!inp) { log('no input'); return 'x'; }
    log('input rect ' + JSON.stringify(inp.getBoundingClientRect()));
    inp.focus();
    inp.value = 'linux';
    inp.dispatchEvent(new InputEvent('input', { bubbles: true, data: 'x', inputType: 'insertText' }));
    log('typed; active=' + (document.activeElement === inp));
    const steps = [
        [2500, () => {
            const rows = document.querySelectorAll('[role=option], .ytSuggestionComponentSuggestion, .sbsb_c, yt-formatted-string');
            log('option-like elements: ' + rows.length);
            const opt = document.querySelector('[role=option]');
            if (!opt) { log('no suggestion rows'); return; }
            const r = opt.getBoundingClientRect();
            log('first option ' + opt.tagName + '.' + opt.className + ' rect ' + JSON.stringify(r) + ' text=' + opt.textContent.trim().slice(0, 30));
            const cx = r.x + r.width / 2, cy = r.y + r.height / 2;
            const at = document.elementFromPoint(cx, cy);
            log('elementFromPoint -> ' + (at ? at.tagName + '.' + at.className + ' inside option=' + opt.contains(at) : 'null'));
            for (const t of ['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click'])
                (at || opt).dispatchEvent(new MouseEvent(t, { bubbles: true, cancelable: true, clientX: cx, clientY: cy, button: 0, view: window }));
            log('dispatched click');
        }],
        [9000, () => { log('location=' + location.href + ' video results=' + document.querySelectorAll('ytd-video-renderer').length + ' input=' + inp.value); }],
    ];
    for (const [ms, f] of steps) setTimeout(() => { try { f(); } catch (e) { log('step threw ' + e + ' ' + String(e.stack).slice(0, 200)); } }, ms);
    return 'started';
})()

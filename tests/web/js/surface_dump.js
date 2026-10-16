// WB_EVAL snippet for the host harness: print the surface.js dump, 30 entries per console.log line (the console budget is
// 400 lines); entries are separated by " ;; "
(function () {
    var a = __surface.split('\n');
    for (var i = 0; i < a.length; i += 30) console.log('S|' + a.slice(i, i + 30).join(' ;; '));
})();

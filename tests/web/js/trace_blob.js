// Counts Blob constructions (how many, how many bytes, how long) on a page: WB_PRE=tests/web/js/trace_blob.js WB_DUMP=__blobStats WB_DUMP_FILE=out.txt build-host/wbrowse URL ...
(function () {
    var B = window.Blob, n = 0, bytes = 0, ms = 0, big = [];
    window.Blob = class extends B {
        constructor(p, o) { var t = Date.now(); super(p, o); var d = Date.now() - t; ms += d; n++; bytes += this.size; if (d >= 5 && big.length < 8) big.push(this.size + 'B ' + d + 'ms type=' + this.type); }
    };
    window.__blobStats = function () { return 'blobs=' + n + ' bytes=' + bytes + ' ms=' + ms + ' slow: ' + big.join(' | '); };
})();

importScripts('worker_lib.js');
var shown = 'main';
onmessage = function (e) { postMessage([libValue(), shown, libCounter + 1]); };

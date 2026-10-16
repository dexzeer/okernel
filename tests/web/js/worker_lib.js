// a classic library: top-level var/function become globals of the worker
var libCounter = 2;
function libValue() { return 'lib:' + libCounter; }

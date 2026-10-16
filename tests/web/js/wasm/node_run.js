// node runner: node node_run.js t_x.js  (runs lib.js, then the test in the same global scope); WASM_PRE = JS to run first
const fs = require('fs'), path = require('path'), vm = require('vm');
if (process.env.WASM_PRE) vm.runInThisContext(process.env.WASM_PRE);
for (const f of ['lib.js', 'dis.js', process.argv[2]]) vm.runInThisContext(fs.readFileSync(path.join(__dirname, f), 'utf8'), { filename: f });

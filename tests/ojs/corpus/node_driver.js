// node_driver.js — runs the corpus files in node's global scope (indirect eval), as
// plain scripts: require/module/exports are this module's locals, so UMD bundles
// take their browser-global branch exactly as in ojs
const fs = require('fs');
for (const f of process.argv.slice(2)) (0, eval)(fs.readFileSync(f, 'utf8'));

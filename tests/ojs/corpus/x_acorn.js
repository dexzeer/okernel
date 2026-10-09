// libs: acorn.js
// acorn parses the sample (module goal) and itself-like code; the AST JSON is compared
var A = acorn;
attempt('module ast', function () { return A.parse(SAMPLE_SRC, { ecmaVersion: 'latest', sourceType: 'module', locations: true }); });
attempt('tokens', function () {
    var t = [];
    for (var tok of A.tokenizer(SAMPLE_SRC, { ecmaVersion: 'latest', sourceType: 'module' })) t.push(tok.type.label + ':' + (tok.value === undefined ? '' : String(tok.value)));
    return t.join(' ');
});
attempt('script ast', function () { return A.parse('var a = 1; function f(x) { return x + a; } f(2); with (o) { y }', { ecmaVersion: 5 }); });
attempt('error', function () { try { A.parse('let let = 1', { ecmaVersion: 2020 }); } catch (e) { return e.message + ' @' + e.pos; } });
attempt('regexps', function () { return A.parse('/(?<y>\\d{4})-\\k<y>/v; /[\\p{L}--[a-z]]/v; /a(?=b)(?!c)(?<=d)(?<!e)/s', { ecmaVersion: 'latest' }).body.map(function (s) { return s.expression.regex; }); });
// a big synthetic program
attempt('big', function () {
    var src = [];
    for (var i = 0; i < 400; i++) src.push('function f' + i + '(a, b = ' + i + ') { const o = { k' + i + ': [a, ...b], get g() { return a?.b ?? ' + i + '; } }; return o; }');
    var ast = A.parse(src.join('\n'), { ecmaVersion: 'latest' });
    return __fnv(JSON.stringify(ast)) + ' ' + ast.body.length;
});

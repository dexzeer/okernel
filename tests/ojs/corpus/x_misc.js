// libs: moment.js marked.js esprima.js handlebars.js jsyaml.js decimal.js ramda.js immutable.js
attempt('moment', function () {
    var m = moment.utc(1700000000123);
    return [m.format(), m.format('dddd, MMMM Do YYYY, h:mm:ss a'), m.clone().add(1, 'month').subtract(3, 'days').format('YYYY-MM-DD'), m.startOf('week').toISOString(),
            moment.utc('2024-02-29').add(1, 'year').format('L'), moment.duration(123456789).humanize(), moment.utc([2020, 0, 31]).diff(moment.utc([2019, 11, 1]), 'days'),
            moment.utc('2016-13-01', 'YYYY-MM-DD').isValid(), moment.utc('25/12/2023 14:05', 'DD/MM/YYYY HH:mm').valueOf()];
});
attempt('marked', function () {
    return marked.parse('# Title\n\nSome *emphasis* and **strong** text with `code` and a [link](http://x.y "t").\n\n- a\n- b\n  1. c\n  2. d\n\n> quote\n\n```js\nvar x = 1 < 2;\n```\n\n| a | b |\n|---|:-:|\n| 1 | 2 |\n\n<div>html</div>\n\nAuto <http://example.com> foo_bar_baz ~~del~~\n');
});
attempt('esprima', function () { return esprima.parseScript('var a = function* (x) { yield x; }; for (const k of [1,2]) { a: { break a; } } (a) => ({ ...a, [k]: `t${k}` })', { range: true, tokens: true }); });
attempt('esprima-module', function () { return esprima.tokenize('export const x = /ab+c/gi.test("abbc") ? 0x10 : .5e1;', { comment: true }); });
attempt('handlebars', function () {
    Handlebars.registerHelper('up', function (s) { return String(s).toUpperCase(); });
    Handlebars.registerPartial('item', '<li>{{up name}}{{#if extra}} ({{extra}}){{/if}}</li>');
    var t = Handlebars.compile('<h1>{{title}}</h1><ul>{{#each items}}{{> item}}{{else}}<li>none</li>{{/each}}</ul>{{{raw}}} {{esc}} {{#with obj}}{{a.b}}{{/with}} {{lookup arr 1}}');
    return t({ title: 'T', items: [{ name: 'a', extra: 1 }, { name: 'b' }], raw: '<b>x</b>', esc: '<i>&"\'', obj: { a: { b: 'deep' } }, arr: ['x', 'y'] });
});
attempt('yaml', function () {
    var doc = jsyaml.load('a: 1\nb: [1, 2, {c: d}]\nmulti: |\n  line1\n  line2\nanchors:\n  base: &b {x: 1}\n  ext:\n    <<: *b\n    y: 2\ndate: 2001-12-14t21:59:43.10-05:00\nnum: 0x1F\nflt: .inf\n');
    return [doc, jsyaml.dump({ s: 'a: b', n: [1, null, true], o: { 'k k': 'multi\nline' } })];
});
attempt('decimal', function () {
    var D = Decimal;
    D.set({ precision: 50 });
    return [new D(1).div(3).toString(), new D(2).sqrt().toString(), new D('1e-30').plus('1').toFixed(35), new D(10).pow(-20).toString(), new D(123.456).toSignificantDigits(4).toString(),
            new D('-0.000123').toExponential(2), D.exp(1).toString(), D.ln(10).toString(), new D(2).pow(200).toString(), new D('9.87654321e100').mod(7).toString()];
});
attempt('ramda', function () {
    var R2 = R;
    var f = R2.pipe(R2.filter(R2.propEq(true, 'ok')), R2.map(R2.prop('v')), R2.reduce(R2.add, 0));
    return [f([{ ok: true, v: 1 }, { ok: false, v: 2 }, { ok: true, v: 3 }]), R2.groupBy(R2.prop('t'), [{ t: 'a' }, { t: 'b' }, { t: 'a' }]), R2.lensPath(['a', 'b']) && R2.set(R2.lensPath(['a', 'b']), 5, {}),
            R2.zipObj(['a', 'b'], [1, 2]), R2.curryN(3, function (a, b, c) { return a + b + c; })(1)(2)(3), R2.sortWith([R2.descend(R2.prop('n'))], [{ n: 1 }, { n: 3 }, { n: 2 }]), R2.toPairs({ x: 1 }), R2.uniqWith(R2.eqBy(Math.abs), [1, -1, 2])];
});
attempt('immutable', function () {
    var I = Immutable;
    var m = I.Map({ a: 1, b: I.List([1, 2, 3]) });
    var m2 = m.setIn(['b', 1], 9).update('a', function (x) { return x + 1; });
    var s = I.Seq([1, 2, 3, 4, 5, 6]).filter(function (x) { return x % 2; }).map(function (x) { return x * x; });
    var big = I.List(I.Range(0, 2000)).map(function (x) { return x * 3; }).filter(function (x) { return x % 2 === 0; });
    return [m2.toJS(), m.equals(m2), I.is(I.Map({ x: [1] }), I.Map({ x: [1] })), s.toArray(), big.size, big.last(), I.OrderedSet(['b', 'a', 'b']).toArray(), I.fromJS({ a: { b: [1] } }).getIn(['a', 'b', 0]),
            I.Map().set(NaN, 1).get(NaN), I.Record({ x: 0, y: 0 })({ x: 2 }).toJS(), m.hashCode() === I.Map({ a: 1, b: I.List([1, 2, 3]) }).hashCode()];
});

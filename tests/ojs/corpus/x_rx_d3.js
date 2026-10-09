// libs: rxjs.js d3.js
attempt('rx', function () {
    var out = [], O = rxjs.operators;
    rxjs.from([1, 2, 3, 4, 5, 6]).pipe(O.filter(function (x) { return x % 2 === 0; }), O.map(function (x) { return x * 10; }), O.scan(function (a, b) { return a + b; }, 0), O.take(2)).subscribe(function (v) { out.push(v); });
    rxjs.merge(rxjs.of('a', 'b'), rxjs.of('c').pipe(O.startWith('s'))).pipe(O.toArray()).subscribe(function (v) { out.push(v.join('')); });
    var s = new rxjs.BehaviorSubject(1); s.pipe(O.distinctUntilChanged(), O.pairwise()).subscribe(function (p) { out.push(p.join('>')); }); s.next(1); s.next(2); s.next(3);
    rxjs.combineLatest([rxjs.of(1, 2), rxjs.of('x')]).subscribe(function (v) { out.push(v.join('')); });
    rxjs.throwError(function () { return new Error('boom'); }).pipe(O.catchError(function (e) { return rxjs.of('caught ' + e.message); })).subscribe(function (v) { out.push(v); });
    return out;
});
attempt('d3 array', function () {
    var a = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5];
    return [d3.extent(a), d3.mean(a), d3.median(a), d3.quantile(a.slice().sort(d3.ascending), 0.25), d3.deviation(a), d3.bisectLeft([1, 2, 3, 4], 3), d3.group(a, function (x) { return x % 3; }).size,
            Array.from(d3.rollup(a, function (v) { return v.length; }, function (x) { return x > 3; })), d3.range(0, 1, 0.2), d3.ticks(0, 1, 7), d3.cumsum([1, 2, 3]).join(), d3.shuffler(function () { return 0.5; })([1, 2, 3, 4])];
});
attempt('d3 format', function () { return [d3.format('.2f')(Math.PI), d3.format(',')(1234567.891), d3.format('$,.2f')(-1234.5), d3.format('.3s')(0.000123), d3.format('+.1%')(0.123), d3.format('#x')(255), d3.format('e')(12345), d3.format('08.2f')(-3.14159), d3.format('.2~f')(1.5)]; });
attempt('d3 scale', function () {
    var x = d3.scaleLinear().domain([0, 100]).range([0, 960]).nice(), l = d3.scaleLog().domain([1, 1000]).range([0, 3]), t = d3.scaleUtc().domain([new Date(Date.UTC(2020, 0, 1)), new Date(Date.UTC(2020, 11, 31))]);
    return [x(42), x.invert(480), x.ticks(5), l(10), l.ticks().length, t.ticks(4).map(function (d) { return d.toISOString(); }), d3.scaleBand().domain(['a', 'b', 'c']).range([0, 100]).padding(0.1).bandwidth(), d3.scaleOrdinal(d3.schemeCategory10)('x')];
});
attempt('d3 interp', function () { return [d3.interpolate('red', 'blue')(0.5), d3.interpolate({ a: 1, b: 'x 5 y' }, { a: 3, b: 'x 15 y' })(0.25), d3.interpolateNumber(0, 10)(0.3), d3.color('steelblue').darker(1).formatHex(), d3.hsl('#abcdef').toString(), d3.interpolateViridis(0.4), d3.easeCubic(0.3)]; });
attempt('d3 shape', function () { return [d3.line().curve(d3.curveCatmullRom)([[0, 0], [10, 20], [20, 5], [30, 30]]), d3.arc()({ innerRadius: 10, outerRadius: 20, startAngle: 0, endAngle: Math.PI / 3 }), d3.pie()([1, 2, 3]).map(function (a) { return a.endAngle.toFixed(6); })]; });
attempt('d3 time', function () { var f = d3.utcFormat('%Y-%m-%d %H:%M:%S %a %j %U'); return [f(new Date(Date.UTC(2021, 6, 4, 13, 5, 9))), d3.utcParse('%d/%m/%Y')('25/12/2023').toISOString(), d3.utcDay.count(new Date(Date.UTC(2020, 0, 1)), new Date(Date.UTC(2021, 0, 1))), d3.utcMonth.range(new Date(Date.UTC(2020, 0, 15)), new Date(Date.UTC(2020, 4, 1))).length]; });
attempt('d3 hierarchy', function () { var root = d3.hierarchy({ n: 'r', c: [{ n: 'a', v: 3 }, { n: 'b', c: [{ n: 'c', v: 1 }, { n: 'd', v: 2 }] }] }, function (d) { return d.c; }).sum(function (d) { return d.v || 0; }); d3.treemap().size([100, 50])(root); return root.descendants().map(function (d) { return d.data.n + ':' + d.value + ':' + [d.x0, d.y0, d.x1, d.y1].map(function (z) { return z.toFixed(3); }).join(','); }); });

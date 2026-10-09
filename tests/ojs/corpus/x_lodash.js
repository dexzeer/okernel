// libs: lodash.js
var L = _;
var users = [{ user: 'barney', age: 36, active: true, tags: ['a', 'b'] }, { user: 'fred', age: 40, active: false, tags: ['b'] }, { user: 'pebbles', age: 1, active: true, tags: [] }];
attempt('chain', function () { return L.chain(users).filter('active').sortBy('age').map(function (o) { return o.user + ':' + o.age; }).value(); });
attempt('groupBy', function () { return L.groupBy([6.1, 4.2, 6.3], Math.floor); });
attempt('debounce exists', function () { return typeof L.debounce(function () {}, 10); });
attempt('template', function () { return L.template('hello <%= user %>! <% L.each(tags, function(t) { %><b><%- t %></b><% }); %>', { imports: { L: L } })(users[0]); });
attempt('merge', function () { return L.merge({ a: [{ b: 2 }, { d: 4 }] }, { a: [{ c: 3 }, { e: 5 }] }); });
attempt('cloneDeep', function () { var o = { a: new Date(0), b: /x/g, c: new Map([[1, 2]]), d: [1, { e: 'f' }] }; var c = L.cloneDeep(o); return [c.a.getTime(), String(c.b), c.c.get(1), c.d, c !== o, c.d !== o.d]; });
attempt('isEqual', function () { return [L.isEqual({ a: [1, { b: NaN }] }, { a: [1, { b: NaN }] }), L.isEqual(new Set([1]), new Set([2])), L.isEqual([1, , 3], [1, undefined, 3])]; });
attempt('strings', function () { return [L.camelCase('Foo Bar'), L.kebabCase('fooBar'), L.snakeCase('Foo-Bar'), L.startCase('--foo-bar--'), L.deburr('déjà vu'), L.escape('<a href="x">'), L.pad('abc', 8, '_-'), L.truncate('hi-diddly-ho there, neighborino', { length: 24, separator: ' ' }), L.words('fred, barney, & pebbles')]; });
attempt('numbers', function () { return [L.round(4.006, 2), L.floor(-4.006, 2), L.clamp(-10, -5, 5), L.inRange(3, 2, 4), L.sum([4, 2, 8, 6]), L.mean([4, 2, 8, 6]), L.random(0, 0), L.toInteger('3.2'), L.toSafeInteger(Infinity)]; });
attempt('arrays', function () { return [L.chunk([1, 2, 3, 4, 5], 2), L.difference([2, 1], [2, 3]), L.flattenDeep([1, [2, [3, [4]], 5]]), L.intersectionBy([2.1, 1.2], [2.3, 3.4], Math.floor), L.uniqBy([{ x: 1 }, { x: 2 }, { x: 1 }], 'x'), L.zip(['a', 'b'], [1, 2], [true, false]), L.sortedIndex([30, 50], 40), L.pullAt([5, 10, 15, 20], [1, 3]), L.range(0, 20, 5), L.shuffle.length]; });
attempt('objects', function () { return [L.get({ a: [{ b: { c: 3 } }] }, 'a[0].b.c'), L.set({}, 'x[0].y.z', 5), L.pick({ a: 1, b: '2', c: 3 }, ['a', 'c']), L.omitBy({ a: 1, b: '2', c: 3 }, L.isNumber), L.invert({ a: 1, b: 2, c: 1 }), L.mapValues({ a: { n: 1 } }, 'n'), L.defaultsDeep({ a: { b: 2 } }, { a: { b: 1, c: 3 } }), L.toPairs({ a: 1, b: 2 }), L.result({ a: { b: function () { return 7; } } }, 'a.b')]; });
attempt('memoize', function () { var n = 0; var f = L.memoize(function (x) { n++; return x * 2; }); f(1); f(1); f(2); return [n, f.cache.size]; });
attempt('curry', function () { var f = L.curry(function (a, b, c) { return [a, b, c]; }); return [f(1)(2)(3), f(1, 2)(3), f(L, 2)(1)(3)]; });

// libs: react.js reactdomserver.js
var h = React.createElement;
var Ctx = React.createContext('none');
function Item(p) { var s = React.useState(p.n * 2); return h('li', { className: 'i' + p.n, 'data-x': p.n, style: { marginTop: p.n, color: 'red' } }, 'item ', s[0], p.n % 2 ? h('b', null, 'odd') : null); }
function List(p) { var v = React.useContext(Ctx); var m = React.useMemo(function () { return p.n * 10; }, [p.n]); return h('ul', { title: v + m }, Array.from({ length: p.n }, function (_, i) { return h(Item, { key: i, n: i }); })); }
class Box extends React.Component { render() { return h('section', { id: 'box', dangerouslySetInnerHTML: undefined }, h('h1', null, this.props.title, ' & <esc>'), this.props.children); } }
attempt('render', function () { return ReactDOMServer.renderToString(h(Ctx.Provider, { value: 'ctx' }, h(Box, { title: 'Hello' }, h(List, { n: 7 }), h('input', { value: 'v', readOnly: true }), h('textarea', { defaultValue: 'ta' }), h(React.Fragment, null, h('br'), 'text')))); });
attempt('static', function () { return ReactDOMServer.renderToStaticMarkup(h('div', null, h('select', { value: 'b', onChange: function () {} }, h('option', { value: 'a' }, 'A'), h('option', { value: 'b' }, 'B')), h('svg', { viewBox: '0 0 10 10' }, h('circle', { cx: 5, cy: 5, r: 4, strokeWidth: 2 })))); });
attempt('big', function () { var rows = []; for (var i = 0; i < 300; i++) rows.push(h('tr', { key: i }, h('td', null, i), h('td', null, String(i * i)))); return __fnv(ReactDOMServer.renderToStaticMarkup(h('table', null, h('tbody', null, rows)))); });

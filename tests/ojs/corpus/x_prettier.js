// libs: prettier.js prettier-babel.js prettier-estree.js
// prettier formats with async APIs: promises, async functions, big closure graphs
var P = prettier, plugins = [prettierPlugins.babel, prettierPlugins.estree];
attemptAsync('sample', function () { return P.format(SAMPLE_SRC, { parser: 'babel', plugins: plugins }); });
attemptAsync('ugly', function () {
    return P.format('function   f ( a,b ){return a+b} ; const x={a:1,b:[1,2,3].map(x=>x*2),c:`t${1}`}; if(x){f(1,2)}else{while(0){}}', { parser: 'babel', plugins: plugins, semi: false, singleQuote: true, printWidth: 40 });
});
attemptAsync('json', function () { return P.format('{"a":1,"b":[true,false,null,{"c":"d"}]}', { parser: 'json', plugins: plugins }); });
attemptAsync('error', function () { return P.format('let = ;', { parser: 'babel', plugins: plugins }).catch(function (e) { return e.message.split('\n')[0]; }); });

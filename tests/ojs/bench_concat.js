// quadratic-concat check: building a long string one character at a time must be fast
function t(name, f) { const a = Date.now(); const r = f(); print(name + ': ' + (Date.now() - a) + ' ms'); return r; }
t('y += c (27352 x)', () => { let y = ''; for (let i = 0; i < 27352; i++) y += 'abc'.charAt(Math.floor(Math.random() * 3)); return y.length; });
t('y += c (200000 x)', () => { let y = ''; for (let i = 0; i < 200000; i++) y += 'a'; return y.length; });
t('y = y + c + d (100000 x)', () => { let y = ''; for (let i = 0; i < 100000; i++) y = y + 'a' + 'b'; return y.length; });
t('array join (200000)', () => { const a = []; for (let i = 0; i < 200000; i++) a.push('a'); return a.join('').length; });
t('Math.random x 1e6', () => { let s = 0; for (let i = 0; i < 1000000; i++) s += Math.random(); return s > 0; });
t('charAt x 1e6', () => { const s = 'abcdefghij'; let n = 0; for (let i = 0; i < 1000000; i++) n += s.charAt(i % 10).length; return n; });
print('done');

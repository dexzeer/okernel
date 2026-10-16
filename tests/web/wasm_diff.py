#!/usr/bin/env python3
"""Compare two wasm test outputs line by line, token by token (a line is "name tok tok ..."). Prints the first mismatches."""
import sys

a = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
b = open(sys.argv[2], encoding='utf-8', errors='replace').read().split('\n')
shown = 0
for i in range(max(len(a), len(b))):
    x = a[i] if i < len(a) else '<missing>'
    y = b[i] if i < len(b) else '<missing>'
    if x == y:
        continue
    tx, ty = x.split(' '), y.split(' ')
    name = tx[0] if tx else '?'
    bad = [(k, tx[k] if k < len(tx) else '-', ty[k] if k < len(ty) else '-') for k in range(max(len(tx), len(ty))) if (tx[k] if k < len(tx) else None) != (ty[k] if k < len(ty) else None)]
    print('line %d %s: %d differing tokens; first: %s' % (i + 1, name, len(bad), ', '.join('#%d got %s want %s' % t for t in bad[:6])))
    shown += 1
    if shown >= 12:
        print('...')
        break
sys.exit(1 if shown else 0)

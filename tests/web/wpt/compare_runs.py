# python3 tests/web/wpt/compare_runs.py <old_out_dir> <new_out_dir> [prefix [n_gains]]  -> gains / losses between two run_wpt.py result dirs
import json,sys,collections
a={r['test']:r['verdict'] for r in json.load(open(sys.argv[1]+'/results.json'))}
b={r['test']:r['verdict'] for r in json.load(open(sys.argv[2]+'/results.json'))}
pre=sys.argv[3] if len(sys.argv)>3 else ''
gain=[t for t in b if b[t]=='pass' and a.get(t)!='pass' and t.startswith(pre)]
loss=[t for t in b if b[t]!='pass' and a.get(t)=='pass' and t.startswith(pre)]
print('gain',len(gain),'loss',len(loss), 'base pass in subset', sum(1 for t in b if a.get(t)=='pass'), 'new pass', sum(1 for t in b if b[t]=='pass'))
for t in loss[:40]: print(' LOSS',t)
if len(sys.argv)>4:
    for t in gain[:int(sys.argv[4])]: print(' GAIN',t)

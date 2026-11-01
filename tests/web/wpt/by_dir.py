# python3 tests/web/wpt/by_dir.py <out_dir> [prefix]  -> failures per test-name family, biggest first
import json,collections,sys,re
d=sys.argv[1]; pre=sys.argv[2] if len(sys.argv)>2 else ''
rs=json.load(open(d+'/results.json'))
c=collections.Counter(); tot=collections.Counter()
for r in rs:
    t=r['test']
    if not t.startswith(pre): continue
    p=t.split('/')
    k='/'.join(p[:3]) if len(p)>3 else '/'.join(p[:2]) if len(p)>2 else p[0]
    # group files by name stem
    if len(p)<=3: k='/'.join(p[:-1])+'/'+re.sub(r'[-_.]?\d.*$','',p[-1])
    tot[k]+=1
    if r['verdict'] in ('fail','error'): c[k]+=1
for k,v in c.most_common(70): print(v, tot[k], k)

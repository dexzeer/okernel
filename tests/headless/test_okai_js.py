#!/usr/bin/env python3
"""okai page-script test (ojs), offline: local fixture server on :8001.

  1. tests/web/js/basic.html: 37 DOM/event/layout/storage/custom-element
     checks run inside the kernel realm -> "[js] RESULT ALLPASS"
  2. app.html: defer script + fetch() of JSON (list rendered) + a timer
  3. a click on a <div> with a JS click listener updates the page
  4. a link whose click listener calls preventDefault() does not navigate
  5. typing into an <input> fires `input` events the page mirrors
  6. location.href from script navigates
PASS requires every step and no faults / allocator errors / realm aborts.
"""
import sys, os, time, re, subprocess, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WWW = os.path.join(OUTDIR, "www-js")
os.makedirs(WWW, exist_ok=True)
shutil.copy(os.path.join(ROOT, "tests", "web", "js", "basic.html"), os.path.join(WWW, "basic.html"))
open(os.path.join(WWW, "app.html"), "w").write("""<!doctype html>
<html><head><title>js app</title>
<style>body{margin:24px;font-family:sans-serif;background:#f6f7fb}
#btn{display:inline-block;padding:20px 40px;background:#3366cc;color:#fff;font-size:24px}
#out{font-size:28px;margin:20px 0}#nolink{font-size:24px}input{font-size:20px}
li{font-size:20px}</style>
<script src="app.js" defer></script></head>
<body><div id="btn">Click me</div><div id="out">waiting</div>
<p><a id="nolink" href="nowhere.html">prevented link</a></p>
<p><input id="inp" placeholder="type"></p>
<div id="mirror">-</div>
<p><a id="go" href="#">go (script navigation)</a></p>
<ul id="items"></ul>
</body></html>""")
open(os.path.join(WWW, "app.js"), "w").write("""
const out = document.getElementById('out');
let clicks = 0;
document.getElementById('btn').addEventListener('click', () => {
  clicks++; out.textContent = 'clicked ' + clicks; console.log('JSTEST click ' + clicks);
});
document.getElementById('nolink').addEventListener('click', e => {
  e.preventDefault(); console.log('JSTEST link prevented');
});
document.getElementById('inp').addEventListener('input', e => {
  document.getElementById('mirror').textContent = e.target.value;
  console.log('JSTEST input ' + e.target.value);
});
document.getElementById('go').addEventListener('click', e => {
  e.preventDefault(); location.href = 'done.html';
});
fetch('data.json').then(r => r.json()).then(d => {
  const ul = document.getElementById('items');
  d.items.forEach(t => { const li = document.createElement('li'); li.textContent = t; ul.appendChild(li); });
  console.log('JSTEST fetched ' + d.items.length);
}).catch(e => console.log('JSTEST fetch failed ' + e));
let ticks = 0;
const iv = setInterval(() => { ticks++; if (ticks === 3) { clearInterval(iv); console.log('JSTEST timer 3'); } }, 100);
console.log('JSTEST ready ' + document.readyState);
document.addEventListener('DOMContentLoaded', () => console.log('JSTEST dcl ' + document.readyState));
""")
open(os.path.join(WWW, "data.json"), "w").write('{"items":["alpha","beta","gamma"]}')
open(os.path.join(WWW, "done.html"), "w").write("<!doctype html><title>done</title><h1>Script navigation worked</h1>")
open(os.path.join(WWW, "nowhere.html"), "w").write("<!doctype html><title>WRONG</title><p>should not load")

SRVLOG = os.path.join(OUTDIR, "www-js.log")
srvlog = open(SRVLOG, "w")
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8001", "--directory", WWW],
                       stdout=srvlog, stderr=subprocess.STDOUT)
time.sleep(1)
results = []


def step(name, ok):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name, flush=True)


tag = sys.argv[1] if len(sys.argv) > 1 else "okaijs"
vm = OkVM(tag)
try:
    vm.wait_for("[mem] heap", timeout=40)
    time.sleep(10)
    vm.type_string("okai http://10.0.2.2:8001/basic.html\n")
    ok = vm.wait_for(r"\[js\] RESULT", timeout=120)
    log = vm.serial()
    m = re.search(r"\[js\] RESULT (\w+) (\d+)", log)
    step("basic.html DOM suite in the kernel realm: " + (m.group(0) if m else "no result"),
         m and m.group(1) == "ALLPASS")
    if m and m.group(1) != "ALLPASS":
        for l in log.splitlines():
            if "FAIL " in l: print("   ", l)

    # 2. app page (address bar)
    time.sleep(2)
    vm.type_string("g")
    time.sleep(0.5)
    vm.type_string("http://10.0.2.2:8001/app.html\n")
    step("defer script ran (readyState interactive)", vm.wait_for(r"JSTEST ready interactive", timeout=90))
    step("DOMContentLoaded after defer scripts", vm.wait_for(r"JSTEST dcl interactive", timeout=30))
    step("fetch() JSON -> DOM list", vm.wait_for(r"JSTEST fetched 3", timeout=60))
    step("setInterval x3", vm.wait_for(r"JSTEST timer 3", timeout=30))
    time.sleep(3)   # let the render settle (link regions get logged)

    # 3. JS click listener on a <div> (no link/field region: target = element at point)
    vm.click_at(90, 40, done=lambda s: "JSTEST click" in s)
    step("click listener on a div", vm.wait_for(r"JSTEST click 1", timeout=20))

    # 4. preventDefault on a link
    before = vm.serial().count("parse: count=")
    vm.click_link("nowhere", max_iters=20) if vm.find_link("nowhere") else None
    time.sleep(3)
    s = vm.serial()
    step("link click prevented by script", "JSTEST link prevented" in s and
         s.count("parse: count=") == before and "nowhere.html" not in "".join(
             l for l in s.splitlines() if "LINK HIT" in l))

    # 5. input events: focus the field (its region is logged as field[..])
    fields = re.findall(r"\[okai\] field\[\d+\] x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.serial())
    if fields:
        fx, fy, fw, fh = map(int, fields[-1])
        vm.click_at(fx + 20, fy + fh // 2, done=lambda s: "focus input" in s)
    vm.type_string("hi")
    step("input events", vm.wait_for(r"JSTEST input hi", timeout=20))

    # 6. script navigation
    n0 = vm.serial().count("parse: count=")
    vm.click_link("#", max_iters=20) if vm.find_link("#") else None
    t_end = time.time() + 40
    nav = False
    while time.time() < t_end:
        s = vm.serial()
        if s.count("parse: count=") > n0 and "script navigation -> " in s:
            nav = True
            break
        time.sleep(1)
    step("location.href navigation", nav)

    vm.dump()
    out = os.environ.get("OKVM_PNG")
    if out:
        from PIL import Image
        Image.open(vm.PPM).save(out)
    log = vm.serial()
    bad = [l for l in log.splitlines()
           if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper() or
           "abort()" in l or ("EXCEPTION" in l.upper() and "#" in l)]
    step("no faults / allocator errors / realm aborts", not bad)
    if bad: print("   ", bad[:5])
    errs = [l for l in log.splitlines() if l.startswith("[js] error")]
    print("js errors:", len(errs), errs[:5])
finally:
    vm.kill()
    srv.terminate()
print("RESULT:", "PASS" if results and all(r for _, r in results) else "FAIL")
sys.exit(0 if results and all(r for _, r in results) else 1)

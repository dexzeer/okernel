import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from okvm import OkVM

vm = OkVM()
time.sleep(8)  # boot

# Open okai home page (no network needed)
vm.type_string("okai\n")
time.sleep(3.0)

vm.dump()
ppm = vm.PPM
out = os.path.expanduser("~/okvm/css_box.png")
os.system(f"convert {ppm} {out} 2>/dev/null || cp {ppm} {out}")
print("saved", out)
vm.kill()

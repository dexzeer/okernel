#!/usr/bin/env python3
"""Real Microsoft binaries (findstr.exe, sort.exe from the host's System32) on our kernel32/msvcrt, with their .mui language files.

    mkdir -p build-host/extra_win/win/en-US build-host/extra_win/data
    cp /c/Windows/System32/{findstr,sort}.exe build-host/extra_win/win/; cp /c/Windows/System32/en-US/{findstr,sort}.exe.mui build-host/extra_win/win/en-US/
    printf 'apple pie\nbanana split\ncherry tart\napricot jam\nblueberry\n' > build-host/extra_win/data/words.txt
    printf 'pear\napple\nzebra\nmango\n' > build-host/extra_win/data/fruit.txt
    sh tools/lx/build_tests.sh build-host/extra_win && python3 tests/headless/test_lx_msbin.py
"""
import sys, os, re, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
from okvm import OkVM

BS = chr(92)
fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

vm = OkVM("msbin")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    def run(cmd, expect):
        n = vm.serial().count("exited, status")
        vm.type_string(cmd + "\n")
        check(vm.wait_for(expect, timeout=40), f"`{cmd}` prints {expect!r}")
        end = time.time() + 15
        while time.time() < end and vm.serial().count("exited, status") <= n: time.sleep(0.5)
    run("lx /win/findstr.exe /?", "Regular expression quick reference")
    run("lx /win/findstr.exe /n ap '"+BS+"data"+BS+"words.txt'", "4:apricot jam")
    run("lx /win/findstr.exe /I /C:berry '"+BS+"data"+BS+"words.txt'", "blueberry")
    run("lx /win/sort.exe '" + BS + "data" + BS + "fruit.txt'", "zebra")
    log = vm.serial()
    i = log.find("apple\nmango\npear\nzebra")
    check(i >= 0, "sort.exe output is in order")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)

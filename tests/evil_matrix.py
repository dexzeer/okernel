#!/usr/bin/env python3
# evil_matrix.py — runs the demolition matrix: fresh evil_server + fresh
# t_tlsa client per attack, parses the oracle, checks expectations.
# Exit 0 iff every attack behaved as expected (PASS where required,
# FAIL with a sane reason where required).
import os, re, signal, socket, subprocess, sys, time

SRV = [sys.executable, "tests/evil_server.py"]
CLI = ["./build-host/t_tlsa"]
BASE_PORT = 14443

# name -> (server_attack, sni, root, rounds, expect, expect_reason)
# expect: "pass" (client completes) or "fail" (client must refuse)
# expect_reason: fail_reason int or None (don't care, just must fail)
M = [
    # baselines / correctness
    ("base-ec",      ("base-ec", "evil.example.com", None, 1, "pass", None)),
    ("base-rsa",     ("base-rsa", "evil.example.com", None, 1, "pass", None)),
    ("base-p384",    ("base-p384", "evil.example.com", "tests/adversarial/at_p384_root.der", 1, "pass", None)),
    ("base-frag64",  ("base-frag64", "evil.example.com", None, 1, "pass", None)),
    ("base-coalesce",("base-coalesce", "evil.example.com", None, 1, "pass", None)),
    ("base-noalpn",  ("base-noalpn", "evil.example.com", None, 1, "pass", None)),
    ("ip-literal",   ("ip-literal", "1.2.3.4", None, 1, "pass", None)),
    ("dns-case",     ("base-ec", "EVIL.EXAMPLE.COM", None, 1, "pass", None)),
    ("fqdn-dot",     ("base-ec", "evil.example.com.", None, 1, "pass", None)),
    ("staple-good",  ("staple-good", "evil.example.com", None, 1, "pass", None)),
    ("muststaple-good", ("muststaple-good", "evil.example.com", None, 1, "pass", None)),
    ("psk-ok",       ("psk-ok", "evil.example.com", None, 2, "pass", None)),
    # CV forgery
    ("cv-garbage",   ("cv-garbage", "evil.example.com", None, 1, "fail", 4)),
    ("cv-wrongtx",   ("cv-wrongtx", "evil.example.com", None, 1, "fail", 4)),
    ("cv-clientlabel", ("cv-clientlabel", "evil.example.com", None, 1, "fail", 4)),
    ("cv-padr",      ("cv-padr", "evil.example.com", None, 1, "fail", 4)),
    ("cv-rgen",      ("cv-rgen", "evil.example.com", None, 1, "fail", 4)),
    ("cv-szero",     ("cv-szero", "evil.example.com", None, 1, "fail", 4)),
    ("cv-wrongalg",  ("cv-wrongalg", "evil.example.com", None, 1, "fail", 4)),
    ("cv-pkcs1",     ("cv-pkcs1", "evil.example.com", None, 1, "fail", 6)),
    ("cv-pss-salt20",("cv-pss-salt20", "evil.example.com", None, 1, "fail", 4)),
    ("cv-pss-mgf384",("cv-pss-mgf384", "evil.example.com", None, 1, "fail", 4)),
    # chain
    ("ch-rogue",     ("ch-rogue", "evil.example.com", None, 1, "fail", 4)),
    ("ch-wronghost", ("ch-wronghost", "evil.example.com", None, 1, "fail", 4)),
    ("ch-wild2",     ("base-ec", "a.b.evil.example.com", None, 1, "fail", 4)),
    ("ch-expired",   ("ch-expired", "evil.example.com", None, 1, "fail", 4)),
    ("ch-ku",        ("ch-ku", "evil.example.com", None, 1, "fail", 4)),
    ("ch-eku",       ("ch-eku", "evil.example.com", None, 1, "fail", 4)),
    ("ch-crit",      ("ch-crit", "evil.example.com", None, 1, "fail", 4)),
    ("ch-rsa1024",   ("ch-rsa1024", "evil.example.com", None, 1, "fail", 4)),
    ("ch-noca",      ("ch-noca", "evil.example.com", None, 1, "fail", 4)),
    ("ch-reordered", ("ch-reordered", "evil.example.com", None, 1, "fail", 4)),
    ("ch-v1",        ("ch-v1", "evil.example.com", None, 1, "fail", 4)),
    ("ch-kubadpad",  ("ch-kubadpad", "evil.example.com", None, 1, "fail", 4)),
    ("ch-emptycert", ("ch-emptycert", "evil.example.com", None, 1, "fail", 6)),
    ("ch-dnsip",     ("base-ec", "1.2.3.4", None, 1, "fail", 4)),
    # state machine / record
    ("sm-hrr",       ("sm-hrr", "evil.example.com", None, 1, "fail", 6)),
    ("sm-certreq",   ("sm-certreq", "evil.example.com", None, 1, "fail", 6)),
    ("sm-dupee",     ("sm-dupee", "evil.example.com", None, 1, "fail", 6)),
    ("sm-doublecert",("sm-doublecert", "evil.example.com", None, 1, "fail", 6)),
    ("sm-skipcv",    ("sm-skipcv", "evil.example.com", None, 1, "fail", 6)),
    ("sm-skipcert",  ("sm-skipcert", "evil.example.com", None, 1, "fail", 6)),
    ("sm-2ndsh",     ("sm-2ndsh", "evil.example.com", None, 1, "fail", 6)),
    ("sm-fin31",     ("sm-fin31", "evil.example.com", None, 1, "fail", 6)),
    ("sm-fintrail",  ("sm-fintrail", "evil.example.com", None, 1, "fail", 6)),
    ("sm-ccs2",      ("sm-ccs2", "evil.example.com", None, 1, "fail", 6)),
    ("sm-ccs0",      ("sm-ccs0", "evil.example.com", None, 1, "fail", 6)),
    ("sm-plainalert",("sm-plainalert", "evil.example.com", None, 1, "fail", 6)),
    ("sm-sid0",      ("sm-sid0", "evil.example.com", None, 1, "fail", 6)),
    ("sm-suite1301", ("sm-suite1301", "evil.example.com", None, 1, "fail", 6)),
    ("sm-group17",   ("sm-group17", "evil.example.com", None, 1, "fail", 6)),
    ("sm-nosv",      ("sm-nosv", "evil.example.com", None, 1, "fail", 6)),
    ("sm-noks",      ("sm-noks", "evil.example.com", None, 1, "fail", 6)),
    ("sm-dupsh",     ("sm-dupsh", "evil.example.com", None, 1, "fail", 6)),
    ("sm-kstrail",   ("sm-kstrail", "evil.example.com", None, 1, "fail", 6)),
    ("sm-cvtrail",   ("sm-cvtrail", "evil.example.com", None, 1, "fail", 6)),
    ("sm-shtrail",   ("sm-shtrail", "evil.example.com", None, 1, "fail", 6)),
    ("sm-ver0304",   ("sm-ver0304", "evil.example.com", None, 1, "fail", 6)),
    ("sm-comp1",     ("sm-comp1", "evil.example.com", None, 1, "fail", 6)),
    ("sm-eeh2",      ("sm-eeh2", "evil.example.com", None, 1, "fail", 6)),
    ("sm-eedup",     ("sm-eedup", "evil.example.com", None, 1, "fail", 6)),
    ("sm-eetrail",   ("sm-eetrail", "evil.example.com", None, 1, "fail", 6)),
    ("sm-17exts",    ("sm-17exts", "evil.example.com", None, 1, "fail", 6)),
    ("sm-keyupdate", ("sm-keyupdate", "evil.example.com", None, 1, "fail", 6)),
    ("sm-earlynst",  ("sm-earlynst", "evil.example.com", None, 1, "fail", 6)),
    ("sm-plainclose",("sm-plainclose", "evil.example.com", None, 1, "fail", 6)),
    ("sm-encclose",  ("sm-encclose", "evil.example.com", None, 1, "pass", None)),
    ("sm-truncmid",  ("sm-truncmid", "evil.example.com", None, 1, "fail", None)),
    ("sm-oversize",  ("sm-oversize", "evil.example.com", None, 1, "fail", 6)),
    ("sm-badmac",    ("sm-badmac", "evil.example.com", None, 1, "fail", 3)),
    ("sm-loworder",  ("sm-loworder", "evil.example.com", None, 1, "fail", 6)),
    ("sm-staplebig", ("sm-staplebig", "evil.example.com", None, 1, "fail", 6)),
    # OCSP content
    ("staple-badsig",("staple-badsig", "evil.example.com", None, 1, "fail", 4)),
    ("staple-wrongserial", ("staple-wrongserial", "evil.example.com", None, 1, "fail", 4)),
    ("staple-stale", ("staple-stale", "evil.example.com", None, 1, "fail", 4)),
    ("staple-revoked",("staple-revoked", "evil.example.com", None, 1, "fail", 4)),
    ("staple-responder", ("staple-responder", "evil.example.com", None, 1, "fail", 4)),
    ("muststaple-none", ("muststaple-none", "evil.example.com", None, 1, "fail", 4)),
    # resumption
    ("psk-foreign",  ("psk-foreign", "evil.example.com", None, 2, "fail2", 6)),
    ("psk-certafter",("psk-certafter", "evil.example.com", None, 2, "fail2", 6)),
    ("psk-wrongkeys",("psk-wrongkeys", "evil.example.com", None, 2, "fail2", 3)),
    ("psk-tamper",   ("psk-tamper", "evil.example.com", None, 2, "pass", None)),
    ("pin-change",   ("pin-change", "evil.example.com", None, 2, "fail2", 4)),
    ("base-rsa4096", ("base-rsa4096", "evil.example.com", None, 1, "pass", None)),
    ("base-frag7",   ("base-frag7", "evil.example.com", None, 1, "pass", None)),
    ("ee-empty",     ("ee-empty", "evil.example.com", None, 1, "pass", None)),
    ("cv-alg384-on-256", ("cv-alg384-on-256", "evil.example.com", None, 1, "fail", 4)),
    ("cv-empty",     ("cv-empty", "evil.example.com", None, 1, "fail", 6)),
    ("ch-6cert",     ("ch-6cert", "evil.example.com", None, 1, "fail", 4)),
    ("nc-bad",       ("nc-bad", "evil.example.io", None, 1, "fail", 4)),
    ("san-over",     ("san-over", "h0.example.com", None, 1, "fail", 4)),
    ("sm-encalert",  ("sm-encalert", "evil.example.com", None, 1, "fail", 2)),
    ("sm-nstbad",    ("sm-nstbad", "evil.example.com", None, 1, "fail", 6)),
    ("sm-appdata-early", ("sm-appdata-early", "evil.example.com", None, 1, "fail", 6)),
    ("staple-sha256",("staple-sha256", "evil.example.com", None, 1, "pass", None)),
]

def wait_port(port, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return True
        except OSError:
            time.sleep(0.1)
    return False

def run_one(idx, name, spec):
    atk, sni, root, rounds, expect, reason = spec
    port = BASE_PORT + (idx % 200)
    srv = subprocess.Popen(SRV + ["--port", str(port), "--attack", atk,
                                  "--conns", str(rounds)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
    ok, detail, out = False, "no-result", ""
    try:
        # No port probe: a probe connection would consume one of the
        # server's accept slots and shift per-connection attack scripts.
        # Fixed sleep instead (python+crypto import is ~1s).
        time.sleep(2.0)
        if srv.poll() is not None:
            out = "[matrix] server exited before client ran"
            return ok, detail, out
        env = dict(os.environ)
        env["TLSA_SNI"] = sni
        if root:
            env["TLSA_ROOT"] = root
        else:
            env.pop("TLSA_ROOT", None)
        args = CLI + [str(port)] + (["twice"] if rounds == 2 else [])
        try:
            cli = subprocess.run(args, capture_output=True, text=True,
                                 timeout=90, env=env)
            out = cli.stdout + cli.stderr
        except subprocess.TimeoutExpired as te:
            out = (te.stdout or "") + (te.stderr or "") if isinstance(
                te.stdout, str) else ""
            out += "\n[matrix] CLIENT TIMEOUT"
        cur = re.findall(r"\[tlsa\] run done: n=(-?\d+) fail_reason=(\d+)", out)
        if expect == "pass":
            ok = (len(cur) == rounds and all(int(n) > 0 for n, r in cur)
                  and "TLSA-INTEROP: PASS" in out)
            detail = cur
        elif expect == "fail":
            ok = (len(cur) >= 1 and int(cur[0][0]) < 0 and
                  (reason is None or int(cur[0][1]) == reason))
            detail = cur
        elif expect == "fail2":
            ok = (len(cur) == 2 and int(cur[0][0]) > 0 and int(cur[1][0]) < 0 and
                  (reason is None or int(cur[1][1]) == reason))
            detail = cur
        else:
            detail = cur
    finally:
        try:
            srv.send_signal(signal.SIGTERM)
            try:
                sout, _ = srv.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                srv.kill()
                sout, _ = srv.communicate(timeout=5)
        except Exception:
            sout = ""
        if sout:
            out = out + "\n----- evil_server log -----\n" + sout[-1500:]
    return ok, detail, out

def main():
    only = sys.argv[1:] or None
    fails = []
    for i, (name, spec) in enumerate(M):
        if only and name not in only:
            continue
        ok, detail, out = run_one(i, name, spec)
        exp = spec[4]
        mark = "OK " if ok else "!!! BYPASS/FAIL"
        print(f"[{mark}] {name:18s} expect={exp:5s} got={detail}", flush=True)
        if not ok:
            fails.append(name)
            print(f"----- server+client log for {name} -----")
            print(out[-2500:])
            print(f"----- end {name} -----")
    print(f"\n{len(M) - len(fails)}/{len(M)} as expected;"
          f" {len(fails)} unexpected: {fails}")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())

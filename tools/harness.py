#!/usr/bin/env python3
"""Drive the display through its edge cases via the test-override endpoint.

    tools/harness.py list              # show cases
    tools/harness.py run               # every case, pausing so you can look
    tools/harness.py run --case wrap2
    tools/harness.py soak --hours 8    # cycle fast, unattended, watch for leaks
    tools/harness.py clear             # release the override

Each case asserts the device accepted it and is rendering it. Rendering itself
is a human check, so `run` pauses and tells you what you should be seeing.
Stdlib only, no pip install.
"""
import argparse, json, os, sys, time, urllib.error, urllib.request

HOST = os.environ.get("DEVICE_HOST", "192.168.1.229")
BASE = f"http://{HOST}"

# (name, payload, what you should see) — chosen to hit the font cascade, the
# word-wrap path, the bottom-bar formatters, and every relay branch.
CASES = [
    ("short",     dict(ident="N123AB", t="C172", alt=3500, dist=4.2, seats=4),
     "big font, one line, PVT relay"),
    ("wrap2",     dict(ident="TEST", t="A388", alt=38000, dist=12.5, seats=500),
     "long name wrapped to 2 lines, COM relay"),
    ("longest",   dict(ident="TEST", t="B77W", alt=41000, dist=99.9, seats=396),
     "smallest title font, no clipping at 256px"),
    ("unknown_t", dict(ident="ZZZ999", t="QQQQ", alt=1000, dist=1.0),
     "falls back to raw type code, no crash"),
    ("ground",    dict(ident="N500GD", t="C208", alt=-2, dist=0.3, seats=9),
     "altitude cell reads GND"),
    ("no_alt",    dict(ident="N600NA", t="C172", alt=-1, dist=2.0, seats=4),
     "altitude cell reads em-dash"),
    ("zero_dist", dict(ident="OVERHEAD", t="B738", alt=2000, dist=0.0, seats=189),
     "distance 0.0 renders, no divide-by-zero"),
    ("far",       dict(ident="FARAWAY", t="B738", alt=43000, dist=999.9, seats=189),
     "3-digit distance still fits its cell"),
    ("neg_alt",   dict(ident="BELOWSEA", t="C172", alt=-300, dist=5.0, seats=4),
     "negative altitude renders, treated sanely"),
    ("mil",       dict(ident="RCH512", t="C17", alt=28000, dist=30.0, op="MIL"),
     "MIL relay only (IN4)"),
    ("com",       dict(ident="UAL1234", t="B738", alt=35000, dist=20.0, op="COM"),
     "COM relay only (IN3)"),
    ("pvt",       dict(ident="N77XX", t="SR22", alt=8000, dist=6.0, op="PVT"),
     "PVT relay only (IN2)"),
    ("unicode",   dict(ident="ÜNICÖDE", t="C172", alt=5000, dist=3.0, seats=4),
     "non-ASCII callsign does not corrupt the display"),
    ("empty",     dict(ident="", t="", alt=0, dist=0.0),
     "all-empty payload degrades gracefully, no blank/garbage screen"),
    ("longident", dict(ident="ABCDEFGHIJKLMNOP", t="B738", alt=30000, dist=15.0, seats=189),
     "over-long ident truncated, not overflowed"),
    # Emergency banner: replaces ONLY the type-name area. The bottom row is
    # physically labelled on the bezel, so distance|seats|altitude must stay put.
    ("sq_7700",   dict(ident="UAL123", t="B738", alt=31000, dist=12.0, seats=189, squawk="7700"),
     "banner 'EMERGENCY 7700' over 'UAL123 B738'; bottom row unchanged"),
    ("sq_7600",   dict(ident="AAL456", t="A320", alt=18000, dist=7.5, seats=186, squawk="7600"),
     "banner 'RADIO FAIL 7600'; bottom row unchanged"),
    ("sq_7500",   dict(ident="DAL789", t="B77W", alt=35000, dist=22.0, seats=396, squawk="7500"),
     "banner 'HIJACK 7500'; bottom row unchanged"),
    ("sq_normal", dict(ident="SWA100", t="B738", alt=30000, dist=9.0, seats=189, squawk="1200"),
     "ordinary squawk: normal type name, no banner"),
    ("sq_long",   dict(ident="LONGCALLSIGN99", t="B77W", alt=41000, dist=99.9, seats=396, squawk="7700"),
     "long ident on the banner must not overflow 256px"),
]


def req(method, path, body=None, timeout=10):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.status, resp.read().decode()


def health():
    _, b = req("GET", "/healthz")
    return json.loads(b)


def apply_case(name, payload):
    """Push a case and confirm the device actually took it. Returns health dict."""
    st, body = req("PUT", "/test/closest", payload)
    if st != 200:
        raise RuntimeError(f"{name}: PUT returned {st}: {body}")
    time.sleep(1.5)  # let loop() pick it up and render
    h = health()
    if not h.get("test_override"):
        raise RuntimeError(f"{name}: device did not activate the override")
    return h


def cmd_run(a):
    cases = [c for c in CASES if not a.case or c[0] in a.case]
    if not cases:
        sys.exit(f"no case matching {a.case}")
    fails = []
    for name, payload, expect in cases:
        try:
            h = apply_case(name, payload)
            print(f"[ok]   {name:<10} heap={h['heap_free']:<7} showing={h['ident']!r} "
                  f"op={h['op']}  -> expect: {expect}")
        except Exception as e:
            fails.append(name)
            print(f"[FAIL] {name:<10} {e}")
        time.sleep(a.dwell)
    req("DELETE", "/test/closest")
    print(f"\n{len(cases) - len(fails)}/{len(cases)} cases applied cleanly"
          + (f", failed: {', '.join(fails)}" if fails else ""))
    print("override released; device back on live data")
    return 1 if fails else 0


def cmd_soak(a):
    """Cycle every case fast for hours. Looking for heap decline and reboots,
    not for correct pixels, so nothing pauses for a human here."""
    end = time.time() + a.hours * 3600
    start_heap, n, errs, reboots = None, 0, 0, 0
    last_uptime = 0
    while time.time() < end:
        for name, payload, _ in CASES:
            if time.time() >= end:
                break
            try:
                h = apply_case(name, payload)
                if start_heap is None:
                    start_heap = h["heap_free"]
                if h["uptime_ms"] < last_uptime:
                    reboots += 1
                    print(f"  !! reboot detected ({h['reset_reason']}) after {n} cycles")
                last_uptime = h["uptime_ms"]
                n += 1
                if n % 25 == 0:
                    print(f"  {n} applied  heap {h['heap_free']} "
                          f"(start {start_heap}, delta {h['heap_free']-start_heap:+})"
                          f"  min {h['heap_min']}  errs {errs}  reboots {reboots}")
            except Exception as e:
                errs += 1
                print(f"  error after {n}: {e}")
                time.sleep(5)
        time.sleep(a.dwell)
    try:
        req("DELETE", "/test/closest")
    except Exception:
        pass
    print(f"\nsoak done: {n} applications, {errs} errors, {reboots} reboots")
    return 1 if (errs or reboots) else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    r = sub.add_parser("run")
    r.add_argument("--case", action="append", help="run only this case (repeatable)")
    r.add_argument("--dwell", type=float, default=4.0, help="seconds to hold each case")
    s = sub.add_parser("soak")
    s.add_argument("--hours", type=float, default=8.0)
    s.add_argument("--dwell", type=float, default=0.5)
    sub.add_parser("clear")
    a = ap.parse_args()

    if a.cmd == "list":
        for n, p, e in CASES:
            print(f"{n:<10} {json.dumps(p)}\n{'':<10} -> {e}")
        return 0
    if a.cmd == "clear":
        req("DELETE", "/test/closest")
        print("override cleared")
        return 0
    try:
        return cmd_run(a) if a.cmd == "run" else cmd_soak(a)
    except urllib.error.URLError as e:
        sys.exit(f"cannot reach {BASE}: {e}")


if __name__ == "__main__":
    sys.exit(main())

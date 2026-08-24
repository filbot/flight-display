#!/usr/bin/env python3
"""Drop the Wi-Fi link on demand and verify the firmware's reconnect path.

    tools/wifitest.py [--hold 90]

The device is unreachable while the link is held down, so this cannot poll
through the outage: it triggers the drop, waits, then reconstructs what happened
from the serial log (captured over USB, unaffected by the network) and from the
uptime counter, which proves whether the device rebooted.
"""
import argparse, json, os, re, subprocess, sys, time, urllib.error, urllib.request
from datetime import datetime, timezone

HOST = os.environ.get("DEVICE_HOST", "192.168.1.229")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL = os.path.join(os.environ.get("LOGDIR", os.path.join(ROOT, "logs")), "serial.log")


def req(method, path, body=None, timeout=10):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(f"http://{HOST}" + path, data=data, method=method,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.read().decode()


def health(timeout=6):
    return json.loads(req("GET", "/healthz", timeout=timeout))


def serial_since(mark):
    out = []
    with open(SERIAL, errors="replace") as f:
        for i, ln in enumerate(f):
            if i >= mark:
                out.append(ln.rstrip())
    return out


def serial_len():
    with open(SERIAL, errors="replace") as f:
        return sum(1 for _ in f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hold", type=int, default=90, help="seconds to hold the link down")
    a = ap.parse_args()

    before = health()
    mark = serial_len()
    print(f"before: uptime {before['uptime_ms']//1000}s  rssi {before['rssi']}  "
          f"ok {before['fetch_ok']} fail {before['fetch_fail']} streak {before['fail_streak']}")

    print(f"dropping Wi-Fi for {a.hold}s ...")
    print("  " + req("POST", "/test/wifi-drop", {"hold_ms": a.hold * 1000}).strip())

    # The device is off-network now; wait for it to come back on its own.
    deadline = time.time() + a.hold + 180
    back_at = None
    while time.time() < deadline:
        time.sleep(5)
        try:
            h = health(timeout=4)
            back_at = time.time()
            break
        except Exception:
            pass
    if not back_at:
        print("[FAIL] device never came back within the window")
        return 1

    time.sleep(20)  # let it settle and complete a fetch
    after = health()
    lines = serial_since(mark)

    def has(pat):
        return [l for l in lines if re.search(pat, l, re.IGNORECASE)]

    dropped = has(r"TEST: dropping Wi-Fi")
    disc = has(r"WiFi disconnected\. Reason: (\d+)")
    expired = has(r"TEST: Wi-Fi hold expired")
    retried = has(r"WiFi retry: calling WiFi\.begin")
    gotip = has(r"got IP")
    stale = has(r"Displayed data stale")
    breaker = has(r"API unreachable for")
    crash = has(r"Guru Meditation|task_wdt|Backtrace:")
    boot = has(r"\[Boot\] Flight Display starting")

    rebooted = after["uptime_ms"] < before["uptime_ms"]
    results = [
        ("link actually dropped", bool(disc),
         disc[0].split("] ")[-1] if disc else "no disconnect event"),
        ("stayed down for the hold", bool(expired), expired[0].split("] ")[-1] if expired else "hold never expired"),
        ("our retry loop ran", bool(retried), f"{len(retried)} WiFi.begin() retry(ies)"),
        ("reconnected", bool(gotip), gotip[-1].split("] ")[-1] if gotip else "never got an IP"),
        ("no reboot", not rebooted and not boot,
         f"uptime {before['uptime_ms']//1000}s -> {after['uptime_ms']//1000}s"),
        ("no crash or watchdog", not crash, f"{len(crash)} crash line(s)"),
        ("circuit breaker stayed quiet", not breaker,
         "breaker must not fire while Wi-Fi is down — it is for API-unreachable-with-link-up"),
        ("fetches resumed", after["fetch_ok"] > before["fetch_ok"],
         f"ok {before['fetch_ok']} -> {after['fetch_ok']}"),
        ("streak cleared", after["fail_streak"] == 0, f"streak {after['fail_streak']}"),
    ]
    print()
    for name, ok, detail in results:
        print(f"[{'ok  ' if ok else 'FAIL'}] {name:<28} {detail}")
    if stale:
        print(f"         (display cleared to the dimmed splash: {stale[-1].split('] ')[-1]})")
    bad = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results)-len(bad)}/{len(results)} checks passed"
          + (f"; failing: {', '.join(bad)}" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

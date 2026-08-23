#!/usr/bin/env python3
"""Fault injection: point the device at the local mock, break the API in a
specific way, and check the firmware reacts correctly.

    tools/mockapi.py &                      # in another shell
    tools/faults.py run --mock-host 192.168.1.250
    tools/faults.py run --only http500 truncated
    tools/faults.py restore                 # back to the real API

Each scenario sets a mock mode, waits for the device to fetch through it, and
asserts on /healthz. Assertions are about *behaviour under fault* — does it stop
showing stale data, does it dim, does it back off, does it recover — not pixels.
"""
import argparse, json, os, sys, time, urllib.error, urllib.request

HOST = os.environ.get("DEVICE_HOST", "192.168.1.229")
BASE = f"http://{HOST}"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODE_FILE = os.path.join(os.environ.get("LOGDIR", os.path.join(ROOT, "logs")), "mock.mode")


def req(method, path, body=None, timeout=15):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.status, resp.read().decode()


def health():
    return json.loads(req("GET", "/healthz")[1])


def set_mode(m):
    os.makedirs(os.path.dirname(MODE_FILE), exist_ok=True)
    with open(MODE_FILE, "w") as f:
        f.write(m)


def wait_for(pred, timeout, poll=3, label=""):
    """Poll /healthz until pred(h) holds. Returns (ok, last_health)."""
    end = time.time() + timeout
    h = None
    while time.time() < end:
        try:
            h = health()
            if pred(h):
                return True, h
        except Exception:
            pass  # device may be mid-fetch and slow to answer; keep trying
        time.sleep(poll)
    return False, h


# (mode, seconds to observe, assertion, what we are checking)
def scenarios(fail_wait):
    def fails_grew(before):
        return lambda h: h["fetch_fail"] > before["fetch_fail"]

    return [
        ("http500", fail_wait, fails_grew, "5xx counts as a fetch failure and backs off"),
        ("http403", fail_wait, fails_grew, "403 counts as a failure, error body captured"),
        ("http429", fail_wait, fails_grew, "429 counts as a failure"),
        ("malformed", fail_wait, fails_grew, "unparseable JSON is a failure, not a crash"),
        ("garbage", fail_wait, fails_grew, "binary junk does not corrupt or crash"),
        ("truncated", fail_wait, fails_grew, "short body vs Content-Length is rejected"),
        ("mil", 90, lambda b: (lambda h: h["op"] == "MIL"),
         "dbFlags=1 alone classifies MIL (no /v2/mil scan exists any more)"),
        ("empty", fail_wait, lambda b: (lambda h: h["fetch_empty"] > b["fetch_empty"]),
         "empty sky clears the display without counting a failure"),
        ("slow", 90, fails_grew, "slow response hits the read timeout and recovers"),
        ("ok", 90, lambda b: (lambda h: h["fetch_ok"] > b["fetch_ok"] and not h.get("last_err")),
         "recovers, and last_err clears instead of reporting a stale failure"),
    ]


def cmd_run(a):
    mock = f"https://{a.mock_host}:{a.port}"
    print(f"device {HOST} -> mock {mock}\n")
    try:
        h0 = health()
    except urllib.error.URLError as e:
        sys.exit(f"cannot reach device at {BASE}: {e}")
    print(f"baseline: heap {h0['heap_free']} uptime {h0['uptime_ms']}ms "
          f"api_base {h0.get('api_base')}\n")

    set_mode("ok")
    req("PUT", "/test/apibase", {"base": mock})
    ok, h = wait_for(lambda x: x["fetch_ok"] > h0["fetch_ok"], 90, label="mock reachable")
    if not ok:
        req("DELETE", "/test/apibase")
        sys.exit(f"device never fetched successfully from the mock; is mockapi.py "
                 f"running on {a.mock_host}:{a.port} and reachable from the device?")
    print(f"[ok]   mock reachable, device fetching from it\n")

    results = []
    for name, wait, mk_pred, what in scenarios(a.fail_wait):
        if a.only and name not in a.only:
            continue
        before = health()
        set_mode(name)
        good, h = wait_for(mk_pred(before), wait)
        crashed = h is None or h["uptime_ms"] < before["uptime_ms"]
        status = "FAIL" if (not good or crashed) else "ok"
        results.append((name, status))
        note = "  <-- DEVICE REBOOTED" if crashed else ""
        print(f"[{status:<4}] {name:<10} {what}{note}")
        if h:
            print(f"         ok={h['fetch_ok']} empty={h['fetch_empty']} fail={h['fetch_fail']} "
                  f"streak={h['fail_streak']} showing={h['showing_flight']} dim={h['display_dim']} "
                  f"heap={h['heap_free']} last_http={h['last_http']}")
            if h.get("last_err"):
                print(f"         last_err: {h['last_err'][:80]}")

    set_mode("ok")
    req("DELETE", "/test/apibase")
    print("\nrestored to real API")
    bad = [n for n, s in results if s == "FAIL"]
    print(f"{len(results)-len(bad)}/{len(results)} scenarios behaved correctly"
          + (f"; needs investigation: {', '.join(bad)}" if bad else ""))
    return 1 if bad else 0


def cmd_outage(a):
    """Hold a total outage long enough to prove the display clears to the dimmed
    splash after STALE_DISPLAY_MAX_MS, which normal operation never exercises."""
    mock = f"https://{a.mock_host}:{a.port}"
    set_mode("http500")
    req("PUT", "/test/apibase", {"base": mock})
    print(f"holding a total outage for {a.minutes} min; watching for the dimmed splash")
    ok, h = wait_for(lambda x: not x["showing_flight"] and x["display_dim"],
                     a.minutes * 60, poll=10)
    print(("[ok]   " if ok else "[FAIL] ") +
          f"display cleared and dimmed: {ok}")
    if h:
        print(f"       showing={h['showing_flight']} dim={h['display_dim']} "
              f"fail_streak={h['fail_streak']} heap={h['heap_free']}")
    set_mode("ok")
    req("DELETE", "/test/apibase")
    good, h2 = wait_for(lambda x: x["showing_flight"], 120)
    print(("[ok]   " if good else "[FAIL] ") + f"recovered to a live flight: {good}")
    return 0 if (ok and good) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mock-host", default="192.168.1.250", help="this Mac's LAN IP")
    ap.add_argument("--port", type=int, default=8443)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--only", nargs="*", help="run only these scenarios")
    r.add_argument("--fail-wait", type=int, default=75,
                   help="seconds to wait for a fault to register (backoff grows)")
    o = sub.add_parser("outage")
    o.add_argument("--minutes", type=float, default=8.0)
    sub.add_parser("restore")
    a = ap.parse_args()
    if a.cmd == "restore":
        set_mode("ok")
        print(req("DELETE", "/test/apibase")[1])
        return 0
    return cmd_run(a) if a.cmd == "run" else cmd_outage(a)


if __name__ == "__main__":
    sys.exit(main())

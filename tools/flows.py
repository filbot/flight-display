#!/usr/bin/env python3
"""Tests for whole firmware flows, asserted against what the device actually
REQUESTED — not just what it displayed.

    tools/flows.py --mock-host 192.168.1.250

The mock records every request to logs/mock.requests.jsonl, which is the only
way to verify things like the km->nautical-mile conversion and the tiered radius
fallback: both are invisible from the display alone.

Run under tools/with-device-lock.sh so nothing else drives the device.
"""
import argparse, json, os, re, sys, time, urllib.error, urllib.request

HOST = os.environ.get("DEVICE_HOST", "192.168.1.229")
BASE = f"http://{HOST}"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.environ.get("LOGDIR", os.path.join(ROOT, "logs"))
MODE = os.path.join(LOGDIR, "mock.mode")
REQ_LOG = os.path.join(LOGDIR, "mock.requests.jsonl")

# From config.h. The API takes NAUTICAL MILES; the firmware converts.
SEARCH_RADIUS_KM = 10
SEARCH_RADIUS_FALLBACK_KM = 100
NM_PER_KM = 0.5399568
EXPECT_PRIMARY_NM = int(SEARCH_RADIUS_KM * NM_PER_KM + 0.5)
EXPECT_FALLBACK_NM = int(SEARCH_RADIUS_FALLBACK_KM * NM_PER_KM + 0.5)

results = []


def req(method, path, body=None, timeout=15):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.status, resp.read().decode()


def health():
    return json.loads(req("GET", "/healthz")[1])


def set_mode(m):
    with open(MODE, "w") as f:
        f.write(m)


def search_reqs(rows):
    """Aircraft-search requests, whichever endpoint served them.

    The primary circle moved to /v2/point when the emergency-squawk feature
    landed; the widened fallback stays on /v2/closest."""
    return [r for r in rows if "/v2/point/" in r["path"] or "/v2/closest/" in r["path"]]


def radius_of(r):
    return int(r["path"].rstrip("/").split("/")[-1])


def reqs_since(mark):
    out = []
    if not os.path.exists(REQ_LOG):
        return out
    for i, line in enumerate(open(REQ_LOG)):
        if i < mark:
            continue
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def req_count():
    return sum(1 for _ in open(REQ_LOG)) if os.path.exists(REQ_LOG) else 0


def force_fetch(mock):
    """Re-PUT the base to reset the fetch timer and fetch immediately."""
    req("PUT", "/test/apibase", {"base": mock})


def wait_fetches(before, need, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        h = health()
        if h["fetch_ok"] + h["fetch_empty"] + h["fetch_fail"] >= before + need:
            time.sleep(1.5)
            return h
        time.sleep(1)
    return health()


def check(name, ok, detail, why):
    results.append((name, ok))
    print(f"[{'ok  ' if ok else 'FAIL'}] {name:<22} {why}\n{'':<9}{detail}", flush=True)


def t_radius_conversion(mock):
    """SEARCH_RADIUS_KM is real km; the API parameter is nautical miles."""
    set_mode("ok")
    mark = req_count()
    c = health()
    force_fetch(mock)
    wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1)
    got = search_reqs(reqs_since(mark))
    radii = [radius_of(r) for r in got]
    ok = bool(radii) and radii[0] == EXPECT_PRIMARY_NM
    check("radius_conversion", ok,
          f"requested radius {radii[:3]} nm, expected {EXPECT_PRIMARY_NM} "
          f"(= {SEARCH_RADIUS_KM} km); path {got[0]['path'] if got else 'none'}",
          f"{SEARCH_RADIUS_KM} km must be sent as {EXPECT_PRIMARY_NM} nm, not as km")


def t_home_coords(mock):
    """The URL must carry the configured home position."""
    mark = req_count()
    c = health()
    force_fetch(mock)
    wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1)
    got = search_reqs(reqs_since(mark))
    m = re.search(r"/v2/(?:closest|point)/(-?[\d.]+)/(-?[\d.]+)/", got[0]["path"]) if got else None
    ok = bool(m) and abs(float(m.group(1)) - 47.6506) < 1e-4 and abs(float(m.group(2)) + 122.3694) < 1e-4
    check("home_coords", ok, f"path {got[0]['path'] if got else 'none'}",
          "URL carries HOME_LAT/HOME_LON at full precision")


def t_tiered_fallback(mock):
    """Never exercised before: the widened search only fires on an empty primary."""
    set_mode("far")  # nothing inside the primary circle, one aircraft beyond it
    mark = req_count()
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    got = search_reqs(reqs_since(mark))
    radii = [radius_of(r) for r in got]
    widened = EXPECT_FALLBACK_NM in radii
    ok = widened and radii[:1] == [EXPECT_PRIMARY_NM] and h.get("showing_flight")
    check("tiered_fallback", ok,
          f"radii requested {radii[:4]} (want {EXPECT_PRIMARY_NM} then {EXPECT_FALLBACK_NM}); "
          f"showing={h.get('showing_flight')} ident={h.get('ident')!r}",
          "empty primary circle must trigger one widened search and display the result")
    set_mode("ok")


def t_no_widen_when_found(mock):
    """The widened search must NOT fire when the primary circle has an aircraft."""
    set_mode("ok")
    mark = req_count()
    c = health()
    force_fetch(mock)
    wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1)
    got = search_reqs(reqs_since(mark))
    radii = [radius_of(r) for r in got]
    # The assertion is ONE call per cycle, not which radius. Under sticky wide
    # mode the device may legitimately still be on the wide radius and drop back
    # to the primary only once it sees an aircraft close enough — what must
    # never happen is paying for two searches in one cycle.
    ok = len(radii) == 1
    check("no_widen_when_found", ok, f"radii requested {radii[:4]}",
          "a populated primary circle must cost exactly one API call")


def t_user_agent(mock):
    """The device must send a contact-carrying UA; the mock 403s generic ones."""
    set_mode("uaenforce")
    mark = req_count()
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1)
    got = reqs_since(mark)
    uas = {r["ua"] for r in got}
    rejected = [r for r in got if r["status"] == 403]
    ok = bool(uas) and not rejected
    check("user_agent", ok, f"UA sent {list(uas)[:1]}, {len(rejected)} rejected",
          "UA must carry contact info or the real API returns 403")
    set_mode("ok")


def t_override_ttl(mock):
    """The test override must expire and hand the display back to live data."""
    req("PUT", "/test/closest", {"ident": "TTLTEST", "t": "B738", "alt": 10000, "dist": 5.0})
    time.sleep(3)
    a = health()
    ok_active = a.get("test_override") and a.get("ident") == "TTLTEST"
    req("DELETE", "/test/closest")
    time.sleep(3)
    b = health()
    ok = ok_active and not b.get("test_override")
    check("override_ttl", ok,
          f"active: override={a.get('test_override')} ident={a.get('ident')!r}; "
          f"after DELETE: override={b.get('test_override')}",
          "PUT /test/closest takes over the display and DELETE releases it")


def t_empty_clears(mock):
    """An empty-but-valid response must clear immediately, not count as failure."""
    set_mode("empty")
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    ok = (not h["showing_flight"]) and h["display_dim"] and h["fetch_empty"] > c["fetch_empty"] \
        and h["fetch_fail"] == c["fetch_fail"]
    check("empty_clears", ok,
          f"showing={h['showing_flight']} dim={h['display_dim']} "
          f"empty {c['fetch_empty']}->{h['fetch_empty']} fail {c['fetch_fail']}->{h['fetch_fail']}",
          "empty sky clears the display, dims it, and is not counted as a failure")
    set_mode("ok")


def t_emergency_preempts(mock):
    """An emergency squawk anywhere in the circle outranks the nearest aircraft.

    The mock puts 7700 on the 8 km C172 while a normal B738 sits at 3 km, so a
    correct implementation shows the C172 — the whole point of fetching every
    aircraft rather than just the closest one."""
    set_mode("emergency")
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    ok = h.get("squawk") == "7700" and h.get("emergency") == "EMERGENCY 7700" \
        and h.get("type") == "C172"
    check("emergency_preempts", ok,
          f"showing type={h.get('type')!r} ident={h.get('ident')!r} "
          f"squawk={h.get('squawk')!r} banner={h.get('emergency')!r} "
          f"dist={h.get('dist_km')}",
          "7700 on a farther aircraft beats a normal nearer one")
    set_mode("ok")


def t_lifeguard(mock):
    """lifeguard has NO squawk equivalent — it exists only in the status field,
    so a squawk-only implementation is blind to it."""
    set_mode("lifeguard")
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    ok = h.get("emergency") == "LIFEGUARD" and h.get("type") == "C172" \
        and h.get("emergency_status") == "lifeguard"
    check("lifeguard", ok,
          f"type={h.get('type')!r} status={h.get('emergency_status')!r} "
          f"banner={h.get('emergency')!r} squawk={h.get('squawk')!r}",
          "medical flight is surfaced from the status field, with no squawk set")
    set_mode("ok")


def t_alert_priority(mock):
    """Severity beats proximity: a 7700 at 8 km outranks a lifeguard at 3 km."""
    set_mode("priority")
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    ok = h.get("emergency") == "EMERGENCY 7700" and h.get("type") == "C172"
    check("alert_priority", ok,
          f"type={h.get('type')!r} dist={h.get('dist_km')} banner={h.get('emergency')!r}",
          "the more severe alert wins even though the other aircraft is closer")
    set_mode("ok")


def t_normal_squawk_ignored(mock):
    """An ordinary squawk must change nothing — the nearest aircraft still wins."""
    set_mode("ok")
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    ok = h.get("emergency") == "" and h.get("type") == "B738"
    check("normal_squawk_ignored", ok,
          f"showing type={h.get('type')!r} squawk={h.get('squawk')!r} "
          f"banner={h.get('emergency')!r}",
          "ordinary squawk leaves the nearest-aircraft behaviour untouched")


def t_uses_point_endpoint(mock):
    """The primary search must use /v2/point; the fallback must stay /v2/closest."""
    set_mode("far")  # empty primary forces the widened fallback too
    mark = req_count()
    c = health()
    force_fetch(mock)
    wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    got = [r["path"] for r in reqs_since(mark) if "/v2/" in r["path"]]
    point = [p for p in got if "/point/" in p]
    closest = [p for p in got if "/closest/" in p]
    # BOTH searches must use /v2/point. A single-aircraft /v2/closest response
    # can fail the position-freshness gate, which declared the whole ring empty
    # and showed "No aircraft nearby" over a busy sky — and it also silently
    # disabled emergency scanning in the widened ring.
    ok = bool(point) and not closest
    check("uses_point_endpoint", ok,
          f"point requests {len(point)}, closest requests {len(closest)}; {got[:3]}",
          "both the primary and widened searches scan all aircraft, not just the nearest")
    set_mode("ok")


def t_recovers(mock):
    set_mode("ok")
    c = health()
    force_fetch(mock)
    h = wait_fetches(c["fetch_ok"] + c["fetch_empty"] + c["fetch_fail"], 1, timeout=90)
    ok = h["showing_flight"] and not h["display_dim"] and not h.get("last_err")
    check("recovers", ok,
          f"showing={h['showing_flight']} ident={h.get('ident')!r} dim={h['display_dim']} "
          f"last_err={h.get('last_err')!r}",
          "returns to a live flight at full brightness with no stale error")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mock-host", default="192.168.1.250")
    ap.add_argument("--port", type=int, default=8443)
    a = ap.parse_args()
    mock = f"https://{a.mock_host}:{a.port}"
    try:
        health()
    except urllib.error.URLError as e:
        sys.exit(f"cannot reach device at {BASE}: {e}")
    set_mode("ok")
    req("PUT", "/test/apibase", {"base": mock})
    time.sleep(4)
    for t in (t_radius_conversion, t_home_coords, t_tiered_fallback, t_no_widen_when_found,
              t_uses_point_endpoint, t_emergency_preempts, t_lifeguard, t_alert_priority,
              t_normal_squawk_ignored,
              t_user_agent, t_override_ttl, t_empty_clears, t_recovers):
        try:
            t(mock)
        except Exception as e:
            check(t.__name__, False, f"exception: {e}", "test raised")
    set_mode("ok")
    req("DELETE", "/test/apibase")
    bad = [n for n, ok in results if not ok]
    print(f"\n{len(results)-len(bad)}/{len(results)} flows correct"
          + (f"; failing: {', '.join(bad)}" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

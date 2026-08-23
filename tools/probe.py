#!/usr/bin/env python3
"""Drive every JSON/parse/classify branch in the firmware with a crafted payload
and record what the device actually concluded.

    tools/probe.py --mock-host 192.168.1.250          # one full pass
    tools/probe.py --mock-host 192.168.1.250 --loop   # repeat until stopped

Each case writes logs/mock.body, forces an immediate refetch, then reads
/healthz and compares against `expect`. Results append to logs/probe.jsonl so a
long run can be summarised later by tools/nightreport.py.

`expect` is a dict of /healthz fields; None means "must be absent/empty".
A case with expect=None is exploratory: recorded, never failed. Use those for
behaviour we want measured but have not yet decided is right or wrong.
"""
import argparse, json, os, sys, tempfile, time, urllib.error, urllib.request

HOST = os.environ.get("DEVICE_HOST", "192.168.1.229")
BASE = f"http://{HOST}"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.environ.get("LOGDIR", os.path.join(ROOT, "logs"))
BODY = os.path.join(LOGDIR, "mock.body")
MODE = os.path.join(LOGDIR, "mock.mode")
OUT = os.path.join(LOGDIR, "probe.jsonl")

BASE_AC = {
    "hex": "abc123", "type": "adsb_icao", "flight": "TEST123 ", "r": "N123AB",
    "t": "B738", "alt_baro": 30000, "lat": 47.6520, "lon": -122.3700,
    "seen_pos": 1.0, "category": "A3", "desc": "BOEING 737-800",
}


def ac(**kw):
    """Base aircraft with overrides; a value of ... deletes the key."""
    a = dict(BASE_AC)
    for k, v in kw.items():
        if v is ...:
            a.pop(k, None)
        else:
            a[k] = v
    return json.dumps({"ac": [a], "total": 1})


# (name, raw body, expect-dict-or-None, what this exercises)
CASES = [
    # --- ident preference chain: flight -> r -> hex -> "(unknown)"
    ("ident_flight",   ac(), {"ident": "TEST123"}, "flight wins, trailing pad trimmed"),
    ("ident_r",        ac(flight=...), {"ident": "N123AB"}, "no flight falls back to registration"),
    ("ident_hex",      ac(flight=..., r=...), {"ident": "abc123"}, "no flight/r falls back to hex"),
    ("ident_none",     ac(flight=..., r=..., hex=...), {"ident": "(unknown)"}, "all identifiers absent"),
    # Regression for the padding-callsign fix: "        " must fall through to
    # the registration and must NOT count as a callsign (which forced COM).
    ("ident_ws_only",  ac(flight="        "), {"ident": "N123AB", "has_callsign": False},
     "padding-only flight falls through to registration, not a callsign"),
    ("ident_ws_pvt",   ac(flight="        ", t="C172", r=...), {"op": "PVT"},
     "padding-only callsign on a light aircraft classifies PVT, not COM"),
    ("ident_lead_ws",  ac(flight="   ABC12"), {"ident": "ABC12"}, "leading padding trimmed"),
    ("ident_empty",    ac(flight=""), {"ident": "N123AB"}, "empty flight falls through to r"),
    ("ident_long",     ac(flight="ABCDEFGHIJKLMNOPQRSTUVWXYZ"), None, "over-long ident truncation"),
    ("ident_unicode",  ac(flight="ÜNI✈CODE"), None, "multi-byte ident handling"),

    # --- position gating (extractLatLon)
    ("pos_ok",         ac(), {"showing_flight": True}, "fresh position accepted"),
    ("pos_no_seenpos", ac(seen_pos=...), {"showing_flight": False}, "missing seen_pos must be rejected"),
    ("pos_stale",      ac(seen_pos=9999), {"showing_flight": False}, "stale position must be rejected"),
    ("pos_seenpos_null", ac(seen_pos=None), None, "null seen_pos — treated as fresh?"),
    ("pos_zero",       ac(lat=0, lon=0), {"showing_flight": False}, "(0,0) must be rejected"),
    ("pos_missing",    ac(lat=..., lon=...), {"showing_flight": False}, "absent lat/lon rejected"),
    ("pos_antipode",   ac(lat=-47.65, lon=57.63), None, "far-side distance maths"),

    # --- altitude branches
    ("alt_ground_str", ac(alt_baro="ground"), None, "alt_baro string 'ground'"),
    ("alt_zero",       ac(alt_baro=0), None, "0 ft treated as on-ground"),
    ("alt_negative",   ac(alt_baro=-250), None, "negative altitude treated as on-ground"),
    ("alt_geom_only",  ac(alt_baro=..., alt_geom=17500), None, "alt_geom fallback"),
    ("alt_absent",     ac(alt_baro=..., alt_geom=...), None, "no altitude at all"),
    ("alt_numstring",  ac(alt_baro="12345"), None, "numeric string, not 'ground'"),
    ("alt_huge",       ac(alt_baro=999999), None, "absurd altitude"),
    ("alt_minus_one",  ac(alt_baro=-1), None, "-1 collides with the absent sentinel"),

    # --- type / description
    ("type_known",     ac(t="A320"), None, "known ICAO type"),
    ("type_unknown",   ac(t="ZZZZ"), None, "unknown type falls back to desc"),
    ("type_absent",    ac(t=...), None, "no type at all"),
    ("type_nodesc",    ac(t="ZZZZ", desc=...), None, "unknown type and no desc"),
    ("type_prefix",    ac(t="B77L"), None, "family-prefix heuristic path"),
    ("type_long",      ac(t="VERYLONGTYPECODE"), None, "over-long type code"),

    # --- classification
    ("cls_mil",        ac(dbFlags=1), {"op": "MIL"}, "dbFlags bit 0 = military"),
    ("cls_ladd",       ac(dbFlags=8), None, "LADD flag is not military"),
    ("cls_mil_ladd",   ac(dbFlags=9), {"op": "MIL"}, "military bit set alongside others"),
    ("cls_dbflags_str", ac(dbFlags="1"), None, "dbFlags as a string"),
    ("cls_small",      ac(t="C172", dbFlags=...), {"op": "PVT"}, "small aircraft = private"),
    ("cls_airliner",   ac(t="B738"), {"op": "COM"}, "airliner with callsign = commercial"),
    ("cls_nocallsign", ac(t="B738", flight=..., r=...), None, "airliner with no callsign"),
    ("cls_cat_a7",     ac(t=..., category="A7", desc=...), None, "rotorcraft category"),
    ("cls_cat_b2",     ac(t=..., category="B2", desc=...), None, "balloon category"),

    # --- malformed / structural
    ("bad_ac_object",  json.dumps({"ac": {"hex": "x"}}), {"showing_flight": False}, "ac is an object not an array"),
    ("bad_ac_missing", json.dumps({"total": 0}), {"showing_flight": False}, "no ac key at all"),
    ("bad_root_array", json.dumps([BASE_AC]), {"showing_flight": False}, "root is an array"),
    ("bad_root_scalar", "42", {"showing_flight": False}, "root is a scalar"),
    ("bad_empty_obj",  "{}", {"showing_flight": False}, "empty object"),
    ("bad_nulls",      json.dumps({"ac": [{k: None for k in BASE_AC}]}), {"showing_flight": False}, "every field null"),
    ("bad_deep",       json.dumps({"ac": [BASE_AC], "x": {"y": {"z": [1] * 50}}}), None, "extra deep/unfiltered data"),
    ("bad_many_ac",    json.dumps({"ac": [BASE_AC] * 40}), None, "40 aircraft vs the 2048-byte doc"),
    ("bad_bignum",     ac(alt_baro=10**18), None, "value beyond int32"),
    ("bad_types",      ac(lat="47.65", lon="-122.37"), None, "lat/lon sent as strings"),
]


def stage(body):
    """Swap the mock body atomically; a truncating write races the mock's read."""
    fd, tmp = tempfile.mkstemp(dir=LOGDIR)
    with os.fdopen(fd, "w") as f:
        f.write(body)
    os.replace(tmp, BODY)


def req(method, path, body=None, timeout=15):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.status, resp.read().decode()


def health():
    return json.loads(req("GET", "/healthz")[1])


SYNC = json.dumps({"ac": [dict(BASE_AC, flight="SYNCPING", r="SYNCPING", hex="5y5c00")], "total": 1})


def _counter(h):
    return h.get("fetch_ok", 0) + h.get("fetch_empty", 0) + h.get("fetch_fail", 0)


def _await_fetch(mock, before, need=2):
    """Wait for `need` fetches to complete.

    One is not enough: a fetch already in flight when the body was staged
    completes against the OLD body and still bumps the counter, so the probe
    samples the previous case. Requiring two guarantees at least one fetch
    both started and finished after the swap.
    """
    h = health()
    for _ in range(60):
        time.sleep(1)
        h = health()
        if _counter(h) >= before + need:
            time.sleep(2.0)  # let the render settle before sampling
            return health()
    return h


def run_case(name, body, expect, why, mock):
    # Sync barrier: stage a sentinel payload and wait until the device is
    # actually showing it. Counting fetches alone is not enough — an in-flight
    # fetch from the previous payload (or from the live API) can complete just
    # after the switch, so the probe sampled the previous case's result in ~1%
    # of overnight runs. Now the case only starts from a known state.
    stage(SYNC)
    for _ in range(4):
        h0 = health()
        req("PUT", "/test/apibase", {"base": mock})
        h = _await_fetch(mock, _counter(h0))
        if h.get("ident") == "SYNCPING":
            break
    stage(body)
    # Re-PUT the same base to reset g_nextFetchAt and force an immediate fetch,
    # so a case costs ~4s instead of a full 30s cycle.
    # One sample, not three: calling health() three times let a fetch land
    # between the calls, corrupting the baseline so the wait below fell through
    # and sampled the previous case's result (~1% of runs overnight).
    h = None
    for _ in range(4):
        before = _counter(health())
        req("PUT", "/test/apibase", {"base": mock})
        h = _await_fetch(mock, before)
        if h.get("ident") != "SYNCPING":
            break  # the device has moved off the sentinel, so this is our payload
    got = {"ident": h.get("ident"), "op": h.get("op"), "type": h.get("type"),
           "title": h.get("title"), "alt_ft": h.get("alt_ft"), "dist_km": h.get("dist_km"),
           "has_callsign": h.get("has_callsign"), "db_flags": h.get("db_flags"),
           "showing_flight": h.get("showing_flight"), "fetch_ok": h.get("fetch_ok"),
           "fetch_fail": h.get("fetch_fail"), "heap_free": h.get("heap_free"),
           "uptime_ms": h.get("uptime_ms")}
    status = "info"
    if expect:
        bad = {k: (v, got.get(k)) for k, v in expect.items() if got.get(k) != v}
        status = "pass" if not bad else "FAIL"
        got["mismatch"] = bad
    return status, got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mock-host", default="192.168.1.250")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    mock = f"https://{a.mock_host}:{a.port}"
    os.makedirs(LOGDIR, exist_ok=True)
    with open(MODE, "w") as f:
        f.write("custom")

    npass = nfail = ninfo = 0
    try:
        while True:
            for name, body, expect, why in CASES:
                if a.only and name not in a.only:
                    continue
                try:
                    status, got = run_case(name, body, expect, why, mock)
                except Exception as e:
                    status, got = "ERROR", {"error": str(e)}
                npass += status == "pass"; nfail += status == "FAIL"; ninfo += status == "info"
                rec = {"ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                       "case": name, "status": status, "why": why, "got": got}
                with open(OUT, "a") as f:
                    f.write(json.dumps(rec) + "\n")
                mark = {"pass": "ok  ", "FAIL": "FAIL", "info": "--  ", "ERROR": "ERR "}[status]
                extra = ""
                if status == "FAIL":
                    extra = f"  mismatch={got.get('mismatch')}"
                elif status == "info":
                    extra = (f"  ident={got.get('ident')!r} op={got.get('op')} "
                             f"title={got.get('title')!r} alt={got.get('alt_ft')} "
                             f"show={got.get('showing_flight')}")
                print(f"[{mark}] {name:<18} {why}{extra}", flush=True)
            if not a.loop:
                break
    finally:
        with open(MODE, "w") as f:
            f.write("ok")
        try:
            req("DELETE", "/test/apibase")
        except Exception:
            pass
    print(f"\n{npass} pass, {nfail} FAIL, {ninfo} informational")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Summarize a soak run: reboots, heap drift, Wi-Fi, API health, outages.

    tools/report.py                 # everything in logs/
    tools/report.py --since 24      # last 24 hours only

Reads logs/health.jsonl and logs/serial.log written by tools/monitor.sh.
Read-only; it never touches the device.
"""
import argparse, json, os, re, sys
from datetime import datetime, timedelta, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.environ.get("LOGDIR", os.path.join(ROOT, "logs"))


def parse_ts(s):
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def load_health(since):
    rows = []
    path = os.path.join(LOGDIR, "health.jsonl")
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
                r["_ts"] = parse_ts(r["ts"])
            except (ValueError, KeyError):
                continue  # a torn last line during an active write is normal
            if since is None or r["_ts"] >= since:
                rows.append(r)
    return rows


def fmt_dur(ms):
    s = int(ms // 1000)
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    return (f"{d}d " if d else "") + f"{h:02d}:{m:02d}:{s:02d}"


def report_health(rows):
    if not rows:
        print("no health samples")
        return
    up = [r for r in rows if r.get("ok")]
    miss = len(rows) - len(up)
    span = rows[-1]["_ts"] - rows[0]["_ts"]
    print(f"WINDOW      {rows[0]['ts']} -> {rows[-1]['ts']}  ({span})")
    print(f"SAMPLES     {len(rows)}  reachable {len(up)}  unreachable {miss}"
          f"  ({100.0*miss/len(rows):.1f}% miss)")
    if not up:
        print("device never answered /healthz in this window")
        return

    h = [r["health"] for r in up]

    # Reboots: uptime going backwards is the only reliable signal, since a fast
    # crash-and-recover can happen entirely between two polls.
    reboots = []
    for prev, cur in zip(up, up[1:]):
        if cur["health"]["uptime_ms"] < prev["health"]["uptime_ms"]:
            reboots.append((cur["ts"], cur["health"].get("reset_reason", "?")))
    print(f"UPTIME      {fmt_dur(h[-1]['uptime_ms'])} (current), last reset {h[-1].get('reset_reason')}")
    print(f"REBOOTS     {len(reboots)}")
    for ts, why in reboots[-10:]:
        print(f"              {ts}  reason={why}")

    free = [x["heap_free"] for x in h]
    mins = [x["heap_min"] for x in h]
    print(f"HEAP        free now {free[-1]}  min-ever {min(mins)}  "
          f"largest-block {h[-1].get('heap_largest_block')}")
    # Compare halves: a real leak shows as a persistent downward shift, which a
    # first-vs-last sample can't distinguish from ordinary fetch-cycle noise.
    half = len(free) // 2
    if half:
        d = sum(free[half:]) / len(free[half:]) - sum(free[:half]) / half
        flag = "  <-- LEAK?" if d < -8000 else ""
        print(f"HEAP DRIFT  {d:+.0f} bytes (2nd half vs 1st half avg){flag}")

    rssi = [x["rssi"] for x in h if x.get("wifi_up")]
    if rssi:
        print(f"WIFI        rssi min {min(rssi)} avg {sum(rssi)/len(rssi):.0f} max {max(rssi)} dBm"
              f"  down-samples {sum(1 for x in h if not x.get('wifi_up'))}")

    ok, empty, fail = h[-1]["fetch_ok"], h[-1]["fetch_empty"], h[-1]["fetch_fail"]
    tot = ok + empty + fail
    print(f"FETCH       ok {ok}  empty {empty}  fail {fail}"
          + (f"  ({100.0*fail/tot:.1f}% fail)" if tot else ""))
    print(f"LAST HTTP   {h[-1].get('last_http')}  {h[-1].get('last_err','')}".rstrip())

    codes = {}
    for x in h:
        c = x.get("last_http")
        if c and c != 200:
            codes[c] = codes.get(c, 0) + 1
    if codes:
        print("HTTP ERRS   " + "  ".join(f"{c}x{n}" for c, n in sorted(codes.items())))

    # No-data stretches: consecutive samples with nothing on screen. This is the
    # user-visible failure, so it gets reported in wall-clock, not sample counts.
    outages, run = [], None
    for r in up:
        if not r["health"].get("showing_flight"):
            run = run or r["ts"]
        elif run:
            outages.append((run, r["ts"]))
            run = None
    if run:
        outages.append((run, "ongoing"))
    print(f"NO-DATA     {len(outages)} stretch(es)")
    for a, b in outages[-10:]:
        dur = "ongoing" if b == "ongoing" else str(parse_ts(b) - parse_ts(a))
        print(f"              {a} -> {b}  ({dur})")
    print(f"DIM NOW     {h[-1].get('display_dim')}   showing "
          f"{h[-1].get('ident') or '(splash)'}")


SERIAL_FLAGS = [
    (re.compile(r"Guru Meditation|abort\(\)|assert failed|Backtrace:"), "CRASH"),
    (re.compile(r"CRITICAL|restarting"), "RESTART"),
    (re.compile(r"MIL scan incomplete"), "MIL_INCOMPLETE"),
    (re.compile(r"Heap low|heap"), "HEAP"),
    (re.compile(r"WiFi retry|Wi-Fi"), "WIFI"),
    (re.compile(r"HTTP status: (?!200)"), "HTTP_ERR"),
    (re.compile(r"Fetch failed"), "FETCH_FAIL"),
    (re.compile(r"\[Boot\]"), "BOOT"),
]


def report_serial(tail):
    path = os.path.join(LOGDIR, "serial.log")
    if not os.path.exists(path):
        print("\nno serial log")
        return
    with open(path, errors="replace") as f:
        lines = f.readlines()
    counts, samples = {}, {}
    for ln in lines:
        for rx, tag in SERIAL_FLAGS:
            if rx.search(ln):
                counts[tag] = counts.get(tag, 0) + 1
                samples.setdefault(tag, []).append(ln.rstrip())
                break
    print(f"\nSERIAL      {len(lines)} lines")
    for tag in sorted(counts, key=lambda t: -counts[t]):
        print(f"  {tag:<15} {counts[tag]}")
    for tag in ("CRASH", "RESTART", "MIL_INCOMPLETE"):
        for ln in samples.get(tag, [])[-tail:]:
            print(f"    {ln}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", type=float, metavar="HOURS", help="only the last N hours")
    ap.add_argument("--tail", type=int, default=3, help="sample lines per serial flag")
    a = ap.parse_args()
    since = datetime.now(timezone.utc) - timedelta(hours=a.since) if a.since else None
    report_health(load_health(since))
    report_serial(a.tail)


if __name__ == "__main__":
    main()

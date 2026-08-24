#!/usr/bin/env python3
"""Compile the overnight run into docs/soak-report.md.

    tools/nightreport.py [--since HOURS]

Reads logs/health.jsonl, logs/serial.log, logs/probe.jsonl and
logs/overnight.log. Writes a dated report and prints it. Safe to run any time,
as often as you like — it only reads logs.
"""
import argparse, collections, json, os, re, subprocess, sys, time
from datetime import datetime, timedelta, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.environ.get("LOGDIR", os.path.join(ROOT, "logs"))
OUT = os.path.join(ROOT, "docs", "soak-report.md")


def ts(s):
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def load_health(since):
    rows = []
    p = os.path.join(LOGDIR, "health.jsonl")
    if not os.path.exists(p):
        return rows
    for line in open(p):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
            r["_ts"] = ts(r["ts"])
        except (ValueError, KeyError):
            continue
        if since is None or r["_ts"] >= since:
            rows.append(r)
    return rows


def load_probe(since):
    rows = []
    p = os.path.join(LOGDIR, "probe.jsonl")
    if not os.path.exists(p):
        return rows
    for line in open(p):
        try:
            r = json.loads(line)
            r["_ts"] = ts(r["ts"])
        except (ValueError, KeyError):
            continue
        if since is None or r["_ts"] >= since:
            rows.append(r)
    return rows


def serial_lines(since):
    p = os.path.join(LOGDIR, "serial.log")
    if not os.path.exists(p):
        return []
    out = []
    for ln in open(p, errors="replace"):
        try:
            if since and ts(ln.split(" ", 1)[0]) < since:
                continue
        except (ValueError, IndexError):
            continue
        out.append(ln.rstrip())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", type=float, default=None, metavar="HOURS")
    a = ap.parse_args()
    since = datetime.now(timezone.utc) - timedelta(hours=a.since) if a.since else None

    health = load_health(since)
    probe = load_probe(since)
    lines = serial_lines(since)
    up = [r for r in health if r.get("ok")]
    h = [r["health"] for r in up]

    L = []
    W = L.append
    now = datetime.now(timezone.utc)
    W(f"# Flight Display — soak report\n")
    W(f"Generated {now.strftime('%Y-%m-%d %H:%M')} UTC"
      + (f" · window: last {a.since:g}h" if a.since else " · window: full history") + "\n")

    # ---- availability and stability
    W("## Run summary\n")
    if health:
        span = health[-1]["_ts"] - health[0]["_ts"]
        miss = len(health) - len(up)
        W(f"- Window `{health[0]['ts']}` → `{health[-1]['ts']}` ({span}), "
          f"{len(health)} health samples, {miss} unreachable ({100.0*miss/len(health):.1f}%).")
    reboots = [(c["ts"], c["health"].get("reset_reason"))
               for p, c in zip(up, up[1:])
               if c["health"]["uptime_ms"] < p["health"]["uptime_ms"]]
    if h:
        W(f"- Uptime now {h[-1]['uptime_ms']//1000}s, last reset `{h[-1].get('reset_reason')}`; "
          f"{len(reboots)} reboot(s) observed in window.")
        for t, why in reboots[-8:]:
            W(f"  - `{t}` reason `{why}`")
        free = [x["heap_free"] for x in h]
        half = len(free) // 2
        drift = (sum(free[half:]) / max(1, len(free[half:])) - sum(free[:half]) / max(1, half)) if half else 0
        W(f"- Heap: now {free[-1]}, min-ever {min(x['heap_min'] for x in h)}, "
          f"largest block {h[-1].get('heap_largest_block')}, half-vs-half drift {drift:+.0f} bytes.")
        rssi = [x["rssi"] for x in h if x.get("wifi_up")]
        if rssi:
            W(f"- Wi-Fi: RSSI min {min(rssi)} / avg {sum(rssi)/len(rssi):.0f} / max {max(rssi)} dBm, "
              f"{sum(1 for x in h if not x.get('wifi_up'))} sample(s) with the link down.")
        W(f"- Fetch counters (lifetime since last boot): ok {h[-1]['fetch_ok']}, "
          f"empty {h[-1]['fetch_empty']}, fail {h[-1]['fetch_fail']}.")
    W("")

    # ---- crashes / watchdog
    crash = [l for l in lines if re.search(r"Guru Meditation|assert failed|Backtrace:|task_wdt", l)]
    deliberate = [l for l in lines if "hanging loop() on purpose" in l]
    W("## Crashes and watchdog\n")
    W(f"- {len(crash)} crash/watchdog line(s) in the serial window"
      + (f", of which {len(deliberate)} follow a deliberate /test/hang." if deliberate else "."))
    for l in crash[-5:]:
        W(f"  - `{l[:150]}`")
    W("")

    # ---- latency
    W("## Measured latency\n")
    ev, durs = [], []
    start = None
    for l in lines:
        m = re.search(r"\[FD\]\[(\d+)\] (HTTP GET|HTTP status: (-?\d+))", l)
        if not m:
            continue
        if m.group(2) == "HTTP GET":
            start = int(m.group(1))
        elif start is not None:
            durs.append(int(m.group(1)) - start)
            start = None
    if durs:
        d = sorted(durs)
        W(f"- {len(d)} request(s): p50 {d[len(d)//2]}ms, p95 {d[int(len(d)*0.95)]}ms, max {max(d)}ms.")
    W("")

    # ---- probe matrix
    W("## Data-path probe matrix\n")
    if probe:
        by = collections.defaultdict(list)
        for r in probe:
            by[r["case"]].append(r)
        npass = sum(1 for r in probe if r["status"] == "pass")
        nfail = sum(1 for r in probe if r["status"] == "FAIL")
        W(f"- {len(probe)} case-runs over {len(by)} distinct cases: {npass} pass, {nfail} fail.")
        # a case that ever failed is worth naming
        flaky = {c: rs for c, rs in by.items() if any(r["status"] == "FAIL" for r in rs)}
        for c, rs in sorted(flaky.items()):
            nf = sum(1 for r in rs if r["status"] == "FAIL")
            W(f"  - `{c}` failed {nf}/{len(rs)} run(s); last mismatch: "
              f"`{[r['got'].get('mismatch') for r in rs if r['status']=='FAIL'][-1]}`")
        W("")
        W("Observed behaviour for the exploratory cases (latest run of each):\n")
        W("| case | ident | op | title | alt_ft | shown |")
        W("|---|---|---|---|---|---|")
        for c in sorted(by):
            r = by[c][-1]
            if r["status"] != "info":
                continue
            g = r["got"]
            W(f"| `{c}` | `{g.get('ident')!r}` | {g.get('op')} | {g.get('title')} "
              f"| {g.get('alt_ft')} | {g.get('showing_flight')} |")
    W("")

    # ---- suite results scraped from the overnight log
    W("## Suite results\n")
    olog = os.path.join(LOGDIR, "overnight.log")
    if os.path.exists(olog):
        txt = open(olog, errors="replace").read()
        for label, pat in (("fault scenarios", r"(\d+)/(\d+) scenarios behaved correctly"),
                           ("display cases", r"(\d+)/(\d+) cases applied cleanly"),
                           ("probe passes", r"(\d+) pass, (\d+) FAIL")):
            ms = re.findall(pat, txt)
            if ms:
                worst = min(ms, key=lambda t: int(t[0]) - (int(t[1]) if label != "probe passes" else 0))
                W(f"- {label}: {len(ms)} run(s), most recent `{ms[-1][0]}/{ms[-1][1]}`, worst `{worst[0]}/{worst[1]}`.")
        passes = len(re.findall(r"--- pass (\d+) done", txt))
        W(f"- {passes} complete overnight pass(es).")
    W("")

    # ---- performance
    W("## Performance\n")
    if h and "loop_count" in h[-1]:
        per = [x for x in h if "loop_count" in x]
        # counters are cumulative since boot, so work in deltas and drop the
        # pairs that straddle a reboot
        pairs = [(a, b) for a, b in zip(per, per[1:]) if b["loop_count"] > a["loop_count"]]
        if pairs:
            def rate(key):
                tot = sum(b[key] - a[key] for a, b in pairs)
                secs = sum((b["uptime_ms"] - a["uptime_ms"]) / 1000.0 for a, b in pairs)
                return tot, (tot / secs if secs else 0), secs
            it, ips, secs = rate("loop_count")
            W(f"- Loop: {it:,} iterations over {secs/3600:.1f}h = **{ips:.0f}/s**. "
              f"Worst single iteration seen: **{max(x['loop_max_ms'] for x in per)} ms** "
              f"(task watchdog fires at {25_000} ms).")
            for k, label in (("loop_gt50ms", ">50ms"), ("loop_gt500ms", ">500ms"), ("loop_gt5s", ">5s")):
                if k in per[-1]:
                    tot, r, _ = rate(k)
                    W(f"  - iterations {label}: {tot:,} ({r*60:.1f}/min)")
            rc, rr, _ = rate("render_count")
            W(f"- Renders: {rc:,} ({rr*60:.1f}/min), worst "
              f"**{max(x['render_max_ms'] for x in per)} ms**.")
            W(f"- API fetch: worst **{max(x['api_ms_max'] for x in per)} ms**, "
              f"last {per[-1]['api_ms_last']} ms.")
            if "local_ms_max" in per[-1]:
                W(f"- Local receiver fetch: worst **{max(x['local_ms_max'] for x in per)} ms**, "
                  f"last {per[-1]['local_ms_last']} ms.")
                lo, lm, lf = per[-1].get("local_ok",0), per[-1].get("local_miss",0), per[-1].get("local_fail",0)
                tot = lo + lm + lf
                W(f"  - outcomes: ok {lo:,}, miss {lm:,}, fail {lf:,}"
                  + (f" ({100.0*lf/tot:.1f}% fail)" if tot else ""))
            # what share of wall-clock is spent blocked
            blk50, _, secs = rate("loop_gt50ms")
            W(f"- Blocking budget: the loop runs at ~{ips:.0f}/s, so anything over "
              f"50 ms is an outlier; {blk50:,} occurred in {secs/3600:.1f}h.")
    W("")

    W("## Findings\n")
    W("See `docs/findings.md` — that file is hand-maintained and is the durable record.\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    body = "\n".join(L)
    with open(OUT, "w") as f:
        f.write(body)
    print(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())

# Flight Display — soak report

Generated 2026-08-24 12:59 UTC · window: last 8h

## Run summary

- Window `2026-08-24T05:00:30Z` → `2026-08-24T12:59:54Z` (7:59:24), 892 health samples, 22 unreachable (2.5%).
- Uptime now 12759s, last reset `TASK_WDT`; 3 reboot(s) observed in window.
  - `2026-08-24T05:20:32Z` reason `SW`
  - `2026-08-24T08:52:04Z` reason `TASK_WDT`
  - `2026-08-24T09:27:49Z` reason `TASK_WDT`
- Heap: now 198432, min-ever 140168, largest block 110580, half-vs-half drift -15 bytes.
- Wi-Fi: RSSI min -56 / avg -47 / max -40 dBm, 0 sample(s) with the link down.
- Fetch counters (lifetime since last boot): ok 376, empty 13, fail 5.

## Crashes and watchdog

- 16 crash/watchdog line(s) in the serial window.
  - `2026-08-24T09:39:53Z E (2148735) task_wdt: CPU 0: IDLE0`
  - `2026-08-24T09:39:53Z E (2148735) task_wdt: CPU 1: IDLE1`
  - `2026-08-24T09:39:53Z E (2148735) task_wdt: Aborting.`
  - `2026-08-24T09:39:53Z E (2148735) task_wdt: Print CPU 1 backtrace`
  - `2026-08-24T09:39:53Z Backtrace: 0x4008b77b:0x3ffcd890 0x401a98c9:0x3ffcd8b0 0x4008f4c7:0x3ffcd8d0 0x4008e4cd:0x3ffcd8f0`

## Measured latency

- 1410 request(s): p50 974ms, p95 4007ms, max 16712ms.

## Data-path probe matrix


## Suite results

- fault scenarios: 15 run(s), most recent `10/10`, worst `9/10`.
- display cases: 15 run(s), most recent `15/15`, worst `15/15`.
- probe passes: 15 run(s), most recent `9/14`, worst `9/14`.
- 0 complete overnight pass(es).

## Performance

- Loop: 24,284,924 iterations over 7.6h = **886/s**. Worst single iteration seen: **22217 ms** (task watchdog fires at 25000 ms).
  - iterations >50ms: 7,128 (15.6/min)
  - iterations >500ms: 900 (2.0/min)
  - iterations >5s: 67 (0.1/min)
- Renders: 3,878 (8.5/min), worst **17 ms**.
- API fetch: worst **16711 ms**, last 1010 ms.
- Local receiver fetch: worst **1572 ms**, last 122 ms.
  - outcomes: ok 917, miss 2,087, fail 0 (0.0% fail)
- Blocking budget: the loop runs at ~886/s, so anything over 50 ms is an outlier; 7,128 occurred in 7.6h.

## Findings

See `docs/findings.md` — that file is hand-maintained and is the durable record.

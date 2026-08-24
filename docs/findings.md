# Flight Display — findings ledger

Durable record of everything the hardening campaign has established. One entry
per finding, newest section first. Status is `open`, `fixed`, or `wontfix`.
`docs/soak-report.md` is regenerated from logs; **this file is the memory**.

---

## 2026-08-24: #14 fixed — network I/O moved off loop()

| # | Severity | Finding | Status |
|---|---|---|---|
| 14 | medium | A fetch that cannot resolve DNS blocks `loop()` for ~15s; combined with connect and read, one fetch could hold a single iteration for ~31s against a 25s watchdog. | **fixed 2026-08-24** |

A dedicated FreeRTOS task (`netTaskFn`, core 0, 16 KB stack) now performs the
blocking calls. Deliberately narrow: the task does **only** the I/O and hands
back a result. Backoff, the circuit breaker, staleness, relays and all rendering
stay in `loop()`, because u8g2 and SPI are not thread safe and because keeping
the decisions in one place is what makes it reviewable.

The task is **not** subscribed to the task watchdog — a 31s fetch is legitimate
there, and the whole point was that it can no longer stall the watchdogged task.
A genuinely wedged job is caught by a stall check in `loop()`
(`NET_STALL_RESTART_MS`, 3 min).

**Measured:**

| | before | after |
|---|---|---|
| worst `loop()` iteration | **8065 ms** | **17-18 ms** |
| iterations >500 ms | 897 per 7.6h | **0** |
| iterations >5 s | 67 per 7.6h | **0** |

Even after a fault suite containing a 40s slow response and a hang mode,
`loop_max` stayed at 18 ms with zero iterations over 50 ms. The network can hang
for 40 seconds and the loop does not notice. Cost: ~18 KB of heap for the task
stack (195 KB -> 177 KB free), min-ever still 122 KB.

### Three bugs this change introduced, all caught before commit

1. **Deadlock on first fetch.** The collect step sat *after* the "is a fetch due"
   branch, but `g_nextFetchAt` only advances when a result is applied — so the
   due branch stayed true forever and returned before ever collecting. One job
   ran, then nothing. Collect now happens first, unconditionally.
2. **Watchdog feeds on the wrong task.** `esp_task_wdt_reset()` calls inside the
   fetch path now run on the unsubscribed network task, where each returns
   `ESP_ERR_NOT_FOUND` and logs an error — enough serial spam to help corrupt the
   capture. Removed; loop() not blocking is the protection now.
3. **Interleaved logging.** Three tasks (loop, network, Wi-Fi events) write to
   `Serial`, and the old macros made three separate calls per line, so lines
   shredded each other. `log.h` now formats into one buffer and emits it under a
   mutex, and the remaining raw `Serial.print` calls were routed through it.

### And a monitoring bug that wasted the most time

Serial captures were badly corrupted, which looked exactly like dropped bytes and
sent me hunting a hardware fault. The real cause was **accumulated duplicate
readers**: `pkill -f monitor.sh` reaps the wrapper but orphans the `cat` holding
the port, and repeated restarts left several readers splitting the same byte
stream. `tools/monitor.sh` now traps EXIT/INT/TERM to kill its process group and
refuses to start on top of a stale reader.

Two smaller fixes found along the way: the timestamping pipeline forked `date(1)`
per line and block-buffered through `tr` (now one line-buffered perl pass), and
the sanitising regex replaced the **newline itself**, collapsing the log into one
run-on line.

Verified: flows 13/13, faults 10/10, display harness 24/24, wifitest 9/9.

---


## Decision 2026-08-24: nearest means nearest, ground aircraft included

A 20.8-minute watch showed the display sitting static for 30s+ stretches (worst
124s) whenever the nearest aircraft was on the ground — 30 consecutive `nopos`
results, because surface traffic frequently transmits without a usable position.
Half the window was spent on ground aircraft, since Boeing Field and SEA are both
inside the 10 km circle.

**Filip's call: leave it. If the nearest aircraft is on the ground, show it.**

So this is NOT a defect and must not be "optimised" later:

- Do not skip `alt = GND` aircraft in `selectAircraft()`.
- Do not add a distance penalty for surface traffic.
- A frozen distance on a parked aircraft is *correct* — it is not moving.

Measured behaviour to expect, for reference when something looks wrong:

| | median gap between display updates | worst |
|---|---|---|
| airborne | 5s | — |
| ground | 31s | 186s |

Local-refresh hit rate over the full window was **71%** (213 ok, 40 `nopos`,
40 `stale`, 6 not-found, 10 skipped). An earlier "100%" figure was a favourable
5-minute sample of airborne traffic and should not be treated as the baseline.

---


## 2026-08-24: "distance and altitude don't change often enough" — validated

Filip reported intermittent freezes. The update path itself is **correct**:
compared against the receiver's live truth over 18 samples, the device tracked
distance with **0.00 km error** and updated twice per 8s, exactly the 4s interval.
Nothing wrong with extraction, the network, the render guard or the arithmetic.

The freezes came entirely from attempts that produced no update, and those had
been collapsed into one `local_miss` counter that made a working path look
broken. Split into four:

| bucket | meaning |
|---|---|
| `local_miss` | record genuinely not in the feed |
| `local_nopos` | aircraft heard, but with **no position at all** |
| `local_stale` | position older than `LOCAL_RX_MAX_AGE_S` |
| `local_skip` | not attempted (distance gate or give-up counter) |

Measured: `ok 51, notfound 0, nopos 0, stale 8` — **staleness was the only
cause**, at 14%. Extraction was flawless, confirmed independently by the host
test at 56/56 records against a real 23 KB payload.

### Fix

`LOCAL_RX_MAX_AGE_S` 15s -> 30s. The threshold was being judged in the abstract
rather than against what it competes with: the alternative to a 16s-old local
position is an API value up to `FETCH_INTERVAL_MS` (30s) old, so discarding the
fresher number to keep the staler one is backwards.

Result over 5 minutes: **74 ok, 0 notfound, 0 nopos, 0 stale — 100% of attempts
produce a live update**, against 86% before.

### Caveats worth keeping

- `local_nopos` cannot be fixed by any threshold: dump1090 lists aircraft it
  hears without ever resolving a position. One sample showed 21 of 69 attempts in
  that bucket, so the miss profile swings a lot with which aircraft is on screen.
- These counters are meaningless during test runs — mock hexes are never in the
  receiver. Only read them against the live API.
- An earlier hypothesis that extraction was truncating on large bodies was
  **wrong**: the byte-scanned diagnostic added to chase it never logged a single
  line, which is what proved the misses were rejections rather than short reads.

---


## 2026-08-24 15:05 UTC: "No aircraft nearby" investigated — the display was wrong

Filip saw the splash briefly. Health data pins it to **15:05:30 -> 15:06:34**
(~64s, 2 fetch cycles) with `fetch_empty` going 3 -> 4, so it was a genuine
empty result rather than a network failure. But "empty" there meant the **100 km**
search found nothing acceptable, and a 100 km circle around Seattle at 8am is
never empty. The logs hold **278 cycles** where both searches came back empty.

### Two bugs found

| # | Severity | Finding | Status |
|---|---|---|---|
| 20 | **high** | The widened search used `/v2/closest`, which returns **exactly one** aircraft. If that single aircraft failed the position-freshness gate it was rejected and the entire 100 km ring was declared empty, with no alternative to fall back to — showing "No aircraft nearby" over a busy sky. Both searches now use `/v2/point`, which also restores emergency scanning in the widened ring. | **fixed 2026-08-24** |
| 21 | medium | **Self-inflicted, same morning.** Sticky wide mode compared against `SEARCH_RADIUS_KM` (10 km), but the API takes integer nautical miles so 10 km is sent as 5 nm = **9.26 km**. An aircraft in the 9.26-10 km band was invisible to the primary search yet passed the exit test, so wide mode toggled every cycle and paid for two searches — exactly what the optimization was meant to stop. Now compared against `primaryQueryRadiusKm()` with 10% hysteresis. Confirmed live against an aircraft at 9.95 km. | **fixed 2026-08-24** |

### Instrumentation gap closed

`No valid aircraft found in response` was logged identically whether the API sent
zero aircraft or sent some that we rejected, making the two indistinguishable
afterwards. It now reads `No displayable aircraft: N returned, M rejected on
position/staleness`, and `/healthz` exposes `last_ac_returned` / `last_ac_rejected`.
Already useful: live samples read `0 returned, 0 rejected`, confirming that a
quiet primary circle really is empty rather than being discarded by our gate.

### #17 miss rate corrected

A clean 2-minute measurement on live data gives **29 ok / 1 miss / 0 skip** — a
97% hit rate. The overnight "70% miss" was dominated by the extractor bug, not by
coverage. Test-suite runs contaminate these counters badly (mock hexes are never
in the receiver), so only measure them against the live API.

### Not a fault: power loss

The device vanished from both USB and Wi-Fi mid-regression. The CP2102 and the
entire monitor-hub tree disappeared from the USB bus together, so the dock lost
power — the board came back with `reset_reason POWERON`. Worth remembering that
a total disappearance of both transports at once points at power, not firmware.

Verified after: flows 13/13, faults 10/10, display harness 24/24.

---


## 2026-08-24 morning: the four optimizations, applied

### #16 watchdog exposure — fixed

`resolveApiHost()` resolves the API host with `WiFi.hostByName()` *before* the
HTTP call and feeds the watchdog in between. DNS previously ran inside
`http.begin()` on top of connect and read, so one fetch could hold a single
`loop()` iteration for ~31s against a 25s watchdog — the cause of both
production resets. The lookup and the fetch are now two spans of at most ~16s,
the lwIP cache makes `http.begin()`'s own lookup instant, and a DNS failure now
exits fast instead of proceeding. `esp_task_wdt_reset()` also runs between the
primary and widened fetches so they can never share one un-fed span.

### #2 double fetch per cycle — fixed, with a caught regression

Sticky wide mode: once the widened search is what is working, it is used alone
(one call, not two) and drops back automatically as soon as it finds an aircraft
inside the primary circle. 522 widenings in 7.6h becomes roughly one call per
cycle.

**A regression this introduced, caught by the test suite:** the widened search
originally used `/v2/closest`, which returns a single aircraft — silently
switching OFF emergency-squawk scanning for however many hours the sky stayed
quiet, which is exactly when nobody would notice. `emergency_preempts` failed
and exposed it. Wide mode now uses `/v2/point` too. Affordable because wide mode
only engages when the sky *is* quiet: measured **9.7 KB / 20 aircraft** in the
wide ring at that hour, against 44 KB / 95 at midday when the primary circle is
busy and wide mode never runs.

`no_widen_when_found` also had to be corrected: it asserted "not the fallback
radius", which sticky wide mode legitimately violates. The real invariant is
**exactly one search per cycle**, and that is what it now checks.

### #18 local parse cost — fixed, after shipping two bugs

Replaced the whole-body JSON parse with a targeted record extractor. Measured on
device: **~44 ms typical, 26-137 ms range**, against p50 120 / p95 310 / max
1572 ms before.

Two bugs shipped on the way, both of which looked identical to "the aircraft is
not being heard", and neither of which a device test could distinguish:

1. The needle was a fixed `"hex":"..."`, but this receiver emits
   `"hex": "a24ba7"` **with a space** (Python `json.dumps`). It never matched
   once — 0 hits across every attempt.
2. The capture buffer was 384 bytes while real records run to **523 bytes**;
   11 of 16 records did not fit, and overrunning returned "not found".

Both were found only by lifting the parser into `local_rx.h` and testing it on
the host against a real captured payload (`tools/hosttest/test_local_rx.cpp`,
16/16 records at chunk sizes 64/128/512/4096). `extractLocalRecord()` now
reports truncation separately from a miss, so an undersized buffer can never
again masquerade as an absent aircraft.

### #17 local miss rate — partly a measurement artifact

The 70% miss rate was measured with the extractor described above, so it
conflated genuine non-coverage with bug (1). With the extractor fixed the device
now hits **33/33 with 0 misses**. The distance gate was therefore widened from
15 km to a 50 km backstop, leaving the 3-strike per-hex miss counter to do the
fine-grained work. The true coverage figure needs re-measuring overnight.

### Data source note

The `:8080/aircraft` endpoint is Filip's own LCD helper service, not stock
PiAware. Per <https://filbot.com/piaware-data-display/> the authoritative file is
`/run/dump1090-fa/aircraft.json`; the service re-serves it via Python, which is
where the `"hex": "..."` spacing comes from. It appears faithful — full dump1090
field set, and 100% hit rate once the extractor was fixed — so the earlier zero
hits were entirely our bug, not the service. Port 80 is closed on the Pi, so the
stock SkyAware path is not reachable; switching to it later is a `LOCAL_RX_PORT`
and `LOCAL_RX_PATH` change only.

Verified: flows 13/13, faults 10/10, display harness 24/24, host type-table
tests green, host local-rx tests 16/16.

---


## 2026-08-24 05:20-12:56 UTC: first clean observational baseline (7.6h)

No synthetic fault injection, no mock, no test override — verified: `overnight.log`
has no entries in the window, zero mock requests, and all 845 samples used
`api.adsb.lol`. So everything below is real-world behaviour.

**The device rebooted twice on its own.** That is the headline, and it is new.

### New findings

| # | Severity | Finding | Status |
|---|---|---|---|
| 16 | **high** | **The task watchdog fired twice in production** (08:51:34, 09:39:53), both times with `loopTask (CPU 1)` blocked inside an `HTTP GET` and both CPUs idle — waiting on network I/O, not spinning. Worst *surviving* iteration was **22,217 ms** against the 25,000 ms watchdog. My earlier margin estimate in finding #11 was **wrong**: it assumed ~16s worst case, but DNS (up to 15s, finding #14) happens *inside* `http.begin()` on top of connect (8s) and read (8s), and the tiered fallback can issue **two** fetches in a single `loop()` iteration. Worst case is therefore ~31s for one fetch and roughly double that for a widened search — comfortably past the watchdog. | **fixed 2026-08-24** |
| 17 | medium | **The local-receiver refresh misses 70% of the time** (893 ok vs 2,087 miss, 0 fail). Cause: overnight the nearby sky is empty, so 61% of displayed aircraft came from the 100 km fallback (median distance 14.1 km, max 23.9 km) and are not heard locally with a fresh position. Each miss still costs a full fetch and 16 KB parse. | **fixed 2026-08-24** (partly a measurement artifact) |
| 18 | medium | **The local fetch costs p50 120 ms / p95 310 ms / max 1572 ms** on device against 27 ms measured with curl. The gap is the 16 KB JSON parse on a 240 MHz MCU, not the network. At a 4 s interval that is ~3% of wall clock spent parsing to extract one aircraft's position. | **fixed 2026-08-24** |
| 19 | low | `nightreport.py`'s "Suite results" section ignored `--since` and read the whole `overnight.log`, so a night with **no** test runs reported 15 passes from previous days — making a clean baseline look contaminated. | **fixed 2026-08-24** |

### Reliability, measured

- **Heap is excellent**: -17 bytes half-vs-half drift over 7.6h, min-ever 140,168
  (9x the 15,000 critical threshold), largest block flat at 110,580. No leak, no
  fragmentation trend.
- **Wi-Fi is excellent**: zero `wifi_up=false` samples, RSSI -56/-47/-40 dBm.
- **Fetch outcomes** since the last boot: 373 ok, 13 empty, 5 fail.
- 22 of 869 health samples unreachable (2.5%), scattered rather than clustered,
  coinciding with the long blocking windows rather than forming outages.
- **API latency degraded overnight**: p50 966 ms but p95 **2,885 ms** and max
  **16,602 ms**, against a p95 of 1,230 ms measured the previous afternoon. Not
  ours to fix, but it is what pushed loop iterations past the watchdog — the
  timeouts were sized against a faster network than we actually got.

### Performance, measured

- Loop runs at **885 iterations/s** (24.1 M over 7.6h). Renders are cheap and
  not a problem: 3,850 (8.5/min), worst **17 ms**.
- Blocking: 7,101 iterations >50 ms, 897 >500 ms, 67 >5 s. Lower bound on time
  spent blocked is **1,060 s = 3.9% of wall clock**, essentially all of it in
  HTTP fetches.
- The tiered fallback widened **522 times** in the window — for 61% of the night
  the device made **two** API calls per cycle instead of one, doubling both the
  API load and the per-iteration blocking window. This is the direct mechanism
  behind the 08:51 watchdog trip, whose log line immediately before the abort was
  `Nothing within 10 km, widening to 100 km`.

### What the data does NOT support

- No heap leak, no fragmentation growth, no Wi-Fi instability, no display or
  render cost problem, and no evidence of any fault in the alert, classification
  or parse paths. The only reliability defect found is #16.

---


## 2026-08-24: local ADS-B receiver feeds the live numbers

Filip runs PiAware on the roof. It serves dump1090 `aircraft.json` at
`http://192.168.1.243:8080/aircraft` — a `FlightAwareLCDHTTP` service, not the
usual SkyAware path (`/skyaware/data/aircraft.json` and `:8080/data/...` both
404, which is why it took a port scan to find). 45 aircraft, 16 KB, **27 ms**.

Two proposed indicators were measured and rejected before writing any firmware:

- **"Is my receiver hearing this aircraft?"** — overlap is **100% (15/15)**
  across five samples. A rooftop antenna hears everything adsb.lol reports
  within 10 km, so the indicator would be lit permanently.
- **Reception freshness** — local `seen` is 0.0-0.2s (median 0.1) against
  adsb.lol's 0.0-0.457s (median 0.275). Local is fresher in 11 of 14 samples,
  by about **0.2 seconds**. Invisible on a display that updates every 30s.

The same sampling exposed the thing actually worth fixing: **the display was up
to 30 seconds stale.** Over 137 seconds the nearest aircraft changed twice and
one aircraft's distance swung 2.90 -> 4.24 nm, all invisible between API polls.

So the receiver now refreshes distance and altitude every 4s
(`FEATURE_LOCAL_RX`). adsb.lol still owns identity, type, classification and
which aircraft to show — the local feed carries no `t`, `desc`, `r` or
`dbFlags`, so it cannot replace the API, only sharpen it.

Measured on device: one aircraft tracked 6.08 -> 3.62 mi over 85s with altitude
stepping 2025 -> 1875 ft, updating on nearly every 5s sample. 46 local fetches,
**0 misses, 0 failures**, `seen_pos` 0.1-1.8s. Heap unchanged.

Design notes worth keeping:

- **IP, never a hostname** — a failed DNS lookup costs a fixed 15s of blocked
  `loop()` (finding #14). There is no reason to risk that for a LAN device.
- Plain HTTP, short timeouts (2s connect / 3s read), and a 60s backoff after any
  failure so a switched-off receiver cannot stall `loop()` every 4s.
- A local failure is **never** counted as an API fetch failure — the same
  distinction the Wi-Fi fix drew between "we did not try" and "we tried and
  failed".
- `PatientStream` was widened from `WiFiClientSecure&` to `Client&` so the same
  gap-tolerant reader serves both the TLS API fetch and this plain-HTTP one.
- Re-render is gated by `sameFlightDisplay()`, which already ignores sub-0.1 km
  jitter, so a redraw only happens when a cell would actually read differently.

Verified: flows 13/13, faults 10/10, display harness 24/24. Mock hexes were
checked against the live receiver for collisions (none), so the suites cannot be
perturbed by the local refresh.

---


## 2026-08-23 late: cleared the low-severity backlog

Seven findings closed. Each was measured as unreachable through `/v2/closest`,
but "unreachable today" is a property of the endpoint, not of the code — and
`/v2/point` had already changed which endpoint we use, so leaving them was no
longer clearly the safer choice.

- **#7 altitude sentinel** — reordered so the surface rule is applied to any
  value that is *present*, leaving `-1` to mean only "absent". A real -1 ft
  reading now reads as `GND` instead of an em-dash, and no sentinel had to move.
- **#5 alt_baro strings** — only the exact string `"ground"` means ground. Any
  other string is unknown rather than silently rendered as `GND`.
- **#6 implausible altitudes** — decoded through `long long` and range-checked
  (-2000 to 200000 ft). A value beyond int32 is now rejected as unknown instead
  of wrapping to something `<= 0` and displaying as `GND`.
- **#8 `seen_pos: null`** — an explicit null (or a non-numeric value) is an
  unknown age, so the position is rejected. `containsKey()` alone passed a null
  whose `as<double>()` is 0, which read as "brand new".
- **#9 B categories** — glider, balloon/airship, parachutist, ultralight, UAV
  and spacecraft all classify PVT rather than falling through to the callsign
  rule and landing on COM.
- **#4 `TISB_OTHER`** — removed. At 10 characters it could never match a
  `char[8]` `typeCode`, and the host test now enforces the 7-character limit.
- **#10 library pinning** — `sketch.yaml` records the platform and library
  versions actually tested. The ArduinoJson major matters: under 7.x
  `StaticJsonDocument<N>` ignores `N` and uses the heap; under 6.x it is a real
  fixed pool. Build with `arduino-cli compile --profile esp32`.

Eleven probe cases were promoted from exploratory to asserted, so each of these
behaviours is now pinned rather than merely observed.

Verified: probe 11/11 on the changed cases, flows 13/13, faults 10/10, display
harness 24/24, wifitest 9/9, host type-table test all green (492 entries).

### Deliberately not fixed

**#14 (DNS stalls `loop()` for 15s)** and **#11 (connect and read are separate
8s budgets)** stay open on purpose. There is no cheap safe fix: the ESP32 core
exposes no bounded `hostByName()`, the lwIP DNS timeout lives in `sdkconfig`
rather than the Arduino API, and connecting by cached IP would break TLS SNI to
adsb.lol's ingress. The honest options are to accept it — 15s sits inside the
25s watchdog, and during an internet outage backoff caps at 60s so the stall is
roughly a 25% duty cycle on a device that has nothing useful to do anyway — or
to move fetching onto its own FreeRTOS task so `loop()` never blocks. The latter
is a real architectural change and should be a deliberate decision, not a
drive-by fix.

---


## 2026-08-23 22:00 UTC: Wi-Fi reconnect path — finally tested

Two AP experiments failed to disassociate the device (the router was cycled, not
the access point), so the path was tested directly instead with a new
`POST /test/wifi-drop` hook: it disables auto-reconnect, drops the association,
and holds it down for a self-expiring interval. `tools/wifitest.py` triggers it,
waits out the outage it cannot poll through, then reconstructs events from the
serial log and the uptime counter. **9/9 checks passed.**

Confirmed working:

- Association really dropped (`WiFi disconnected. Reason: 8`), stayed down for
  the full hold, and `connectWiFi()` — not the SDK — brought it back.
- **Reconnect took 2 seconds**: hold expired 22:01:47, `WiFi.begin()` the same
  second, `Got IP` at 22:01:49, first successful fetch at 22:01:53.
- No reboot (uptime 31s -> 147s continuous), no crash, no watchdog trip.
- The circuit breaker correctly stayed silent. `g_fetchFailCount` is only
  incremented when `WiFi.status() == WL_CONNECTED`, so a Wi-Fi outage can never
  trip a breaker meant for "API unreachable while the link is up".
- Fetches while the link is down **fail instantly** — the `WiFi.status()` guard
  at the top of `fetchClosestAt()` returns before any DNS work, so finding #14's
  15-second DNS stall does *not* apply to Wi-Fi outages. Only to internet ones.

### New finding

| # | Severity | Finding | Status |
|---|---|---|---|
| 15 | low | **No backoff during a Wi-Fi outage.** Because `g_fetchFailCount` is deliberately frozen while the link is down, `backoffMs()` is always called with 1 and the retry interval stays pinned at ~4.3s — observed 16 times in a 90s hold, every one logging `Fetch failed (attempt 0)`. Each attempt is instant so there is no real cost, but it produces ~840 log lines an hour and, once the display has cleared, ~840 full-framebuffer splash redraws an hour. Contrast with an API outage, where backoff correctly grows to the 60s cap. | **fixed 2026-08-23** |

**#15 fix:** a down link is no longer routed through the failure path at all.
`loop()` now defers the cycle at the normal `FETCH_INTERVAL_MS` and calls the
extracted `expireStaleDisplay()`, so the display still ages out to the dimmed
splash exactly as before. Measured over a 120s outage: `Fetch failed (attempt 0)`
went **16 -> 0**, and the down/up transitions each log once instead of every
cycle.

**A regression the fix introduced, and its fix.** Deferring to the 30s interval
meant that after the link returned the device waited out the remainder before
fetching — measured at **26 seconds** of stale display post-recovery, where the
old 4.3s retry had recovered almost immediately. Setting `g_nextFetchAt =
millis()` in the `GOT_IP` handler brings that to **1 second**. Worth recording
that removing wasteful retries quietly removed the fast recovery they were
providing.

### Still not covered

The 60-second `WIFI_RETRY_INTERVAL_MS` cadence in `connectWiFi()`. The hook
forces an immediate retry when the hold expires, and with a live AP the first
`WiFi.begin()` always succeeds, so the periodic-retry branch never runs. Testing
it needs an AP that is genuinely absent for several minutes.

---


## 2026-08-23 21:21 UTC: AP restart — what it did and did not test

**It did not exercise the Wi-Fi reconnect path.** The serial log (captured over
USB, so unaffected by the network) shows **no `WiFi disconnected` event** during
the outage: the device kept its association throughout. `connectWiFi()`,
`WIFI_RETRY_INTERVAL_MS` and the disconnect handler never ran. What actually
happened was a ~2 minute loss of internet/DNS with the Wi-Fi link up.

That is still a realistic and common failure, and the firmware handled it well:

- 4 consecutive failed fetches, backoff 4.3s -> 8s -> 16.3s -> 36s, then a clean
  200 at 21:23:08. Total outage 21:21:04 -> 21:23:10, about 2m06s.
- **No reboot**: uptime ran continuously 1944s -> 2074s across the event.
- Circuit breaker correctly did not fire (4 failures against a threshold of 60).
- Display correctly kept the last aircraft rather than clearing: the outage was
  ~2 min against `STALE_DISPLAY_MAX_MS` of 5 min.
- `fail_streak` returned to 0 and heap was unaffected.

### New finding

| # | Severity | Finding | Status |
|---|---|---|---|
| 14 | medium | A fetch that cannot resolve DNS blocks `loop()` for **exactly 15.003s**, four times out of four — nearly double `HTTP_CONNECT_TIMEOUT_MS` (8s), because lwIP's DNS resolution happens inside `http.begin()` and is not covered by that timeout. Successful fetches take 0.9-1.3s. This is the *common* real-world failure (internet down, Wi-Fi up), not a corner case, and it leaves only ~10s of margin under the 25s task watchdog. During each 15s window OTA and the HTTP server are unresponsive. | open |

Mitigations worth considering for #14: cache the resolved address and reuse it
when resolution fails, resolve explicitly via `WiFi.hostByName()` with a shorter
bounded timeout, or accept it and keep `LOOP_WDT_TIMEOUT_S` comfortably above 15s
(it is, at 25s — but the margin is smaller than the 8s config implies).

### Still owed

A real Wi-Fi disassociation test. It needs the AP **off for 3+ minutes**, or the
device carried out of range, so the link genuinely drops and the reconnect loop
runs. A quick restart is not enough — the association survived this one.

---


## 2026-08-23: alert feature (squawk codes + emergency status)

Extended beyond the three transponder codes to the ADS-B emergency/priority
status field, because **`lifeguard`, `minfuel` and `downed` have no squawk
equivalent** — they are transmitted only in that field, so the squawk-only
version could never have seen them. Verified directly: the `lifeguard` flow test
shows the banner while the aircraft squawks an ordinary 1200.

One priority ladder (`kAlerts[]`) serves both sources: unlawful 6, downed 5,
general 4, minfuel 3, nordo 2, lifeguard 1. Severity beats proximity — a 7700 at
8 km outranks a lifeguard at 3 km — and distance only separates equals. Where
the two sources disagree the more severe wins (squawk 7600 plus status
`unlawful` renders `HIJACK 7500`). `ALERT_PREEMPT_MIN_PRIORITY` raises the
preempt bar if medical flights turn out to be too common to be interesting.

While an alert is showing the whole panel is inverted (`ALERT_INVERT_DISPLAY`),
XOR-filled so nothing shifts position — the bottom row keeps its three cells and
its alignment with the bezel labels, and only the polarity changes. Reviewed live
across all seven states before being kept.

Trade-off accepted deliberately: an inverted panel lights most of its pixels,
which runs against the 60%-brightness change made for panel life. Alerts are rare
and brief so the aggregate cost is negligible, but a long-loitering emergency
would hold the panel lit. If that ever matters, blinking between normal and
inverted would be both more noticeable and half the lit time.

Verified: flows 13/13, faults 10/10, display harness 24/24.

## 2026-08-23: emergency squawk feature

The primary search moved from `/v2/closest` to `/v2/point`, so the firmware now
sees every aircraft in the circle instead of only the nearest. `selectAircraft()`
picks the nearest aircraft as before, except that an aircraft squawking 7500,
7600 or 7700 anywhere in the circle preempts it — that is the entire reason for
fetching the whole list.

The widened fallback deliberately stays on `/v2/closest`: it only fires when the
nearby sky is empty, and then the only question is "nearest thing anywhere",
which 591 B answers as well as 44 KB would.

Display: the banner replaces **only** the type-name area. The bottom row is
untouched by design — its three cells are physically labelled on the display
bezel, so their meaning must never shift. Verified by feeding identical data with
and without a 7700 and confirming the bottom-row inputs are byte-identical.

Verified: `emergency_preempts` (7700 on the 8 km C172 beats a normal 3 km B738),
`normal_squawk_ignored` (ordinary code changes nothing), `uses_point_endpoint`
(point for primary, closest for fallback), plus five display cases covering all
three codes, an ordinary code, and an over-long callsign on the banner.

**Test-side note:** switching endpoints broke three older flow tests that grepped
for `/v2/closest/` to find the primary request — and one of them,
`no_widen_when_found`, had been passing *vacuously* on an empty list rather than
asserting anything. Both are fixed. Worth remembering that a test which stops
matching reality can fail loudly or silently, and the silent one is worse.

---


## 2026-08-23 evening: the response-size limit, investigated

Finding #12 was real, not a mock artifact — it reproduced against a corrected,
faithful mock. Instrumenting the stream at the moment of parsing settled it:

    len=3621  avail=3621  -> parses
    len=7221  avail=0     -> EmptyInput ... yet avail=7221 just 16ms later

Above roughly 3.6 KB the response spans more than one TLS record, so when the
headers finish parsing the body has not landed yet. `available()` is 0,
ArduinoJson's reader gives up, and the result is `EmptyInput`. **The same failure
family as the old /v2/mil scanner:** "nothing buffered yet" is not "no data".

### Fixed

- `fetchClosestAt()` now waits (bounded by `HTTP_READ_TIMEOUT_MS`) for the first
  body byte before handing the stream to ArduinoJson. The ceiling moved from
  **~3.6 KB to ~14 KB**, a 4x improvement. Verified: 3621 B, 7221 B and 14421 B
  all parse where the last two previously failed.
- Removed a dead `client.setTimeout(clientTimeoutSec)` that passed **seconds**
  into `Stream::setTimeout`, which takes milliseconds. It looked like a live bug
  and was merely inert: `HTTPClient::connect()` overwrites the stream timeout
  from `_tcpTimeout` immediately afterwards. Measured at runtime as 8000 ms.

### Still open

| # | Severity | Finding | Status |
|---|---|---|---|
| 13 | medium (blocks `/v2/point`) | Above ~14 KB the parse failed as **`IncompleteInput`** — a *mid-stream* gap rather than a start-of-stream one, which the first-byte wait could not address. | **fixed 2026-08-23** |

**#13 fix:** `PatientStream`, a `Stream` wrapper whose `readBytes()` waits for the
next burst instead of reporting end-of-input, bounded by one overall deadline so
`loop()` stays inside the watchdog. Only `readBytes()` needed overriding —
ArduinoJson reads exclusively through it — and reading the client directly is
safe because these responses always carry `Content-Length` (never chunked).

Measured ceiling, before and after the two fixes together:

| Payload | Originally | After first-byte wait | After `PatientStream` |
|---|---|---|---|
| 3.6 KB | OK | OK | OK |
| 7.2 KB | fail | OK | OK |
| 14.4 KB | fail | OK | OK |
| 28.8 KB | fail | fail | **OK** |
| 57.6 KB | fail | fail | **OK** |
| 112 KB | fail | fail | fails as `NoMemory` |

The ceiling moved from ~3.6 KB to beyond 57 KB — 16x — and the remaining limit
is now an honest resource constraint rather than a spurious truncation. Heap
returns to ~195.6 KB after every parse, so nothing leaks.

**What this unlocks, measured live at HOME:** `/v2/point` returns 2.6 KB at the
10 km primary radius (5 aircraft) and 44.2 KB at the 100 km fallback (95
aircraft). Both now parse. The primary circle is the safe one: 2.6 KB is trivial,
whereas a 44 KB parse transiently consumes most of the heap. So the recommended
shape is **`/v2/point` for the primary circle, `/v2/closest` for the rare
fallback** — the fallback only fires when the sky nearby is empty, and then the
only question is "nearest thing anywhere", which `/v2/closest` answers in 591 B.

Caveat for peak traffic: 2.6 KB was 5 aircraft on a quiet evening 10 km from
SEA. An arrival push could put 20-30 aircraft in the same circle (10-16 KB),
which is now comfortably inside the ceiling but was not before this fix.

**Upstream caveat noticed while reading the core:** `NetworkClient::readBytes()`
computes its deadline as `int to = millis() + getTimeout()`. `millis()` exceeds
`INT_MAX` after ~24.8 days, so the deadline arithmetic overflows on a long-lived
device. Not ours to fix, but it sits directly on this device's 24/7 path and is
worth knowing about alongside the 49.7-day `millis()` rollover.

---

## API surface we are not using (2026-08-23 survey)

Sampled 131 live aircraft via `/v2/point`. We consume 12 fields; the response
carries ~40. Percentages are how often the field was actually present, which is
the thing that decides whether a feature is worth building.

**Worth taking, in order:**

| Field | Present | Why it matters |
|---|---|---|
| `dst` | 100% | The API already returns distance **in nautical miles from the query point** — exactly the number we render. Adopting it deletes `haversineKm()` and `deg2rad()` outright. The cheapest win here: less code, not more. |
| `squawk` + `emergency` | 96% / 95% | Squawk 7500 (hijack), 7600 (radio failure), 7700 (general emergency), and an explicit `emergency` string. A genuinely striking thing for a display to surface, and it costs one comparison. |
| `dir` | 100% | Bearing from the query point. On a display next to a window, "which way do I look" is arguably better than distance. |
| `gs` | 100% | Ground speed in knots — a natural third metric for the bottom bar. |
| `baro_rate` / `geom_rate` | 62% / 35% | Vertical rate: a climb/descent arrow next to the altitude. Note the coverage gap; needs a fallback to blank. |
| `seen` | 100% | Age of the *whole record*, where `seen_pos` is only the position's age. A cheap extra staleness guard. |
| `mlat` / `tisb` | 100% (arrays) | Non-empty means the position is multilaterated or TIS-B rebroadcast rather than direct ADS-B — i.e. less trustworthy. Relevant given how much we care about position trust. |

**Endpoints we do not call** (all verified live): `/v2/hex/{hex}`,
`/v2/reg/{reg}`, `/v2/callsign/{cs}`, `/v2/type/{t}`, `/v2/sqk/{sqk}`,
`/v2/point/{lat}/{lon}/{r}` (all aircraft in a radius, not just the nearest),
`/v2/ladd`, `/v2/pia`. `/v2/point` is the interesting one: it would allow "3
aircraft overhead" or picking the most *interesting* aircraft rather than merely
the closest.

**Not worth it:** `nic`, `rc`, `sil`, `sda`, `gva`, `nac_p`, `nac_v`,
`version`, `nic_baro` are ADS-B integrity/accuracy metadata with no display
value. `ias`/`tas`/`mach`/`roll`/`wd`/`ws` are present on under 1% of aircraft.

**Operational note:** the real API rate-limits at roughly **1 request/second** —
a rapid endpoint sweep earns `429`s. The device's 30s interval is far inside
that, but test scripts must pace themselves. This is a further argument for
testing against `tools/mockapi.py` rather than the live API.

---

## 2026-08-23 morning: three fixes applied

Prioritised by measurement rather than by the severity labels, which the data
corrected in both directions.

### Fixed

- **#1 padding-only callsign** — ADS-B pads `flight` to 8 chars, so an aircraft
  with no callsign arrived as `"        "`. `*identSrc` only rejects the empty
  string, so padding won the ident chain: blank title, and `hasCallsign` went
  true so a light aircraft classified COM and fired the wrong relay. Now tested
  with `hasNonSpace()`, and leading padding is trimmed as well as trailing.
  Measured rate first: **1 in 2002 live aircraft** (0.05%), so this was rarer
  than its "high" label implied — fixed anyway because it blanks the display.
  Regression cases `ident_ws_only`, `ident_ws_pvt`, `ident_lead_ws`.
- **#2 stale `last_err`** — the field kept the last message that happened to
  fail, so `/healthz` reported `Too Many Requests` beside a `200` indefinitely.
  It had already misled this campaign's own reporting. Now cleared on every
  attempt and populated for transport-level errors too. The fault suite's `ok`
  scenario asserts it clears on recovery.
- **#3 splash never pixel-shifted** — `pixelShiftIdx()` only re-rendered through
  `renderFlight()`, so the splash was 100% static. Steady-state exposure is only
  0.2% (the sky is rarely empty at 10 km), but during a sustained outage it is
  100%, which is exactly the case the dimming was added for. `showSplash()` now
  stores its text and `drawSplash()` applies the same offsets; `loop()` redraws
  it on shift changes. Verified under a real outage: two distinct shift
  positions with the redraw counter incrementing, dimmed throughout.

### New finding

| # | Severity | Finding | Status |
|---|---|---|---|
| 12 | low (unreachable, **partly fixed** — see the evening entry) | Responses above roughly **3.6 KB fail to parse**, reported as `EmptyInput`. Bisected: 20 aircraft / 3621 B parses, 40 aircraft / 7221 B fails, with the mock verified to serve the body byte-identically. Not reachable in production — `/v2/closest` returns exactly one aircraft (~200 B), confirmed against the live API. Mechanism not established; it is **not** pool exhaustion, because under ArduinoJson 7 `StaticJsonDocument<2048>` is plain `JsonDocument` with a cosmetic `capacity()` and the 2048 is inert. | open |

Finding #10 is confirmed in more detail: the `<2048>` template argument has no
effect at all under the linked 7.4.3, so both the size and the "stack-allocated,
no heap allocation" comments in `fetchClosestAt` are inaccurate.

### Test artifacts found and fixed (not firmware defects)

- `tools/probe.py` staged payloads with `open(BODY, "w")`, which truncates in
  place; a fetch landing mid-write read an empty file and the firmware correctly
  reported `EmptyInput`. Now an atomic `os.replace()`. This one masqueraded as a
  firmware parse bug.
- The fetch-counter barrier could be satisfied by an **in-flight fetch of the
  previous payload**, so a case sampled the prior result. Replaced with a
  sentinel barrier: stage a `SYNCPING` payload, wait until the device reports it,
  then stage the real payload and wait until it moves off the sentinel.
- **Concurrent test drivers.** `tools/overnight.sh` was still looping while
  manual suites were run interactively. `probe.py`, `faults.py` and `harness.py`
  all stage into the *same* `logs/mock.body` and all switch the device's
  `api_base`, so two drivers at once read each other's payloads. This — not the
  fetch barrier — is what produced 19 bogus "failures" in 51 cases on a build
  that passes every case in isolation, and it also explains the single `garbage`
  fault failure (overnight's pass-14 fault suite straddled two OTA flashes).
  Fixed with `tools/with-device-lock.sh`, a mkdir-based mutex that every driver
  now goes through. *An earlier version of this entry blamed the one-vs-two
  fetch-completion barrier; that diagnosis was wrong.* The barrier was made
  two-completion anyway, which is sound, but it was not the cause.
- Both races together produced the 1% overnight flake rate. Worth recording that
  **stale reads hid a real failure**: `bad_many_ac` appeared to pass all night
  only because it was reading the previous case's success.
- Lesson for this harness: every batch-mode probe failure so far has been the
  harness, not the firmware, and each was only distinguishable by re-running the
  case in isolation. Treat a batch failure as unproven until isolated.

---

## Overnight result, 2026-08-23 05:00–14:00 UTC (9h, 13 full passes)

Stability was clean. Everything below either confirms the 23:30 findings or is new.

- **No firmware faults found overnight.** 13 complete passes: fault suite 10/10
  every time, display harness 15/15 every time, 724 probe case-runs. Zero
  crashes, zero watchdog trips, zero circuit-breaker restarts, zero
  heap-critical restarts. The only two reboots were my own OTA flashes.
- **No heap leak.** Half-vs-half drift +65 bytes over 538 samples; largest block
  constant at 110580. Min-ever dipped to 119852 under fault load, still 8x the
  15000 critical threshold.
- **Wi-Fi rock solid**: RSSI −47/−43/−41 dBm, zero samples with the link down,
  1 unreachable health sample in 538 (0.2%) and it coincides with an OTA flash.
- **NEW, finding #11 below**: real-world fetch latency has a much longer tail
  than the p95 suggested — 15 of 2796 requests exceeded 9s, with a real
  api.adsb.lol request reaching **14.46s and still returning 200**.
- The 7 probe failures (1% of runs) were a **defect in my test harness, not the
  firmware** — see "Test artifacts" below. Re-verified deterministic 6/6.

| # | Severity | Finding | Status |
|---|---|---|---|
| 11 | medium | Connect and read timeouts are **separate 8s budgets**, so one fetch can legitimately block `loop()` for ~16s; observed max 14.46s against the real API, returning 200. That leaves only ~10s of margin under the 25s task watchdog. Anyone lowering `LOOP_WDT_TIMEOUT_S`, raising the HTTP timeouts, or adding a second blocking call to the fetch cycle must re-check this margin. | open |

### Test artifacts (not firmware defects)

- 7 of 724 probe runs failed across 6 cases, each returning **exactly the previous
  case's value**. Root cause was in `tools/probe.py`: the baseline fetch counter
  was built from *three separate* `health()` HTTP calls, so a fetch landing
  between them corrupted the baseline, the wait loop fell through, and the probe
  sampled stale state. Fixed to a single sample; `pos_no_seenpos`, the flakiest
  case at 2/15, then passed 6/6. No firmware behaviour was implicated.
- Three altitude cases (`alt_huge`, `alt_numstring`, `alt_geom_only`) looked wrong
  in the first pass on 2026-08-22 for the same reason and were cleared on
  isolated re-run.

---

## Night of 2026-08-22 → 23: full logic and data-path sweep

Method: every parse/classify branch driven with a crafted payload through a local
HTTPS mock (`tools/probe.py`, 48 cases), fault injection (`tools/faults.py`,
10 scenarios), display edge cases (`tools/harness.py`, 15 cases), a host-side
unit test of the type table (`tools/hosttest/`, 493 entries), and continuous
serial + `/healthz` capture.

### Confirmed defects

| # | Severity | Finding | Status |
|---|---|---|---|
| 1 | **high** | A `flight` field of only spaces (common in real ADS-B) yields an **empty ident** and is misclassified **COM**: `hasCallsign` is set from the untrimmed string, so padding counts as a callsign, and the `flight -> r -> hex` fallback never runs. Display shows a blank title where the callsign belongs. | **fixed 2026-08-23** |
| 2 | medium | `last_err` in `/healthz` is never cleared on success or on transport errors, so it reports a stale message indefinitely — observed showing `Too Many Requests` while `last_http` was `200`. Misleads any monitoring built on it. | **fixed 2026-08-23** |
| 3 | medium | The **no-data splash never pixel-shifts**. `pixelShiftIdx()` only re-renders through `renderFlight()`, so during a multi-day outage the splash is 100% static. Dimming to 25% mitigates burn-in but does not eliminate it. | **fixed 2026-08-23** |
| 4 | low | Table entry `TISB_OTHER` (10 chars) can never be matched: `FlightInfo.typeCode` is `char[8]`, so the API value is truncated to `TISB_OT` before lookup. Harmless today only because the `TISB` prefix heuristic catches it first. No guard enforces the 7-char limit. | **fixed 2026-08-23** |
| 5 | low | Any **non-`"ground"` string** in `alt_baro` (e.g. `"12345"`) renders as `GND`. The check is `is<const char*>()`, which does not verify the value is actually `"ground"`. | **fixed 2026-08-23** |
| 6 | low | Altitudes beyond int32 (`alt_baro: 1e18`) wrap through `as<int32_t>()` to a value `<= 0` and render as `GND` rather than being rejected. | **fixed 2026-08-23** |
| 7 | low | A genuine altitude of exactly **-1 ft** is indistinguishable from "absent" — both use `-1` — so it renders as an em-dash instead of `GND`. | **fixed 2026-08-23** |
| 8 | low | `seen_pos: null` is treated as **fresh**: `as<double>()` yields `0`. Violates the function's own stated rule that unknown age must not be trusted (`containsKey` passes, the null does not). | **fixed 2026-08-23** |
| 9 | low | ADS-B category `B2` (balloon/airship) falls through to the callsign rule and classifies **COM**. Only `A1/A7` map to PVT and `A3/A4/A5` to COM. | **fixed 2026-08-23** |
| 10 | info | `CLAUDE.md` documents ArduinoJson **6.x**; the installed and linked library is **7.4.3**, where `StaticJsonDocument` is deprecated and heap-backed. The `// stack-allocated` and "no heap allocation" comments in `fetchClosestAt` are therefore inaccurate. | **fixed 2026-08-23** |

### Verified correct (no action needed)

- Ident preference chain `flight -> r -> hex -> "(unknown)"`, including empty-string fallthrough.
- Position gating: missing `seen_pos`, stale `seen_pos`, `(0,0)`, and absent `lat`/`lon` are all correctly rejected.
- Haversine: antipodal case returns 20015.006 km against a true half-circumference of 20015.09 km.
- Malformed input: `ac` as object, missing `ac`, root array, root scalar, empty object, and all-null fields each degrade to the splash without a crash or reboot.
- 40 aircraft in one response parse cleanly (ArduinoJson 7 grows on the heap; no overflow).
- `dbFlags` handling: bit 0 set (alone or with other bits) classifies MIL; LADD-only (8) does not; a string `"1"` is coerced and still classifies MIL.
- Type table: 493 entries, correctly sorted, unique, every entry reachable by binary search, case-insensitive, plausible seat counts, no blank models, all friendly names fit `char[64]`.
- Over-long idents and type codes truncate safely; multi-byte UTF-8 idents render without corruption.

### Earlier in the campaign (already fixed)

- `HTTPClient::addHeader()` silently drops `User-Agent`/`Connection`/`Accept-Encoding`/`Host`, so the contact UA never left the device and adsb.lol returned 403. Fixed with `setUserAgent()`.
- `/v2/mil` read loop treated a momentary TLS buffer drain (`!available()`) as end-of-stream, consuming 3.9 KB of a 24 KB body; and its completion test (`!client.connected()`) could never be true against a keep-alive server. Both fixed, then the whole scan was deleted as provably redundant.
- Arduino core never subscribes `loopTask` to the task watchdog. Fixed: 25s TWDT, verified firing via `POST /test/hang`.
- HTTP timeouts were 23x the measured p95. Cut 30s/15s to 8s/8s; a hung server now stalls `loop()` for a measured 8.4s.

### Still owed

- **Wi-Fi AP disruption test** — needs a physical router power-cycle or the device taken out of range. The harness cannot sever the link it depends on.

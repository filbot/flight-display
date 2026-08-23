# Flight Display — findings ledger

Durable record of everything the hardening campaign has established. One entry
per finding, newest section first. Status is `open`, `fixed`, or `wontfix`.
`docs/soak-report.md` is regenerated from logs; **this file is the memory**.

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
| 1 | **high** | A `flight` field of only spaces (common in real ADS-B) yields an **empty ident** and is misclassified **COM**: `hasCallsign` is set from the untrimmed string, so padding counts as a callsign, and the `flight -> r -> hex` fallback never runs. Display shows a blank title where the callsign belongs. | open |
| 2 | medium | `last_err` in `/healthz` is never cleared on success or on transport errors, so it reports a stale message indefinitely — observed showing `Too Many Requests` while `last_http` was `200`. Misleads any monitoring built on it. | open |
| 3 | medium | The **no-data splash never pixel-shifts**. `pixelShiftIdx()` only re-renders through `renderFlight()`, so during a multi-day outage the splash is 100% static. Dimming to 25% mitigates burn-in but does not eliminate it. | open |
| 4 | low | Table entry `TISB_OTHER` (10 chars) can never be matched: `FlightInfo.typeCode` is `char[8]`, so the API value is truncated to `TISB_OT` before lookup. Harmless today only because the `TISB` prefix heuristic catches it first. No guard enforces the 7-char limit. | open |
| 5 | low | Any **non-`"ground"` string** in `alt_baro` (e.g. `"12345"`) renders as `GND`. The check is `is<const char*>()`, which does not verify the value is actually `"ground"`. | open |
| 6 | low | Altitudes beyond int32 (`alt_baro: 1e18`) wrap through `as<int32_t>()` to a value `<= 0` and render as `GND` rather than being rejected. | open |
| 7 | low | A genuine altitude of exactly **-1 ft** is indistinguishable from "absent" — both use `-1` — so it renders as an em-dash instead of `GND`. | open |
| 8 | low | `seen_pos: null` is treated as **fresh**: `as<double>()` yields `0`. Violates the function's own stated rule that unknown age must not be trusted (`containsKey` passes, the null does not). | open |
| 9 | low | ADS-B category `B2` (balloon/airship) falls through to the callsign rule and classifies **COM**. Only `A1/A7` map to PVT and `A3/A4/A5` to COM. | open |
| 10 | info | `CLAUDE.md` documents ArduinoJson **6.x**; the installed and linked library is **7.4.3**, where `StaticJsonDocument` is deprecated and heap-backed. The `// stack-allocated` and "no heap allocation" comments in `fetchClosestAt` are therefore inaccurate. | open |

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

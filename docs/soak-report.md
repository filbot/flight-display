# Flight Display — soak report

Generated 2026-08-23 13:59 UTC · window: last 9h

## Run summary

- Window `2026-08-23T05:00:24Z` → `2026-08-23T13:59:32Z` (8:59:08), 538 health samples, 1 unreachable (0.2%).
- Uptime now 30292s, last reset `SW`; 2 reboot(s) observed in window.
  - `2026-08-23T05:06:35Z` reason `SW`
  - `2026-08-23T05:34:50Z` reason `SW`
- Heap: now 195620, min-ever 119852, largest block 110580, half-vs-half drift +38 bytes.
- Wi-Fi: RSSI min -47 / avg -43 / max -41 dBm, 0 sample(s) with the link down.
- Fetch counters (lifetime since last boot): ok 1285, empty 153, fail 158.

## Crashes and watchdog

- 0 crash/watchdog line(s) in the serial window.

## Measured latency

- 2322 request(s): p50 933ms, p95 1204ms, max 13568ms.

## Data-path probe matrix

- 730 case-runs over 48 distinct cases: 299 pass, 7 fail.
  - `bad_ac_object` failed 1/15 run(s); last mismatch: `{'showing_flight': [False, True]}`
  - `ident_empty` failed 1/15 run(s); last mismatch: `{'ident': ['N123AB', '']}`
  - `ident_flight` failed 1/15 run(s); last mismatch: `{'ident': ['TEST123', 'SKW4100']}`
  - `ident_hex` failed 1/15 run(s); last mismatch: `{'ident': ['abc123', 'N123AB']}`
  - `pos_no_seenpos` failed 2/21 run(s); last mismatch: `{'showing_flight': [False, True]}`
  - `pos_zero` failed 1/15 run(s); last mismatch: `{'showing_flight': [False, True]}`

Observed behaviour for the exploratory cases (latest run of each):

| case | ident | op | title | alt_ft | shown |
|---|---|---|---|---|---|
| `alt_absent` | `'TEST123'` | COM | Boeing 737-800 | -1 | True |
| `alt_geom_only` | `'TEST123'` | COM | Boeing 737-800 | 17500 | True |
| `alt_ground_str` | `'TEST123'` | COM | Boeing 737-800 | -2 | True |
| `alt_huge` | `'TEST123'` | COM | Boeing 737-800 | 999999 | True |
| `alt_minus_one` | `'TEST123'` | COM | Boeing 737-800 | -1 | True |
| `alt_negative` | `'TEST123'` | COM | Boeing 737-800 | -2 | True |
| `alt_numstring` | `'TEST123'` | COM | Boeing 737-800 | -2 | True |
| `alt_zero` | `'TEST123'` | COM | Boeing 737-800 | -2 | True |
| `bad_bignum` | `'TEST123'` | COM | Boeing 737-800 | -2 | True |
| `bad_deep` | `'TEST123'` | COM | Boeing 737-800 | 30000 | True |
| `bad_many_ac` | `'TEST123'` | COM | Boeing 737-800 | 30000 | True |
| `bad_types` | `'TEST123'` | COM | Boeing 737-800 | 30000 | True |
| `cls_cat_a7` | `'TEST123'` | PVT | Unknown Aircraft | 30000 | True |
| `cls_cat_b2` | `'TEST123'` | COM | Unknown Aircraft | 30000 | True |
| `cls_dbflags_str` | `'TEST123'` | MIL | Boeing 737-800 | 30000 | True |
| `cls_ladd` | `'TEST123'` | COM | Boeing 737-800 | 30000 | True |
| `cls_nocallsign` | `'abc123'` | COM | Boeing 737-800 | 30000 | True |
| `ident_long` | `'ABCDEFGHIJKLMNO'` | COM | Boeing 737-800 | 30000 | True |
| `ident_unicode` | `'ÜNI✈CODE'` | COM | Boeing 737-800 | 30000 | True |
| `ident_ws_only` | `''` | COM | Boeing 737-800 | 30000 | True |
| `pos_antipode` | `'TEST123'` | COM | Boeing 737-800 | 30000 | True |
| `pos_seenpos_null` | `'TEST123'` | COM | Boeing 737-800 | 30000 | True |
| `type_absent` | `'TEST123'` | COM | BOEING 737-800 | 30000 | True |
| `type_known` | `'TEST123'` | COM | Airbus A320 | 30000 | True |
| `type_long` | `'TEST123'` | COM | BOEING 737-800 | 30000 | True |
| `type_nodesc` | `'TEST123'` | COM | ZZZZ | 30000 | True |
| `type_prefix` | `'TEST123'` | COM | Boeing 777-200LR | 30000 | True |
| `type_unknown` | `'TEST123'` | COM | BOEING 737-800 | 30000 | True |

## Suite results

- fault scenarios: 13 run(s), most recent `10/10`, worst `10/10`.
- display cases: 13 run(s), most recent `15/15`, worst `15/15`.
- probe passes: 13 run(s), most recent `18/2`, worst `18/2`.
- 13 complete overnight pass(es).

## Findings

See `docs/findings.md` — that file is hand-maintained and is the durable record.

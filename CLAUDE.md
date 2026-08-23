# CLAUDE.md — Flight Display Project Guide

## Project Overview

ESP32-based ADS-B flight tracker. Queries [adsb.lol](https://api.adsb.lol) every 30 seconds for the nearest aircraft within a configurable radius. Classifies each aircraft as **MIL** (military), **COM** (commercial), or **PVT** (private) using the API's `dbFlags` military bit, seat-count heuristics, and ADS-B category codes. Renders aircraft name, distance, seats, and altitude on a 256x64 SSD1322 OLED. Controls a 4-channel relay module (status + one per category).

## Hardware

| Component | Spec | Notes |
|-----------|------|-------|
| MCU | ESP32-WROOM-32 | 240 MHz, ~320 KB free heap, no PSRAM |
| Display | NHD-5.5-25664UCG3 (SSD1322) | 256x64, 1-bit green mono, SPI |
| SPI Pins | CS=5, DC=16, RST=17, SCLK=18, MOSI=23 | HW SPI, rotated 180° (U8G2_R2) |
| Relay | 4-channel module, active-LOW default | GPIOs: IN1=33, IN2=32, IN3=26, IN4=27 |
| Power | USB 5V, 3.3V logic | Low TX power during boot to reduce inrush |

Display driver: `U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI` with full framebuffer.

## File Layout

| File | Purpose |
|------|---------|
| `flight-display.ino` | Main firmware: WiFi, API fetch, JSON parse, classification, OLED rendering, relay control, OTA, test HTTP server |
| `aircraft_types.h` | `AircraftTypeInfo` struct, `kTypeInfo[]` lookup table (sorted, binary search), `aircraftLookup()`, `aircraftFriendlyName()`, `aircraftSeatMax()`, family-prefix heuristic fallbacks |
| `config.h` | Per-installation secrets/settings (**gitignored** — never commit) |
| `example-config.h` | Template for `config.h` |
| `log.h` | Leveled logging macros: `LOG_ERROR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG` with compile-time `LOG_LEVEL` |
| `AGENTS.md` | AI agent operating guide (ESP32 firmware conventions) |
| `README.md` | Project documentation, hardware setup, API details |
| `tools/monitor.sh` | Detached soak monitor: serial capture + 60s `/healthz` poll into gitignored `logs/` |
| `tools/report.py` | Summarizes a soak run: reboots, heap drift, Wi-Fi, fetch mix, no-data stretches |
| `tools/harness.py` | Injects display edge cases via `PUT /test/closest`; `soak` mode cycles them unattended |
| `tools/mockapi.py` | Faithful local HTTPS replica of api.adsb.lol: real routes, honours lat/lon/radius against a synthetic fleet, reproduces the UA-403 / 429 / keep-alive quirks, logs every request |
| `tools/flows.py` | Whole-flow tests asserted on what the device *requested* (radius conversion, tiered fallback, UA, override TTL, empty-clears) |
| `tools/with-device-lock.sh` | Mutex every device driver must go through — two at once corrupt each other |

## Build

- **IDE**: Arduino IDE or `arduino-cli`
- **Board**: ESP32 Dev Module (`esp32:esp32:esp32`)
- **Partition scheme**: **Minimal SPIFFS (1.9MB APP with OTA)** — required; the sketch exceeds the default 1.25MB app partition. CLI: `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs .`. Do NOT use "Huge APP" (removes the second app slot and breaks OTA).
- **Libraries**: U8g2 (olikraus), ArduinoJson 6.x, WiFi/WiFiClientSecure/HTTPClient/WebServer (ESP32 core)
- **Setup**: Copy `example-config.h` to `config.h`, fill in WiFi credentials, `HOME_LAT`/`HOME_LON`, `SEARCH_RADIUS_KM`

## Architecture

```
WiFi Connect → API Fetch (adsb.lol /v2/closest)
    ↓
JSON Parse (streamed, filtered) → FlightInfo struct
    ↓
Classify (dbFlags → seat heuristics → category → callsign)   [no network I/O]
    ↓
Render on OLED (dynamic font sizing, 1-2 line title + 3 bottom cells)
    ↓
Control Relays (status=ON, exactly one category relay ON)
```

**Key timing**: 30s fetch interval with exponential backoff on failure. No blocking `delay()` in main loop.

**Feature flags** (compile-time, overridable in config.h):
- `FEATURE_OTA` (default 1) — ArduinoOTA updates
- `FEATURE_TEST_ENDPOINT` (default 1) — Test HTTP server on port 80

## API

- **Base**: `https://api.adsb.lol`
- **Endpoint**: `GET /v2/closest/{lat}/{lon}/{radius}` — returns nearest aircraft. **The radius parameter is nautical miles**, not km — the firmware converts `SEARCH_RADIUS_KM` (real km) to nm when building the URL.
- **Tiered search** (verified by `tools/flows.py`: requests 5 nm then 54 nm): primary `SEARCH_RADIUS_KM` circle first; only if empty, one extra call at `SEARCH_RADIUS_FALLBACK_KM` (default 100) so the display always shows the nearest aircraft when anything is in range.
- **MIL endpoint**: `GET /v2/mil` exists but is **deliberately unused**. Every aircraft it returns already carries `dbFlags` bit 0, and the API omits `dbFlags` entirely when it would be zero, so the list cannot add information the closest response has not already given. Verified over 802 aircraft across 4 regions: `dbFlags` never appears as an explicit 0, and all 7 aircraft with bit 0 set were in the mil list. Do not reintroduce a scan of it.
- **Fields used**: `hex`, `flight`, `r`, `t`, `alt_baro`, `alt_geom` (fallback when `alt_baro` absent), `lat`, `lon`, `seen_pos`, `category`, `dbFlags`, `desc` (title fallback for unknown types)
- **Fields available but unused**: `gs` (ground speed), `track`, `geom_rate`, `nav_altitude_mcp`, `emergency`, `type` (message type — never use as a `t` fallback), `ownOp` (operator)

## Coding Conventions

- **Functions**: `static` throughout (single translation unit)
- **Config**: `#ifndef` guards on all constants — override in `config.h`
- **Scheduling**: Cooperative via `millis()` — never `delay()` in main loop
- **Memory**: Prefer fixed char arrays over Arduino `String` in hot paths. Use `snprintf` with bounds. `PROGMEM`/`F()` for string literals where applicable.
- **Display**: U8g2 full-buffer pattern: `clearBuffer()` → draw → `sendBuffer()`
- **Logging**: `LOG_INFO("message %d", value)` — compile-time filtered via `LOG_LEVEL`
- **Error handling**: All I/O has bounded timeouts. Graceful degradation (show "—" for missing data, "No data" splash on API failure).

## Commit Conventions

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
type(scope): description
```

**Types**: `feat`, `fix`, `refactor`, `docs`, `chore`, `perf`, `data`
**Scopes**: `display`, `api`, `types`, `relay`, `wifi`, `ota`, `test`

Examples:
- `data(types): add 30 missing GA aircraft to kTypeInfo table`
- `perf(types): sort kTypeInfo and implement binary search lookup`
- `fix(api): add retry with exponential backoff for fetch failures`
- `refactor(api): replace String with fixed char arrays in FlightInfo`

## Key Patterns

- **Classification priority**: dbFlags military bit → seat count (≤15 = PVT) → ADS-B category (runs whenever seats didn't classify) → callsign presence (has callsign = COM, else PVT). `classifyOp()` does no network I/O at all.
- **Aircraft lookup**: Binary search on sorted, unique-key `kTypeInfo[]` by ICAO only (no IATA fallback — API `t` is always ICAO), then ~15 family-prefix heuristics. Unknown types fall back to the API `desc` field, then the raw code.
- **Staleness**: Positions without fresh `seen_pos` are rejected. Displayed flight clears to splash after `STALE_DISPLAY_MAX_MS` (default 5 min) without a successful refresh; an empty-but-valid API response ("no aircraft nearby") clears immediately and doesn't count as a fetch failure.
- **Display rendering**: Font cascade (32pt → 24pt → 20pt → 10pt → 9pt → 6pt) with 2-line word wrapping. Bottom bar: distance | seats | altitude in 3 equal cells.
- **Relay mapping**: Status=IN1, PVT=IN2, COM=IN3, MIL=IN4 (configurable via `RELAY_*_PIN`)
- **Test override**: `PUT /test/closest` with JSON body to inject test flight data (5-min TTL)
- **Health**: `GET /healthz` returns JSON telemetry (uptime, reset reason, heap free/min/largest-block, RSSI, fetch ok/empty/fail counters, last HTTP status and error body, last-data age, what's on screen, dim state). `GET /` stays plain `OK`.
- **Display brightness**: `DISPLAY_CONTRAST` is **153** (60% of full scale), not 255 — the SSD1322 contrast register sets drive current, so running the panel below maximum slows luminance decay on a 24/7 display. Splashes run at `DISPLAY_CONTRAST_DIM` (25% of that, so 38) to limit burn-in during outages; `renderFlight()` restores normal brightness. Both are `#ifndef`-guarded, so override in `config.h` if the panel reads too dim.

## Gotchas

- `SCREEN_WIDTH` defaults to 128 in .ino but config.h overrides to 256 — always check config.h
- Display is rotated 180° (`U8G2_R2`) — (0,0) is bottom-right of physical display
- ADS-B pads `flight` to 8 characters, so "no callsign" arrives as `"        "`, not as an empty string or an absent key. Test it with `hasNonSpace()`, never `*p` — padding otherwise wins the ident chain (blank title) and counts as a callsign (misclassifies PVT as COM).
- Under the linked ArduinoJson **7.4.3**, `StaticJsonDocument<N>` is plain `JsonDocument`: `N` is ignored and allocation is on the heap. Responses above ~3.6 KB fail to parse as `EmptyInput` regardless of `N`; unreachable today because `/v2/closest` returns one aircraft.
- - `dbFlags` is **absent**, not zero, for non-military aircraft — the API omits the key entirely. `FlightInfo.dbFlags` uses -1 for absent, so test `dbFlags >= 0 && (dbFlags & 1)`; treating absence as unknown rather than as a negative is what previously justified the redundant `/v2/mil` scan.
- `WiFi.setAutoReconnect(true)` handles most reconnects but has no backoff
- The Arduino core initializes the task watchdog (5s, panic) but **never subscribes `loopTask`**, so a wedged `loop()` hangs forever by default. `setup()` now reconfigures it to `LOOP_WDT_TIMEOUT_S` (25s) and subscribes. Any new blocking I/O in `loop()` must either finish inside that window or call `esp_task_wdt_reset()` as it works, like the MIL scan does.
- HTTP timeouts are sized from measured latency (p50 957ms / p95 1270ms over 428 live fetches), not guessed. Keep them well under `LOOP_WDT_TIMEOUT_S` or a slow API will look like a hang and reboot the device.
- `client.setInsecure()` — no TLS cert verification (standard for ESP32 IoT)
- `alt_baro` is a number in feet OR the string `"ground"` — parsed to the `ALT_GROUND` sentinel (-2), rendered as `GND`
- Duplicate ICAO variants (freighter/winglet) must NOT be added back to `kTypeInfo[]` — the `t` field can't distinguish them, and binary search requires unique sorted keys
- **`HTTPClient::addHeader()` SILENTLY DROPS `User-Agent`, `Connection`, `Accept-Encoding` and `Host`** (`HTTPClient.cpp:992` filters them; it writes its own at `:1150`). The call compiles, runs, and does nothing — that's how the generic built-in UA reached adsb.lol and earned a 403. Use `setUserAgent()` / `setAcceptEncoding()` instead. The `_acceptEncoding` default (`identity;q=1,chunked;q=0.1,*;q=0`) already prefers identity, so no setter is needed for that.
- adsb.lol returns **403 with body `User-Agent too generic; include valid contact info.`** for anonymous-looking clients. `API_USER_AGENT` must carry a real email or project URL and is set in `config.h` (gitignored). The same request from a laptop can return 200 while the device gets 403, so don't test this with curl alone — read the device's own error body.
- Non-2xx API responses: always log `http.getString()` (bounded). The status code alone hid this 403's cause completely.
- `arduino-cli monitor` **exits immediately when stdin is not a TTY**, so it cannot be used for detached/background logging. Read the port directly, holding the fd open with `exec 3<>` so the `stty` settings survive (each fresh open resets the port to 9600 on macOS).
- **Never hand a raw TLS stream to a streaming parser.** ArduinoJson stops the instant a read returns short, so a momentary gap ends the document (`IncompleteInput`), and an empty buffer at the start returns `EmptyInput`. `fetchClosestAt()` wraps the client in `PatientStream`, whose `readBytes()` waits for the next burst under one overall deadline. This lifted the response ceiling from ~3.6 KB to beyond 57 KB. Only `readBytes()` needs overriding, and direct client reads are safe only while responses carry `Content-Length` (never chunked).
- **`stream.available() == 0` is NOT end-of-stream.** Over TLS the body arrives in bursts and the buffer drains between them. Breaking the read loop on `!available()` truncated every `/v2/mil` scan at ~4 KB of 25 KB (that scan has since been deleted, but the lesson applies to any streamed body). Loop until `Content-Length` (`http.getSize()`) is satisfied; treat only *closed connection + empty buffer* as EOF, with a wall-clock timeout as the bound.
- Conversely, **`client.connected()` stays true after a complete body** when the server keeps the connection alive, so it cannot be used to detect a clean end either.
- Raw serial capture contains non-printable boot-ROM bytes, which makes `grep` treat `logs/serial.log` as binary and print *nothing* for patterns that are present. `tools/monitor.sh` now squashes them on write; for older logs use `grep -a`.
- **Opening the USB serial port reboots the board** (DTR toggle). A reset logged at the moment a monitor attaches is an artifact, not a fault. USB flashing and serial logging contend for the port — stop the logger or flash via OTA.

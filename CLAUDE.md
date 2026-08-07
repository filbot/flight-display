# CLAUDE.md — Flight Display Project Guide

## Project Overview

ESP32-based ADS-B flight tracker. Queries [adsb.lol](https://api.adsb.lol) every 30 seconds for the nearest aircraft within a configurable radius. Classifies each aircraft as **MIL** (military), **COM** (commercial), or **PVT** (private) using military API lookup, seat-count heuristics, and ADS-B category codes. Renders aircraft name, distance, seats, and altitude on a 256x64 SSD1322 OLED. Controls a 4-channel relay module (status + one per category).

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
Classify (dbFlags → MIL cache → seat heuristics → category → callsign)
    ↓
Render on OLED (dynamic font sizing, 1-2 line title + 3 bottom cells)
    ↓
Control Relays (status=ON, exactly one category relay ON)
```

**Key timing**: 30s fetch interval with exponential backoff on failure. No blocking `delay()` in main loop.

**Feature flags** (compile-time, overridable in config.h):
- `FEATURE_OTA` (default 1) — ArduinoOTA updates
- `FEATURE_MIL_LOOKUP` (default 1) — Military aircraft detection via `/v2/mil`
- `FEATURE_TEST_ENDPOINT` (default 1) — Test HTTP server on port 80

## API

- **Base**: `https://api.adsb.lol`
- **Endpoint**: `GET /v2/closest/{lat}/{lon}/{radius}` — returns nearest aircraft
- **MIL endpoint**: `GET /v2/mil` — all military aircraft (used for cache misses)
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

- **Classification priority**: dbFlags military bit → MIL cache lookup → seat count (≤15 = PVT) → ADS-B category (runs whenever seats didn't classify) → callsign presence (has callsign = COM, else PVT)
- **MIL cache**: 16-entry array, 6h positive / 1h negative TTL, fixed `char[8]` hex keys. An incomplete `/v2/mil` scan (timeout/stall) reports failure, never "not military".
- **Aircraft lookup**: Binary search on sorted, unique-key `kTypeInfo[]` by ICAO only (no IATA fallback — API `t` is always ICAO), then ~15 family-prefix heuristics. Unknown types fall back to the API `desc` field, then the raw code.
- **Staleness**: Positions without fresh `seen_pos` are rejected. Displayed flight clears to splash after `STALE_DISPLAY_MAX_MS` (default 5 min) without a successful refresh; an empty-but-valid API response ("no aircraft nearby") clears immediately and doesn't count as a fetch failure.
- **Display rendering**: Font cascade (32pt → 24pt → 20pt → 10pt → 9pt → 6pt) with 2-line word wrapping. Bottom bar: distance | seats | altitude in 3 equal cells.
- **Relay mapping**: Status=IN1, PVT=IN2, COM=IN3, MIL=IN4 (configurable via `RELAY_*_PIN`)
- **Test override**: `PUT /test/closest` with JSON body to inject test flight data (5-min TTL)

## Gotchas

- `SCREEN_WIDTH` defaults to 128 in .ino but config.h overrides to 256 — always check config.h
- Display is rotated 180° (`U8G2_R2`) — (0,0) is bottom-right of physical display
- The `/v2/mil` endpoint returns ALL military aircraft globally and streams the full response — this blocks loop() for potentially seconds. Use `dbFlags` first.
- `WiFi.setAutoReconnect(true)` handles most reconnects but has no backoff
- `client.setInsecure()` — no TLS cert verification (standard for ESP32 IoT)
- `alt_baro` is a number in feet OR the string `"ground"` — parsed to the `ALT_GROUND` sentinel (-2), rendered as `GND`
- Duplicate ICAO variants (freighter/winglet) must NOT be added back to `kTypeInfo[]` — the `t` field can't distinguish them, and binary search requires unique sorted keys

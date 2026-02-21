# ESP32 ADS-B Flight Display

This project uses an **ESP32** to query the [adsb.lol](https://api.adsb.lol) API for nearby flights and display the information on an OLED screen. The display shows flight details such as:

- **Flight number / Callsign**
- **Origin and destination airports**  
- **Aircraft type and max seats**  
- **Altitude and distance from your receiver**  
- **Operator classification:** PVT (private), COM (commercial), MIL (military)

---

## Hardware Used

| Component | Description | Notes |
|-----------|-------------|-------|
| **ESP32 Dev Board** | Main microcontroller | Any ESP32 dev module with Wi‑Fi support |
| **5.5" OLED Display** | 256×64 px, SSD1322 (NHD‑5.5‑25664UCG3) | SPI: `CS=5`, `DC=16`, `RST=17`, `SCLK=18`, `MOSI=23` (MISO not used) |
| **USB Cable** | For programming and power | 5V supply via USB |

Notes
- Display uses U8g2 driver with full‑frame buffer and 1‑bit green mono output.
- Pins are configured in `config.h` and can be changed as needed.
- Power the OLED per vendor specs; logic is 3.3V.

### Relay Calibration

If relays don’t match the expected channel (e.g., COM lights the wrong relay or a different channel energizes on boot):
- Set `RELAY_ACTIVE_HIGH` according to your module (most 4‑ch boards are active‑LOW = 0).
- Map roles explicitly by overriding in `config.h`:
  - `RELAY_STATUS_PIN`, `RELAY_COM_PIN`, `RELAY_PVT_PIN`, `RELAY_MIL_PIN`
- Optionally disable the dedicated status channel: `#define RELAY_USE_STATUS_CHANNEL 0`.
- Use the test endpoint to push test flight data with specific `op` class to validate relay wiring:
  - `PUT http://<device-ip>/test/closest` with body `{"ident":"TEST","op":"COM"}` (repeat for PVT, MIL)

## Libraries

- `U8g2` (by olikraus) for SSD1322 rendering
- `ArduinoJson` for JSON parsing
- `WiFi`, `WiFiClientSecure`, `HTTPClient`, `WebServer` (bundled with ESP32 core)

---

## API Details

This project uses the [adsb.lol](https://api.adsb.lol) API to retrieve live aircraft data.  
Endpoints used:

### Closest Flight

GET /v2/closest/{lat}/{lon}/{radius}

- **lat**: Latitude in decimal degrees  
- **lon**: Longitude in decimal degrees  
- **radius**: Search radius in kilometers (**integer**)  

Example:
https://api.adsb.lol/v2/closest/40.6413/-73.7781/32

Returns the closest aircraft object with fields like:
```json
{
  "hex": "A388AF",
  "flight": "DAL327",
  "r": "N327DU",
  "t": "BCS3",
  "category": "A3",
  "alt_baro": 35000,
  "gs": 445,
  "lat": 40.645,
  "lon": -73.782
}

### Optional MIL classification

To detect military aircraft the device can query `/v2/mil` once per new hex and cache the result for several hours.
- Toggle at compile time with `#define FEATURE_MIL_LOOKUP 0/1` (default: 1).
- If disabled, MIL classification is inferred only from type/seat heuristics.

### Local Test Endpoint

The device exposes a simple test endpoint (when enabled) to push JSON and render without calling the API:

- `PUT http://<device-ip>/test/closest`
  - Body: either a minimal test schema `{ "t": "B738", "ident": "TEST123", "alt": 32000, "dist": 12.3 }` or the full `/v2/closest` schema.
- Toggle at compile time with `#define FEATURE_TEST_SERVER 0/1` (default: 1).

Notes
- JSON parsing is filtered and streamed to minimize RAM (~8 KB doc).
- All network I/O has bounded timeouts; Wi‑Fi reconnects automatically.

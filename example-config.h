// Copy this file to config.h and adjust for your setup.
// Do NOT commit config.h with real credentials.

#pragma once

// Wi‑Fi
#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASSWORD"

// Location and search radius (real kilometers — converted to the API's
// nautical-mile parameter internally)
#define HOME_LAT 00.0000
#define HOME_LON -000.0000
#define SEARCH_RADIUS_KM 10
// Wider radius tried only when the primary circle is empty, so the display
// always shows the nearest aircraft when anything is in range
// #define SEARCH_RADIUS_FALLBACK_KM 100

// API base (https enabled)
#define API_BASE "https://api.adsb.lol"

// OLED hardware (SSD1322 SPI)
#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 64
// SPI pins for the OLED
#define PIN_CS 5     // /CS
#define PIN_DC 16    // D/C
#define PIN_RST 17   // /RES
#define PIN_CLK 18   // SCLK
#define PIN_MOSI 23  // SDIN

// 4-channel relay module GPIO (active level configured in sketch)
#define RELAY_IN1_PIN 33
#define RELAY_IN2_PIN 32
#define RELAY_IN3_PIN 26
#define RELAY_IN4_PIN 27

// Optional: explicit role mapping (uncomment and adjust to match wiring).
// These are the sketch defaults: STATUS=IN1, PVT=IN2, COM=IN3, MIL=IN4.
// #define RELAY_STATUS_PIN RELAY_IN1_PIN
// #define RELAY_PVT_PIN    RELAY_IN2_PIN
// #define RELAY_COM_PIN    RELAY_IN3_PIN
// #define RELAY_MIL_PIN    RELAY_IN4_PIN
// #define RELAY_ACTIVE_HIGH 0      // 0 for active-LOW boards, 1 for active-HIGH

// --- OTA (ArduinoOTA) ---
// Set FEATURE_OTA to 1 to enable over-the-air updates.
// Provide a strong OTA password for production.
// #define FEATURE_OTA 1
// #define OTA_HOSTNAME "flight-display"
// #define OTA_PORT 3232
// #define OTA_PASSWORD "change-me"

// adsb.lol rejects generic User-Agents with 403 — it wants real contact info.
// Put your own email or project URL here before flashing.
// #define API_USER_AGENT "flight-display/1.0 (ESP32 ADS-B display; you@example.com)"

// Display brightness (SSD1322 contrast register, 0-255). Default is 153 (60%)
// to extend panel life on a 24/7 display; splashes run at a quarter of this.
// #define DISPLAY_CONTRAST 153

// --- Local ADS-B receiver (optional) ---
// If you run PiAware / dump1090 / readsb, the device can refresh distance and
// altitude from it every few seconds between the 30s API polls, so the numbers
// move smoothly instead of ticking. Identity and type still come from the API.
// Use an IP ADDRESS: a failed DNS lookup blocks loop() for a fixed 15 seconds.
// Find the JSON path with: curl http://<ip>:8080/aircraft
// #define FEATURE_LOCAL_RX 1
// #define LOCAL_RX_HOST "192.168.1.243"
// #define LOCAL_RX_PORT 8080
// #define LOCAL_RX_PATH "/aircraft"

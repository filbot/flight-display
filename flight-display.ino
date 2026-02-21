// ESP32 ADS-B Flight Display
// - Connects to Wi‑Fi
// - Calls adsb.lol /v2/closest/{lat}/{lon}/{radius}
// - Parses nearest aircraft and renders summary on SSD1322 OLED

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#ifdef ESP32
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#endif
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <WebServer.h>
#if defined(ESP32)
#include <esp_system.h>
#endif

#include "config.h"  // Create from config.example.h and do not commit secrets
#include "aircraft_types.h"
#include "log.h"

// FlightInfo is used across parsing, test overrides, and rendering.
// Uses fixed char arrays to avoid heap allocation in hot paths.
struct FlightInfo {
  char ident[16];      // flight/callsign or registration/hex fallback
  char typeCode[8];    // aircraft type (t)
  char category[4];    // raw ADS-B category code (A1-A7, B1-B7, C1-C3)
  long altitudeFt;
  double lat;
  double lon;
  double distanceKm;
  char hex[8];         // transponder hex id
  bool hasCallsign;
  char opClass[4];     // MIL/COM/PVT
  bool valid;
  int seatOverride;    // if >0, override seat display
  int dbFlags;         // adsb.lol dbFlags field; bit 0 = military

  FlightInfo() : altitudeFt(-1), lat(NAN), lon(NAN), distanceKm(NAN),
                 hasCallsign(false), valid(false), seatOverride(-1), dbFlags(-1) {
    ident[0] = '\0';
    typeCode[0] = '\0';
    category[0] = '\0';
    hex[0] = '\0';
    opClass[0] = '\0';
  }
};
// Local function prototypes used before definitions
static void relaysPowerOnly();
static void showSplash(const char *msgTop, const char *msgBottom = nullptr);
static void drawCentered(const String &text, int16_t baselineY);

// -----------------------------
// OTA configuration (overridable in config.h)
// -----------------------------
#ifndef FEATURE_OTA
#define FEATURE_OTA 1  // Set to 0 to disable Arduino OTA
#endif
#ifndef OTA_PORT
#define OTA_PORT 3232
#endif
#ifndef OTA_HOSTNAME
#define OTA_HOSTNAME "flight-display"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""  // Empty = no auth (DEV ONLY). Set a strong password for production.
#endif

// Relay module pins and logic (override in config.h if desired)
#ifndef RELAY_IN1_PIN
#define RELAY_IN1_PIN 33
#endif
#ifndef RELAY_IN2_PIN
#define RELAY_IN2_PIN 32
#endif
#ifndef RELAY_IN3_PIN
#define RELAY_IN3_PIN 26
#endif
#ifndef RELAY_IN4_PIN
#define RELAY_IN4_PIN 27
#endif

// Explicit role mapping (override any of these to match wiring)
#ifndef RELAY_STATUS_PIN
#define RELAY_STATUS_PIN RELAY_IN1_PIN
#endif
#ifndef RELAY_PVT_PIN
#define RELAY_PVT_PIN RELAY_IN2_PIN
#endif
#ifndef RELAY_COM_PIN
#define RELAY_COM_PIN RELAY_IN3_PIN
#endif
#ifndef RELAY_MIL_PIN
#define RELAY_MIL_PIN RELAY_IN4_PIN
#endif

#ifndef RELAY_ACTIVE_HIGH
#define RELAY_ACTIVE_HIGH 0  // 0=active LOW modules; set 1 if active HIGH
#endif
// Status (power) indicator is always ON during runtime.

// Boot staging: allow power to settle before Wi‑Fi/TLS ramps current
#ifndef BOOT_POWER_SETTLE_MS
#define BOOT_POWER_SETTLE_MS 1200
#endif

// Use lower TX power during connection to reduce inrush; bump after got IP
#ifndef WIFI_BOOT_TXPOWER
#define WIFI_BOOT_TXPOWER WIFI_POWER_8_5dBm
#endif
#ifndef WIFI_RUN_TXPOWER
#define WIFI_RUN_TXPOWER WIFI_POWER_15dBm
#endif

static inline void relayWrite(int pin, bool on) {
  if (pin < 0) return;  // allow disabling a channel by setting pin to -1
  digitalWrite((uint8_t)pin, (RELAY_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)));
}

static void relaysInit() {
  const int rolePins[] = { RELAY_STATUS_PIN, RELAY_PVT_PIN, RELAY_COM_PIN, RELAY_MIL_PIN };
  const int INACTIVE = (RELAY_ACTIVE_HIGH ? LOW : HIGH);  // OFF level
  // Preload OFF level before switching to OUTPUT to avoid glitches
  for (size_t i = 0; i < sizeof(rolePins) / sizeof(rolePins[0]); ++i) {
    int p = rolePins[i];
    if (p < 0) continue;
    digitalWrite((uint8_t)p, INACTIVE);
    pinMode((uint8_t)p, OUTPUT);
    digitalWrite((uint8_t)p, INACTIVE);
  }
  // Turn status ON (power indicator); keep categories OFF
  relayWrite(RELAY_STATUS_PIN, true);
}

static void relaysPowerOnly() {
  // Status ON, categories OFF
  relayWrite(RELAY_STATUS_PIN, true);
  relayWrite(RELAY_PVT_PIN, false);
  relayWrite(RELAY_COM_PIN, false);
  relayWrite(RELAY_MIL_PIN, false);
}

static void relaysShowCategory(const char* opClass) {
  // Status ON, exactly one category ON
  relayWrite(RELAY_STATUS_PIN, true);
  relayWrite(RELAY_PVT_PIN, false);
  relayWrite(RELAY_COM_PIN, false);
  relayWrite(RELAY_MIL_PIN, false);
  if (strcmp(opClass, "PVT") == 0) relayWrite(RELAY_PVT_PIN, true);
  else if (strcmp(opClass, "COM") == 0) relayWrite(RELAY_COM_PIN, true);
  else if (strcmp(opClass, "MIL") == 0) relayWrite(RELAY_MIL_PIN, true);
}

#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 128
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 64
#endif

#ifndef FEATURE_MIL_LOOKUP
#define FEATURE_MIL_LOOKUP 1
#endif

// SPI OLED (SSD1322) via U8g2, full framebuffer, HW SPI (rotated 180°)
U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI u8g2(U8G2_R2, PIN_CS, PIN_DC, PIN_RST);

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;  // 20s
static const uint32_t FETCH_INTERVAL_MS = 30000;        // 30s between API calls
static const uint32_t HTTP_CONNECT_TIMEOUT_MS = 15000;  // HTTP connect timeout
static const uint32_t HTTP_READ_TIMEOUT_MS = 30000;     // HTTP read timeout

// Exponential backoff with jitter for retry logic
static uint32_t backoffMs(uint8_t attempt, uint32_t base = 2000, uint32_t cap = 60000) {
  uint32_t exp = base << min((uint8_t)attempt, (uint8_t)5);
  uint32_t jitter = (exp >> 3) * (esp_random() & 0x7) / 7;  // ~±12.5% jitter
  return min(cap, exp + jitter);
}
static uint8_t g_fetchFailCount = 0;
static uint32_t g_nextFetchAt = 0;
// Wi‑Fi reconnect state
static bool wifiConnecting = false;
static bool wifiInitialized = false;
static bool wifiEverBegun = false;

#if FEATURE_OTA
static bool g_otaReady = false;        // true after ArduinoOTA.begin while Wi‑Fi has IP
static volatile bool g_inOta = false;  // set during OTA to pause app work
#endif

// MIL cache — uses fixed char arrays to avoid heap allocation
struct MilCacheEntry {
  char hex[8];    // ICAO hex is always 6 chars + null
  uint32_t ts;
  bool isMil;
  bool valid;
};
static const size_t MIL_CACHE_SIZE = 16;
static MilCacheEntry g_milCache[MIL_CACHE_SIZE];

// Flight info structure used across network parsing, test overrides, and rendering
// (struct FlightInfo defined earlier)

// -----------------------------
// Test HTTP server (/test/closest)
// -----------------------------
#ifndef FEATURE_TEST_ENDPOINT
#define FEATURE_TEST_ENDPOINT 1  // Set 0 to remove test HTTP endpoint
#endif
#ifndef TEST_OVERRIDE_TTL_MS
#define TEST_OVERRIDE_TTL_MS (5UL * 60UL * 1000UL)  // 5 minutes
#endif

#if FEATURE_TEST_ENDPOINT
static WebServer g_http(80);
static bool g_httpStarted = false;
struct TestOverride {
  bool active = false;
  uint32_t expiresAt = 0;
  FlightInfo fi;       // prepared record to render
  bool dirty = false;  // force immediate render on next loop
} g_test;

static void httpHandleRoot() {
  g_http.send(200, "text/plain", "OK");
}

static void httpHandleGetClosest() {
  StaticJsonDocument<256> doc;
  bool alive = g_test.active && (int32_t)(millis() - g_test.expiresAt) < 0;
  doc["active"] = alive;
  doc["expires_in_ms"] = alive ? (int32_t)((int32_t)g_test.expiresAt - (int32_t)millis()) : 0;
  doc["ident"] = (const char*)g_test.fi.ident;
  doc["op"] = (const char*)g_test.fi.opClass;
  doc["t"] = (const char*)g_test.fi.typeCode;
  doc["alt"] = g_test.fi.altitudeFt;
  doc["dist"] = g_test.fi.distanceKm;
  String out;
  serializeJson(doc, out);
  g_http.send(200, "application/json", out);
}

static void httpHandlePutClosest() {
  if (!g_http.hasArg("plain")) {
    g_http.send(400, "text/plain", "Missing body");
    return;
  }
  const String &body = g_http.arg("plain");
  StaticJsonDocument<384> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    g_http.send(400, "text/plain", String("Bad JSON: ") + err.c_str());
    return;
  }
  FlightInfo fi;
  fi.valid = true;
  const char* identSrc = doc["ident"].isNull() ? "TEST" : doc["ident"].as<const char*>();
  strncpy(fi.ident, identSrc, sizeof(fi.ident) - 1); fi.ident[sizeof(fi.ident) - 1] = '\0';
  const char* tSrc = doc["t"].isNull() ? "" : doc["t"].as<const char*>();
  strncpy(fi.typeCode, tSrc, sizeof(fi.typeCode) - 1); fi.typeCode[sizeof(fi.typeCode) - 1] = '\0';
  fi.altitudeFt = doc["alt"].isNull() ? -1 : doc["alt"].as<long>();
  fi.distanceKm = doc["dist"].isNull() ? NAN : doc["dist"].as<double>();
  fi.hasCallsign = true;
  if (!doc["op"].isNull()) {
    strncpy(fi.opClass, doc["op"].as<const char*>(), sizeof(fi.opClass) - 1);
    fi.opClass[sizeof(fi.opClass) - 1] = '\0';
  }
  if (!doc["seats"].isNull()) fi.seatOverride = doc["seats"].as<int>();
  if (fi.opClass[0] == '\0') {
    strncpy(fi.opClass, (fi.seatOverride > 0 && fi.seatOverride <= 15) ? "PVT" : "COM", sizeof(fi.opClass) - 1);
    fi.opClass[sizeof(fi.opClass) - 1] = '\0';
  }
  g_test.fi = fi;
  g_test.active = true;
  g_test.expiresAt = millis() + TEST_OVERRIDE_TTL_MS;
  g_test.dirty = true;
  StaticJsonDocument<128> ok;
  ok["status"] = "ok";
  ok["until_ms"] = g_test.expiresAt;
  String out;
  serializeJson(ok, out);
  g_http.send(200, "application/json", out);
}

static void httpHandleDeleteClosest() {
  g_test.active = false;
  g_test.dirty = false;
  g_test.expiresAt = 0;
  g_http.send(200, "application/json", "{\"status\":\"cleared\"}");
}

static void httpStartOnce() {
  if (g_httpStarted) return;
  g_http.on("/", HTTP_GET, httpHandleRoot);
  g_http.on("/healthz", HTTP_GET, httpHandleRoot);
  g_http.on("/test/closest", HTTP_GET, httpHandleGetClosest);
  g_http.on("/test/closest", HTTP_PUT, httpHandlePutClosest);
  g_http.on("/test/closest", HTTP_DELETE, httpHandleDeleteClosest);
  g_http.begin();
  g_httpStarted = true;
  LOG_INFO("HTTP test server listening on :80");
}
#endif
static bool milCacheLookup(const char* hex, bool &isMilOut) {
  if (!hex || !*hex) return false;
  uint32_t now = millis();
  const uint32_t TTL = 6UL * 60UL * 60UL * 1000UL;  // 6 hours
  for (size_t i = 0; i < MIL_CACHE_SIZE; ++i) {
    if (g_milCache[i].valid && strcasecmp(g_milCache[i].hex, hex) == 0) {
      if ((now - g_milCache[i].ts) < TTL) {
        isMilOut = g_milCache[i].isMil;
        return true;
      }
    }
  }
  return false;
}

static void milCacheStore(const char* hex, bool isMil) {
  if (!hex || !*hex) return;
  uint32_t now = millis();
  // Update existing
  for (size_t i = 0; i < MIL_CACHE_SIZE; ++i) {
    if (g_milCache[i].valid && strcasecmp(g_milCache[i].hex, hex) == 0) {
      g_milCache[i].isMil = isMil;
      g_milCache[i].ts = now;
      return;
    }
  }
  // Insert into first empty or oldest
  size_t idx = 0;
  uint32_t oldestAge = 0;
  for (size_t i = 0; i < MIL_CACHE_SIZE; ++i) {
    if (!g_milCache[i].valid) { idx = i; break; }
    uint32_t age = now - g_milCache[i].ts;
    if (age > oldestAge) { oldestAge = age; idx = i; }
  }
  strncpy(g_milCache[idx].hex, hex, sizeof(g_milCache[idx].hex) - 1);
  g_milCache[idx].hex[sizeof(g_milCache[idx].hex) - 1] = '\0';
  g_milCache[idx].isMil = isMil;
  g_milCache[idx].ts = now;
  g_milCache[idx].valid = true;
}

static bool fetchIsMilitaryByHex(const char* hex, bool &isMilOut) {
  isMilOut = false;
  if (WiFi.status() != WL_CONNECTED || !hex || !*hex) return false;
  String url = String(API_BASE);
  if (!url.startsWith("http")) url = String("https://") + url;
  url += "/v2/mil";
  LOG_INFO("HTTP GET %s", url.c_str());

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);

  WiFiClientSecure client;
  client.setInsecure();
  if (!http.begin(client, url)) {
    Serial.println("[HTTP] begin() failed (mil)");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Connection", "close");
  http.addHeader("User-Agent", "ESP32-FlightDisplay/1.0");

  int code = http.GET();
  LOG_INFO("HTTP status (mil): %d", code);
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  // Build needle: "hex":"abcdef"
  char needle[32];
  {
    char lowerHex[8];
    strncpy(lowerHex, hex, sizeof(lowerHex) - 1);
    lowerHex[sizeof(lowerHex) - 1] = '\0';
    for (char* p = lowerHex; *p; ++p) *p = tolower(*p);
    snprintf(needle, sizeof(needle), "\"hex\":\"%s\"", lowerHex);
  }
  size_t needleLen = strlen(needle);

  char carry[32] = "";
  const size_t CHUNK = 1024;
  char buf[CHUNK + 1];
  unsigned long deadline = millis() + HTTP_READ_TIMEOUT_MS;
  Stream &s = http.getStream();
  while (millis() < deadline) {
    int n = s.readBytes(buf, CHUNK);
    if (n <= 0) {
      delay(10);
      yield();
      if (!s.available()) break;
      else continue;
    }
    deadline = millis() + HTTP_READ_TIMEOUT_MS;
    buf[n] = '\0';
    // Combine carry + current chunk for searching
    // Simple approach: search in buf (lowered), check carry overlap
    for (int j = 0; j < n; ++j) buf[j] = tolower(buf[j]);
    if (strstr(buf, needle)) {
      isMilOut = true;
      http.end();
      return true;
    }
    // Keep tail overlap for cross-chunk matches
    if ((size_t)n >= needleLen) {
      memcpy(carry, buf + n - needleLen, needleLen);
      carry[needleLen] = '\0';
    }
  }
  http.end();
  return true;  // completed scan, not found => not mil
}

#if FEATURE_OTA
static void otaBeginOnce() {
  if (g_otaReady) return;
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
    LOG_INFO("OTA auth enabled");
  } else {
    LOG_WARN("OTA password empty; allow unauthenticated updates (DEV ONLY)");
  }

  ArduinoOTA.onStart([]() {
    g_inOta = true;
    LOG_INFO("OTA start");
    // Visual indicator + put relays in safe state
    showSplash("Updating...", "Do not power off");
    relaysPowerOnly();
  });
  ArduinoOTA.onEnd([]() {
    LOG_INFO("OTA end");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint32_t lastDraw = 0;
    uint32_t now = millis();
    if (now - lastDraw < 150) return;  // rate-limit drawing
    lastDraw = now;
    uint8_t pct = (total ? (progress * 100U / total) : 0);
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x12_tf);
    char line[48];
    snprintf(line, sizeof(line), "OTA %u%%", (unsigned)pct);
    drawCentered(String(line), (SCREEN_HEIGHT / 2));
    u8g2.sendBuffer();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    LOG_ERROR("OTA error: %d", (int)error);
    g_inOta = false;  // allow app to resume
  });
  ArduinoOTA.begin();
  g_otaReady = true;
  LOG_INFO("OTA ready: %s.local:%d", OTA_HOSTNAME, OTA_PORT);
}
#endif

// (moved earlier)

// Shared display state so network and test endpoints can update consistently
static bool g_haveDisplayed = false;
static FlightInfo g_lastShown;

static const char* classifyOp(const FlightInfo &fi) {
  // 1) dbFlags bit 0 = military (from API, avoids blocking /v2/mil call)
  if (fi.dbFlags >= 0 && (fi.dbFlags & 1)) return "MIL";

  // 2) MIL cache / live lookup as fallback
  if (FEATURE_MIL_LOOKUP && fi.hex[0]) {
    bool isMil = false;
    if (milCacheLookup(fi.hex, isMil)) {
      if (isMil) return "MIL";
    } else {
      bool ok = fetchIsMilitaryByHex(fi.hex, isMil);
      if (ok) milCacheStore(fi.hex, isMil);
      if (isMil) return "MIL";
    }
  }

  // 3) Seat-based: small aircraft (<= 15 seats) are PVT
  if (fi.typeCode[0]) {
    uint16_t maxSeats = 0;
    if (aircraftSeatMax(fi.typeCode, maxSeats)) {
      if (maxSeats > 0 && maxSeats <= 15) return "PVT";
    }
  }

  // 4) ADS-B category as supplementary signal when type is unknown
  if (fi.category[0] && !fi.typeCode[0]) {
    // A1=Light, A7=Rotorcraft → likely private
    if (strcmp(fi.category, "A1") == 0 || strcmp(fi.category, "A7") == 0) return "PVT";
    // A3=Large, A4=High vortex, A5=Heavy → likely commercial
    if (strcmp(fi.category, "A3") == 0 || strcmp(fi.category, "A4") == 0 || strcmp(fi.category, "A5") == 0) return "COM";
  }

  // 5) Default: COM if it carries a callsign, else PVT
  return fi.hasCallsign ? "COM" : "PVT";
}

static bool sameFlightDisplay(const FlightInfo &a, const FlightInfo &b) {
  if (!a.valid && !b.valid) return true;
  if (a.valid != b.valid) return false;
  if (strcmp(a.ident, b.ident) != 0) return false;
  if (strcmp(a.typeCode, b.typeCode) != 0) return false;
  if (a.altitudeFt != b.altitudeFt) return false;
  if (strcmp(a.opClass, b.opClass) != 0) return false;
  // Consider distances equal within 0.1 km to avoid flicker
  double da = isnan(a.distanceKm) ? 0 : a.distanceKm;
  double db = isnan(b.distanceKm) ? 0 : b.distanceKm;
  if (fabs(da - db) > 0.1) return false;
  return true;
}

static void drawCentered(const String &text, int16_t baselineY) {
  uint16_t w = u8g2.getUTF8Width(text.c_str());
  int16_t x = (SCREEN_WIDTH - (int)w) / 2;
  if (x < 0) x = 0;
  u8g2.drawUTF8(x, baselineY, text.c_str());
}

static void showSplash(const char *msgTop, const char *msgBottom) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  // Fonts
  const uint8_t *titleFont = u8g2_font_10x20_tf;
  const uint8_t *bodyFont = u8g2_font_6x12_tf;

  // Measure line heights
  u8g2.setFont(titleFont);
  int16_t titleAscent = u8g2.getAscent();
  int16_t titleDescent = -u8g2.getDescent();
  int16_t titleH = titleAscent + titleDescent;

  u8g2.setFont(bodyFont);
  int16_t bodyAscent = u8g2.getAscent();
  int16_t bodyDescent = -u8g2.getDescent();
  int16_t bodyH = bodyAscent + bodyDescent;

  const int16_t gap = 6;  // vertical spacing between lines
  int16_t totalH = titleH + gap + bodyH + ((msgBottom) ? (gap + bodyH) : 0);
  if (totalH < 0) totalH = 0;
  int16_t y0 = (SCREEN_HEIGHT - totalH) / 2;

  // Draw title centered
  u8g2.setFont(titleFont);
  int16_t base = y0 + titleAscent;
  drawCentered("Flight Display", base);

  // Draw first message
  u8g2.setFont(bodyFont);
  base = y0 + titleH + gap + bodyAscent;
  drawCentered(String(msgTop), base);

  // Optional second message
  if (msgBottom) {
    base = y0 + titleH + gap + bodyH + gap + bodyAscent;
    drawCentered(String(msgBottom), base);
  }

  u8g2.sendBuffer();
}

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  // Configure once in setup; do not change config while connecting
  if (!wifiInitialized) return;
  // Begin only once; rely on auto-reconnect afterwards
  if (wifiEverBegun) return;
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  showSplash("Connecting Wi-Fi...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiConnecting = true;
  wifiEverBegun = true;
}

static double deg2rad(double deg) {
  return deg * PI / 180.0;
}
static double haversineKm(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0;  // km
  double dLat = deg2rad(lat2 - lat1);
  double dLon = deg2rad(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) + cos(deg2rad(lat1)) * cos(deg2rad(lat2)) * sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

// Maximum acceptable age for a position (seconds). Align with tar1090 defaults (~15s),
// but allow up to 45s to be tolerant of intermittent updates on ESP32.
#ifndef POSITION_MAX_AGE_S
#define POSITION_MAX_AGE_S 45
#endif

static bool extractLatLon(JsonObject obj, double &outLat, double &outLon) {
  // Only accept positions that are not too stale
  if (obj.containsKey("seen_pos")) {
    double seenPos = obj["seen_pos"].as<double>();
    if (seenPos > POSITION_MAX_AGE_S) return false;
  }
  if (obj.containsKey("lat") && obj.containsKey("lon")) {
    outLat = obj["lat"].as<double>();
    outLon = obj["lon"].as<double>();
    if (!(outLat == 0.0 && outLon == 0.0)) return true;  // ignore (0,0)
  }
  // Do not use lastPosition fallback to avoid selecting stale tracks
  return false;
}

static FlightInfo parseClosest(JsonVariant root) {
  FlightInfo res;
  if (!root.is<JsonObject>()) return res;
  JsonObject robj = root.as<JsonObject>();
  if (!robj.containsKey("ac") || !robj["ac"].is<JsonArray>()) return res;
  JsonArray ac = robj["ac"].as<JsonArray>();
  if (ac.size() == 0) return res;
  JsonObject obj = ac[0].as<JsonObject>();

  double lat, lon;
  if (!extractLatLon(obj, lat, lon)) return res;

  // Build ident preference chain: flight -> r -> hex
  const char* identSrc = nullptr;
  bool hasCallsign = false;
  if (obj["flight"]) {
    identSrc = obj["flight"].as<const char*>();
    hasCallsign = (identSrc && *identSrc);
  }
  if (!identSrc || !*identSrc) {
    if (obj["r"]) identSrc = obj["r"].as<const char*>();
  }
  if (!identSrc || !*identSrc) {
    if (obj["hex"]) identSrc = obj["hex"].as<const char*>();
  }
  if (!identSrc || !*identSrc) identSrc = "(unknown)";

  // Copy and trim ident
  strncpy(res.ident, identSrc, sizeof(res.ident) - 1);
  res.ident[sizeof(res.ident) - 1] = '\0';
  // Trim trailing whitespace
  for (int i = strlen(res.ident) - 1; i >= 0 && res.ident[i] == ' '; --i) res.ident[i] = '\0';

  res.altitudeFt = obj["alt_baro"].isNull() ? -1 : obj["alt_baro"].as<long>();

  // "t" = aircraft type designator (B738, A320, etc.)
  // Note: "type" is a different field (message type: adsb_icao, tisb, mlat) — do NOT use as fallback
  const char* typeSrc = obj["t"].isNull() ? nullptr : obj["t"].as<const char*>();
  if (typeSrc) { strncpy(res.typeCode, typeSrc, sizeof(res.typeCode) - 1); res.typeCode[sizeof(res.typeCode) - 1] = '\0'; }

  const char* catSrc = obj["category"].isNull() ? nullptr : obj["category"].as<const char*>();
  if (catSrc) { strncpy(res.category, catSrc, sizeof(res.category) - 1); res.category[sizeof(res.category) - 1] = '\0'; }

  const char* hexSrc = obj["hex"].isNull() ? nullptr : obj["hex"].as<const char*>();
  if (hexSrc) { strncpy(res.hex, hexSrc, sizeof(res.hex) - 1); res.hex[sizeof(res.hex) - 1] = '\0'; }

  // dbFlags: bit 0 = military
  res.dbFlags = obj["dbFlags"].isNull() ? -1 : obj["dbFlags"].as<int>();

  res.valid = true;
  res.lat = lat;
  res.lon = lon;
  res.distanceKm = haversineKm(HOME_LAT, HOME_LON, lat, lon);
  res.hasCallsign = hasCallsign;
  return res;
}

static bool fetchNearestFlight(FlightInfo &out) {
  if (WiFi.status() != WL_CONNECTED) return false;

  auto buildUrl = [](bool tls) {
    String base = String(API_BASE);
    if (tls) {
      if (base.startsWith("http://")) base.replace("http://", "https://");
      if (!base.startsWith("http")) base = String("https://") + base;
    } else {
      if (base.startsWith("https://")) base.replace("https://", "http://");
      if (!base.startsWith("http")) base = String("http://") + base;
    }
    base += "/v2/closest/";
    base += String(HOME_LAT, 6);
    base += "/";
    base += String(HOME_LON, 6);
    base += "/";
    base += String(SEARCH_RADIUS_KM);
    return base;
  };

  // Single HTTPS attempt; server enforces HTTPS and may close HTTP
  String url = buildUrl(true);
  LOG_INFO("HTTP GET %s", url.c_str());
  LOG_DEBUG("WiFi RSSI: %d dBm", WiFi.RSSI());
  LOG_DEBUG("Free heap: %u", (unsigned)ESP.getFreeHeap());

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  WiFiClientSecure client;
  client.setInsecure();
  uint32_t clientTimeoutSec = (HTTP_READ_TIMEOUT_MS + 999) / 1000;
  client.setTimeout(clientTimeoutSec);
  if (!http.begin(client, url)) {
    Serial.println("[HTTP] begin() failed (TLS)");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Connection", "close");
  http.addHeader("User-Agent", "ESP32-FlightDisplay/1.0");

  int code = http.GET();
  LOG_INFO("HTTP status: %d", code);
  if (code != HTTP_CODE_OK) {
    LOG_WARN("HTTP error: %s", http.errorToString(code).c_str());
    http.end();
    return false;
  }

  size_t contentLength = http.getSize();
  LOG_DEBUG("HTTP Content-Length: %u", (unsigned)contentLength);

  // Build a filter to only keep what we need
  StaticJsonDocument<256> filter;
  JsonArray acArr = filter.createNestedArray("ac");
  JsonObject acObj = acArr.createNestedObject();
  acObj["flight"] = true;
  acObj["r"] = true;
  acObj["hex"] = true;
  acObj["t"] = true;
  acObj["alt_baro"] = true;
  acObj["lat"] = true;
  acObj["lon"] = true;
  acObj["seen_pos"] = true;
  acObj["category"] = true;
  acObj["dbFlags"] = true;  // bit 0 = military

  StaticJsonDocument<2048> doc;  // stack-allocated; filtered single-aircraft response is well under 1KB
  // Prefer streamed parsing to minimize RAM and fragmentation
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    LOG_WARN("JSON parse error (streamed): %s", err.c_str());
    return false;
  }

  FlightInfo closest = parseClosest(doc.as<JsonVariant>());
  if (closest.valid) {
    LOG_INFO("Closest %s  dist %.2f km", closest.ident, closest.distanceKm);
    // Classify operation (MIL/COM/PVT)
    const char* op = classifyOp(closest);
    strncpy(closest.opClass, op, sizeof(closest.opClass) - 1);
    closest.opClass[sizeof(closest.opClass) - 1] = '\0';
    LOG_INFO("Classified op: %s", closest.opClass);
    out = closest;
    return true;
  }
  LOG_INFO("No valid aircraft found in response");
  return false;
}

// test server removed

static void renderFlight(const FlightInfo &fi) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  // 1) Top line: friendly aircraft name, allow pseudo/unknown fallbacks
  char friendlyBuf[64];
  bool isPseudo = false;
  bool hasFriendly = false;
  if (fi.typeCode[0]) {
    hasFriendly = aircraftFriendlyNameBuf(fi.typeCode, friendlyBuf, sizeof(friendlyBuf));
  }
  if (!hasFriendly && fi.typeCode[0]) {
    // Detect pseudo/non-aircraft types
    if (strncasecmp(fi.typeCode, "TISB", 4) == 0) {
      strncpy(friendlyBuf, "TIS-B Target", sizeof(friendlyBuf));
      isPseudo = true; hasFriendly = true;
    } else if (strncasecmp(fi.typeCode, "ADSB", 4) == 0) {
      strncpy(friendlyBuf, "ADS-B Target", sizeof(friendlyBuf));
      isPseudo = true; hasFriendly = true;
    } else if (strncasecmp(fi.typeCode, "MLAT", 4) == 0) {
      strncpy(friendlyBuf, "MLAT Target", sizeof(friendlyBuf));
      isPseudo = true; hasFriendly = true;
    } else if (strncasecmp(fi.typeCode, "MODE", 4) == 0) {
      strncpy(friendlyBuf, "Mode-S Target", sizeof(friendlyBuf));
      isPseudo = true; hasFriendly = true;
    }
  }
  if (!hasFriendly) strncpy(friendlyBuf, "Unknown Aircraft", sizeof(friendlyBuf));
  String friendly = String(friendlyBuf);

  // Bottom metrics font (~50% larger than before)
  const uint8_t *bottomFont = u8g2_font_9x18_tf;  // was 6x12
  u8g2.setFont(bottomFont);
  int16_t bottomAscent = u8g2.getAscent();
  int16_t bottomDescent = -u8g2.getDescent();
  int16_t bottomH = bottomAscent + bottomDescent;
  const int16_t gapTopBottom = 2;  // minimal gap above bottom line
  const int16_t topAvail = SCREEN_HEIGHT - bottomH - gapTopBottom;

  // Title fonts from largest to smaller; allow wrapping to up to 2 lines
  const uint8_t *titleFonts[] = {
    u8g2_font_logisoso32_tf,
    u8g2_font_logisoso24_tf,
    u8g2_font_logisoso20_tf,
    u8g2_font_10x20_tf,
    u8g2_font_9x15_tf,
    u8g2_font_6x12_tf
  };
  const size_t NFONTS = sizeof(titleFonts) / sizeof(titleFonts[0]);

  String line1 = friendly;
  String line2 = String("");
  const uint8_t *chosen = titleFonts[NFONTS - 1];
  auto tryWrapTwoLines = [&](const String &s, String &o1, String &o2) -> bool {
    // Attempt to split at spaces; choose split minimizing max line width and fitting within width
    o1 = s;
    o2 = String("");
    int bestIdx = -1;
    int bestWorst = INT_MAX;
    for (int i = 1; i < (int)s.length() - 1; ++i) {
      if (s[i] != ' ') continue;
      String a = s.substring(0, i);
      String b = s.substring(i + 1);
      uint16_t wa = u8g2.getUTF8Width(a.c_str());
      uint16_t wb = u8g2.getUTF8Width(b.c_str());
      if (wa <= SCREEN_WIDTH && wb <= SCREEN_WIDTH) {
        int worst = max((int)wa, (int)wb);
        if (worst < bestWorst) {
          bestWorst = worst;
          bestIdx = i;
          o1 = a;
          o2 = b;
        }
      }
    }
    return bestIdx >= 0;
  };

  for (size_t i = 0; i < NFONTS; ++i) {
    u8g2.setFont(titleFonts[i]);
    int16_t asc = u8g2.getAscent();
    int16_t desc = -u8g2.getDescent();
    int16_t lh = asc + desc;
    // One-line fit within width and height
    if (u8g2.getUTF8Width(friendly.c_str()) <= SCREEN_WIDTH && lh <= topAvail) {
      chosen = titleFonts[i];
      line1 = friendly;
      line2 = String("");
      break;
    }
    // Try two-line wrap for this font if two lines fit height
    if (2 * lh + 2 <= topAvail) {
      String a, b;
      if (tryWrapTwoLines(friendly, a, b)) {
        chosen = titleFonts[i];
        line1 = a;
        line2 = b;
        break;
      }
    }
  }

  // Draw title (one or two lines), vertically centered within topAvail
  u8g2.setFont(chosen);
  int16_t ascT = u8g2.getAscent();
  int16_t descT = -u8g2.getDescent();
  int16_t lhT = ascT + descT;
  int16_t totalTitleH = lhT + ((line2.length() > 0) ? (2 + lhT) : 0);
  if (totalTitleH > topAvail) totalTitleH = topAvail;  // safety
  int16_t yStart = (topAvail - totalTitleH) / 2 + ascT;
  drawCentered(line1, yStart);
  if (line2.length() > 0) {
    drawCentered(line2, yStart + lhT + 2);
  }

  // 2) Bottom line: distance, seats, altitude (small font), evenly spaced across bottom
  u8g2.setFont(bottomFont);
  int16_t descent = u8g2.getDescent();
  int16_t yBottom = SCREEN_HEIGHT - 1 - (descent < 0 ? -descent : descent);

  char distStr[16] = "\xE2\x80\x94";  // em-dash
  if (!isnan(fi.distanceKm)) snprintf(distStr, sizeof(distStr), "%.1fkm", fi.distanceKm);

  char seatsStr[8] = "\xE2\x80\x94";  // em-dash
  if (!isPseudo) {
    uint16_t maxSeats = 0;
    if (fi.seatOverride > 0) snprintf(seatsStr, sizeof(seatsStr), "%d", fi.seatOverride);
    else if (fi.typeCode[0] && aircraftSeatMax(fi.typeCode, maxSeats) && maxSeats > 0) snprintf(seatsStr, sizeof(seatsStr), "%u", (unsigned)maxSeats);
  }

  char altStr[16] = "\xE2\x80\x94";  // em-dash
  if (fi.altitudeFt >= 0) snprintf(altStr, sizeof(altStr), "%ldft", fi.altitudeFt);

  const int cells = 3;
  const int cellW = SCREEN_WIDTH / cells;
  auto drawCenteredInCell = [&](int idx, const char* text) {
    uint16_t bw = u8g2.getUTF8Width(text);
    int16_t cx = idx * cellW + (cellW - (int)bw) / 2;
    if (cx < 0) cx = 0;
    u8g2.drawUTF8(cx, yBottom, text);
  };

  drawCenteredInCell(0, distStr);
  drawCenteredInCell(1, seatsStr);
  drawCenteredInCell(2, altStr);

  u8g2.sendBuffer();
}

void setup() {
  // Simple, explicit relay init and boot state
  relaysInit();

  Serial.begin(115200);
  delay(20);
  Serial.println(F("\n[Boot] Flight Display starting..."));
#if defined(ESP32)
  auto rr = esp_reset_reason();
  Serial.print(F("[Boot] Reset reason: "));
  switch (rr) {
    case ESP_RST_POWERON: Serial.println(F("POWERON")); break;
    case ESP_RST_EXT: Serial.println(F("EXT")); break;
    case ESP_RST_SW: Serial.println(F("SW")); break;
    case ESP_RST_PANIC: Serial.println(F("PANIC")); break;
    case ESP_RST_INT_WDT: Serial.println(F("INT_WDT")); break;
    case ESP_RST_TASK_WDT: Serial.println(F("TASK_WDT")); break;
    case ESP_RST_WDT: Serial.println(F("WDT")); break;
    case ESP_RST_DEEPSLEEP: Serial.println(F("DEEPSLEEP")); break;
    case ESP_RST_BROWNOUT: Serial.println(F("BROWNOUT")); break;
    case ESP_RST_SDIO: Serial.println(F("SDIO")); break;
    default: Serial.println((int)rr); break;
  }
#endif
  // Wi‑Fi event logging and dynamic power management
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        LOG_WARN("WiFi disconnected. Reason: %d", info.wifi_sta_disconnected.reason);
        wifiConnecting = false;
        // Drop TX power while attempting to reconnect to reduce current spikes
        WiFi.setTxPower(WIFI_BOOT_TXPOWER);
        WiFi.setSleep(true);
#if FEATURE_OTA
        g_otaReady = false;
        g_inOta = false;
#endif
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.print(F("[WiFi] Got IP: "));
        Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));
        wifiConnecting = false;
        // Raise TX power after association; disable modem sleep for responsiveness
        WiFi.setTxPower(WIFI_RUN_TXPOWER);
        WiFi.setSleep(false);
        // Wi‑Fi up; print reminder of server endpoint
        Serial.println(F("[HTTP] Server ready on port 80"));
#if FEATURE_OTA
        otaBeginOnce();
#endif
#if FEATURE_TEST_ENDPOINT
        httpStartOnce();
#endif
        break;
      default: break;
    }
  });
  // Configure Wi‑Fi once
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  // Start with low TX power and enable sleep for gentle connect
  WiFi.setTxPower(WIFI_BOOT_TXPOWER);
  WiFi.setSleep(true);
  wifiInitialized = true;
  // Display init (U8g2 handles SPI HW init for HW SPI constructor)
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  showSplash("Booting...");
  // Allow power rails to settle before enabling Wi‑Fi/TLS
  delay(BOOT_POWER_SETTLE_MS);
  connectWiFi();

#if FEATURE_TEST_ENDPOINT
  // Start HTTP listener early; it will become reachable after Wi‑Fi is up
  httpStartOnce();
#endif
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

#if FEATURE_TEST_ENDPOINT
  if (g_httpStarted) {
    g_http.handleClient();
  }
#endif

#if FEATURE_OTA
  if (g_otaReady) {
    ArduinoOTA.handle();
  }
  if (g_inOta) {
    // Skip all other work during OTA to ensure smooth flashing
    yield();
    return;
  }
#endif

  // If test override is active, render it preferentially
#if FEATURE_TEST_ENDPOINT
  if (g_test.active && (int32_t)(millis() - g_test.expiresAt) < 0) {
    if (g_test.dirty || !g_haveDisplayed || !sameFlightDisplay(g_test.fi, g_lastShown)) {
      renderFlight(g_test.fi);
      g_lastShown = g_test.fi;
      g_haveDisplayed = true;
      relaysShowCategory(g_lastShown.opClass);
      g_test.dirty = false;
    }
    // Skip network fetch while override active
    yield();
    return;
  }
#endif

  uint32_t now = millis();
  if (now >= g_nextFetchAt || g_nextFetchAt == 0) {
    FlightInfo nearest;
    if (fetchNearestFlight(nearest)) {
      g_fetchFailCount = 0;
      g_nextFetchAt = millis() + FETCH_INTERVAL_MS;
      if (!g_haveDisplayed || !sameFlightDisplay(nearest, g_lastShown)) {
        renderFlight(nearest);
        g_lastShown = nearest;
        g_haveDisplayed = true;
        relaysShowCategory(g_lastShown.opClass);
      }
    } else {
      g_fetchFailCount = min((uint8_t)(g_fetchFailCount + 1), (uint8_t)8);
      g_nextFetchAt = millis() + backoffMs(g_fetchFailCount);
      LOG_WARN("Fetch failed (attempt %d), next retry in %lu ms", g_fetchFailCount, backoffMs(g_fetchFailCount));
      if (!g_haveDisplayed) {
        showSplash("No data", "Check Wi-Fi/API");
      }
    }
  }

  // Keep relays consistent during boot
  if (!g_haveDisplayed) {
    relaysPowerOnly();
  }

  // Cooperative yield without blocking
  yield();
}

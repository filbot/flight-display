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
#include "esp_task_wdt.h"
#if defined(ESP32)
#include <esp_system.h>
#endif

#include "config.h"  // Create from config.example.h and do not commit secrets
#include "aircraft_types.h"
#include "local_rx.h"
#include "log.h"

// Sentinel for alt_baro == "ground" (aircraft on the surface)
static constexpr int32_t ALT_GROUND = -2;
static constexpr int32_t ALT_UNKNOWN = -1;  // field absent; a PRESENT -1 is treated as ground

// Distinguishes "API answered, nothing displayable nearby" from real failures
// so quiet skies don't trigger backoff or the restart circuit breaker.
enum FetchResult : uint8_t { FETCH_OK, FETCH_EMPTY, FETCH_FAIL };

// FlightInfo is used across parsing, test overrides, and rendering.
// Uses fixed char arrays to avoid heap allocation in hot paths.
// Ahead of the first function definition in the file: the Arduino build injects
// generated prototypes before that point, and they reference this type.
enum NetJob : uint8_t { NET_NONE = 0, NET_API, NET_LOCAL };

struct FlightInfo {
  char ident[16];      // flight/callsign or registration/hex fallback
  char typeCode[8];    // aircraft type (t)
  char category[4];    // raw ADS-B category code (A1-A7, B1-B7, C1-C3)
  char desc[48];       // API type description; title fallback when table misses
  int32_t altitudeFt;  // ALT_UNKNOWN = absent, ALT_GROUND = on the surface
  double lat;
  double lon;
  double distanceKm;
  char hex[8];         // transponder hex id
  bool hasCallsign;
  char opClass[4];     // MIL/COM/PVT
  bool valid;
  int16_t seatOverride;    // if >0, override seat display
  int16_t dbFlags;         // adsb.lol dbFlags field; bit 0 = military
  char squawk[8];          // transponder code; 7500/7600/7700 are emergencies
  char emergency[12];      // ADS-B emergency/priority status; "none" when normal

  FlightInfo() : altitudeFt(ALT_UNKNOWN), lat(NAN), lon(NAN), distanceKm(NAN),
                 hasCallsign(false), valid(false), seatOverride(-1), dbFlags(-1) {
    ident[0] = '\0';
    typeCode[0] = '\0';
    category[0] = '\0';
    desc[0] = '\0';
    hex[0] = '\0';
    opClass[0] = '\0';
    squawk[0] = '\0';
    emergency[0] = '\0';
  }
};

// Alert conditions, most severe first. Two independent sources feed the same
// ladder: the three reserved transponder codes, and the ADS-B emergency/priority
// status field. They overlap for hijack/radio/general, but lifeguard, minimum
// fuel and downed exist ONLY in the status field — a squawk-only check is blind
// to them.
struct AlertLevel {
  const char* status;  // adsb.lol "emergency" value
  const char* squawk;  // equivalent transponder code, or nullptr if none exists
  const char* label;   // shown in the type-name area
  uint8_t priority;    // higher wins when several aircraft are alerting
};
static const AlertLevel kAlerts[] = {
  { "unlawful",  "7500", "HIJACK 7500",     6 },
  { "downed",    nullptr, "DOWNED AIRCRAFT", 5 },
  { "general",   "7700", "EMERGENCY 7700",  4 },
  { "minfuel",   nullptr, "MIN FUEL",        3 },
  { "nordo",     "7600", "RADIO FAIL 7600", 2 },
  { "lifeguard", nullptr, "LIFEGUARD",       1 },
};

// Only conditions at or above this priority take over the display from the
// nearest aircraft. Default 1 shows everything, lifeguard included; raise it if
// medical flights prove too frequent to be interesting.
// Invert the whole panel while an alert is showing, so it is unmissable from
// across the room. Costs OLED lifetime while lit — acceptable because alerts
// are rare and brief, but worth knowing if one ever loiters.
#ifndef ALERT_INVERT_DISPLAY
#define ALERT_INVERT_DISPLAY 1
#endif

#ifndef ALERT_PREEMPT_MIN_PRIORITY
#define ALERT_PREEMPT_MIN_PRIORITY 1
#endif

// Resolve an aircraft's alert from either source. Returns nullptr when normal.
static const AlertLevel* alertFor(const char* squawk, const char* status) {
  const AlertLevel* best = nullptr;
  for (const AlertLevel &a : kAlerts) {
    bool hit = (status && *status && strcasecmp(status, a.status) == 0)
            || (a.squawk && squawk && strcmp(squawk, a.squawk) == 0);
    if (hit && (!best || a.priority > best->priority)) best = &a;
  }
  return best;
}

static const char* emergencyLabel(const char* squawk, const char* status = nullptr) {
  const AlertLevel* a = alertFor(squawk, status);
  return a ? a->label : nullptr;
}
// Local function prototypes used before definitions
static void relaysPowerOnly();
static void showSplash(const char *msgTop, const char *msgBottom = nullptr);
static void setDisplayDim(bool dim);
static uint8_t pixelShiftIdx();
static bool resolveFriendlyName(const FlightInfo &fi, char* buf, size_t bufSize);
static void drawCentered(const char *text, int16_t baselineY, int8_t dx = 0, int8_t dy = 0);

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


// SPI OLED (SSD1322) via U8g2, full framebuffer, HW SPI (rotated 180°)
U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI u8g2(U8G2_R2, PIN_CS, PIN_DC, PIN_RST);

static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;  // 20s
static constexpr uint32_t FETCH_INTERVAL_MS = 30000;        // 30s between API calls
// Sized from 428 measured live fetches: p50 957ms, p95 1270ms. The old 30s read
// timeout was 23x p95, so a single stalled fetch froze loop() for half a minute
// with OTA and the HTTP server unreachable. 8s keeps ~6x headroom over p95 while
// bounding a stall to well under the watchdog period.
static constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 8000;  // HTTP connect timeout
static constexpr uint32_t HTTP_READ_TIMEOUT_MS = 8000;     // HTTP read timeout

// Heap monitoring thresholds
#ifndef HEAP_WARN_THRESHOLD
#define HEAP_WARN_THRESHOLD 30000
#endif
#ifndef HEAP_CRITICAL_THRESHOLD
#define HEAP_CRITICAL_THRESHOLD 15000
#endif

// Circuit breaker: restart after sustained API failures
#ifndef FETCH_FAIL_RESTART_THRESHOLD
#define FETCH_FAIL_RESTART_THRESHOLD 60  // ~30 min at max backoff
#endif

// Max time a flight stays on screen without a successful refresh before the
// display falls back to the "No data" splash instead of showing stale data
#ifndef STALE_DISPLAY_MAX_MS
#define STALE_DISPLAY_MAX_MS (5UL * 60UL * 1000UL)  // 5 minutes
#endif

// Bottom-bar pixel shift interval (OLED image-sticking mitigation)
#ifndef PIXEL_SHIFT_INTERVAL_MS
#define PIXEL_SHIFT_INTERVAL_MS (3UL * 60UL * 1000UL)  // 3 minutes per step
#endif

// Task watchdog period. Must exceed the longest span loop() can go without a
// feed: a closest fetch is connect(8s) + read(8s) worst case. The MIL scan is
// longer but feeds from inside its own loop, so it does not set this floor.
#ifndef LOOP_WDT_TIMEOUT_S
#define LOOP_WDT_TIMEOUT_S 25
#endif

// Reset reason as text — printed at boot and served by /healthz, so a soak run
// can tell a clean power cycle from a panic or watchdog reboot after the fact.
#if defined(ESP32)
static const char *resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}
#endif

// API base is runtime-switchable so the fault-injection harness can point the
// device at a local mock without reflashing between scenarios. Defaults to the
// compiled API_BASE and is only mutable through the test endpoint, which is
// itself behind FEATURE_TEST_ENDPOINT.
static char g_apiBase[96] = API_BASE;

// adsb.lol returns 403 "User-Agent too generic; include valid contact info."
// for anonymous-looking clients. Real contact string lives in config.h (gitignored).
#ifndef API_USER_AGENT
#define API_USER_AGENT "flight-display/1.0 (ESP32 ADS-B display; set API_CONTACT in config.h)"
#endif

// Display brightness (SSD1322 contrast register, 0-255). The register sets the
// segment drive current, so lowering it cuts the OLED's rate of luminance decay
// roughly in proportion — this panel runs 24/7, so it is held at 60% of full
// scale to buy lifetime rather than driven flat out.
// Perceived brightness falls less than the number suggests (the response is
// non-linear), so raise it here if the panel reads too dim in daylight.
#ifndef DISPLAY_CONTRAST
#define DISPLAY_CONTRAST 153  // 60% of 255
#endif
#ifndef DISPLAY_CONTRAST_DIM
#define DISPLAY_CONTRAST_DIM (DISPLAY_CONTRAST / 4)  // 75% below normal (38 at the default)
#endif

// ponytail: two levels, no fade ramp — add easing only if the step is jarring
static bool g_displayDim = false;
static void setDisplayDim(bool dim) {
  static int8_t s_dim = -1;  // -1 = never applied, force the first write
  if (s_dim == (int8_t)dim) return;
  s_dim = dim;
  g_displayDim = dim;
  u8g2.setContrast(dim ? DISPLAY_CONTRAST_DIM : DISPLAY_CONTRAST);
  LOG_INFO("Display contrast -> %d", dim ? DISPLAY_CONTRAST_DIM : DISPLAY_CONTRAST);
}

// Wider search radius tried only when the primary radius is empty, so the
// display always shows the nearest aircraft when anything is in range.
// Costs one extra API call per cycle only while the primary circle is quiet.
#ifndef SEARCH_RADIUS_FALLBACK_KM
#define SEARCH_RADIUS_FALLBACK_KM 100
#endif

// Exponential backoff with jitter for retry logic
static uint32_t backoffMs(uint8_t attempt, uint32_t base = 2000, uint32_t cap = 60000) {
  uint32_t exp = base << min((uint8_t)attempt, (uint8_t)5);
  uint32_t jitter = (exp >> 3) * (esp_random() & 0x7) / 7;  // ~±12.5% jitter
  return min(cap, exp + jitter);
}
// Lifetime counters for the health endpoint — cheap, and the only way to see
// slow drift (failure ratio, empty-sky ratio) over a multi-day soak.
static uint32_t g_statOk = 0, g_statEmpty = 0, g_statFail = 0;
static int g_statLastHttp = 0;
static char g_statErrBuf[96] = "";
static uint8_t g_fetchFailCount = 0;
static uint32_t g_nextFetchAt = 0;
static uint32_t g_lastDataMs = 0;  // millis() of last successful fetch with data

static void checkHeap() {
#if defined(ESP32)
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
    LOG_ERROR("CRITICAL: Free heap %u < %u, restarting", freeHeap, (uint32_t)HEAP_CRITICAL_THRESHOLD);
    ESP.restart();
  }
  if (freeHeap < HEAP_WARN_THRESHOLD) {
    LOG_WARN("Low heap: %u bytes free", freeHeap);
  }
#endif
}
// Wi‑Fi reconnect state
static bool wifiConnecting = false;
static bool wifiInitialized = false;
static bool wifiEverBegun = false;
static uint32_t wifiLastAttemptAt = 0;
static uint32_t g_wifiDropUntil = 0;  // non-zero while a test drop is being held
// Upper bound on a deliberate Wi-Fi drop. The device is unreachable while held
// down, so the hold MUST self-expire — there is no way to call it back.
#ifndef WIFI_DROP_MAX_MS
#define WIFI_DROP_MAX_MS (10UL * 60UL * 1000UL)
#endif

#ifndef WIFI_RETRY_INTERVAL_MS
#define WIFI_RETRY_INTERVAL_MS 60000  // retry WiFi.begin() every 60s if disconnected
#endif

#if FEATURE_OTA
static bool g_otaReady = false;        // true after ArduinoOTA.begin while Wi‑Fi has IP
static volatile bool g_inOta = false;  // set during OTA to pause app work
#endif

// Flight info structure used across network parsing, test overrides, and rendering
// (struct FlightInfo defined earlier)

// -----------------------------
// Test HTTP server (/test/closest)
// -----------------------------
// Local ADS-B receiver (PiAware / dump1090). Optional, per-installation: set
// FEATURE_LOCAL_RX and LOCAL_RX_HOST in config.h. Use an IP ADDRESS, not a
// hostname — a failed DNS lookup costs a fixed 15s of blocked loop() (see
// finding #14), and there is no reason to risk that for a LAN device.
#ifndef FEATURE_LOCAL_RX
#define FEATURE_LOCAL_RX 0
#endif
#ifndef LOCAL_RX_HOST
#define LOCAL_RX_HOST "192.168.1.243"
#endif
#ifndef LOCAL_RX_PORT
#define LOCAL_RX_PORT 8080
#endif
#ifndef LOCAL_RX_PATH
#define LOCAL_RX_PATH "/aircraft"
#endif
// How often to refresh the live numbers. The API poll is 30s; this is what
// closes the gap, so it wants to be small — the fetch costs ~30ms on the LAN.
#ifndef LOCAL_RX_INTERVAL_MS
#define LOCAL_RX_INTERVAL_MS 4000
#endif
// After a failure, stop hammering a receiver that is switched off.
#ifndef LOCAL_RX_BACKOFF_MS
#define LOCAL_RX_BACKOFF_MS 60000
#endif
// A local record older than this is not "live" and is ignored.
// Backstop only. The overnight 70% miss rate that motivated this gate was
// largely a bug in the record extractor, not lack of coverage — with that fixed
// the receiver hits 100% on aircraft it can hear. So keep the gate generous and
// let the miss counter below do the fine-grained work; an aircraft at 50 km+ is
// genuinely unlikely to be heard and is not worth three probes to confirm.
#ifndef LOCAL_RX_MAX_DIST_KM
#define LOCAL_RX_MAX_DIST_KM 50
#endif
// Give up on an aircraft the receiver plainly cannot hear.
#ifndef LOCAL_RX_MISS_GIVEUP
#define LOCAL_RX_MISS_GIVEUP 3
#endif

// A local record older than this is ignored. The right comparison is not "is it
// fresh in the abstract" but "is it fresher than what it would replace": the
// alternative is API data up to FETCH_INTERVAL_MS (30s) old, so rejecting a
// 16s-old local position to keep a possibly 30s-old API one is backwards.
// Measured at 15s, staleness was the single largest cause of missed live
// updates. Aircraft heard with NO position are a separate bucket this cannot
// help — see local_nopos.
#ifndef LOCAL_RX_MAX_AGE_S
#define LOCAL_RX_MAX_AGE_S 30
#endif
#ifndef LOCAL_RX_CONNECT_TIMEOUT_MS
#define LOCAL_RX_CONNECT_TIMEOUT_MS 2000
#endif
#ifndef LOCAL_RX_READ_TIMEOUT_MS
#define LOCAL_RX_READ_TIMEOUT_MS 3000
#endif

#ifndef FEATURE_TEST_ENDPOINT
#define FEATURE_TEST_ENDPOINT 1  // Set 0 to remove test HTTP endpoint
#endif
#ifndef TEST_OVERRIDE_TTL_MS
#define TEST_OVERRIDE_TTL_MS (5UL * 60UL * 1000UL)  // 5 minutes
#endif

// Declared ahead of the HTTP handlers so /healthz can report what's on screen
static SemaphoreHandle_t g_netMtx = nullptr;
static TaskHandle_t g_netTaskHandle = nullptr;
static NetJob g_netPending = NET_NONE;   // requested by loop()
static NetJob g_netDoneKind = NET_NONE;
static bool g_netBusy = false;
static bool g_netDone = false;
static FlightInfo g_netTarget;           // NET_LOCAL: the aircraft to refresh
static FlightInfo g_netResult;
static FetchResult g_netStatus = FETCH_FAIL;
static uint32_t g_netBusySince = 0;
static uint32_t g_statNetJobs = 0;

#define NET_LOCK()   xSemaphoreTake(g_netMtx, portMAX_DELAY)
#define NET_UNLOCK() xSemaphoreGive(g_netMtx)

// Why a fetch came back empty: how many aircraft the API sent, and how many
// we rejected on position or staleness. Logged identically before, which made
// a genuinely clear sky indistinguishable from us discarding everything.
static int g_lastAcReturned = 0, g_lastAcRejected = 0;
// Performance counters. The central question on this device is what blocks
// loop() and for how long: OTA, the HTTP server and the display all live there,
// and the task watchdog fires at 25s. Measured as the interval between
// successive loop() entries, which for a cooperative loop IS the iteration
// duration, and which needs no instrumentation at the many early returns.
static uint32_t g_loopCount = 0, g_loopMaxMs = 0;
static uint32_t g_loopSlow50 = 0, g_loopSlow500 = 0, g_loopSlow5000 = 0;
static uint32_t g_renderCount = 0, g_renderMaxMs = 0;
static uint32_t g_apiMsLast = 0, g_apiMsMax = 0;
static uint32_t g_localMsLast = 0, g_localMsMax = 0;
#if FEATURE_LOCAL_RX
static uint32_t g_nextLocalAt = 0;
static uint32_t g_statLocalOk = 0, g_statLocalMiss = 0, g_statLocalFail = 0;
static uint32_t g_statLocalSkip = 0;
static uint32_t g_statLocalNoPos = 0, g_statLocalStale = 0;
static float g_localSeenPos = -1.0f;
static char g_localMissHex[8] = "";
static uint8_t g_localMissRun = 0;
#endif
static char g_splashTop[40] = "";
static char g_splashBottom[40] = "";
static bool g_splashActive = false;
static uint32_t g_statSplashDraws = 0;
static bool g_haveDisplayed = false;
static FlightInfo g_lastShown;

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

// Machine-readable telemetry for the soak monitor. Everything here is something
// that drifts or breaks over days: heap, reboots, Wi-Fi, and the fetch outcome mix.
static void httpHandleHealth() {
  StaticJsonDocument<640> d;
  d["uptime_ms"] = millis();
  d["reset_reason"] = resetReasonStr();
  d["heap_free"] = ESP.getFreeHeap();
  d["heap_min"] = ESP.getMinFreeHeap();
  d["heap_largest_block"] = ESP.getMaxAllocHeap();
  d["wifi_up"] = (WiFi.status() == WL_CONNECTED);
  d["rssi"] = WiFi.RSSI();
  d["ip"] = WiFi.localIP().toString();
  d["fetch_ok"] = g_statOk;
  d["fetch_empty"] = g_statEmpty;
  d["fetch_fail"] = g_statFail;
  d["fail_streak"] = g_fetchFailCount;
  d["last_http"] = g_statLastHttp;
  d["last_err"] = (const char*)g_statErrBuf;
  d["last_data_age_ms"] = g_lastDataMs ? (millis() - g_lastDataMs) : -1;
  d["next_fetch_in_ms"] = (int32_t)(g_nextFetchAt - millis());
  d["showing_flight"] = g_haveDisplayed;
  d["display_dim"] = g_displayDim;
  d["splash_active"] = g_splashActive;
#if FEATURE_LOCAL_RX
  d["local_ok"] = g_statLocalOk;
  d["local_miss"] = g_statLocalMiss;
  d["local_fail"] = g_statLocalFail;
  d["local_skip"] = g_statLocalSkip;
  d["local_nopos"] = g_statLocalNoPos;
  d["local_stale"] = g_statLocalStale;
  d["net_jobs"] = g_statNetJobs;
  {
    NET_LOCK();
    d["net_busy"] = g_netBusy;
    d["net_busy_ms"] = g_netBusy ? (millis() - g_netBusySince) : 0;
    NET_UNLOCK();
  }
  d["last_ac_returned"] = g_lastAcReturned;
  d["last_ac_rejected"] = g_lastAcRejected;
  d["local_seen_pos"] = g_localSeenPos;
#endif
  d["wifi_drop_active"] = (g_wifiDropUntil != 0);
  d["splash_draws"] = g_statSplashDraws;
  d["loop_count"] = g_loopCount;
  d["loop_max_ms"] = g_loopMaxMs;
  d["loop_gt50ms"] = g_loopSlow50;
  d["loop_gt500ms"] = g_loopSlow500;
  d["loop_gt5s"] = g_loopSlow5000;
  d["render_count"] = g_renderCount;
  d["render_max_ms"] = g_renderMaxMs;
  d["api_ms_last"] = g_apiMsLast;
  d["api_ms_max"] = g_apiMsMax;
  d["local_ms_last"] = g_localMsLast;
  d["local_ms_max"] = g_localMsMax;
  d["shift_idx"] = pixelShiftIdx();
  d["alt_ft"] = g_lastShown.altitudeFt;
  d["dist_km"] = isnan(g_lastShown.distanceKm) ? -1.0 : g_lastShown.distanceKm;
  d["has_callsign"] = g_lastShown.hasCallsign;
  d["db_flags"] = g_lastShown.dbFlags;
  d["category"] = (const char*)g_lastShown.category;
  d["squawk"] = (const char*)g_lastShown.squawk;
  d["emergency_status"] = (const char*)g_lastShown.emergency;
  {
    const char *al = emergencyLabel(g_lastShown.squawk, g_lastShown.emergency);
    d["emergency"] = al ? al : "";
  }
  {
    char title[64];
    resolveFriendlyName(g_lastShown, title, sizeof(title));
    d["title"] = title;
  }
  d["hex"] = (const char*)g_lastShown.hex;
  d["ident"] = (const char*)g_lastShown.ident;
  d["type"] = (const char*)g_lastShown.typeCode;
  d["op"] = (const char*)g_lastShown.opClass;
  d["api_base"] = (const char*)g_apiBase;
  d["test_override"] = g_test.active && (int32_t)(millis() - g_test.expiresAt) < 0;
  String out;
  serializeJson(d, out);
  g_http.send(200, "application/json", out);
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
  fi.altitudeFt = doc["alt"].isNull() ? ALT_UNKNOWN : doc["alt"].as<int32_t>();
  fi.distanceKm = doc["dist"].isNull() ? NAN : doc["dist"].as<double>();
  fi.hasCallsign = true;
  if (!doc["op"].isNull()) {
    strncpy(fi.opClass, doc["op"].as<const char*>(), sizeof(fi.opClass) - 1);
    fi.opClass[sizeof(fi.opClass) - 1] = '\0';
  }
  if (!doc["seats"].isNull()) fi.seatOverride = doc["seats"].as<int>();
  if (!doc["squawk"].isNull()) {
    strncpy(fi.squawk, doc["squawk"].as<const char*>(), sizeof(fi.squawk) - 1);
    fi.squawk[sizeof(fi.squawk) - 1] = '\0';
  }
  if (!doc["emergency"].isNull()) {
    strncpy(fi.emergency, doc["emergency"].as<const char*>(), sizeof(fi.emergency) - 1);
    fi.emergency[sizeof(fi.emergency) - 1] = '\0';
  }
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

// Point the device at a mock API (fault injection) or back at the real one.
// PUT body: {"base":"https://192.168.1.50:8443"}   DELETE: restore compiled default.
static void httpHandlePutApiBase() {
  if (!g_http.hasArg("plain")) { g_http.send(400, "text/plain", "Missing body"); return; }
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, g_http.arg("plain"))) { g_http.send(400, "text/plain", "Bad JSON"); return; }
  const char *base = doc["base"];
  if (!base || !*base || strlen(base) >= sizeof(g_apiBase)) {
    g_http.send(400, "text/plain", "Bad base");
    return;
  }
  strncpy(g_apiBase, base, sizeof(g_apiBase) - 1);
  g_apiBase[sizeof(g_apiBase) - 1] = '\0';
  g_fetchFailCount = 0;      // a deliberate switch is not a failure streak
  g_nextFetchAt = millis();  // re-fetch immediately against the new base
  LOG_WARN("API base switched to %s", g_apiBase);
  g_http.send(200, "application/json", String("{\"base\":\"") + g_apiBase + "\"}");
}

static void httpHandleDeleteApiBase() {
  strncpy(g_apiBase, API_BASE, sizeof(g_apiBase) - 1);
  g_apiBase[sizeof(g_apiBase) - 1] = '\0';
  g_fetchFailCount = 0;
  g_nextFetchAt = millis();
  LOG_WARN("API base restored to %s", g_apiBase);
  g_http.send(200, "application/json", String("{\"base\":\"") + g_apiBase + "\"}");
}

// Deliberately wedge loop() so the task watchdog can be verified. An unverified
// watchdog is worse than none — it looks like protection and may be misconfigured.
// Blocks forever; the device should reboot after LOOP_WDT_TIMEOUT_S.
// Deliberately drop the Wi-Fi association so the reconnect path can be tested
// without touching the access point. Exercises the disconnect handler, the
// behaviour of the fetch/display logic while the link is down, and recovery
// through connectWiFi(). Auto-reconnect is disabled for the duration so that
// OUR retry loop is what brings the link back, not the SDK's; the GOT_IP
// handler re-enables it.
static void httpHandleWifiDrop() {
  uint32_t hold = 90000;
  if (g_http.hasArg("plain")) {
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, g_http.arg("plain")) && !doc["hold_ms"].isNull()) {
      hold = doc["hold_ms"].as<uint32_t>();
    }
  }
  if (hold > WIFI_DROP_MAX_MS) hold = WIFI_DROP_MAX_MS;
  // Answer before the link goes away, or the caller never hears the reply.
  g_http.send(200, "application/json",
              String("{\"dropping_for_ms\":") + hold + "}");
  g_http.client().flush();
  delay(50);
  g_wifiDropUntil = millis() + hold;
  LOG_WARN("TEST: dropping Wi-Fi for %lu ms", (unsigned long)hold);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
}

static void httpHandleHang() {
  LOG_ERROR("TEST: hanging loop() on purpose, expecting a watchdog reset");
  g_http.send(200, "text/plain", "hanging\n");
  for (;;) {
  }
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
  g_http.on("/healthz", HTTP_GET, httpHandleHealth);
  g_http.on("/test/closest", HTTP_GET, httpHandleGetClosest);
  g_http.on("/test/closest", HTTP_PUT, httpHandlePutClosest);
  g_http.on("/test/closest", HTTP_DELETE, httpHandleDeleteClosest);
  g_http.on("/test/apibase", HTTP_PUT, httpHandlePutApiBase);
  g_http.on("/test/apibase", HTTP_DELETE, httpHandleDeleteApiBase);
  g_http.on("/test/hang", HTTP_POST, httpHandleHang);
  g_http.on("/test/wifi-drop", HTTP_POST, httpHandleWifiDrop);
  g_http.begin();
  g_httpStarted = true;
  LOG_INFO("HTTP test server listening on :80");
}
#endif
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
    drawCentered(line, (SCREEN_HEIGHT / 2));
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

// Pure classification — no network I/O at all.
static const char* classifyOp(const FlightInfo &fi) {
  // 1) dbFlags bit 0 = military. Authoritative on its own: the API omits
  // dbFlags entirely when it would be zero, and every aircraft returned by
  // /v2/mil carries this bit, so scanning that list cannot add information.
  if (fi.dbFlags >= 0 && (fi.dbFlags & 1)) return "MIL";

  // 2) Seat-based: small aircraft (<= 15 seats) are PVT
  if (fi.typeCode[0]) {
    uint16_t maxSeats = 0;
    if (aircraftSeatMax(fi.typeCode, maxSeats)) {
      if (maxSeats > 0 && maxSeats <= 15) return "PVT";
    }
  }

  // 3) ADS-B category as supplementary signal when seats didn't classify
  // (unknown type, or a known type with no seat data e.g. freighters)
  if (fi.category[0]) {
    // A1=Light, A7=Rotorcraft → likely private
    if (strcmp(fi.category, "A1") == 0 || strcmp(fi.category, "A7") == 0) return "PVT";
    // A3=Large, A4=High vortex, A5=Heavy → likely commercial
    if (strcmp(fi.category, "A3") == 0 || strcmp(fi.category, "A4") == 0 || strcmp(fi.category, "A5") == 0) return "COM";
    // B* = glider, balloon/airship, parachutist, ultralight, UAV, spacecraft.
    // None of them are commercial passenger operations.
    if (fi.category[0] == 'B') return "PVT";
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
  if (strcmp(a.squawk, b.squawk) != 0) return false;
  if (strcmp(a.emergency, b.emergency) != 0) return false;
  // Consider distances equal within 0.1 km to avoid flicker
  double da = isnan(a.distanceKm) ? 0 : a.distanceKm;
  double db = isnan(b.distanceKm) ? 0 : b.distanceKm;
  if (fabs(da - db) > 0.1) return false;
  return true;
}

// Cycle through 1px offsets to prevent OLED image sticking on the mostly
// static bottom bar. y only shifts up — the glyphs already sit at the bottom
// edge of the panel opening, so shifting down would clip them.
static const int8_t kPixelShifts[][2] = { {0,0}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0} };
static constexpr uint8_t kPixelShiftCount = sizeof(kPixelShifts) / sizeof(kPixelShifts[0]);
static uint8_t pixelShiftIdx() {
  return (uint8_t)((millis() / PIXEL_SHIFT_INTERVAL_MS) % kPixelShiftCount);
}

static void drawCentered(const char *text, int16_t baselineY, int8_t dx, int8_t dy) {
  uint16_t w = u8g2.getUTF8Width(text);
  int16_t x = (SCREEN_WIDTH - (int)w) / 2;
  if (x < 0) x = 0;
  u8g2.drawUTF8(x + dx, baselineY + dy, text);
}

// The splash is the one screen that can stay up for days — a sustained API or
// Wi-Fi outage pins it. Dimming alone does not move the lit pixels, so it gets
// the same shift as the bottom bar, and loop() redraws it when the step changes.

static void drawSplash(const char *msgTop, const char *msgBottom) {
  g_statSplashDraws++;
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

  const int8_t dx = kPixelShifts[pixelShiftIdx()][0];
  const int8_t dy = kPixelShifts[pixelShiftIdx()][1];

  // Draw title centered
  u8g2.setFont(titleFont);
  int16_t base = y0 + titleAscent;
  drawCentered("Flight Display", base, dx, dy);

  // Draw first message
  u8g2.setFont(bodyFont);
  base = y0 + titleH + gap + bodyAscent;
  drawCentered(msgTop, base, dx, dy);

  // Optional second message
  if (msgBottom) {
    base = y0 + titleH + gap + bodyH + gap + bodyAscent;
    drawCentered(msgBottom, base, dx, dy);
  }

  setDisplayDim(true);
  u8g2.sendBuffer();
}

static void showSplash(const char *msgTop, const char *msgBottom) {
  // Copy before drawing so the pixel-shift redraw in loop() can reuse the text
  // without aliasing its own source buffers.
  strncpy(g_splashTop, msgTop ? msgTop : "", sizeof(g_splashTop) - 1);
  g_splashTop[sizeof(g_splashTop) - 1] = '\0';
  strncpy(g_splashBottom, msgBottom ? msgBottom : "", sizeof(g_splashBottom) - 1);
  g_splashBottom[sizeof(g_splashBottom) - 1] = '\0';
  g_splashActive = true;
  drawSplash(msgTop, msgBottom);
}

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (!wifiInitialized) return;
  // First attempt
  if (!wifiEverBegun) {
    LOG_INFO("WiFi: connecting to %s", WIFI_SSID);
    showSplash("Connecting Wi-Fi...", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiConnecting = true;
    wifiEverBegun = true;
    wifiLastAttemptAt = millis();
    return;
  }
  // Periodic retry if auto-reconnect has not recovered
  uint32_t now = millis();
  if ((now - wifiLastAttemptAt) >= WIFI_RETRY_INTERVAL_MS) {
    LOG_WARN("WiFi retry: calling WiFi.begin() again");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiLastAttemptAt = now;
    wifiConnecting = true;
  }
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

// Feet, or a sentinel. Values outside any plausible flight envelope are junk
// (a value beyond int32 used to wrap to something <= 0 and render as GND, which
// is worse than admitting we do not know).
static constexpr int32_t ALT_PLAUSIBLE_MAX = 200000;   // above any aircraft or balloon
static constexpr int32_t ALT_PLAUSIBLE_MIN = -2000;    // below the lowest dry land
static int32_t decodeAltitude(JsonVariantConst v) {
  long long ft = v.as<long long>();
  if (ft > ALT_PLAUSIBLE_MAX || ft < ALT_PLAUSIBLE_MIN) return ALT_UNKNOWN;
  // Present and at or below sea level means on the surface: seaplanes and
  // taxiing aircraft without a squat switch report 0 or slightly negative.
  if (ft <= 0) return ALT_GROUND;
  return (int32_t)ft;
}

static bool hasNonSpace(const char* s) {
  if (!s) return false;
  for (; *s; ++s) if (*s != ' ') return true;
  return false;
}

static bool extractLatLon(JsonObject obj, double &outLat, double &outLon) {
  // Only accept positions with a known, fresh age. No seen_pos = unknown
  // age = don't trust it.
  // containsKey alone is not enough: a null passes it and as<double>() yields 0,
  // which would read as "brand new" — the opposite of unknown.
  if (!obj.containsKey("seen_pos") || obj["seen_pos"].isNull()) return false;
  if (!obj["seen_pos"].is<float>() && !obj["seen_pos"].is<int>()) return false;
  if (obj["seen_pos"].as<double>() > POSITION_MAX_AGE_S) return false;
  if (obj.containsKey("lat") && obj.containsKey("lon")) {
    outLat = obj["lat"].as<double>();
    outLon = obj["lon"].as<double>();
    if (!(outLat == 0.0 && outLon == 0.0)) return true;  // ignore (0,0)
  }
  // Do not use lastPosition fallback to avoid selecting stale tracks
  return false;
}

static FlightInfo parseAircraft(JsonObject obj) {
  FlightInfo res;
  double lat = NAN, lon = NAN;
  if (!extractLatLon(obj, lat, lon)) return res;

  // Build ident preference chain: flight -> r -> hex.
  // ADS-B pads the flight field to 8 characters, so an aircraft transmitting no
  // callsign arrives as "        ". Testing *identSrc only rejects the empty
  // string, which let padding win the chain: the title rendered blank and
  // hasCallsign went true, classifying a private aircraft as commercial.
  const char* identSrc = nullptr;
  bool hasCallsign = false;
  if (obj["flight"]) {
    const char* f = obj["flight"].as<const char*>();
    if (hasNonSpace(f)) { identSrc = f; hasCallsign = true; }
  }
  if (!hasNonSpace(identSrc) && obj["r"]) {
    const char* r = obj["r"].as<const char*>();
    if (hasNonSpace(r)) identSrc = r;
  }
  if (!hasNonSpace(identSrc) && obj["hex"]) {
    const char* hx = obj["hex"].as<const char*>();
    if (hasNonSpace(hx)) identSrc = hx;
  }
  if (!hasNonSpace(identSrc)) identSrc = "(unknown)";

  // Copy and trim ident
  strncpy(res.ident, identSrc, sizeof(res.ident) - 1);
  res.ident[sizeof(res.ident) - 1] = '\0';
  // Trim trailing then leading spaces (the API pads on both sides in practice)
  for (int i = strlen(res.ident) - 1; i >= 0 && res.ident[i] == ' '; --i) res.ident[i] = '\0';
  if (res.ident[0] == ' ') {
    char* p = res.ident;
    while (*p == ' ') ++p;
    memmove(res.ident, p, strlen(p) + 1);
  }

  // alt_baro is a number in feet, or the string "ground" for surface aircraft.
  // Some targets (MLAT/TIS-B, some GA) omit alt_baro — fall back to alt_geom.
  // Altitude decoding. Order matters: a value that is PRESENT and non-positive
  // means "on the surface", while -1 is reserved for "absent". Applying the
  // surface rule first removes the old ambiguity where a real -1 ft reading was
  // indistinguishable from a missing field.
  //
  // alt_baro is a number in feet, or the exact string "ground". Any other
  // string is not a known encoding, so it is treated as unknown rather than
  // silently rendered as GND.
  if (obj["alt_baro"].is<const char*>()) {
    const char* ab = obj["alt_baro"].as<const char*>();
    res.altitudeFt = (ab && strcasecmp(ab, "ground") == 0) ? ALT_GROUND : ALT_UNKNOWN;
  } else if (!obj["alt_baro"].isNull()) {
    res.altitudeFt = decodeAltitude(obj["alt_baro"]);
  } else if (!obj["alt_geom"].isNull()) {
    res.altitudeFt = decodeAltitude(obj["alt_geom"]);
  } else {
    res.altitudeFt = ALT_UNKNOWN;
  }

  // "t" = aircraft type designator (B738, A320, etc.)
  // Note: "type" is a different field (message type: adsb_icao, tisb, mlat) — do NOT use as fallback
  const char* typeSrc = obj["t"].isNull() ? nullptr : obj["t"].as<const char*>();
  if (typeSrc) { strncpy(res.typeCode, typeSrc, sizeof(res.typeCode) - 1); res.typeCode[sizeof(res.typeCode) - 1] = '\0'; }

  const char* catSrc = obj["category"].isNull() ? nullptr : obj["category"].as<const char*>();
  if (catSrc) { strncpy(res.category, catSrc, sizeof(res.category) - 1); res.category[sizeof(res.category) - 1] = '\0'; }

  // "desc" = full type description from the aircraft DB (title fallback)
  const char* descSrc = obj["desc"].isNull() ? nullptr : obj["desc"].as<const char*>();
  if (descSrc) { strncpy(res.desc, descSrc, sizeof(res.desc) - 1); res.desc[sizeof(res.desc) - 1] = '\0'; }

  const char* hexSrc = obj["hex"].isNull() ? nullptr : obj["hex"].as<const char*>();
  if (hexSrc) { strncpy(res.hex, hexSrc, sizeof(res.hex) - 1); res.hex[sizeof(res.hex) - 1] = '\0'; }

  // dbFlags: bit 0 = military
  res.dbFlags = obj["dbFlags"].isNull() ? -1 : obj["dbFlags"].as<int>();

  const char* sqSrc = obj["squawk"].isNull() ? nullptr : obj["squawk"].as<const char*>();
  if (sqSrc) { strncpy(res.squawk, sqSrc, sizeof(res.squawk) - 1); res.squawk[sizeof(res.squawk) - 1] = '\0'; }

  const char* emSrc = obj["emergency"].isNull() ? nullptr : obj["emergency"].as<const char*>();
  if (emSrc && strcasecmp(emSrc, "none") != 0) {
    strncpy(res.emergency, emSrc, sizeof(res.emergency) - 1);
    res.emergency[sizeof(res.emergency) - 1] = '\0';
  }

  res.valid = true;
  res.lat = lat;
  res.lon = lon;
  res.distanceKm = haversineKm(HOME_LAT, HOME_LON, lat, lon);
  res.hasCallsign = hasCallsign;
  return res;
}

// ArduinoJson's stream reader stops the moment a read returns short, so a
// momentary gap in a TLS stream ends the document part-way (IncompleteInput)
// even though the rest is milliseconds behind. This wrapper makes a gap WAIT
// for the next burst instead of reporting end-of-input, bounded by one overall
// deadline so loop() stays well inside the task watchdog.
//
// Only readBytes() needs overriding: ArduinoJson reads exclusively through it.
// Safe because these responses carry Content-Length (never chunked encoding),
// so reading the client directly needs no de-chunking. Takes Client& so it
// serves both the TLS API fetch and the plain-HTTP local receiver fetch.
class PatientStream : public Stream {
 public:
  PatientStream(Client &c, uint32_t budgetMs)
      : c_(c), deadline_(millis() + budgetMs) {}

  int available() override { return c_.available(); }
  int peek() override { return c_.peek(); }
  size_t write(uint8_t) override { return 0; }
  int read() override {
    char ch;
    return readBytes(&ch, 1) == 1 ? (int)(unsigned char)ch : -1;
  }

  size_t readBytes(char *buffer, size_t length) override {
    size_t got = 0;
    while (got < length) {
      if (!waitForBytes()) break;
      int n = c_.read((uint8_t *)buffer + got, length - got);
      if (n > 0) got += (size_t)n;
    }
    return got;
  }

  bool timedOut() const { return timedOut_; }

 private:
  // True once at least one byte is ready. False only on a hard end: the overall
  // deadline expired, or the peer closed with nothing left buffered.
  bool waitForBytes() {
    while (c_.available() == 0) {
      if ((int32_t)(millis() - deadline_) >= 0) { timedOut_ = true; return false; }
      if (!c_.connected() && c_.available() == 0) return false;
      yield();
    }
    return true;
  }

  Client &c_;
  uint32_t deadline_;
  bool timedOut_ = false;
};

// Choose which aircraft to show from a whole-response list.
//
// Normally that is simply the nearest one, which keeps the display identical to
// the /v2/closest behaviour. The exception is an emergency transponder code:
// 7500/7600/7700 anywhere in range preempts the nearest aircraft, because the
// whole reason for fetching every aircraft in the circle is to notice one.
// Among several emergencies the nearest wins.
static FlightInfo selectAircraft(JsonVariant root) {
  g_lastAcReturned = 0;
  g_lastAcRejected = 0;
  FlightInfo best;            // nearest valid aircraft
  FlightInfo alerted;         // best alerting aircraft, by priority then distance
  uint8_t alertedPri = 0;
  if (!root.is<JsonObject>()) return best;
  JsonObject robj = root.as<JsonObject>();
  if (!robj.containsKey("ac") || !robj["ac"].is<JsonArray>()) return best;

  for (JsonObject obj : robj["ac"].as<JsonArray>()) {
    g_lastAcReturned++;
    FlightInfo fi = parseAircraft(obj);
    if (!fi.valid || isnan(fi.distanceKm)) { g_lastAcRejected++; continue; }
    if (!best.valid || fi.distanceKm < best.distanceKm) best = fi;

    const AlertLevel* a = alertFor(fi.squawk, fi.emergency);
    if (!a || a->priority < ALERT_PREEMPT_MIN_PRIORITY) continue;
    // A more severe condition always outranks a closer one; distance only
    // separates aircraft at the same severity.
    if (a->priority > alertedPri
        || (a->priority == alertedPri && alerted.valid && fi.distanceKm < alerted.distanceKm)) {
      alerted = fi;
      alertedPri = a->priority;
    }
  }
  if (alerted.valid) {
    const AlertLevel* a = alertFor(alerted.squawk, alerted.emergency);
    LOG_WARN("Alert %s (%s/%s) on %s at %.1f km — preempting nearest",
             a ? a->label : "?", alerted.squawk, alerted.emergency,
             alerted.ident, alerted.distanceKm);
    return alerted;
  }
  return best;
}

// Resolve the API host BEFORE the HTTP call, and feed the watchdog in between.
//
// DNS runs inside http.begin() and can take ~15s (finding #14) on top of an 8s
// connect and an 8s read, so one fetch could occupy ~31s of a single loop()
// iteration — past the 25s watchdog. Two production resets came from exactly
// that sum. Doing the lookup separately turns one ~31s un-fed span into two
// spans of at most ~16s, and gives a fast exit when DNS is simply down.
// The result lands in the lwIP cache, so http.begin()'s own lookup is instant.
static bool resolveApiHost(const char* base) {
  const char* p = strstr(base, "://");
  if (!p) return true;
  p += 3;
  char host[64];
  size_t n = 0;
  while (p[n] && p[n] != '/' && p[n] != ':' && n < sizeof(host) - 1) { host[n] = p[n]; ++n; }
  host[n] = '\0';
  if (!host[0]) return true;

  IPAddress ip;
  if (ip.fromString(host)) return true;  // already an address; nothing to resolve

  uint32_t t0 = millis();
  bool ok = (WiFi.hostByName(host, ip) == 1);
  uint32_t took = millis() - t0;
  // No esp_task_wdt_reset() here any more: this runs on the network task, which
  // is deliberately not subscribed to the TWDT. Calling it from an unsubscribed
  // task returns ESP_ERR_NOT_FOUND and logs an error every time — enough serial
  // spam to corrupt the capture. Splitting DNS from connect+read still matters
  // for the fast-exit on failure; the watchdog protection now comes from loop()
  // never blocking at all.
  if (!ok) {
    LOG_WARN("DNS lookup for %s failed after %lu ms", host, (unsigned long)took);
    return false;
  }
  if (took > 1000) LOG_WARN("DNS for %s took %lu ms", host, (unsigned long)took);
  return true;
}

static FetchResult fetchClosestAt(double radiusKm, FlightInfo &out, bool allAircraft = false) {
  if (WiFi.status() != WL_CONNECTED) return FETCH_FAIL;

  // The API's radius parameter is NAUTICAL MILES (verified against its dst
  // field) — convert so the *_KM config constants mean what they say
  int radiusNm = (int)(radiusKm * 0.5399568 + 0.5);
  if (radiusNm < 1) radiusNm = 1;

  // Build URL with stack-allocated buffer (no heap allocation)
  char url[128];
  // /v2/point returns every aircraft in the circle, which is what lets an
  // emergency squawk on a more distant aircraft preempt the nearest one.
  // /v2/closest returns exactly one and stays the choice for the widened
  // fallback search, where a 44 KB body would buy nothing over a 591 B one.
  snprintf(url, sizeof(url), "%s/v2/%s/%.6f/%.6f/%d",
           g_apiBase, allAircraft ? "point" : "closest", HOME_LAT, HOME_LON, radiusNm);
  LOG_INFO("HTTP GET %s", url);
  LOG_DEBUG("WiFi RSSI: %d dBm", WiFi.RSSI());
  LOG_DEBUG("Free heap: %u", (unsigned)ESP.getFreeHeap());

  if (!resolveApiHost(g_apiBase)) return FETCH_FAIL;

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  WiFiClientSecure client;
  client.setInsecure();
  // No client.setTimeout() here: HTTPClient::connect() sets the stream timeout
  // from _tcpTimeout, and Stream::setTimeout takes MILLISECONDS while this once
  // passed seconds — a discrepancy that looked like a bug and was merely dead.
  if (!http.begin(client, url)) {
    LOG_WARN("HTTP begin() failed (TLS)");
    return FETCH_FAIL;
  }
  http.addHeader("Accept", "application/json");
  // setUserAgent, not addHeader: HTTPClient::addHeader silently DROPS
  // User-Agent, Connection, Accept-Encoding and Host, so the addHeader version
  // never left the device and adsb.lol saw the generic built-in UA and 403'd us.
  http.setUserAgent(API_USER_AGENT);

  uint32_t apiStart = millis();
  int code = http.GET();
  g_apiMsLast = millis() - apiStart;
  if (g_apiMsLast > g_apiMsMax) g_apiMsMax = g_apiMsLast;
  g_statLastHttp = code;
  // Cleared on every attempt: this field used to keep the last message that
  // happened to fail, so /healthz reported "Too Many Requests" alongside a 200
  // indefinitely and misled anything monitoring it.
  g_statErrBuf[0] = '\0';
  LOG_INFO("HTTP status: %d", code);
  if (code != HTTP_CODE_OK) {
    LOG_WARN("HTTP error: %s", http.errorToString(code).c_str());
    if (code > 0) {
      String body = http.getString();
      if (body.length() > 200) body.remove(200);
      LOG_WARN("HTTP body: %s", body.c_str());
      strncpy(g_statErrBuf, body.c_str(), sizeof(g_statErrBuf) - 1);
      g_statErrBuf[sizeof(g_statErrBuf) - 1] = '\0';
    } else {
      // Transport-level failure has no body; record why anyway.
      snprintf(g_statErrBuf, sizeof(g_statErrBuf), "transport: %s",
               http.errorToString(code).c_str());
    }
    http.end();
    return FETCH_FAIL;
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
  acObj["alt_geom"] = true;  // fallback when alt_baro is absent
  acObj["lat"] = true;
  acObj["lon"] = true;
  acObj["seen_pos"] = true;
  acObj["category"] = true;
  acObj["dbFlags"] = true;  // bit 0 = military
  acObj["squawk"] = true;    // 7500/7600/7700 are emergencies
  acObj["emergency"] = true; // lifeguard/minfuel/downed have no squawk
  acObj["desc"] = true;     // full type description (title fallback)

  StaticJsonDocument<2048> doc;  // stack-allocated; filtered single-aircraft response is well under 1KB
  // Prefer streamed parsing to minimize RAM and fragmentation
  // PatientStream, not http.getStream(): a bare stream reports a momentary TLS
  // gap as end-of-input, which truncated every response over ~3.6 KB.
  PatientStream body(client, HTTP_READ_TIMEOUT_MS);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    LOG_WARN("JSON parse error (streamed): %s%s", err.c_str(),
             body.timedOut() ? " [read deadline expired]" : "");
    return FETCH_FAIL;
  }

  FlightInfo closest = selectAircraft(doc.as<JsonVariant>());
  if (closest.valid) {
    LOG_INFO("Closest %s  dist %.2f km", closest.ident, closest.distanceKm);
    const char* op = classifyOp(closest);
    strncpy(closest.opClass, op, sizeof(closest.opClass) - 1);
    closest.opClass[sizeof(closest.opClass) - 1] = '\0';
    LOG_INFO("Classified op: %s", closest.opClass);
    out = closest;
    return FETCH_OK;
  }
  // "Empty" has two very different causes and they were logged identically,
  // making it impossible to tell afterwards whether the sky really was clear or
  // whether we rejected everything the API sent.
  LOG_INFO("No displayable aircraft: %d returned, %d rejected on position/staleness",
           g_lastAcReturned, g_lastAcRejected);
  return FETCH_EMPTY;
}

#if FEATURE_LOCAL_RX
// Refresh the two live numbers — distance and altitude — for the aircraft that
// is already on screen, from the local ADS-B receiver.
//
// adsb.lol still owns identity, type, classification and which aircraft to
// show. This only closes the staleness gap: the API is polled every 30s, so
// without this the distance ticks in half-mile jumps and can lag reality by a
// full interval (measured: one aircraft moved 2.90 -> 4.24 nm inside 95s).
// The receiver answers in ~30ms on the LAN with no rate limit, so it can be
// asked every few seconds for free.
//
// Deliberately conservative: it never changes what is displayed, only how
// current the numbers are, and it never counts as an API fetch failure.
// Pull one aircraft's record out of the receiver's JSON without parsing all of
// it. The body is 7-16 KB and ArduinoJson must tokenise every byte to find one
// record, which measured p50 120ms / p95 310ms on device against 27ms of actual
// network time. Searching for the hex and parsing only the matching record cuts
// that to the cost of a few memcmps.
//
// Safe because dump1090 records are flat — arrays like "mlat":[] appear, but no
// nested objects — so the first '}' after the needle ends the record.
// Fills `io` (which arrives as a copy of the aircraft on screen) with a fresher
// position and altitude. Pure producer: no globals for display state, no
// rendering — both belong to loop().
static bool fetchLocalUpdate(FlightInfo &io) {
  if (!io.hex[0]) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  // Skip aircraft the receiver realistically cannot hear, rather than paying a
  // fetch to rediscover that. Overnight this was 70% of all lookups.
  if (!isnan(io.distanceKm) && io.distanceKm > LOCAL_RX_MAX_DIST_KM) {
    g_statLocalSkip++;
    return false;
  }
  if (strcasecmp(g_localMissHex, io.hex) == 0 && g_localMissRun >= LOCAL_RX_MISS_GIVEUP) {
    g_statLocalSkip++;
    return false;
  }
  uint32_t localStart = millis();

  WiFiClient client;  // plain HTTP on the LAN: no TLS, and an IP so no DNS
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(LOCAL_RX_CONNECT_TIMEOUT_MS);
  http.setTimeout(LOCAL_RX_READ_TIMEOUT_MS);

  char url[96];
  snprintf(url, sizeof(url), "http://%s:%d%s", LOCAL_RX_HOST, (int)LOCAL_RX_PORT, LOCAL_RX_PATH);
  if (!http.begin(client, url)) {
    g_statLocalFail++;
    g_nextLocalAt = millis() + LOCAL_RX_BACKOFF_MS;
    return false;
  }
  int code = http.GET();
  int contentLen = http.getSize();
  if (code != HTTP_CODE_OK) {
    http.end();
    g_statLocalFail++;
    LOG_WARN("Local receiver HTTP %d; backing off", code);
    g_nextLocalAt = millis() + LOCAL_RX_BACKOFF_MS;
    return false;
  }

  char hexLower[8];
  strncpy(hexLower, io.hex, sizeof(hexLower) - 1);
  hexLower[sizeof(hexLower) - 1] = '\0';
  for (char *c = hexLower; *c; ++c) *c = tolower(*c);

  // 768 not 384: real records reach ~525 bytes here and grow with nav/wind
  // fields. An undersized buffer silently reported "not found".
  char record[768];
  bool truncated = false;
  PatientStream body(client, LOCAL_RX_READ_TIMEOUT_MS);
  size_t scanned = 0;
  bool found = extractLocalRecord(body, hexLower, record, sizeof(record), &truncated, &scanned);
  http.end();
  g_localMsLast = millis() - localStart;
  if (g_localMsLast > g_localMsMax) g_localMsMax = g_localMsLast;

  if (truncated) {
    g_statLocalFail++;
    LOG_WARN("Local record for %s exceeded %u bytes", hexLower, (unsigned)sizeof(record));
    return false;
  }
  if (!found) {
    // Whether we saw the WHOLE body matters: a short read means the aircraft
    // may well be present and simply beyond where we stopped reading.
    LOG_WARN("Local miss for %s after %u of %d bytes", hexLower,
             (unsigned)scanned, (int)contentLen);
    g_statLocalMiss++;
    if (strcasecmp(g_localMissHex, io.hex) != 0) {
      strncpy(g_localMissHex, io.hex, sizeof(g_localMissHex) - 1);
      g_localMissHex[sizeof(g_localMissHex) - 1] = '\0';
      g_localMissRun = 1;
    } else if (g_localMissRun < 255) {
      g_localMissRun++;
    }
    return false;
  }

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, record)) {
    g_statLocalFail++;
    LOG_WARN("Local record parse error");
    return false;
  }
  JsonObject a = doc.as<JsonObject>();

  // Only trust a position the receiver heard recently. These two cases are very
  // different from "the record is not there" and were previously all lumped
  // into one counter, which made a working path look broken.
  if (a["seen_pos"].isNull() || a["lat"].isNull() || a["lon"].isNull()) {
    g_statLocalNoPos++;
    return false;
  }
  float age = a["seen_pos"].as<float>();
  if (age > LOCAL_RX_MAX_AGE_S) {
    g_statLocalStale++;
    g_localSeenPos = age;
    return false;
  }

  FlightInfo upd = io;  // identity and classification untouched
  upd.lat = a["lat"].as<double>();
  upd.lon = a["lon"].as<double>();
  upd.distanceKm = haversineKm(HOME_LAT, HOME_LON, upd.lat, upd.lon);
  if (!a["alt_baro"].isNull()) {
    upd.altitudeFt = a["alt_baro"].is<const char*>()
        ? (strcasecmp(a["alt_baro"].as<const char*>(), "ground") == 0 ? ALT_GROUND : ALT_UNKNOWN)
        : decodeAltitude(a["alt_baro"]);
  } else if (!a["alt_geom"].isNull()) {
    upd.altitudeFt = decodeAltitude(a["alt_geom"]);
  }
  g_localSeenPos = age;
  g_statLocalOk++;
  g_localMissRun = 0;
  io = upd;
  return true;
}
#endif  // FEATURE_LOCAL_RX

// ---------------------------------------------------------------------------
// Network I/O runs on its own task.
//
// A fetch can legitimately occupy ~31s: lwIP DNS resolution is not bounded by
// HTTP_CONNECT_TIMEOUT_MS and takes a fixed ~15s when it fails (finding #14), on
// top of an 8s connect and an 8s read. Held inside loop() that starved OTA and
// the HTTP server and twice tripped the 25s task watchdog in production.
//
// Deliberately narrow: the task performs ONLY the blocking call and hands back a
// result. Every decision — backoff, the circuit breaker, staleness, relays and
// all rendering — stays in loop(), because u8g2 and the SPI bus are not thread
// safe and because keeping the logic in one place is what makes this reviewable.
// ---------------------------------------------------------------------------
#ifndef NET_TASK_STACK
#define NET_TASK_STACK 16384   // mbedTLS plus ArduinoJson filtering needs room
#endif
// A job outlasting this is hung rather than slow: the longest legitimate fetch
// is DNS(15s) + connect(8s) + read(8s), doubled at worst for a widened search.
#ifndef NET_STALL_RESTART_MS
#define NET_STALL_RESTART_MS 180000
#endif

// True only when nothing is queued, running, or waiting to be collected.
static bool netIdle() {
  NET_LOCK();
  bool r = (g_netPending == NET_NONE && !g_netBusy && !g_netDone);
  NET_UNLOCK();
  return r;
}

static bool netRequest(NetJob job, const FlightInfo *target) {
  NET_LOCK();
  bool ok = (g_netPending == NET_NONE && !g_netBusy && !g_netDone);
  if (ok) {
    g_netPending = job;
    if (target) g_netTarget = *target;
  }
  NET_UNLOCK();
  return ok;
}

static bool netCollect(NetJob &kind, FlightInfo &out, FetchResult &st) {
  NET_LOCK();
  bool ok = g_netDone;
  if (ok) {
    kind = g_netDoneKind;
    out = g_netResult;
    st = g_netStatus;
    g_netDone = false;
  }
  NET_UNLOCK();
  return ok;
}

// Primary radius first; widen only when the nearby sky is empty, so the
// display always has an aircraft when anything is in range while keeping
// the common case at one API call per cycle.
// The radius the primary search actually covers. SEARCH_RADIUS_KM is converted
// to whole nautical miles for the API, so the real circle is smaller than the
// configured value and the two must not be used interchangeably.
static double primaryQueryRadiusKm() {
  int nm = (int)(SEARCH_RADIUS_KM * 0.5399568 + 0.5);
  if (nm < 1) nm = 1;
  return nm / 0.5399568;
}

// Sticky wide mode. Overnight the nearby sky is empty for hours, and the old
// logic paid TWO API calls every cycle to rediscover that — 522 widenings in a
// 7.6h window, doubling both the API load and the per-iteration blocking that
// tripped the watchdog. Once the widened search is what is working, keep using
// it alone, and drop back automatically as soon as it finds something close.
static bool g_wideMode = false;

static FetchResult fetchNearestFlight(FlightInfo &out) {
  if (g_wideMode) {
    // /v2/point, not /v2/closest: a single-aircraft response would silently
    // switch OFF emergency-squawk scanning for however many hours the sky stays
    // quiet, which is precisely when nobody would notice it had stopped.
    // Affordable because wide mode only engages when the sky IS quiet, so the
    // ring is small then — measured 9.7 KB / 20 aircraft, against 44 KB / 95 at
    // midday when the primary circle is busy and wide mode never runs.
    FetchResult fr = fetchClosestAt(SEARCH_RADIUS_FALLBACK_KM, out, /*allAircraft=*/true);
    // Compare against the radius actually QUERIED, with hysteresis. The API
    // takes integer nautical miles, so a 10 km setting is sent as 5 nm = 9.26 km:
    // an aircraft in the 9.26-10 km band is invisible to the primary search yet
    // passed a `<= SEARCH_RADIUS_KM` test, so wide mode toggled every single
    // cycle and paid for two searches instead of one.
    if (fr == FETCH_OK && out.distanceKm <= primaryQueryRadiusKm() * 0.9) {
      // Traffic is back in the primary circle: resume the full /v2/point scan
      // so emergency squawks on non-nearest aircraft are seen again.
      LOG_INFO("Aircraft within %d km again; resuming full scan", (int)SEARCH_RADIUS_KM);
      g_wideMode = false;
    } else if (fr != FETCH_OK) {
      g_wideMode = false;  // nothing out there either — probe normally next time
    }
    return fr;
  }

  FetchResult fr = fetchClosestAt(SEARCH_RADIUS_KM, out, /*allAircraft=*/true);
  if (fr == FETCH_EMPTY && SEARCH_RADIUS_FALLBACK_KM > SEARCH_RADIUS_KM) {
    LOG_INFO("Nothing within %d km, widening to %d km",
             (int)SEARCH_RADIUS_KM, (int)SEARCH_RADIUS_FALLBACK_KM);
    // (No watchdog feed needed between the two fetches: neither runs on the
    // watchdogged task any more.)
    // /v2/point, not /v2/closest. The single aircraft /v2/closest returns may
    // fail the position-freshness gate, and rejecting it declared the entire
    // 100 km ring empty — showing "No aircraft nearby" over a busy sky. With
    // the full list there are alternatives to fall back to.
    fr = fetchClosestAt(SEARCH_RADIUS_FALLBACK_KM, out, /*allAircraft=*/true);
    if (fr == FETCH_OK) g_wideMode = true;
  }
  return fr;
}

// test server removed

// Resolve display title: local table -> pseudo targets (TIS-B, MLAT, etc.)
// -> API desc -> raw type code -> "Unknown Aircraft".
// Returns true if the type is a pseudo (non-aircraft) target.
static bool resolveFriendlyName(const FlightInfo &fi, char* buf, size_t bufSize) {
  const char* typeCode = fi.typeCode;
  bool isPseudo = false;
  bool found = false;
  if (typeCode[0]) {
    found = aircraftFriendlyNameBuf(typeCode, buf, bufSize);
  }
  if (!found && typeCode[0]) {
    struct { const char* prefix; const char* label; } pseudos[] = {
      { "TISB", "TIS-B Target" }, { "ADSB", "ADS-B Target" },
      { "MLAT", "MLAT Target" },  { "MODE", "Mode-S Target" }
    };
    for (auto &p : pseudos) {
      if (strncasecmp(typeCode, p.prefix, 4) == 0) {
        strncpy(buf, p.label, bufSize - 1);
        buf[bufSize - 1] = '\0';
        isPseudo = true; found = true;
        break;
      }
    }
  }
  if (!found && fi.desc[0]) {
    strncpy(buf, fi.desc, bufSize - 1);
    buf[bufSize - 1] = '\0';
    found = true;
  }
  if (!found && typeCode[0]) {
    strncpy(buf, typeCode, bufSize - 1);
    buf[bufSize - 1] = '\0';
    found = true;
  }
  if (!found) {
    strncpy(buf, "Unknown Aircraft", bufSize - 1);
    buf[bufSize - 1] = '\0';
  }
  return isPseudo;
}

// Draw the bottom metric bar: distance | seats | altitude
static void drawBottomBar(const FlightInfo &fi, bool isPseudo, int16_t yBottom) {
  char distStr[16] = "\xE2\x80\x94";  // em-dash
  // Internally everything stays km (API radius, haversine); convert at render only
  if (!isnan(fi.distanceKm)) snprintf(distStr, sizeof(distStr), "%.1fmi", fi.distanceKm * 0.621371);

  char seatsStr[8] = "\xE2\x80\x94";
  if (!isPseudo) {
    uint16_t maxSeats = 0;
    if (fi.seatOverride > 0) snprintf(seatsStr, sizeof(seatsStr), "%d", fi.seatOverride);
    else if (fi.typeCode[0] && aircraftSeatMax(fi.typeCode, maxSeats) && maxSeats > 0) snprintf(seatsStr, sizeof(seatsStr), "%u", (unsigned)maxSeats);
  }

  char altStr[16] = "\xE2\x80\x94";
  if (fi.altitudeFt == ALT_GROUND) snprintf(altStr, sizeof(altStr), "GND");
  else if (fi.altitudeFt >= 0) snprintf(altStr, sizeof(altStr), "%dft", (int)fi.altitudeFt);

  const int8_t dx = kPixelShifts[pixelShiftIdx()][0];
  const int8_t dy = kPixelShifts[pixelShiftIdx()][1];

  const int cells = 3;
  const int cellW = SCREEN_WIDTH / cells;
  const char* items[] = { distStr, seatsStr, altStr };
  for (int i = 0; i < cells; ++i) {
    uint16_t bw = u8g2.getUTF8Width(items[i]);
    int16_t cx = i * cellW + (cellW - (int)bw) / 2;
    if (cx < 0) cx = 0;
    u8g2.drawUTF8(cx + dx, yBottom + dy, items[i]);
  }
}

static void renderFlight(const FlightInfo &fi) {
  uint32_t renderStart = millis();
  g_renderCount++;
  g_splashActive = false;
  setDisplayDim(false);
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  // 1) Top line: friendly aircraft name, or the emergency banner when the
  // aircraft is squawking 7500/7600/7700. The banner occupies the SAME area as
  // the type name and nothing else moves — the bottom row is physically
  // labelled on the bezel, so its three cells must never change meaning.
  char friendlyBuf[64];
  bool isPseudo = resolveFriendlyName(fi, friendlyBuf, sizeof(friendlyBuf));
  const char *alert = emergencyLabel(fi.squawk, fi.emergency);

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

  // Use char[] buffers instead of String to avoid heap allocation
  char line1[64];
  char line2[64];
  strncpy(line1, friendlyBuf, sizeof(line1) - 1);
  line1[sizeof(line1) - 1] = '\0';
  line2[0] = '\0';
  const uint8_t *chosen = titleFonts[NFONTS - 1];

  for (size_t i = 0; i < NFONTS; ++i) {
    u8g2.setFont(titleFonts[i]);
    int16_t asc = u8g2.getAscent();
    int16_t desc = -u8g2.getDescent();
    int16_t lh = asc + desc;
    // One-line fit within width and height
    if (u8g2.getUTF8Width(friendlyBuf) <= SCREEN_WIDTH && lh <= topAvail) {
      chosen = titleFonts[i];
      strncpy(line1, friendlyBuf, sizeof(line1) - 1);
      line1[sizeof(line1) - 1] = '\0';
      line2[0] = '\0';
      break;
    }
    // Try two-line wrap for this font if two lines fit height
    if (2 * lh + 2 <= topAvail) {
      // Find best space-split minimizing max line width
      int bestIdx = -1;
      int bestWorst = INT_MAX;
      size_t slen = strlen(friendlyBuf);
      for (size_t j = 1; j < slen - 1; ++j) {
        if (friendlyBuf[j] != ' ') continue;
        // Temporarily null-terminate at split point to measure first half
        char saved = friendlyBuf[j];
        friendlyBuf[j] = '\0';
        uint16_t wa = u8g2.getUTF8Width(friendlyBuf);
        friendlyBuf[j] = saved;
        uint16_t wb = u8g2.getUTF8Width(friendlyBuf + j + 1);
        if (wa <= SCREEN_WIDTH && wb <= SCREEN_WIDTH) {
          int worst = max((int)wa, (int)wb);
          if (worst < bestWorst) {
            bestWorst = worst;
            bestIdx = (int)j;
          }
        }
      }
      if (bestIdx >= 0) {
        chosen = titleFonts[i];
        memcpy(line1, friendlyBuf, bestIdx);
        line1[bestIdx] = '\0';
        strncpy(line2, friendlyBuf + bestIdx + 1, sizeof(line2) - 1);
        line2[sizeof(line2) - 1] = '\0';
        break;
      }
    }
  }

  if (alert) {
    // Line 1 names the condition, line 2 says which aircraft it is. Sized to fit
    // rather than run through the wrap search, so the banner never re-flows.
    char who[32];
    snprintf(who, sizeof(who), "%s%s%s", fi.ident,
             fi.typeCode[0] ? " " : "", fi.typeCode);
    const uint8_t *alertFonts[] = { u8g2_font_logisoso20_tf, u8g2_font_10x20_tf,
                                    u8g2_font_9x15_tf, u8g2_font_6x12_tf };
    const uint8_t *aFont = alertFonts[3];
    for (auto *f : alertFonts) {
      u8g2.setFont(f);
      int16_t lh = u8g2.getAscent() - u8g2.getDescent();
      if (u8g2.getUTF8Width(alert) <= SCREEN_WIDTH
          && u8g2.getUTF8Width(who) <= SCREEN_WIDTH
          && 2 * lh + 2 <= topAvail) { aFont = f; break; }
    }
    u8g2.setFont(aFont);
    int16_t asc = u8g2.getAscent(), desc = -u8g2.getDescent();
    int16_t lh = asc + desc;
    int16_t total = 2 * lh + 2;
    int16_t y = (topAvail - total) / 2 + asc;
    drawCentered(alert, y);
    drawCentered(who, y + lh + 2);
    u8g2.setFont(bottomFont);
    int16_t d2 = u8g2.getDescent();
    drawBottomBar(fi, isPseudo, SCREEN_HEIGHT - 1 - (d2 < 0 ? -d2 : d2));
#if ALERT_INVERT_DISPLAY
    // XOR the whole framebuffer: lit background, dark text. Nothing moves, so
    // the bottom row still lines up with the labels on the bezel — only the
    // polarity changes. Draw colour 2 is u8g2's XOR mode.
    u8g2.setDrawColor(2);
    u8g2.drawBox(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    u8g2.setDrawColor(1);
#endif
    u8g2.sendBuffer();
    { uint32_t d = millis() - renderStart; if (d > g_renderMaxMs) g_renderMaxMs = d; }
    return;
  }

  // Draw title (one or two lines), vertically centered within topAvail
  u8g2.setFont(chosen);
  int16_t ascT = u8g2.getAscent();
  int16_t descT = -u8g2.getDescent();
  int16_t lhT = ascT + descT;
  int16_t totalTitleH = lhT + ((line2[0] != '\0') ? (2 + lhT) : 0);
  if (totalTitleH > topAvail) totalTitleH = topAvail;  // safety
  int16_t yStart = (topAvail - totalTitleH) / 2 + ascT;
  drawCentered(line1, yStart);
  if (line2[0] != '\0') {
    drawCentered(line2, yStart + lhT + 2);
  }

  // 2) Bottom line: distance, seats, altitude
  u8g2.setFont(bottomFont);
  int16_t descent = u8g2.getDescent();
  int16_t yBottom = SCREEN_HEIGHT - 1 - (descent < 0 ? -descent : descent);
  drawBottomBar(fi, isPseudo, yBottom);

  u8g2.sendBuffer();
  { uint32_t d = millis() - renderStart; if (d > g_renderMaxMs) g_renderMaxMs = d; }
}

void setup() {
  // Simple, explicit relay init and boot state
  relaysInit();

  Serial.begin(115200);
  delay(20);
  // Before the first log line: three tasks (loop, network, Wi-Fi events) all
  // write to Serial, and unguarded writes shred each other's lines.
  logInit();
  LOG_INFO("Boot: Flight Display starting");
#if defined(ESP32)
  LOG_INFO("Boot: reset reason %s", resetReasonStr());
  // The Arduino core initializes the TWDT at 5s but only watches the idle task.
  // Widen it and subscribe loopTask so a wedged loop() reboots instead of
  // hanging silently forever on a 24/7 device.
  {
    esp_task_wdt_config_t wdt = {
      .timeout_ms = LOOP_WDT_TIMEOUT_S * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
    };
    esp_err_t re = esp_task_wdt_reconfigure(&wdt);
    esp_err_t ad = esp_task_wdt_add(NULL);
    LOG_INFO("Task WDT: %ds (reconfigure=%d add=%d)", LOOP_WDT_TIMEOUT_S, (int)re, (int)ad);
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
        LOG_INFO("WiFi: got IP %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
        wifiConnecting = false;
        // Raise TX power after association; disable modem sleep for responsiveness
        WiFi.setTxPower(WIFI_RUN_TXPOWER);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);  // may have been disabled by a test drop
        // Fetch as soon as the link is back. Deferring link-down cycles to the
        // normal 30s interval otherwise leaves the display stale for up to a
        // full interval after recovery (measured at 26s before this line).
        g_nextFetchAt = millis();
        // Wi‑Fi up; print reminder of server endpoint
        LOG_INFO("HTTP server ready on port 80");
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

  // Network task on core 0, away from the rendering that loop() does on core 1.
  g_netMtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(netTaskFn, "net", NET_TASK_STACK, nullptr, 1,
                          &g_netTaskHandle, 0);
  LOG_INFO("Network task started (%d byte stack)", (int)NET_TASK_STACK);
  // Display init (U8g2 handles SPI HW init for HW SPI constructor)
  if (!u8g2.begin()) {
    LOG_ERROR("Display init failed");
  }
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

// The only place that blocks on the network. Never subscribed to the task
// watchdog: a 31s DNS-plus-connect-plus-read is legitimate here, and the point
// of moving it off loop() was precisely so it cannot trip that watchdog. A
// genuinely hung job is caught instead by the stall check in loop().
static void netTaskFn(void *) {
  for (;;) {
    NetJob job = NET_NONE;
    FlightInfo target;

    NET_LOCK();
    if (g_netPending != NET_NONE && !g_netDone) {
      job = g_netPending;
      g_netPending = NET_NONE;
      target = g_netTarget;
      g_netBusy = true;
      g_netBusySince = millis();
    }
    NET_UNLOCK();

    if (job == NET_NONE) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    FlightInfo out;
    FetchResult st = FETCH_FAIL;
    if (job == NET_API) {
      st = fetchNearestFlight(out);
    } else {
      out = target;
      st = fetchLocalUpdate(out) ? FETCH_OK : FETCH_FAIL;
    }

    NET_LOCK();
    g_netResult = out;
    g_netStatus = st;
    g_netDoneKind = job;
    g_netBusy = false;
    g_netDone = true;
    g_statNetJobs++;
    NET_UNLOCK();
  }
}

// Stop showing a flight that can no longer be refreshed and put the dimmed
// splash up instead. Shared by the fetch-failed and link-down paths, because
// the display must age out either way.
static void expireStaleDisplay() {
  if (g_haveDisplayed && (millis() - g_lastDataMs) > STALE_DISPLAY_MAX_MS) {
    LOG_WARN("Displayed data stale > %lu ms, clearing", (unsigned long)STALE_DISPLAY_MAX_MS);
    g_haveDisplayed = false;
    g_lastShown = FlightInfo();
  }
  if (!g_haveDisplayed) {
    showSplash("No data", "Check Wi-Fi/API");
  }
}

void loop() {
  {
    // Interval since the previous entry == duration of the previous iteration,
    // so every early return below is accounted for without touching any of them.
    static uint32_t s_lastEntry = 0;
    uint32_t entry = millis();
    if (s_lastEntry) {
      uint32_t d = entry - s_lastEntry;
      if (d > g_loopMaxMs) g_loopMaxMs = d;
      if (d > 50) g_loopSlow50++;
      if (d > 500) g_loopSlow500++;
      if (d > 5000) g_loopSlow5000++;
    }
    s_lastEntry = entry;
    g_loopCount++;
  }

  if (g_wifiDropUntil && (int32_t)(millis() - g_wifiDropUntil) >= 0) {
    g_wifiDropUntil = 0;
    // Auto-reconnect stays off: connectWiFi() is now the only thing that can
    // restore the link, which is exactly the path under test.
    LOG_WARN("TEST: Wi-Fi hold expired, reconnect allowed");
    wifiLastAttemptAt = millis() - WIFI_RETRY_INTERVAL_MS;  // retry immediately
  }
  // Everything else still runs while the link is held down, so the fetch,
  // staleness and display paths are observed under a real disconnection.
  if (WiFi.status() != WL_CONNECTED && !g_wifiDropUntil) {
    connectWiFi();
  }

  esp_task_wdt_reset();

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

  // Re-render on pixel-shift step changes — a static scene never re-renders
  // through the data path, so the anti-sticking nudge must force it
  static uint8_t s_lastShiftIdx = 0;
  uint8_t shiftIdx = pixelShiftIdx();
  if (shiftIdx != s_lastShiftIdx) {
    s_lastShiftIdx = shiftIdx;
    if (g_haveDisplayed) renderFlight(g_lastShown);
    else if (g_splashActive) drawSplash(g_splashTop, g_splashBottom[0] ? g_splashBottom : nullptr);
  }

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

#if FEATURE_LOCAL_RX
  // Keep the live numbers current between API polls. Only ever a request —
  // the blocking fetch happens on the network task.
  if ((int32_t)(millis() - g_nextLocalAt) >= 0 && g_haveDisplayed && g_lastShown.hex[0]) {
    if (netRequest(NET_LOCAL, &g_lastShown)) {
      g_nextLocalAt = millis() + LOCAL_RX_INTERVAL_MS;
    }
  }
#endif

  uint32_t now = millis();

  // ---- 1. Apply anything the network task has finished ----
  // Collected FIRST, unconditionally. Putting this after the "is a fetch due"
  // branch deadlocked the whole thing: g_nextFetchAt only advances when a result
  // is applied, so the due branch stayed true forever and returned before ever
  // reaching the collect.
  {
    NetJob kind = NET_NONE;
    FlightInfo nearest;
    FetchResult fr = FETCH_FAIL;
    if (netCollect(kind, nearest, fr)) {
#if FEATURE_LOCAL_RX
      if (kind == NET_LOCAL) {
        // Discard if the display moved to a different aircraft while this was in
        // flight — the position would belong to the wrong airframe.
        if (fr == FETCH_OK && g_haveDisplayed
            && strcasecmp(nearest.hex, g_lastShown.hex) == 0
            && !sameFlightDisplay(nearest, g_lastShown)) {
          g_lastShown = nearest;
          renderFlight(g_lastShown);
        }
        yield();
        return;
      }
#endif
      if (fr == FETCH_OK) {
        g_statOk++;
        g_fetchFailCount = 0;
        g_lastDataMs = millis();
        g_nextFetchAt = millis() + FETCH_INTERVAL_MS;
        if (!g_haveDisplayed || !sameFlightDisplay(nearest, g_lastShown)) {
          renderFlight(nearest);
          g_lastShown = nearest;
          g_haveDisplayed = true;
          relaysShowCategory(g_lastShown.opClass);
        }
      } else if (fr == FETCH_EMPTY) {
        g_statEmpty++;
        g_fetchFailCount = 0;
        g_nextFetchAt = millis() + FETCH_INTERVAL_MS;
        if (g_haveDisplayed) {
          g_haveDisplayed = false;
          g_lastShown = FlightInfo();
        }
        showSplash("No aircraft nearby");
      } else {
        g_statFail++;
        if (WiFi.status() == WL_CONNECTED) {
          g_fetchFailCount = min((uint8_t)(g_fetchFailCount + 1), (uint8_t)255);
          if (g_fetchFailCount >= FETCH_FAIL_RESTART_THRESHOLD) {
            LOG_ERROR("API unreachable for %d attempts, restarting", g_fetchFailCount);
            ESP.restart();
          }
        }
        uint32_t retryMs = backoffMs(max(g_fetchFailCount, (uint8_t)1));
        g_nextFetchAt = millis() + retryMs;
        LOG_WARN("Fetch failed (attempt %d), next retry in %lu ms",
                 g_fetchFailCount, (unsigned long)retryMs);
        expireStaleDisplay();
      }
      yield();
      return;
    }
  }

  // ---- 2. A wedged job is caught here; nothing else would notice ----
  {
    NET_LOCK();
    bool stuck = g_netBusy && (millis() - g_netBusySince) > NET_STALL_RESTART_MS;
    NET_UNLOCK();
    if (stuck) {
      LOG_ERROR("Network task stalled > %lu ms, restarting",
                (unsigned long)NET_STALL_RESTART_MS);
      ESP.restart();
    }
  }

  // ---- 3. Ask for a new fetch when one is due ----
  if ((int32_t)(now - g_nextFetchAt) >= 0) {
    checkHeap();

    // A down link is not a failed fetch — no attempt was made. Counting it as
    // one froze g_fetchFailCount at 0 (correctly, so the API circuit breaker
    // cannot misfire) which pinned the retry interval at the backoff floor.
    static bool s_linkDown = false;
    if (WiFi.status() != WL_CONNECTED) {
      if (!s_linkDown) {
        s_linkDown = true;
        LOG_WARN("Wi-Fi down; deferring fetches until the link returns");
      }
      g_nextFetchAt = millis() + FETCH_INTERVAL_MS;
      expireStaleDisplay();
      yield();
      return;
    }
    if (s_linkDown) {
      s_linkDown = false;
      LOG_INFO("Wi-Fi back; resuming fetches");
    }
    netRequest(NET_API, nullptr);
  }

  // Keep relays consistent during boot
  if (!g_haveDisplayed) {
    relaysPowerOnly();
  }

  // Cooperative yield without blocking
  yield();
}

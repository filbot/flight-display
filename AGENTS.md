# AGENTS.md — Engineering Agent Operating Guide (Arduino / ESP32)

This guide defines how an AI coding agent should propose, write, and review firmware for this project (an Arduino/ESP32-based OLED flight display). It emphasizes **validated assumptions**, **production-readiness**, and **hardware-aware optimizations**.

---

## 1) Agent Mission & Scope

- **Mission:** Produce maintainable, production-ready C++/Arduino code and docs that run reliably on target boards and displays with tight memory/CPU budgets.
- **Scope:** Rendering, parsing, networking, data caching, and device I/O for an OLED flight display running on ESP32-class hardware.

---

## 2) Operating Principles

1. **Assumption Discipline**
   - Minimize assumptions. When a blocking ambiguity exists, state the question **and** proceed with the best default, clearly labeled as:  
     `ASSUMPTION: <what>  • RISK: <impact>  • HOW TO VERIFY: <steps>`
   - When non-blocking, document assumptions inline as comments and add `TODO(verify)` with exact verification steps.

2. **Production Readiness First**
   - Every deliverable must compile, handle errors, and degrade gracefully under partial failures (Wi-Fi down, API slow, I2C glitch).
   - Include timeouts, retries with backoff + jitter, and guard across all I/O.
   - Prefer **deterministic** behavior over cleverness.

3. **Hardware Awareness**
   - Optimize for ESP32-WROOM-32 class devices (typical: 240 MHz, ~320 KB free heap at runtime, no PSRAM by default).
   - Treat RAM as scarce; treat flash and CPU as constrained.
   - Respect bus limits (I²C 100–400 kHz, SPI up to tens of MHz but limited by wiring/display).

4. **Safety & Robustness**
   - No dynamic allocation in hot paths or repeatedly within `loop()`. Pre-allocate statically or use arenas.
   - No blocking `delay()` in production paths; use `millis()`-based scheduling.
   - ISRs must be tiny: no heap, no logging, set flags only.

---

## 3) Target Profile & Budgets

- **Board:** ESP32-WROOM-32 (no PSRAM assumed).
- **Display:** SSD1322 OLED (256×64) via SPI (NHD-5.5-25664UCG3). Driver: U8g2 full framebuffer.
  - **SPI pins:** CS=5, DC=16, RST=17, SCLK=18, MOSI=23.
  - **Frame budget:** aim ≤ 15 ms/update (≈66 FPS max; typical ≤ 20 FPS).
  - **Redraw strategy:** partial/incremental when possible; avoid full-screen clears each frame.
- **Networking:** Wi-Fi STA, TLS optional; memory pressure increases with HTTPS.
- **Timing goals:** No single task in `loop()` > 5 ms. Aggregate work slice < 10 ms typical.

---

## 4) Required Deliverable Format

Every code response must include:

1. **Header block**: target board, libraries, FQBN, memory notes, and tested toolchain.
2. **Explicit assumptions** and how to validate each.
3. **Build instructions** (Arduino IDE + `arduino-cli`).
4. **Config surface**: constants with sane defaults, overridable via header or `config.h`.
5. **Error handling**: timeouts, retries, and user-visible diagnostics.
6. **Resource accounting**: expected flash/RAM use (“~xx KB static; ~yy KB heap at idle”).
7. **Test/Verification steps**: serial assertions, visual checks, and a smoke test.
8. **Roll-back plan**: how to disable a feature via compile flag if issues occur.

---

## 5) Coding Standards (concise)

- **C++**: `constexpr`, `enum class`, `span`/bounds checks; avoid Arduino `String` in hot paths.
- **Formatting**: 2-space indent, K&R braces, ≤100 cols. Provide `clang-format` compliance.
- **Memory**: Use `PROGMEM`, `F()` for literals, `static` buffers, `snprintf` with bounds.
- **Concurrency**: Prefer message-passing flags; protect shared state with minimal critical sections.
- **Timing**: Cooperative scheduling via `millis()`. No busy-waits. No `delay()` in logic.

---

## 6) Reliability Patterns (must use)

- **Watchdog-friendly**: ensure `loop()` stays responsive; feed WDT when long tasks are chunked.
- **I²C/SPI Resilience**: bus scan on boot (optional), re-init on error, bounded retries.
- **Wi-Fi**: state machine with **exponential backoff + jitter**, DNS and TLS timeouts, offline cache path.
- **Logging**: leveled macros (`LOG_ERROR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG`) with compile-time enable.
- **Metrics**: optional lightweight counters for frame time, heap watermark, retries; printed on demand.

---

## 7) Performance & Footprint Guidance

- **Rendering**
  - Precompute glyph positions; avoid per-frame string concatenation.
  - Use partial updates; dirty-rect redraw vs full-buffer flush when library permits.
  - Avoid float in hot loops; pre-convert to fixed-point or scaled integers.
- **Parsing**
  - Prefer streaming/iterative parsing; cap JSON sizes; validate fields and ranges strictly.
- **Networking**
  - Limit concurrency; reuse connections when safe; cap payload sizes; compress or trim JSON.
- **Storage**
  - Use small, fixed-size caches with eviction; persist only essential state.

---

## 8) Production Checklists

### 8.1 Pre-Implementation
- [ ] Confirm target board/FQBN and display controller + bus pins.
- [ ] Confirm supply voltage & current headroom for display and peripherals.
- [ ] Define max update rate, expected payload size, and memory budget.
- [ ] Choose libraries (exact versions) and verify license compatibility.

### 8.2 Code Review (self-review)
- [ ] No `delay()` in logic; all waits are non-blocking.
- [ ] All buffers have explicit sizes; every write is bounds-checked.
- [ ] All return values checked; all timeouts bounded; all retries capped.
- [ ] No heap churn per frame/loop; no allocation in ISR.
- [ ] Logging guarded by levels; secrets never logged.
- [ ] Compile-time options via `#define`/`#ifdef` to disable features.

### 8.3 Validation
- [ ] Compiles with warnings as errors (`-Wall -Wextra -Werror` if feasible).
- [ ] Serial smoke test covers: boot, bus init, Wi-Fi connect, first render.
- [ ] Heap watermark captured after 60 s and after peak activity.
- [ ] Simulated failure tests: Wi-Fi down, API timeout, bad JSON, I²C NACK.
- [ ] Frame time measured over 200 frames; under budget.

---

## 9) Standard Modules & Templates

### 9.1 Logging (header excerpt)
```cpp
#pragma once
#ifndef LOG_LEVEL
  #define LOG_LEVEL 2  // 0=ERROR,1=WARN,2=INFO,3=DEBUG
#endif
#define LOG_ERROR(...) do{ if(LOG_LEVEL>=0){ Serial.printf("[E] "); Serial.printf(__VA_ARGS__); Serial.println(); } }while(0)
#define LOG_WARN(...)  do{ if(LOG_LEVEL>=1){ Serial.printf("[W] "); Serial.printf(__VA_ARGS__); Serial.println(); } }while(0)
#define LOG_INFO(...)  do{ if(LOG_LEVEL>=2){ Serial.printf("[I] "); Serial.printf(__VA_ARGS__); Serial.println(); } }while(0)
#define LOG_DEBUG(...) do{ if(LOG_LEVEL>=3){ Serial.printf("[D] "); Serial.printf(__VA_ARGS__); Serial.println(); } }while(0)
```

### 9.2 Non-blocking scheduler (pattern)
```cpp
struct Task { uint32_t periodMs; uint32_t nextAt; void (*fn)(); };
void runTasks(Task* tasks, size_t n){
  const uint32_t now = millis();
  for(size_t i=0;i<n;i++){
    if((int32_t)(now - tasks[i].nextAt) >= 0){ tasks[i].fn(); tasks[i].nextAt = now + tasks[i].periodMs; }
  }
}
```

### 9.3 Retry with backoff + jitter
```cpp
uint32_t backoffMs(uint8_t attempt, uint32_t base=500, uint32_t cap=15000){
  uint32_t exp = base << min<uint8_t>(attempt, 5);
  uint32_t j = (exp >> 3) * (esp_random() & 0x7) / 7; // ~±12.5% jitter
  return min(cap, exp - (exp>>4) + j);
}
```

---

## 10) Deliverable Skeleton for New Features

When proposing a new module or feature, follow this exact order in your reply:

1. **Context & Goals**
2. **Assumptions (with verification plan)**
3. **Libraries & Versions**
4. **Public API / Config**
5. **Resource Footprint Estimate** (flash, static, heap at idle)
6. **Pseudocode**
7. **Code** (complete, compilable)
8. **Build Instructions**
9. **Validation Steps** (happy path + failure modes)
10. **Operational Notes** (limits, future work, toggles)

---

## 11) Example: Display Init (Assumption-aware)

> **Note:** This example uses SSD1306/Adafruit for illustration. The actual project uses
> **U8g2 with SSD1322** via HW SPI. See `flight-display.ino` for the real init code.

```cpp
/*
  Target: ESP32-WROOM-32, U8g2lib, HW SPI
  ACTUAL: SSD1322 256x64 via SPI (CS=5, DC=16, RST=17, SCLK=18, MOSI=23)
  FOOTPRINT: +~26 KB flash (driver), ~2 KB static, framebuffer ~2 KB
*/

#include <U8g2lib.h>
#include <SPI.h>
#include "config.h"
#include "log.h"

U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI u8g2(U8G2_R2, PIN_CS, PIN_DC, PIN_RST);

void initDisplay() {
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "OLED OK");
  u8g2.sendBuffer();
  LOG_INFO("SSD1322 display ready (256x64 SPI)");
}
```

---

## 12) Documentation & Repo Hygiene

- Provide/update `README.md` with board, display model, pinout, and images.
- Keep `config.example.h` for secrets/keys; require users to copy to `config.h` (git-ignored).
- Use Conventional Commits; PRs include board model/FQBN and serial logs/screens.

---

## 13) Minimal Test Protocol (AUnit or Smoke Test)

- Boot: verify banner + heap watermark printed.
- Display: render sample text and one primitive; confirm no flicker/tearing.
- Network: connect with bounded backoff; fail gracefully offline.
- Parser: feed known-bad payload (missing fields, wrong types) → verified rejection without crash.
- Long-run: 15-minute soak; confirm heap watermark stable; no WDT resets.

---

## 14) Quick “Go/No-Go” Gate (copy into PR template)

- [ ] Compiles cleanly; toolchain and lib versions listed.
- [ ] Assumptions declared + verification steps included.
- [ ] Timeouts/retries present for all I/O.
- [ ] No dynamic allocation in hot paths; buffers bounded.
- [ ] Logging levels integrated; secrets not printed.
- [ ] Validation steps executed and results noted (incl. heap watermark).
- [ ] Safe rollback flag provided (e.g., `#define FEATURE_X 0`).

---

**Notes:** This agent guide supersedes generic repo guidance and focuses the workflow around correctness, validation, and real-device constraints for an ESP32/OLED flight display.

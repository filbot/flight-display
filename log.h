// Lightweight logging macros with compile-time levels
// 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#ifndef LOG_TAG
#define LOG_TAG "FD"
#endif

// Long enough for the longest line we emit (a full API URL plus prefix).
#ifndef LOG_LINE_MAX
#define LOG_LINE_MAX 320
#endif

// Logging is called from BOTH loopTask and the network task. The previous
// macros made three separate Serial calls per line (prefix, body, newline), so
// concurrent logs interleaved at those boundaries and the serial capture came
// out shredded — fragments of two lines spliced together, which reads exactly
// like dropped bytes and sent an investigation looking for a flaky USB cable.
//
// One formatted buffer, one guarded emit.
inline SemaphoreHandle_t &logMutexRef() {
  static SemaphoreHandle_t m = nullptr;
  return m;
}

inline void logInit() {
  if (!logMutexRef()) logMutexRef() = xSemaphoreCreateMutex();
}

inline void logEmit(char level, const char *fmt, ...) {
  char line[LOG_LINE_MAX];
  int n = snprintf(line, sizeof(line), "[%c][%s][%lu] ", level, LOG_TAG,
                   (unsigned long)millis());
  if (n < 0) n = 0;
  if (n < (int)sizeof(line)) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
  }
  SemaphoreHandle_t m = logMutexRef();
  if (m) xSemaphoreTake(m, portMAX_DELAY);
  Serial.println(line);
  if (m) xSemaphoreGive(m);
}

#define LOG_ERROR(...) do { if (LOG_LEVEL >= 0) logEmit('E', __VA_ARGS__); } while (0)
#define LOG_WARN(...)  do { if (LOG_LEVEL >= 1) logEmit('W', __VA_ARGS__); } while (0)
#define LOG_INFO(...)  do { if (LOG_LEVEL >= 2) logEmit('I', __VA_ARGS__); } while (0)
#define LOG_DEBUG(...) do { if (LOG_LEVEL >= 3) logEmit('D', __VA_ARGS__); } while (0)

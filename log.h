// Lightweight logging macros with compile-time levels
// 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
#pragma once

#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#ifndef LOG_TAG
#define LOG_TAG "FD"
#endif

#define LOG_ERROR(...) \
  do { \
    if (LOG_LEVEL >= 0) { \
      Serial.printf("[E][%s][%lu] ", LOG_TAG, (unsigned long)millis()); \
      Serial.printf(__VA_ARGS__); \
      Serial.println(); \
    } \
  } while (0)
#define LOG_WARN(...) \
  do { \
    if (LOG_LEVEL >= 1) { \
      Serial.printf("[W][%s][%lu] ", LOG_TAG, (unsigned long)millis()); \
      Serial.printf(__VA_ARGS__); \
      Serial.println(); \
    } \
  } while (0)
#define LOG_INFO(...) \
  do { \
    if (LOG_LEVEL >= 2) { \
      Serial.printf("[I][%s][%lu] ", LOG_TAG, (unsigned long)millis()); \
      Serial.printf(__VA_ARGS__); \
      Serial.println(); \
    } \
  } while (0)
#define LOG_DEBUG(...) \
  do { \
    if (LOG_LEVEL >= 3) { \
      Serial.printf("[D][%s][%lu] ", LOG_TAG, (unsigned long)millis()); \
      Serial.printf(__VA_ARGS__); \
      Serial.println(); \
    } \
  } while (0)

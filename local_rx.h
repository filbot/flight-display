// Extracting one aircraft record from a dump1090 aircraft.json stream without
// parsing the whole body. Split into its own header so tools/hosttest can
// exercise it against real captured payloads: it is hand-rolled scanning in a
// hot path, and it has already shipped one bug (a fixed `"hex":"..."` needle
// that never matched this receiver's `"hex": "..."` spacing).
#pragma once
#include <Arduino.h>
#include <string.h>
#include <ctype.h>

#ifndef LOCAL_RX_READ_TIMEOUT_MS
#define LOCAL_RX_READ_TIMEOUT_MS 3000
#endif

// Match `"hex"` followed by optional whitespace, a colon, more optional
// whitespace, a quote, the target, and a closing quote. Emitters differ: this
// receiver pretty-prints as `"hex": "a24ba7"` with a space, while dump1090's
// own writer uses none. Matching a fixed `"hex":"..."` found nothing at all.
// Returns the position of the `"hex"` token on a match, else nullptr.
static char* matchHexToken(char* s, const char* hexLower) {
  for (char* p = strstr(s, "\"hex\""); p; p = strstr(p + 1, "\"hex\"")) {
    char* q = p + 5;                       // past "hex"
    while (*q == ' ' || *q == '\t') ++q;
    if (*q != ':') continue;
    ++q;
    while (*q == ' ' || *q == '\t') ++q;
    if (*q != '"') continue;
    ++q;
    size_t i = 0;
    while (hexLower[i] && tolower((unsigned char)q[i]) == hexLower[i]) ++i;
    if (hexLower[i] == '\0' && q[i] == '"') return p;
  }
  return nullptr;
}

// Records run to ~525 bytes on this receiver and can be larger with nav/wind
// fields present, so the caller's buffer must have real headroom. `truncated`
// separates "the buffer was too small" from "the aircraft is not in the feed":
// an undersized buffer previously returned false and looked exactly like the
// aircraft simply not being heard, which hid the bug on device entirely.
static bool extractLocalRecord(Stream &in, const char* hexLower, char* out, size_t outSize,
                               bool* truncated = nullptr) {
  if (truncated) *truncated = false;
  // Long enough to hold `"hex"` plus spacing, the colon, quotes and the value,
  // so a token split across a chunk boundary is still matched.
  const size_t nlen = 20;
  const size_t CH = 512;
  char chunk[CH + 1];
  char carry[24] = "";      // tail of the previous chunk, so a needle spanning
  size_t carryLen = 0;      // a boundary is still found
  size_t outLen = 0;
  bool capturing = false;
  uint32_t start = millis();

  while ((millis() - start) < LOCAL_RX_READ_TIMEOUT_MS) {
    int n = in.readBytes(chunk, CH);
    if (n <= 0) break;
    chunk[n] = '\0';

    if (!capturing) {
      char joined[sizeof(carry) + CH + 1];
      memcpy(joined, carry, carryLen);
      memcpy(joined + carryLen, chunk, (size_t)n + 1);
      char *hit = matchHexToken(joined, hexLower);
      if (hit) {
        capturing = true;
        out[outLen++] = '{';   // "hex" is the first field of each record
        for (char *p = hit; *p; ++p) {
          if (outLen >= outSize - 1) { if (truncated) *truncated = true; return false; }
          out[outLen++] = *p;
          if (*p == '}') { out[outLen] = '\0'; return true; }
        }
      } else {
        carryLen = ((size_t)n >= nlen) ? nlen - 1 : (size_t)n;
        memcpy(carry, chunk + n - carryLen, carryLen);
        carry[carryLen] = '\0';
      }
    } else {
      for (int i = 0; i < n; ++i) {
        if (outLen >= outSize - 1) { if (truncated) *truncated = true; return false; }
        out[outLen++] = chunk[i];
        if (chunk[i] == '}') { out[outLen] = '\0'; return true; }
      }
    }
  }
  return false;
}


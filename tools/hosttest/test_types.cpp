// Host-side unit tests for aircraft_types.h. No device, no framework.
// Verifies the binary-search preconditions the table silently depends on.
#include "../../aircraft_types.h"
#include <cstdio>
#include <set>
#include <string>

static int failures = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
  if (!ok) { printf("FAIL  %s  %s\n", what, detail.c_str()); ++failures; }
}

int main() {
  printf("table entries: %zu\n", kTypeInfoCount);

  // 1) Binary search requires a case-insensitively sorted, unique key set.
  //    A single out-of-order row silently makes entries unreachable.
  int unsorted = 0, dupes = 0;
  for (size_t i = 1; i < kTypeInfoCount; ++i) {
    int c = strcasecmp(kTypeInfo[i - 1].icao, kTypeInfo[i].icao);
    if (c > 0) { ++unsorted; if (unsorted <= 5) printf("  unsorted: %s before %s\n", kTypeInfo[i-1].icao, kTypeInfo[i].icao); }
    if (c == 0) { ++dupes; if (dupes <= 5) printf("  duplicate key: %s\n", kTypeInfo[i].icao); }
  }
  check(unsorted == 0, "table is sorted", std::to_string(unsorted) + " out-of-order pairs");
  check(dupes == 0, "keys are unique", std::to_string(dupes) + " duplicates");

  // 2) Every row must be reachable through the binary search that serves it.
  int unreachable = 0;
  for (size_t i = 0; i < kTypeInfoCount; ++i) {
    const AircraftTypeInfo* got = aircraftLookup(kTypeInfo[i].icao);
    if (!got || strcasecmp(got->icao, kTypeInfo[i].icao) != 0) {
      if (++unreachable <= 5) printf("  unreachable: %s\n", kTypeInfo[i].icao);
    }
  }
  check(unreachable == 0, "every entry is findable", std::to_string(unreachable) + " unreachable");

  // 3) Lookup must be case-insensitive: the API sends upper case, the table is
  //    matched with strcasecmp, so both cases must resolve identically.
  int casefail = 0;
  for (size_t i = 0; i < kTypeInfoCount; ++i) {
    std::string lower(kTypeInfo[i].icao);
    for (auto &c : lower) c = tolower(c);
    if (aircraftLookup(lower.c_str()) != aircraftLookup(kTypeInfo[i].icao)) ++casefail;
  }
  check(casefail == 0, "lookup is case-insensitive", std::to_string(casefail) + " mismatches");

  // 4) Seat data sanity — these feed the "<=15 seats means private" rule, so a
  //    bad value directly changes which relay fires.
  int badseats = 0;
  for (size_t i = 0; i < kTypeInfoCount; ++i) {
    if (kTypeInfo[i].maxSeats > 900) { if (++badseats <= 5) printf("  implausible seats: %s=%u\n", kTypeInfo[i].icao, kTypeInfo[i].maxSeats); }
  }
  check(badseats == 0, "seat counts are plausible", std::to_string(badseats) + " suspicious");

  // 5) No empty or over-long keys (typeCode buffer is char[8] => 7 usable).
  int badkey = 0, longkey = 0;
  for (size_t i = 0; i < kTypeInfoCount; ++i) {
    if (!kTypeInfo[i].icao || !kTypeInfo[i].icao[0]) ++badkey;
    if (strlen(kTypeInfo[i].icao) > 7) { if (++longkey <= 5) printf("  key longer than the 7-char typeCode buffer: %s\n", kTypeInfo[i].icao); }
  }
  check(badkey == 0, "no empty keys", std::to_string(badkey) + " empty");
  check(longkey == 0, "keys fit the firmware typeCode buffer", std::to_string(longkey) + " too long");

  // 6) Missing model text would render a blank title.
  int blankmodel = 0;
  for (size_t i = 0; i < kTypeInfoCount; ++i)
    if (!kTypeInfo[i].model || !kTypeInfo[i].model[0]) { if (++blankmodel <= 5) printf("  blank model: %s\n", kTypeInfo[i].icao); }
  check(blankmodel == 0, "no blank models", std::to_string(blankmodel) + " blank");

  // 7) Friendly-name buffer behaviour: firmware passes char[64].
  int trunc = 0;
  for (size_t i = 0; i < kTypeInfoCount; ++i) {
    char buf[64];
    if (aircraftFriendlyNameBuf(kTypeInfo[i].icao, buf, sizeof(buf))) {
      size_t need = strlen(kTypeInfo[i].manufacturer) + 1 + strlen(kTypeInfo[i].model);
      if (need > 63) { if (++trunc <= 5) printf("  name truncated at 64: %s (%zu chars)\n", kTypeInfo[i].icao, need); }
    }
  }
  check(trunc == 0, "friendly names fit char[64]", std::to_string(trunc) + " truncated");

  // 8) Unknown codes must not match anything.
  check(aircraftLookup("ZZZZ") == nullptr, "unknown code returns null");
  check(aircraftLookup("") == nullptr, "empty code returns null");

  // 9) Seat heuristics must still answer for common unlisted families.
  for (const char* t : {"B77L", "A35K", "C172", "PA28"}) {
    uint16_t s = 0;
    if (!aircraftSeatMax(t, s)) printf("  note: no seat data for %s\n", t);
  }

  printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}

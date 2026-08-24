// Minimal Arduino shim so aircraft_types.h compiles on the host for unit tests.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <string>
#include <strings.h>
using std::size_t;
struct String {
  std::string s;
  String() {}
  String(const char* p) : s(p ? p : "") {}
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  void trim() {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
  }
  void toUpperCase() { for (auto &c : s) c = toupper(c); }
  String operator+(const char* rhs) const { String r; r.s = s + (rhs ? rhs : ""); return r; }
  String operator+(const String& rhs) const { String r; r.s = s + rhs.s; return r; }
  bool operator==(const char* rhs) const { return s == (rhs ? rhs : ""); }
};
inline String operator+(const char* lhs, const String& rhs) { String r; r.s = std::string(lhs ? lhs : "") + rhs.s; return r; }

// --- host-test shims for stream/timing used by local_rx.h ---
#include <chrono>
inline unsigned long millis() {
  static auto t0 = std::chrono::steady_clock::now();
  return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
}
class Stream {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual size_t write(uint8_t) = 0;
  virtual size_t readBytes(char *buffer, size_t length) = 0;
  virtual ~Stream() {}
};

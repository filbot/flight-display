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
};

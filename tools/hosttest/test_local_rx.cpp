// Host test for the local-receiver record extractor, run against a REAL
// captured aircraft.json. This parser is hand-rolled scanning in a hot path and
// already shipped one bug that a device test could not distinguish from "the
// aircraft simply is not being heard".
#include "Arduino.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

// Minimal Stream that serves a string in chunks, mimicking a network read.
class StringStream : public Stream {
 public:
  StringStream(const std::string &s, size_t chunk) : s_(s), chunk_(chunk) {}
  int available() override { return (int)(s_.size() - pos_); }
  int read() override { return pos_ < s_.size() ? (unsigned char)s_[pos_++] : -1; }
  int peek() override { return pos_ < s_.size() ? (unsigned char)s_[pos_] : -1; }
  size_t write(uint8_t) override { return 0; }
  size_t readBytes(char *buf, size_t len) override {
    size_t n = std::min({len, chunk_, s_.size() - pos_});
    memcpy(buf, s_.data() + pos_, n);
    pos_ += n;
    return n;
  }
 private:
  std::string s_; size_t pos_ = 0, chunk_;
};

#include "../../local_rx.h"

static int failures = 0;
static void check(bool ok, const std::string &what) {
  if (!ok) { printf("FAIL  %s\n", what.c_str()); ++failures; }
}

int main(int argc, char **argv) {
  if (argc < 2) { printf("usage: %s <aircraft.json>\n", argv[0]); return 2; }
  std::ifstream f(argv[1]);
  std::stringstream ss; ss << f.rdbuf();
  std::string body = ss.str();
  printf("payload: %zu bytes\n", body.size());

  // pull every hex out of the payload the naive way, as ground truth
  std::vector<std::string> hexes;
  for (size_t i = body.find("\"hex\""); i != std::string::npos; i = body.find("\"hex\"", i + 1)) {
    size_t q = body.find('"', body.find(':', i) + 1);
    size_t e = body.find('"', q + 1);
    hexes.push_back(body.substr(q + 1, e - q - 1));
  }
  printf("aircraft in payload: %zu\n", hexes.size());
  check(!hexes.empty(), "payload contains aircraft");

  // Every hex must be findable, at several chunk sizes, so a record or token
  // straddling a read boundary is still matched.
  for (size_t chunk : {64u, 128u, 512u, 4096u}) {
    int found = 0;
    for (const auto &h : hexes) {
      std::string lower = h;
      for (auto &c : lower) c = tolower(c);
      StringStream st(body, chunk);
      char out[768];  // must match the firmware's buffer
      bool trunc = false;
      if (extractLocalRecord(st, lower.c_str(), out, sizeof(out), &trunc)) {
        found++;
        // the record must be valid-looking and be the RIGHT aircraft
        if (!strstr(out, lower.c_str())) { printf("  chunk %zu: %s -> wrong record\n", chunk, h.c_str()); }
      } else if (found < 3) {
        printf("  chunk %zu: %s %s\n", chunk, h.c_str(), trunc ? "TRUNCATED" : "NOT FOUND");
      }
    }
    printf("chunk %-5zu: found %d/%zu\n", chunk, found, hexes.size());
    check(found == (int)hexes.size(), "all hexes found at chunk " + std::to_string(chunk));
  }

  // A hex that is not present must not match anything
  StringStream st(body, 512);
  char out[768];
  check(!extractLocalRecord(st, "zzzzzz", out, sizeof(out)), "absent hex returns false");

  // an undersized buffer must report truncation, never a silent miss
  {
    std::string lower = hexes[0];
    for (auto &c : lower) c = tolower(c);
    StringStream small(body, 512);
    char tiny[80];
    bool trunc = false;
    bool got = extractLocalRecord(small, lower.c_str(), tiny, sizeof(tiny), &trunc);
    check(!got && trunc, "undersized buffer reports truncation rather than a silent miss");
  }
  printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}

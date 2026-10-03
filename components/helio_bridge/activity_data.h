#pragma once
#include <cstdint>
#include <vector>
#include "protocol.h"
#include "diagnostic_codec.h"

namespace esphome::helio_bridge::activity_data {
// Timestamp reported by the strap, including its signed quarter-hour UTC offset.
inline uint32_t timestamp(const uint8_t *p) {
  const int y = p[0] | (p[1] << 8), m = p[2], d = p[3];
  const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  const int days[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 || d > days[m-1] ||
      p[4] > 23 || p[5] > 59 || p[6] > 59 || int8_t(p[7]) < -48 || int8_t(p[7]) > 56) return 0;
  int n = 0;
  for (int year = 1970; year < y; ++year) n += 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  for (int month = 1; month < m; ++month) n += days[month-1];
  return uint32_t(int64_t(n + d - 1) * 86400 + p[4]*3600 + p[5]*60 + p[6] - int8_t(p[7])*900);
}
class Transfer {
 public:
  bool start(uint32_t size, uint32_t first, uint32_t now) {
    raw.clear(); expected_ = size; sequence_ = 0; started_ = false;
    // Bound RAM and reject future/implausible windows, including broken timezone decoding.
    if (size % 8 || size > 120 * 8 || !first || first > now + 60 ||
        first + (size ? size / 8 - 1 : 0) * 60 > now + 60) return false;
    start_time = first; started_ = true; return true;
  }
  bool feed(const uint8_t *p, size_t n) {
    if (!started_ || n < 2 || p[0] != sequence_ || raw.size() + n - 1 > expected_) { started_ = false; return false; }
    ++sequence_; raw.insert(raw.end(), p + 1, p + n); return true;
  }
  bool complete(bool crc_present, uint32_t crc) const {
    return started_ && raw.size() == expected_ && (!crc_present || diagnostic::crc(raw.data(), raw.size()) == crc);
  }
  std::vector<uint8_t> raw;
  uint32_t start_time{0};
 private:
  uint32_t expected_{0}; uint8_t sequence_{0}; bool started_{false};
};
}  // namespace esphome::helio_bridge::activity_data

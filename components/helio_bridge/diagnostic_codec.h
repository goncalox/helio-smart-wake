#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace esphome::helio_bridge::diagnostic {
inline uint32_t crc(const uint8_t *p, size_t n) {
  uint32_t c = 0xffffffff;
  while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0); }
  return ~c;
}
// Zero-run encoding is lossless; all nonzero bytes are literal.
inline std::vector<uint8_t> encode(const uint8_t *p, size_t n) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < n;) {
    if (p[i]) { out.push_back(p[i++]); continue; }
    size_t run = 1;
    while (i + run < n && !p[i + run] && run < 255) ++run;
    out.push_back(0); out.push_back(run); i += run;
  }
  return out;
}
inline bool decode(const uint8_t *p, size_t n, size_t expected, std::vector<uint8_t> &out) {
  out.clear();
  for (size_t i = 0; i < n;) {
    if (p[i]) out.push_back(p[i++]);
    else { if (++i == n || !p[i]) return false; out.insert(out.end(), p[i++], 0); }
    if (out.size() > expected) return false;
  }
  return out.size() == expected;
}
}  // namespace esphome::helio_bridge::diagnostic

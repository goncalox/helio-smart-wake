#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace esphome::helio_bridge::protocol {
inline uint32_t read32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint16_t read16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline void write32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}
inline uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return ~crc;
}
inline std::vector<std::vector<uint8_t>> frame(uint16_t endpoint, const std::vector<uint8_t> &data,
                                             size_t plain_size, uint16_t mtu, uint8_t handle, bool encrypted) {
  std::vector<std::vector<uint8_t>> result;
  const size_t max_write = std::min<size_t>(512, std::max<uint16_t>(23, mtu) - 3);
  size_t offset = 0;
  uint8_t sequence = 0;
  while (offset < data.size()) {
    const size_t header = sequence == 0 ? 11 : 5;
    const size_t count = std::min(data.size() - offset, max_write - header);
    std::vector<uint8_t> out(header + count, 0);
    out[0] = 3;
    out[1] = (sequence == 0 ? 1 : 0) | (offset + count == data.size() ? 6 : 0) | (encrypted ? 8 : 0);
    out[3] = handle;
    out[4] = sequence++;
    if (header == 11) {
      write32(out.data() + 5, plain_size);
      out[9] = uint8_t(endpoint);
      out[10] = uint8_t(endpoint >> 8);
    }
    std::copy_n(data.data() + offset, count, out.data() + header);
    result.push_back(std::move(out));
    offset += count;
  }
  return result;
}
struct Message {
  uint16_t endpoint{};
  uint32_t plain_size{};
  uint8_t handle{}, count{};
  bool encrypted{}, ack{};
  std::vector<uint8_t> data;
};
class Receiver {
 public:
  void reset() { active_ = false; data_.clear(); }
  // -1 malformed, 0 incomplete/not a data frame, 1 complete.
  int feed(const uint8_t *p, size_t n, Message &out) {
    if (n < 5 || p[0] != 3) return 0;
    const bool first = p[1] & 1;
    const size_t header = first ? 11 : 5;
    if (n < header) { reset(); return -1; }
    if (first) {
      reset();
      if (p[4] != 0) return -1;
      plain_size_ = read32(p + 5);
      if (plain_size_ == 0 || plain_size_ > 2048) return -1;
      encrypted_ = p[1] & 8;
      expected_size_ = encrypted_ ? ((plain_size_ + 8 + 15) / 16) * 16 : plain_size_;
      endpoint_ = read16(p + 9);
      handle_ = p[3];
      next_count_ = 0;
      active_ = true;
    }
    if (!active_ || p[3] != handle_ || p[4] != next_count_ || bool(p[1] & 8) != encrypted_ ||
        data_.size() + n - header > expected_size_) { reset(); return -1; }
    next_count_++;
    data_.insert(data_.end(), p + header, p + n);
    if (!(p[1] & 2)) return 0;
    if (data_.size() != expected_size_) { reset(); return -1; }
    out = {endpoint_, plain_size_, handle_, p[4], encrypted_, bool(p[1] & 4), std::move(data_)};
    reset();
    return 1;
  }
 private:
  bool active_{false}, encrypted_{false};
  uint16_t endpoint_{};
  uint32_t plain_size_{}, expected_size_{};
  uint8_t handle_{}, next_count_{};
  std::vector<uint8_t> data_;
};
}  // namespace esphome::helio_bridge::protocol

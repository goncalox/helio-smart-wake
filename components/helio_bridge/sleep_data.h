#pragma once
#include "protocol.h"
#include <array>

namespace esphome::helio_bridge::sleep_data {
constexpr size_t RECORD_SIZE = 594;
constexpr size_t MAX_RECORDS = 32;
struct Snapshot {
  uint32_t onset{}, end{}, through{}, signature{};
  uint16_t sleep_minutes{}, awake_minutes{}, timeline_sleep_minutes{};
  uint8_t stage{};
  bool valid{}, is_nap{}, accounting_complete{};
};
inline const char *stage_name(uint8_t stage) {
  switch (stage) {
    case 4: return "Light";
    case 5: return "Deep";
    case 7: return "Awake";
    case 8: return "REM";
    default: return "Unknown";
  }
}
inline bool decode(const uint8_t *p, size_t size, uint32_t now, Snapshot &result) {
  result = {};
  if (size != RECORD_SIZE || now < 1577836800U) return false;
  const uint32_t midnight = protocol::read32(p + 4);
  if (midnight < 1577836800U || midnight > uint64_t(now) + 172800) return false;
  const uint32_t base = midnight - 86400;
  const auto start = protocol::read16(p + 10), end = protocol::read16(p + 12);
  const unsigned minutes = unsigned(protocol::read16(p + 0x24a)) + protocol::read16(p + 0x24c) + protocol::read16(p + 0x24e);
  if (start < end && end <= 4320 && end - start <= 1440 && minutes > 0 && minutes <= 1440 &&
      uint64_t(base) + end * 60 <= uint64_t(now) + 300) {
    result.onset = base + start * 60;
    result.end = base + end * 60;
    result.sleep_minutes = minutes;
    result.valid = true;
  }
  // Night and daytime timelines each occupy 50 five-byte entries.
  // 0x80 in the daytime block denotes a gap, not a measured sleep stage.
  uint32_t night_through = 0;
  for (unsigned block = 0; block < 2; block++) {
    const unsigned count = p[0x54 + block];
    if (count > 50) return false;
    uint32_t previous_end = 0;
    for (unsigned i = 0; i < count; i++) {
      const auto *segment = p + 0x56 + block * 250 + i * 5;
      const unsigned from = protocol::read16(segment), to = protocol::read16(segment + 2);
      const uint8_t stage = segment[4];
      if (from > to || to >= 4320 || (i && from < previous_end)) return false;
      previous_end = to + 1;
      if (block == 1 && stage == 0x80) continue;
      if (stage != 4 && stage != 5 && stage != 7 && stage != 8) return false;
      const uint64_t through = uint64_t(base) + (to + 1) * 60;
      if (through > uint64_t(now) + 300) return false;
      if (block == 0) night_through = std::max(night_through, uint32_t(through));
      if (through > result.through) {
        result.through = through;
        result.stage = stage;
        result.valid = true;
      }
    }
  }
  // During an ongoing night the stage timeline and onset can be available
  // before the end marker or summary totals have been finalized.
  if (!result.onset && night_through && start <= 4320) {
    const uint32_t onset = base + start * 60;
    if (onset <= now && onset < night_through && night_through - onset <= 86400) {
      result.onset = onset;
      if (minutes > 0 && minutes <= 1440) result.sleep_minutes = minutes;
    }
  }
  // Count only explicitly recorded night stages after sleep onset.
  // A missing interval is unknown, so it cannot establish a complete sleep budget.
  if (result.onset && night_through > result.onset && result.through == night_through) {
    const uint32_t accounted_end = std::min(night_through, now / 60 * 60);
    uint32_t cursor = result.onset, awake = 0, asleep = 0;
    bool continuous = true;
    for (unsigned i = 0; i < p[0x54]; i++) {
      const auto *segment = p + 0x56 + i * 5;
      const uint32_t from = std::max(result.onset, base + protocol::read16(segment) * 60U);
      const uint32_t to = std::min(accounted_end, base + (protocol::read16(segment + 2) + 1U) * 60U);
      if (to <= from) continue;
      if (from != cursor) { continuous = false; break; }
      if (segment[4] == 7) awake += to - from;
      else asleep += to - from;
      cursor = to;
    }
    if (continuous && cursor == accounted_end && cursor > result.onset && cursor - result.onset <= 86400) {
      result.accounting_complete = true;
      result.awake_minutes = awake / 60;
      result.timeline_sleep_minutes = asleep / 60;
    }
  }
  // If the newest stage belongs to a daytime nap, associate its own onset.
  if (p[0x17] > 10) return false;
  for (unsigned i = 0; i < p[0x17]; i++) {
    const auto *nap = p + 0x18 + i * 6;
    const unsigned from = protocol::read16(nap), to = protocol::read16(nap + 2), duration = protocol::read16(nap + 4);
    if (from > to || to >= 4320 || duration == 0 || duration > 1440 || duration != to - from + 1) return false;
    const uint32_t nap_start = base + from * 60, nap_end = base + (to + 1) * 60;
    if (nap_end > uint64_t(now) + 300) return false;
    if (nap_end >= result.end && (!result.through || (result.through > nap_start && result.through <= nap_end))) {
      result.onset = nap_start;
      result.end = nap_end;
      result.sleep_minutes = duration;
      result.valid = true;
      result.is_nap = true;
      result.accounting_complete = false;
      result.awake_minutes = result.timeline_sleep_minutes = 0;
    }
  }
  result.signature = protocol::crc32(p, size);
  return result.valid;
}

// Fixed-memory streaming receiver; publication waits for full length and CRC validation.
class Transfer {
 public:
  bool start(uint32_t expected, uint32_t now) {
    *this = Transfer{};
    expected_ = expected;
    now_ = now;
    valid_ = expected <= RECORD_SIZE * MAX_RECORDS && expected % RECORD_SIZE == 0;
    return valid_;
  }
  bool feed(const uint8_t *p, size_t size) {
    if (!valid_ || size < 2 || p[0] != counter_ || received_ + size - 1 > expected_) return valid_ = false;
    counter_++;
    for (size_t i = 1; i < size; i++) {
      crc_ ^= p[i];
      for (unsigned bit = 0; bit < 8; bit++) crc_ = (crc_ >> 1) ^ ((crc_ & 1) ? 0xedb88320U : 0);
      record_[received_++ % RECORD_SIZE] = p[i];
      if (received_ % RECORD_SIZE == 0) {
        Snapshot candidate;
        if (decode(record_.data(), record_.size(), now_, candidate)) {
          usable_++;
          if (!latest_.valid || std::max(candidate.through, candidate.end) >= std::max(latest_.through, latest_.end)) latest_ = candidate;
        } else rejected_++;
      }
    }
    return true;
  }
  bool complete(bool has_crc, uint32_t crc) const { return valid_ && received_ == expected_ && (!has_crc || crc == ~crc_); }
  uint32_t bytes() const { return received_; }
  unsigned records() const { return expected_ / RECORD_SIZE; }
  unsigned usable() const { return usable_; }
  unsigned rejected() const { return rejected_; }
  const Snapshot &latest() const { return latest_; }
 private:
  std::array<uint8_t, RECORD_SIZE> record_{};
  Snapshot latest_{};
  uint32_t expected_{}, received_{}, now_{}, crc_{0xffffffffU};
  unsigned usable_{}, rejected_{};
  uint8_t counter_{};
  bool valid_{false};
};
}  // namespace esphome::helio_bridge::sleep_data

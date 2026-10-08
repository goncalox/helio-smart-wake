#pragma once
#include "sleep_data.h"
#include "stage_model.h"
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace esphome::helio_bridge::smart_wake {
constexpr uint16_t MAX_EARLY_MINUTES = 60;
struct Settings {
  uint16_t duration_minutes{510}, early_minutes{15};
  uint8_t migration_pending{0}, reserved{0};
  bool valid() const {
    return duration_minutes >= 360 && duration_minutes <= 600 && early_minutes <= MAX_EARLY_MINUTES;
  }
};
// Legacy snapshot ABI only; the firmware cannot schedule wake alarms.
// Retained for existing Home Assistant model/history migration.
struct Session {
  uint32_t version{3}, night_start{}, session_end{}, onset{}, confirmed{}, attempted{};
  Settings settings{};
  uint8_t finished{}, early_selected{}, manual_override{}, cancel_pending{};
  uint16_t awake_minutes{};
};
// Version 1 had two bytes of trailing padding. Reuse them to preserve the saved night's layout.
static_assert(sizeof(Session) == 36 && offsetof(Session, awake_minutes) == 34, "Persisted smart wake layout changed");
// Separate preference: keep the deployed 36-byte night's storage format intact.
struct FollowUp {
  uint32_t version{1}, night_start{}, primary{}, confirmed{}, attempted{};
  uint8_t stopped{}, cancel_pending{}, uncertain{}, reserved{};
};
static_assert(sizeof(FollowUp) == 24, "Persisted follow-up layout changed");
enum class WearState : uint8_t { UNKNOWN, WORN, REMOVED };
struct Wear {
  uint32_t sample{}, read_at{};
  WearState state{WearState::UNKNOWN};
  uint8_t kind{}, heart_rate{};
  bool fresh_after(uint32_t alarm, uint32_t now) const {
    return sample >= alarm && sample <= now && now - sample <= 180 &&
           read_at <= now && now - read_at <= 90;
  }
};
inline Wear wear_from_records(const uint8_t *raw, size_t size, uint32_t start, uint32_t now) {
  Wear result;
  if (!raw || !start || !size || size % 8) return result;
  const uint64_t latest = uint64_t(start) + (size / 8 - 1) * 60;
  if (latest > now) return result;
  const auto *row = raw + size - 8;
  result.sample = latest; result.read_at = now; result.kind = row[0]; result.heart_rate = row[3];
  if (row[0] == 115 || row[0] == 118) result.state = WearState::REMOVED;
  else if (row[0] != 255 && row[3] != 0 && row[3] != 255) result.state = WearState::WORN;
  return result;
}
}  // namespace esphome::helio_bridge::smart_wake

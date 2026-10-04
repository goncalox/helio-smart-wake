#pragma once
#include "sleep_data.h"
#include "stage_model.h"
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace esphome::helio_bridge::smart_wake {
constexpr uint32_t WRITE_MARGIN = 30;
constexpr uint32_t SEND_MARGIN = 10;
constexpr uint32_t FOLLOW_INTERVAL = 5 * 60;
constexpr uint32_t STAGE_MAX_AGE = 180;
constexpr uint16_t MAX_EARLY_MINUTES = 60;
struct Settings {
  uint16_t duration_minutes{510}, early_minutes{15};
  uint8_t migration_pending{0}, reserved{0};
  bool valid() const {
    return duration_minutes >= 360 && duration_minutes <= 600 && early_minutes <= MAX_EARLY_MINUTES;
  }
};
// One dated night, independent of the time-only alarm stored on the strap.
struct Session {
  uint32_t version{3}, night_start{}, session_end{}, onset{}, confirmed{}, attempted{};
  Settings settings{};
  uint8_t finished{}, early_selected{}, manual_override{}, cancel_pending{};
  uint16_t awake_minutes{};
};
// Version 1 had two bytes of trailing padding. Reuse them to preserve the saved night's layout.
static_assert(sizeof(Session) == 36 && offsetof(Session, awake_minutes) == 34, "Persisted smart wake layout changed");
inline uint32_t minute_ceiling(uint32_t value) { return (value + 59) / 60 * 60; }
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
inline uint32_t follow_alarm(const FollowUp &s, const Wear &wear, uint32_t now) {
  if (!s.primary || s.stopped || s.cancel_pending || now < s.confirmed ||
      !wear.fresh_after(s.confirmed, now) || wear.state != WearState::WORN) return 0;
  if (s.attempted != s.confirmed) return s.attempted;
  return minute_ceiling(std::max(s.confirmed + FOLLOW_INTERVAL, now + WRITE_MARGIN));
}
inline uint32_t target(const Session &s) {
  return s.onset ? minute_ceiling(s.onset + (s.settings.duration_minutes + s.awake_minutes) * 60U) : 0;
}
inline bool fresh(const sleep_data::Snapshot &data, uint32_t now) {
  return data.valid && data.through && data.through <= uint64_t(now) + 60 &&
         (data.through >= now || now - data.through <= STAGE_MAX_AGE);
}
inline bool usable_onset(const Session &s, const sleep_data::Snapshot &data, uint32_t now) {
  return data.accounting_complete && !data.is_nap && data.onset >= s.night_start && data.onset < s.session_end &&
         data.onset <= now && data.through >= uint64_t(data.onset) + 90 * 60 && fresh(data, now);
}
inline bool light_window(const Session &s, uint32_t now) {
  if (!s.onset || !s.settings.early_minutes || s.early_selected) return false;
  // The window follows the sleep-duration target without a clock-time cap.
  const uint32_t opens = s.onset + (s.settings.duration_minutes + s.awake_minutes - s.settings.early_minutes) * 60U;
  return now >= opens && now < target(s);
}
inline uint32_t desired_alarm(const Session &s, const sleep_data::Snapshot &data, uint32_t now, uint32_t read_at, const stage_model::Prediction &model = {}) {
  if (s.finished || !s.session_end || !s.onset || now >= target(s)) return 0;
  // Resolve an uncertain write before replacing it with a different time.
  if (s.attempted && s.attempted != s.confirmed) return s.attempted;
  if (s.early_selected) return s.attempted;
  const auto normal = target(s);
  if (light_window(s, now) && model.wake_ready(now, data.through) && fresh(data, now) && data.accounting_complete && stage_model::wake_stage(data.stage) && !data.is_nap &&
      data.awake_minutes == s.awake_minutes &&
      data.onset == s.onset && read_at <= now && now - read_at <= 90) {
    const auto early = minute_ceiling(now + WRITE_MARGIN);
    if (early < normal) return early;
  }
  return normal;
}
inline bool writable(uint32_t alarm, uint32_t now) {
  // Never send a time which has passed: a time-only one-off would ring tomorrow.
  return alarm >= uint64_t(now) + WRITE_MARGIN && alarm < uint64_t(now) + 24 * 3600 - 60;
}
inline bool elapsed(const Session &s, uint32_t now) {
  // An unverified write may still have reached the strap; do not re-arm it after its time.
  uint32_t earliest = target(s);
  if (!earliest) earliest = s.session_end;
  if (s.confirmed) earliest = std::min(earliest, s.confirmed);
  if (s.attempted) earliest = std::min(earliest, s.attempted);
  return earliest && now >= earliest;
}
}  // namespace esphome::helio_bridge::smart_wake

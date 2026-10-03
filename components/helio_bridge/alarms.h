#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace esphome::helio_bridge::alarms {
struct Alarm {
  uint8_t slot{}, hour{}, minute{}, repeat{}, flags{};
};
struct Owned {
  uint8_t valid{}, slot{}, hour{}, minute{}, repeat{};
};
inline bool matches(const Alarm &alarm, const Owned &owned) {
  // A one-off alarm may disable itself after firing.
  return owned.valid == 1 && alarm.slot == owned.slot && alarm.hour == owned.hour &&
         alarm.minute == owned.minute && alarm.repeat == owned.repeat && !(alarm.flags & 1);
}
inline bool parse(const std::vector<uint8_t> &data, std::vector<Alarm> &out) {
  out.clear();
  if (data.size() < 2 || data[0] != 0x0a || data.size() != 2U + data[1] * 10U) return false;
  for (unsigned i = 0; i < data[1]; i++) {
    const auto *p = data.data() + 2 + i * 10;
    if (p[2] > 23 || p[3] > 59 || p[4] > 127 ||
        std::any_of(out.begin(), out.end(), [p](const Alarm &a) { return a.slot == p[1]; })) return false;
    out.push_back({p[1], p[2], p[3], p[4], p[0]});
  }
  return true;
}
inline int choose_slot(const std::vector<Alarm> &list, const Owned &owned) {
  for (const auto &alarm : list) if (matches(alarm, owned)) return alarm.slot;
  for (int slot = 0; slot < 10; slot++)
    if (std::none_of(list.begin(), list.end(), [slot](const Alarm &a) { return a.slot == slot; })) return slot;
  return -1;
}
inline std::vector<uint8_t> create(const Owned &alarm) {
  return {3, 1, 4, alarm.slot, alarm.hour, alarm.minute, alarm.repeat, 0, 0, 0, 0, 0};
}
}  // namespace esphome::helio_bridge::alarms

#pragma once
#include <cstdint>

namespace esphome::helio_bridge::read_health {
enum class State : uint8_t { WAIT, READ, OK, FAIL, OFF };
// Receipt time tracks successful transfers, not the age of the last sleep stage.
struct Stream {
  uint32_t received_at{0};
  bool seen{false}, failed{false};
  void received(uint32_t now) { received_at = now; seen = true; failed = false; }
  bool fresh(uint32_t now, uint32_t max_age) const {
    return seen && uint32_t(now - received_at) < max_age;
  }
};
struct Health {
  Stream sleep, activity;
  State state(uint32_t now, uint32_t max_age, bool enabled, bool clock_ready, bool reading) const {
    if (!enabled) return State::OFF;
    if (reading) return State::READ;
    if (sleep.failed || activity.failed) return State::FAIL;
    if (!clock_ready || !sleep.fresh(now, max_age) || !activity.fresh(now, max_age)) return State::WAIT;
    return State::OK;
  }
};
inline const char *label(State state) {
  switch (state) {
    case State::READ: return "READ";
    case State::OK: return "OK";
    case State::FAIL: return "FAIL";
    case State::OFF: return "OFF";
    default: return "WAIT";
  }
}
}  // namespace esphome::helio_bridge::read_health

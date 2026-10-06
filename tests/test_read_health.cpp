#include "read_health.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace esphome::helio_bridge::read_health;
int main() {
  Health h;
  auto status = [&](uint32_t now, uint32_t age = 390000U) {
    return h.state(now, age, true, true, false);
  };
  assert(status(1000) == State::WAIT);
  // A sleep read alone cannot claim the model's activity reads are working.
  h.sleep.received(1000);
  assert(status(1000) == State::WAIT);
  h.activity.received(2000);
  assert(status(2000) == State::OK);
  h.activity.failed = true;
  assert(status(3000) == State::FAIL);
  // A successful sleep fetch must not mask a failed activity fetch.
  h.sleep.received(3000);
  assert(status(3000) == State::FAIL);
  assert(h.state(3000, 390000, true, true, true) == State::READ);
  h.activity.received(4000);
  assert(status(4000) == State::OK);
  h.sleep.failed = true;
  h.activity.received(5000);
  assert(status(5000) == State::FAIL);
  h.sleep.received(6000);
  assert(status(6000) == State::OK);
  // Reduced tolerance when reads are expected every minute.
  assert(status(155000, 150000) == State::WAIT);
  assert(status(155000) == State::OK);
  assert(status(395000) == State::WAIT);
  assert(h.state(6000, 390000, true, false, false) == State::WAIT);
  assert(h.state(6000, 390000, false, true, true) == State::OFF);
  h.sleep.failed = true;
  assert(h.state(6000, 390000, false, true, false) == State::OFF);
  // millis() wraps approximately every 49 days; recent reads remain recent.
  h.sleep.received(0xfffffff0U);
  h.activity.received(0xfffffff0U);
  assert(status(1000) == State::OK);
  assert(status(400000) == State::WAIT);
  // A successful read at uptime zero is valid too.
  h.sleep.received(0);
  h.activity.received(0);
  assert(status(0) == State::OK);
  assert(std::strcmp(label(State::OK), "OK") == 0);
  assert(std::strcmp(label(State::FAIL), "FAIL") == 0);
  assert(std::strcmp(label(State::OFF), "OFF") == 0);
  std::cout << "Read health: both streams, independent failure recovery, cadence, disabled state and uptime wrap passed\n";
}

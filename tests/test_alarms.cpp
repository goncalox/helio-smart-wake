#include "alarms.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge::alarms;
int main() {
  std::vector<Alarm> list;
  assert(parse({10, 0}, list) && list.empty());
  const std::vector<uint8_t> fixture{10, 2, 4, 0, 7, 30, 31, 0, 0, 0, 1, 0, 1, 2, 9, 15, 127, 0, 0, 0, 1, 0};
  assert(parse(fixture, list) && list.size() == 2);
  assert(list[0].hour == 7 && list[0].minute == 30 && list[1].slot == 2);
  Owned empty{}, owned{1, 0, 7, 30, 31};
  assert(choose_slot(list, empty) == 1);
  assert(choose_slot(list, owned) == 0);
  owned.minute = 31;
  assert(choose_slot(list, owned) == 1); // An outside edit must be preserved.
  assert(!matches(list[1], Owned{1, 2, 9, 15, 127})); // Smart flag differs.
  list[0].flags = 0;
  assert(matches(list[0], Owned{1, 0, 7, 30, 31})); // One-off self-disabled.
  auto malformed = fixture;
  malformed.pop_back();
  assert(!parse(malformed, list));
  malformed = fixture;
  malformed[5] = 60;
  assert(!parse(malformed, list));
  malformed = fixture;
  malformed[13] = 0;
  assert(!parse(malformed, list)); // Duplicate slot.
  list.clear();
  for (int i = 0; i < 10; i++) list.push_back({uint8_t(i), 8, 0, 0, 4});
  assert(choose_slot(list, empty) == -1);
  const std::vector<uint8_t> expected{3, 1, 4, 2, 23, 59, 127, 0, 0, 0, 0, 0};
  assert(create(Owned{1, 2, 23, 59, 127}) == expected);
  std::cout << "Alarm parsing, boundary values, free-slot selection and existing-alarm preservation passed\n";
}

#include "smart_wake.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
using namespace esphome::helio_bridge::smart_wake;
constexpr uint32_t day = 1790812800U;
uint32_t desired(const Session &s,const sleep_data::Snapshot &record,uint32_t now,uint32_t at) {
  stage_model::Prediction p;p.valid=true;p.stage=4;p.sample_time=now-60;p.read_at=now;
  return desired_alarm(s,record,now,at,p);
}
int main() {
  Session s;
  s.night_start = day - 6 * 3600;
  s.session_end = day + 18 * 3600;
  sleep_data::Snapshot record{};
  assert(s.settings.valid());
  assert(desired(s, record, day, 0) == 0 && target(s) == 0);
  assert(!usable_onset(s, record, day));
  record.valid = true; record.accounting_complete = true;
  record.onset = day + 3600;
  record.through = day + 3 * 3600;
  record.stage = 4;
  assert(usable_onset(s, record, record.through));
  auto bad = record;
  bad.is_nap = true;
  assert(!usable_onset(s, bad, bad.through));
  bad = record; bad.onset = day - 86400;
  assert(!usable_onset(s, bad, bad.through));
  bad = record; bad.through = record.onset + 30 * 60;
  assert(!usable_onset(s, bad, bad.through));
  assert(!usable_onset(s, record, record.through + 181));
  assert(!usable_onset(s, record, record.through - 61));
  s.onset = record.onset;
  const uint32_t goal = day + 9 * 3600 + 30 * 60;
  assert(target(s) == goal);
  const uint32_t opens = goal - 15 * 60;
  assert(!light_window(s, opens - 1) && light_window(s, opens));
  record.through = opens;
  assert(desired(s, record, opens, opens) == opens + 60);
  assert(desired(s, record, opens + 1, opens) == opens + 60);
  record.stage = 5;
  assert(desired(s, record, opens, opens) == goal);
  record.stage = 8;
  assert(desired(s, record, opens, opens) == goal);
  record.stage = 4;
  assert(desired(s, record, opens + 181, opens + 181) == goal);
  assert(desired(s, record, opens, opens - 91) == goal);
  record.onset--;
  assert(desired(s, record, opens, opens) == goal);
  record.onset++;
  record.is_nap = true;
  assert(desired(s, record, opens, opens) == goal);
  record.is_nap = false;
  s.settings.early_minutes = 0;
  assert(!light_window(s, opens));
  assert(desired(s, record, opens, opens) == goal);
  s.settings.early_minutes = 15;
  s.onset = day + 2 * 3600;
  assert(target(s) == day + 10 * 3600 + 30 * 60);
  assert(light_window(s, day + 10 * 3600 + 15 * 60));
  assert(desired(s, record, target(s), target(s)) == 0);
  s.onset = day + 3600;
  s.awake_minutes = 30;
  record.awake_minutes = 30;
  assert(target(s) == day + 10 * 3600);
  assert(!light_window(s, day + 9 * 3600 + 44 * 60));
  const auto compensated_opens = day + 9 * 3600 + 45 * 60;
  record.through = compensated_opens;
  assert(desired(s, record, compensated_opens, compensated_opens) == compensated_opens + 60);
  record.accounting_complete = false;
  assert(desired(s, record, compensated_opens, compensated_opens) == target(s));
  record.accounting_complete = true;
  s.awake_minutes = 60;
  assert(target(s) == day + 10 * 3600 + 30 * 60 && light_window(s, day + 10 * 3600 + 15 * 60));
  s.awake_minutes = record.awake_minutes = 0;
  s.attempted = s.confirmed = goal;
  assert(!elapsed(s, goal - 1) && elapsed(s, goal));
  s.attempted = opens + 120;
  s.early_selected = 1;
  assert(desired(s, record, opens + 60, opens) == opens + 120);
  assert(elapsed(s, opens + 120)); // An unconfirmed earlier write may have succeeded.
  s.finished = 1;
  assert(desired(s, record, opens, opens) == 0);
  assert(writable(goal, goal - 30));
  assert(!writable(goal, goal - 29));
  assert(!writable(goal, goal + 60));
  assert(!writable(goal + 86400, goal));
  // Even a target after the session-selection boundary is never capped.
  s = {}; s.night_start=day-6*3600; s.session_end=day+18*3600; s.onset=day+16*3600;
  assert(target(s)==day+24*3600+30*60);
  assert(!elapsed(s,day+18*3600));
  assert(desired(s,record,day+19*3600,0)==target(s));
  s.settings.early_minutes = 60;
  assert(s.settings.valid());
  s.settings.early_minutes = 61;
  assert(!s.settings.valid());
  std::cout << "Smart wake: no fallback, night/nap selection, freshness, uncapped duration, early window, uncertain writes and past-time guards passed\n";
}

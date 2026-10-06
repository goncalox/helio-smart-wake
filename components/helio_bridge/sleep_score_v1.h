#pragma once
#include "sleep_data.h"
#include <cmath>
#include <array>
#include <algorithm>

namespace esphome::helio_bridge::sleep_score_v1 {
constexpr uint32_t VERSION=1;
constexpr size_t HISTORY=90,DAYS=3,MINUTES=1440;
constexpr unsigned BASELINE_NIGHTS=7;
struct Minute {uint8_t hr{},movement{},flags{};};
struct Day {uint32_t start{};std::array<Minute,MINUTES> minutes{};};
struct Night {
  uint32_t onset{},end{},observed{},changed{},signature{},first_at{},baseline_nights{},version{VERSION};
  uint16_t asleep{},awake{},awakenings{},activity_minutes{},hr_minutes{},local_onset{},target_minutes{510};
  float mean_hr{},mean_movement{},activity_coverage{},ours{},first_score{},coverage{};
  // Components: duration, continuity, timing, overnight HR, overnight movement.
  std::array<float,5> components{};
  uint8_t available{},helio_score{255},first_helio{255};
  bool mature(uint32_t now) const {
    return onset && end>onset && uint64_t(end)+12*3600<=now &&
      uint64_t(end)+12*3600<=observed && uint64_t(changed)+3600<=now;
  }
};
struct Model {
  uint32_t format{1};uint16_t target_minutes{510};
  std::array<Day,DAYS> days{};
  std::array<Night,HISTORY> nights{};
  bool dirty{};
  bool valid() const {
    if(format!=1 || target_minutes<360 || target_minutes>600)return false;
    for(const auto &d:days) {
      if(d.start && (d.start<1577836800U || d.start%86400))return false;
      for(const auto &m:d.minutes)if(m.flags>3 || (m.hr && (m.hr<30 || m.hr>220)))return false;
    }
    for(const auto &n:nights)if(n.onset) {
      if(n.version!=VERSION || n.end<=n.onset || n.end-n.onset>16*3600 || n.end-n.onset<2*3600 || n.onset<1577836800U ||
          n.observed<n.end || n.changed>n.observed || n.first_at>n.observed ||
          n.asleep+n.awake!=(n.end-n.onset)/60 || n.activity_minutes>(n.end-n.onset)/60 || n.hr_minutes>n.activity_minutes ||
          n.target_minutes<360 || n.target_minutes>600 || n.local_onset>=1440 || n.available>31 || (n.helio_score!=255 && n.helio_score>100) ||
          (n.first_helio!=255 && n.first_helio>100))return false;
      for(float f:n.components)if(!std::isfinite(f)||f<0||f>100)return false;
      for(float f:{n.mean_hr,n.mean_movement,n.activity_coverage,n.ours,n.first_score,n.coverage})
        if(!std::isfinite(f)||f<0)return false;
      if(n.ours>100 || n.first_score>100 || n.coverage>100 || n.activity_coverage>1.01)return false;
    }
    return true;
  }
};
} // namespace esphome::helio_bridge::sleep_score_v1

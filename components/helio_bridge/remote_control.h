#pragma once
#include <cstdint>
namespace esphome::helio_bridge::remote {
enum Status : uint32_t { EMPTY, PENDING, VERIFIED, FAILED, EXPIRED, MANUAL };
struct State {
  uint32_t version{1}, owner{1}, id{}, epoch{}, expires{}, previous{}, status{}, cancel{}, follow{}, manual{};
};
static_assert(sizeof(State)==40);
inline bool valid(const State &s) {
  return s.version==1 && s.owner<=1 && s.status<=MANUAL && s.cancel<=1 && s.follow<=1;
}
inline bool timely(const State &s,uint32_t now) {
  return s.expires>=now && s.expires<=uint64_t(now)+180 &&
      (s.cancel || (s.epoch%60==0 && s.epoch>=uint64_t(now)+10 && s.epoch<uint64_t(now)+86400-60));
}
}

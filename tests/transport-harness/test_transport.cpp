#include "fake_bridge.h"
#include <iostream>
using namespace esphome;
using namespace esphome::helio_bridge;
uint32_t at(int hour,int minute=0,int second=0) {
  ESPTime t{2026,10,8,hour,minute,second,0};t.recalc_timestamp_local();return t.timestamp;
}
std::vector<uint8_t> list(std::initializer_list<alarms::Alarm> alarms) {
  std::vector<uint8_t> v{10,uint8_t(alarms.size())};
  for(auto a:alarms){std::vector<uint8_t> row{a.flags,a.slot,a.hour,a.minute,a.repeat,0,0,0,0,0};v.insert(v.end(),row.begin(),row.end());}
  return v;
}
HelioBridge bridge() {
  persisted.clear();extra_persisted.clear();HelioBridge b;
  b.clock_->now=at(10,2);b.setup_remote_();b.setup_smart_();return b;
}
HelioBridge pending(bool follow=false) {
  auto b=bridge();b.owned_={1,2,10,0,0};
  b.controller_alarm(1,at(10,3),at(10,4),at(10),false,follow);
  assert(b.writes==1 && b.remote_inflight_);b.phase_=HelioBridge::Phase::ALARMS;return b;
}
int main() {
  setenv("TZ","Europe/Lisbon",1);tzset();
  auto b=bridge();assert(b.remote_.owner==1 && !b.controller_owner(false));
  // Previously saved ESP ownership is migrated without losing command identity.
  b.remote_.owner=0;b.remote_.id=9;b.remote_.manual=3;b.save_remote_();
  HelioBridge reboot;reboot.setup_remote_();assert(reboot.remote_.owner==1 && reboot.remote_.id==9 && reboot.remote_.manual==3);
  // Onset, Awake and persisted legacy nights cannot dispatch any alarm.
  b=bridge();b.smart_session_.onset=at(1);b.smart_session_.confirmed=at(10);
  sleep_data::Snapshot snapshot{};snapshot.valid=true;snapshot.stage=7;snapshot.onset=at(1);snapshot.through=at(10);
  b.smart_observe_(snapshot,at(10));b.smart_publish_();assert(!b.writes && !b.cancels);
  b.controller_alarm(1,at(10,1),at(10,4),0,false,false);assert(!b.writes);
  b.controller_fast(at(10,3));assert(b.remote_fast_());b.clock_->now=at(10,3);assert(!b.remote_fast_());
  b.controller_fast(at(10,10));assert(!b.remote_fast_());
  b=pending();b.clock_->now=at(10,2,38);b.handle_alarms_(list({{2,10,0,0,4},{3,8,0,127,4}}));
  assert(b.phase_==HelioBridge::Phase::ALARM_WRITE && b.command==alarms::create(b.owned_) && b.owned_.slot==2);
  b.phase_=HelioBridge::Phase::ALARM_VERIFY;b.handle_alarms_(list({{2,10,3,0,4},{3,8,0,127,4}}));
  assert(b.remote_.status==remote::VERIFIED && b.remote_.previous==at(10,3) && b.close_requested_);
  b.phase_=HelioBridge::Phase::IDLE;b.controller_alarm(1,at(10,3),at(10,4),at(10),false,false);assert(b.writes==1);
  b.controller_alarm(1,at(10,5),at(10,4),at(10),false,false);assert(b.writes==1);
  // A follow-up exists only when requested by HA; its fired one-off may be absent.
  b=pending(true);b.handle_alarms_(list({{3,8,0,127,4}}));assert(b.phase_==HelioBridge::Phase::ALARM_WRITE && b.owned_.slot==0);
  b=pending();b.handle_alarms_(list({{2,7,0,127,4}}));assert(b.command.empty() && b.remote_.manual==1);
  b=pending();b.clock_->now=at(10,2,51);b.handle_alarms_(list({{2,10,0,0,4}}));assert(b.command.empty() && b.remote_.status==remote::FAILED);
  // A lost reply can be recovered by readback close to its scheduled minute.
  b=pending();b.owned_={1,2,10,3,0};b.clock_->now=at(10,2,55);
  b.handle_alarms_(list({{2,10,3,0,4}}));assert(b.command.empty() && b.remote_.status==remote::VERIFIED);
  b=bridge();b.owned_={1,2,10,3,0};b.controller_alarm(1,0,at(10,4),at(10,3),true,false);
  b.phase_=HelioBridge::Phase::ALARMS;b.handle_alarms_(list({{2,10,3,0,4},{3,8,0,127,4}}));assert(b.command==std::vector<uint8_t>({5,1,2}));
  b.phase_=HelioBridge::Phase::ALARM_VERIFY;b.handle_alarms_(list({{3,8,0,127,4}}));assert(!b.owned_.valid && b.remote_.previous==0 && b.remote_.status==remote::VERIFIED);
  b=bridge();b.owned_={1,2,10,3,0};b.controller_alarm(1,0,at(10,4),at(10,3),true,false);
  b.phase_=HelioBridge::Phase::ALARMS;b.handle_alarms_(list({{2,6,0,127,4}}));assert(b.command.empty() && b.remote_.manual==1);
  b=pending();b.save_remote_();HelioBridge restarted;restarted.setup_remote_();assert(restarted.remote_.status==remote::FAILED && !restarted.writes);
  std::cout<<"HA-only transport: owner migration, no autonomous alarms, lease expiry, explicit commands, persistence, duplicate/expired guards, readback and Zepp preservation passed\n";
}

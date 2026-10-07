#include "fake_bridge.h"
#include <iostream>
using namespace esphome;using namespace esphome::helio_bridge;
uint32_t at(int h,int m=0,int sec=0) {ESPTime t{2026,10,4,h,m,sec,0};t.recalc_timestamp_local();return t.timestamp;}
std::vector<uint8_t> list(std::initializer_list<alarms::Alarm> alarms) {
 std::vector<uint8_t> v{10,uint8_t(alarms.size())};
 for(auto a:alarms){std::vector<uint8_t> row{a.flags,a.slot,a.hour,a.minute,a.repeat,0,0,0,0,0};v.insert(v.end(),row.begin(),row.end());}return v;
}
HelioBridge pending() {
 persisted.clear();extra_persisted.clear();HelioBridge b;b.clock_->now=at(10,2);b.setup_smart_();b.smart_enabled_=true;
 b.smart_session_.night_start=at(0)-6*3600;b.smart_session_.session_end=at(18);b.smart_session_.finished=1;
 b.smart_session_.confirmed=b.smart_session_.attempted=at(10);b.smart_session_.onset=at(1,30);
 b.follow_.night_start=b.smart_session_.night_start;b.follow_.primary=b.follow_.confirmed=b.follow_.attempted=at(10);
 b.owned_={1,2,10,0,0};b.wear_={at(10,1),b.clock_->now,smart_wake::WearState::WORN,120,60};b.tick();
 assert(b.follow_.attempted==at(10,5) && b.follow_operation_);b.phase_=HelioBridge::Phase::ALARMS;return b;
}
int main() {
 setenv("TZ","Europe/Lisbon",1);tzset();
 auto b=pending();b.clock_->now+=8;b.handle_alarms_(list({{2,10,0,0,0},{3,8,0,127,4}}));
 assert(b.phase_==HelioBridge::Phase::ALARM_WRITE && b.command==alarms::create(b.owned_) && b.owned_.slot==2);
 b.phase_=HelioBridge::Phase::ALARM_VERIFY;b.handle_alarms_(list({{2,10,5,0,4},{3,8,0,127,4}}));
 assert(b.follow_.confirmed==at(10,5) && !b.follow_.uncertain && b.close_requested_);
 // If the fired one-off was removed, use an empty slot and preserve other alarms.
 b=pending();b.handle_alarms_(list({{3,8,0,127,4}}));assert(b.phase_==HelioBridge::Phase::ALARM_WRITE && b.owned_.slot==0 && !b.smart_session_.manual_override);
 // Never overwrite a different alarm that replaced ours.
 b=pending();b.handle_alarms_(list({{2,7,0,127,4},{3,8,0,127,4}}));assert(b.command.empty() && b.follow_.stopped && b.smart_session_.manual_override);
 // Time guard reselects only a genuinely unsent new candidate.
 b=pending();b.clock_->now=at(10,4,51);b.handle_alarms_(list({{2,10,0,0,0}}));
 assert(b.command.empty() && b.follow_.attempted==at(10) && !b.follow_.uncertain);
 // Readback can recover an already-written candidate with <30 seconds remaining.
 b=pending();b.smart_new_attempt_=false;b.owned_={1,2,10,5,0};b.clock_->now=at(10,4,55);
 b.handle_alarms_(list({{2,10,5,0,4}}));assert(b.command.empty() && b.follow_.confirmed==at(10,5) && b.close_requested_);
 // Removal cancellation respects ownership and stops on an external edit.
 b=pending();b.follow_.confirmed=b.follow_.attempted=at(10,5);b.follow_.stopped=b.follow_.cancel_pending=1;
 b.owned_={1,2,10,5,0};b.operation_=HelioBridge::Operation::CANCEL_ALARM;
 b.handle_alarms_(list({{2,10,5,0,4},{3,8,0,127,4}}));assert(b.command==std::vector<uint8_t>({5,1,2}));
 b.phase_=HelioBridge::Phase::ALARM_VERIFY;b.handle_alarms_(list({{3,8,0,127,4}}));assert(!b.follow_.cancel_pending && !b.owned_.valid);
 b=pending();b.follow_.cancel_pending=b.follow_.stopped=1;b.operation_=HelioBridge::Operation::CANCEL_ALARM;
 b.handle_alarms_(list({{2,6,0,127,4}}));assert(b.command.empty() && !b.follow_.cancel_pending);
 // HA-owned cancellation also reports manual edits when no local follow-up is running.
 b=pending();b.remote_.owner=1;b.remote_inflight_=true;b.follow_operation_=false;
 b.operation_=HelioBridge::Operation::CANCEL_ALARM;
 b.handle_alarms_(list({{2,6,0,127,4}}));assert(b.command.empty() && b.remote_.manual==1);
 // The primary scheduling guard accepts 30s selection after normal 8s connection.
 b=pending();b.follow_operation_=false;b.smart_session_.finished=0;b.smart_session_.confirmed=at(11);
 b.smart_session_.attempted=at(10,3);b.requested_={1,2,10,3,0};b.owned_={1,2,11,0,0};b.clock_->now=at(10,2,38);
 b.handle_alarms_(list({{2,11,0,0,4}}));assert(b.phase_==HelioBridge::Phase::ALARM_WRITE);
 std::cout<<"Production alarm path: 30s/8s buffer, past-time rejection, readback recovery, expired-slot reuse, Zepp preservation and cancellation checks passed\n";
}

#include "fake_bridge.h"
#include <iostream>
using namespace esphome; using namespace esphome::helio_bridge;
uint32_t at(int hour,int minute=0,int seconds=0) { ESPTime t{2026,10,4,hour,minute,seconds,0};t.recalc_timestamp_local();return t.timestamp; }
void fresh(HelioBridge &b,uint32_t sample,smart_wake::WearState state=smart_wake::WearState::WORN) {b.wear_={sample,b.clock_->now,state,120,60};}
void reset() {persisted.clear();extra_persisted.clear();}
HelioBridge armed() {
 reset();HelioBridge b;b.clock_->now=at(6);b.setup_smart_();b.smart_enabled_=true;b.tick();
 b.smart_session_.onset=at(1,30);b.tick();assert(b.writes==1 && b.smart_session_.attempted==at(10));b.ack();
 assert(b.follow_.primary==at(10));return b;
}
int main() {
 setenv("TZ","Europe/Lisbon",1);tzset();
 auto b=armed();b.clock_->now=at(10);b.tick();assert(b.smart_session_.finished && b.writes==1);
 b.clock_->now=at(10,2);fresh(b,at(9,59));b.tick();assert(b.writes==1); // Pre-alarm wear ignored.
 fresh(b,at(10,1));b.tick();assert(b.writes==2 && b.follow_.attempted==at(10,5));
 b.clock_->now+=8;assert(b.smart_write_allowed_());b.ack();assert(b.follow_.confirmed==at(10,5));
 b.clock_->now=at(10,4);fresh(b,at(10,3));b.tick();assert(b.writes==2); // Fixed, no sliding snooze.
 b.clock_->now=at(10,6);fresh(b,at(10,5));b.tick();assert(b.writes==3 && b.follow_.attempted==at(10,10));b.ack();
 // Reboot retains verified follow-up, but never assumes worn without a new observation.
 HelioBridge reboot;reboot.clock_->now=at(10,11);reboot.setup_smart_();reboot.smart_enabled_=true;reboot.owned_=b.owned_;reboot.tick();assert(!reboot.writes);
 fresh(reboot,at(10,10));reboot.tick();assert(reboot.writes==1 && reboot.follow_.attempted==at(10,15));reboot.ack();
 // Removal cancels only the pending reminder; the primary remains historical.
 reboot.clock_->now=at(10,12);fresh(reboot,at(10,11),smart_wake::WearState::REMOVED);reboot.tick();assert(reboot.follow_.stopped && reboot.follow_.cancel_pending);
 reboot.tick();assert(reboot.cancels==1);reboot.ack();assert(!reboot.follow_.cancel_pending && !reboot.follow_.confirmed);
 reboot.clock_->now=at(10,20);fresh(reboot,at(10,19));reboot.tick();assert(reboot.writes==1); // Putting it on again does not restart the morning.
 // A removed strap before any follow-up needs no cancellation.
 b=armed();b.clock_->now=at(10,2);fresh(b,at(10,1),smart_wake::WearState::REMOVED);b.tick();assert(b.writes==1 && !b.cancels && b.follow_.stopped);
 // Missing/stale/future evidence does not manufacture alarms.
 for(int mode=0;mode<4;mode++) {b=armed();b.clock_->now=at(10,4);fresh(b,at(10,3));
  if(mode==0)b.wear_.state=smart_wake::WearState::UNKNOWN;
  if(mode==1)b.wear_.sample=at(10);
  if(mode==2)b.wear_.read_at=b.clock_->now-91;
  if(mode==3)b.wear_.sample=b.clock_->now+1;
  b.tick();assert(b.writes==1);
 }
 // Off and manual override stop the sequence, including after the primary passed.
 b=armed();b.clock_->now=at(10,2);fresh(b,at(10,1));b.tick();b.ack();b.smart_enabled_=false;b.clock_->now+=10;b.tick();assert(b.cancels==1);b.ack();b.tick();assert(b.follow_.stopped);
 b=armed();b.clock_->now=at(10,2);b.smart_manual_override_();fresh(b,at(10,1));b.tick();assert(b.writes==1 && b.follow_.stopped);
 // An uncertain write can be retried only at the same future time; expired attempts stop.
 b=armed();b.clock_->now=at(10,2);fresh(b,at(10,1));b.tick();b.ack(false);
 b.clock_->now+=10;fresh(b,at(10,1));b.tick();assert(b.writes==3 && b.follow_.attempted==at(10,5));b.ack(false);
 b.clock_->now=at(10,5);b.tick();assert(b.writes==3 && b.follow_.stopped);
 // Slow connection rejects an unsent candidate, retaining the primary and permitting a new safe time.
 b=armed();b.clock_->now=at(9,50,30);b.smart_session_.early_selected=1;b.smart_session_.attempted=at(9,51);
 b.smart_new_attempt_=true;b.smart_operation_=true;b.operation_=HelioBridge::Operation::SET_ALARM;
 b.clock_->now+=8;assert(b.smart_write_allowed_());b.clock_->now+=13;assert(!b.smart_write_allowed_());b.smart_unsent_();b.ack(false);
 assert(b.smart_session_.attempted==at(10) && !b.smart_session_.early_selected);
 // Installing on a completed historical morning never creates reminders retroactively.
 reset();smart_wake::Session old;old.night_start=at(0)-6*3600;old.session_end=at(18);old.onset=at(1,30);old.finished=1;old.confirmed=old.attempted=at(10);
 persisted.assign((uint8_t*)&old,(uint8_t*)&old+sizeof(old));HelioBridge upgrade;upgrade.clock_->now=at(11);upgrade.setup_smart_();upgrade.smart_enabled_=true;fresh(upgrade,at(10,59));upgrade.tick();assert(!upgrade.writes && !upgrade.follow_.primary);
 // No clock-time cap is introduced for either the full target or active follow-ups.
 b=armed();b.clock_->now=at(18,1);b.smart_session_.finished=0;b.smart_session_.onset=at(10);
 b.smart_session_.confirmed=b.smart_session_.attempted=at(18,30);b.follow_.primary=b.follow_.confirmed=b.follow_.attempted=at(18,30);
 b.clock_->now=at(18,32);fresh(b,at(18,31));b.tick();assert(b.writes==2 && b.follow_.attempted==at(18,35));
 std::cout<<"Production follow-ups: repeat, removal/cancellation, freshness, restart, off/manual, uncertain writes, slow connections and upgrade safety passed\n";
}

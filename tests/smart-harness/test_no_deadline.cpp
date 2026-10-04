#include <cassert>
#include "fake_bridge.h"
#include <iostream>
using namespace esphome; using namespace esphome::helio_bridge;
uint32_t at(int mon,int day,int h,int m=0) { ESPTime t{2026,mon,day,h,m,0,0}; t.recalc_timestamp_local(); return t.timestamp; }
void seed(const smart_wake::Session &s) { persisted.assign((const uint8_t*)&s,(const uint8_t*)&s+sizeof(s)); }
int main() {
 setenv("TZ","Europe/Lisbon",1);tzset();
 HelioBridge b;b.clock_->now=at(10,3,18);b.setup_smart_();b.smart_enabled_=true;b.tick();
 assert(b.writes==0 && !b.smart_session_.confirmed && b.smart_session_.session_end==at(10,4,18));
 b.clock_->now=at(10,4,4,30);
 sleep_data::Snapshot s{};s.valid=true;s.accounting_complete=true;s.onset=at(10,4,2,51);s.through=b.clock_->now;s.stage=5;s.awake_minutes=3;
 b.smart_observe_(s,b.clock_->now);assert(!b.smart_session_.onset);
 b.clock_->now+=300;s.through=b.clock_->now;b.smart_observe_(s,b.clock_->now);b.tick();
 assert(b.writes==1 && b.smart_session_.attempted==at(10,4,11,24));b.ack();
 b.clock_->now=at(10,4,10);b.tick();assert(!b.smart_session_.finished && b.writes==1);
 HelioBridge reboot;reboot.clock_->now=b.clock_->now;reboot.setup_smart_();reboot.smart_enabled_=true;reboot.tick();
 assert(reboot.writes==0 && reboot.smart_session_.confirmed==at(10,4,11,24));
 b.clock_->now=at(10,4,5);s.through=b.clock_->now;s.awake_minutes=33;b.smart_observe_(s,b.clock_->now);b.tick();
 assert(b.smart_session_.attempted==at(10,4,11,54));b.ack();
 b.clock_->now+=300;s.through=b.clock_->now;b.smart_observe_(s,b.clock_->now);b.tick();assert(b.writes==2 && b.smart_session_.awake_minutes==33);
 b.clock_->now=at(10,4,11,39);s.through=b.clock_->now;s.stage=4;b.model_prediction_.valid=true;b.model_prediction_.stage=4;b.model_prediction_.sample_time=b.clock_->now-60;b.model_prediction_.read_at=b.clock_->now;b.smart_observe_(s,b.clock_->now);b.tick();
 assert(b.smart_session_.attempted==at(10,4,11,41));b.ack();
 b.clock_->now=at(10,4,11,41);b.tick();assert(b.smart_session_.finished);
 b.clock_->now=at(10,4,18);b.tick();assert(b.writes==3 && b.smart_session_.onset==0 && b.smart_session_.confirmed==0);
 // Turning off cancels only an owned smart alarm; turning on creates no fallback.
 persisted.clear();HelioBridge off;off.clock_->now=at(10,4,6);off.setup_smart_();off.smart_enabled_=true;off.tick();
 off.smart_session_.onset=at(10,4,2);off.tick();assert(off.writes==1);off.ack();
 off.smart_enabled_=false;off.clock_->now+=60;off.tick();assert(off.cancels==1);off.ack();off.smart_enabled_=true;off.tick();assert(off.writes==1 && !off.smart_session_.onset);
 // Legacy migration removes the bridge-owned deadline alarm, without adopting a manual alarm.
 smart_wake::Session old;old.version=2;old.night_start=at(10,3,18);old.session_end=at(10,4,10);old.confirmed=old.attempted=old.session_end;
 seed(old);HelioBridge upgrade;upgrade.owned_={1,0,10,0,0};upgrade.clock_->now=at(10,4,6);upgrade.setup_smart_();upgrade.smart_enabled_=true;upgrade.tick();
 assert(upgrade.cancels==1 && upgrade.writes==0 && upgrade.smart_session_.version==3);upgrade.ack();upgrade.tick();
 assert(!upgrade.smart_session_.confirmed && upgrade.writes==0 && upgrade.smart_session_.session_end==at(10,4,18));
 seed(old);HelioBridge manual;manual.owned_={1,1,7,0,0};manual.clock_->now=at(10,4,6);manual.setup_smart_();manual.smart_enabled_=true;manual.tick();assert(manual.cancels==0 && manual.writes==0);
 // Already passed legacy alarms are cleaned up without rearming today's late target.
 old.onset=at(10,4,2,51);old.awake_minutes=3;seed(old);HelioBridge past;past.owned_={1,0,10,0,0};past.clock_->now=at(10,4,13);past.setup_smart_();past.smart_enabled_=true;past.tick();assert(past.cancels==1);past.ack();past.tick();assert(past.writes==0 && past.smart_session_.finished);
 past.clock_->now=at(10,4,18);past.tick();assert(!past.smart_session_.finished && !past.smart_session_.onset && past.writes==0);
 // Unknown-clock migration persists until a clock is available, including another reboot.
 old.version=1;old.awake_minutes=65535;seed(old);HelioBridge no_clock;no_clock.setup_smart_();no_clock.tick();assert(no_clock.smart_session_.settings.migration_pending && no_clock.smart_session_.awake_minutes==0);
 HelioBridge later;later.clock_->now=at(10,4,6);later.setup_smart_();later.smart_enabled_=true;later.tick();assert(!later.smart_session_.settings.migration_pending);
 // Accept and persist the wider setting; reject a window beyond the supported maximum.
 persisted.clear();HelioBridge wide;wide.clock_->now=at(10,4,18);wide.setup_smart_();wide.smart_enabled_=true;wide.tick_smart(8.5,60);
 assert(wide.smart_session_.settings.early_minutes==60 && wide.writes==0);
 HelioBridge wide_reboot;wide_reboot.clock_->now=at(10,4,19);wide_reboot.setup_smart_();assert(wide_reboot.smart_session_.settings.early_minutes==60);
 wide.tick_smart(8.5,61);assert(wide.smart_message_.find("window 0-60min")!=std::string::npos && wide.writes==0);
 // Local date windows follow DST without imposing a wake limit.
 for(int month:{3,10}) { persisted.clear();HelioBridge d;int day=month==3?28:24;d.clock_->now=at(month,day,18);d.setup_smart_();d.smart_enabled_=true;d.tick();assert(d.smart_session_.session_end==at(month,day+1,18));assert(d.smart_session_.session_end-d.smart_session_.night_start==(month==3?23:25)*3600U);assert(d.writes==0); }
 std::cout<<"Production controller: no fallback, targets after 10, compensation, early window, reboot, rollover, cancellation, migration/manual ownership and DST passed\n";
}

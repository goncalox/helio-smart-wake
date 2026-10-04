#include "smart_wake.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
int main() {
 const uint32_t alarm=1791104400,now=alarm+120;
 smart_wake::FollowUp f; f.primary=f.confirmed=f.attempted=alarm;
 uint8_t row[8]={120,0,0,60};
 auto w=smart_wake::wear_from_records(row,8,alarm+60,now);
 assert(w.state==smart_wake::WearState::WORN);
 assert(smart_wake::follow_alarm(f,w,now)==alarm+300);
 // Only post-alarm, current evidence may trigger a follow-up.
 w.sample=alarm-60;assert(!smart_wake::follow_alarm(f,w,now));
 w.sample=now+1;assert(!smart_wake::follow_alarm(f,w,now));
 w.sample=alarm+60;w.read_at=now-91;assert(!smart_wake::follow_alarm(f,w,now));
 w.read_at=now;assert(!smart_wake::follow_alarm(f,w,alarm+300));
 for(uint8_t kind:{uint8_t(115),uint8_t(118),uint8_t(255)}) {
  row[0]=kind;w=smart_wake::wear_from_records(row,8,alarm+60,now);
  assert(w.state==(kind==255?smart_wake::WearState::UNKNOWN:smart_wake::WearState::REMOVED));
  assert(!smart_wake::follow_alarm(f,w,now));
 }
 row[0]=120;
 for(uint8_t hr:{uint8_t(0),uint8_t(255)}) {row[3]=hr;assert(smart_wake::wear_from_records(row,8,alarm+60,now).state==smart_wake::WearState::UNKNOWN);}
 row[3]=60;assert(smart_wake::wear_from_records(row,7,alarm+60,now).state==smart_wake::WearState::UNKNOWN);
 assert(smart_wake::wear_from_records(row,8,now+1,now).state==smart_wake::WearState::UNKNOWN);
 w=smart_wake::wear_from_records(row,8,alarm+360,alarm+380);
 assert(smart_wake::follow_alarm(f,w,alarm+380)==smart_wake::minute_ceiling(alarm+410));
 f.stopped=1;assert(!smart_wake::follow_alarm(f,w,alarm+380));
 assert(smart_wake::minute_ceiling(alarm+59+30)==alarm+120); // XX:59 safely schedules two minutes ahead.
 std::cout<<"30-second rounding and fresh worn/removed/unknown follow-up policy passed\n";
}

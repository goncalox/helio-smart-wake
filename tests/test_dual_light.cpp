#include "smart_wake.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
int main() {
 const uint32_t now=1791030000;
 smart_wake::Session s;s.onset=now-8*3600;s.night_start=s.onset-4*3600;s.session_end=now+8*3600;s.settings.early_minutes=30;
 const auto target=smart_wake::target(s);assert(smart_wake::light_window(s,now));
 sleep_data::Snapshot b{};b.valid=true;b.accounting_complete=true;b.onset=s.onset;b.through=now;b.stage=4;
 stage_model::Prediction p;p.valid=true;p.stage=4;p.sample_time=now-60;p.read_at=now;
 auto desired=[&](){return smart_wake::desired_alarm(s,b,now,now,p);};
 assert(desired()==now+120);
 p.stage=5;assert(desired()==target);p.stage=8;assert(desired()==target);p.stage=7;assert(desired()==target);
 p.stage=4;p.valid=false;assert(desired()==target);p.valid=true;
 p.read_at=now-91;assert(desired()==target);p.read_at=now;
 p.sample_time=now-181;assert(desired()==target);p.sample_time=now+1;assert(desired()==target);
 p.sample_time=now-180;assert(desired()==target); // Fresh but not aligned with the strap minute.
 p.sample_time=now-60;b.stage=5;assert(desired()==target);b.stage=8;assert(desired()==target);b.stage=4;
 b.through=now-181;assert(desired()==target);b.through=now;
 b.is_nap=true;assert(desired()==target);b.is_nap=false;
 s.awake_minutes=1;assert(desired()==smart_wake::target(s));s.awake_minutes=0;
 assert(smart_wake::desired_alarm(s,b,now,now-91,p)==target);
 // End-of-window alarm remains even when the model disagrees.
 p.stage=5;assert(desired()==target);
 s.settings.early_minutes=0;p.stage=4;assert(desired()==target);
 s.settings.early_minutes=30;s.confirmed=s.attempted=now+120;s.early_selected=1;p.valid=false;
 assert(desired()==now+120); // A saved alarm remains locked, not repeatedly moved.
 // A 60-minute window opens an hour before target, while still requiring both Light inputs.
 s={};s.onset=now-8*3600;s.night_start=s.onset-4*3600;s.session_end=now+8*3600;s.settings.early_minutes=60;
 b={};b.valid=true;b.accounting_complete=true;b.onset=s.onset;b.stage=4;
 const uint32_t wide_target=smart_wake::target(s), opens=wide_target-3600;
 assert(!smart_wake::light_window(s,opens-1) && smart_wake::light_window(s,opens));
 const uint32_t middle=wide_target-45*60;b.through=middle;p.valid=true;p.stage=4;p.sample_time=middle-60;p.read_at=middle;
 assert(smart_wake::desired_alarm(s,b,middle,middle,p)==smart_wake::minute_ceiling(middle+120));
 p.stage=5;assert(smart_wake::desired_alarm(s,b,middle,middle,p)==wide_target);
 assert(!smart_wake::light_window(s,wide_target));
 // Latest HR must be valid and the strap worn, independent of the model classification.
 uint8_t raw[40]{};for(int i=0;i<5;i++){raw[i*8]=120;raw[i*8+3]=52;}
 auto pred=stage_model::from_records(raw,40,now-300,now);assert(pred.valid && pred.sample_time==now-60);
 raw[32]=115;assert(!stage_model::from_records(raw,40,now-300,now).valid);raw[32]=120;
 raw[35]=255;assert(!stage_model::from_records(raw,40,now-300,now).valid);raw[35]=52;
 for(int i=0;i<3;i++)raw[i*8+3]=255;
 assert(!stage_model::from_records(raw,40,now-300,now).valid);
 assert(!stage_model::from_records(raw,32,now-300,now).valid);
 std::cout<<"Dual gate: both light, disagreement, missing/stale/future/unaligned inputs, nap/accounting exclusion, full-target retention and locked alarm checks passed\n";
}

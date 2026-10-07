from pathlib import Path
import subprocess
root=Path('components/helio_bridge').resolve()
build=Path('.test-build/activity-tests'); build.mkdir(parents=True, exist_ok=True)
mock=r'''#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <string>
#include <cassert>
#include "activity_data.h"
#include "read_health.h"
#include "smart_wake.h"
#define ESP_LOGI(...) ((void)0)
uint32_t fake_ms=100000;
uint32_t millis() { return fake_ms; }
namespace esphome::helio_bridge {
struct ESPTime { uint32_t timestamp=1790938800; uint16_t year=2026; uint8_t month=10,day_of_month=2,hour=11,minute=0;
 static ESPTime from_epoch_utc(uint32_t t) { ESPTime r; r.timestamp=t;return r; }};
struct Clock { ESPTime utcnow() { return {}; } };
namespace text_sensor { struct TextSensor {void publish_state(const char*){}}; }
class HelioBridge { public:
 struct Remote {bool owner=false;} remote_;
 bool remote_fast_() const {return false;}
 read_health::Health read_health_;
 bool smart_enabled_=false; smart_wake::Session smart_session_{};
 stage_model::Prediction model_prediction_{}; text_sensor::TextSensor *model_stage_sensor_=nullptr;
 smart_wake::Wear wear_{};
 bool following=false;
 bool follow_monitoring_(uint32_t) const { return following; }
 int model_calls=0,score_calls=0;
 void score_activity_(const uint8_t*,size_t,uint32_t){++score_calls;}
 void update_model_(const uint8_t*,size_t,uint32_t){++model_calls;}
 enum class Phase { IDLE, SLEEP_ACK, ACTIVITY_START, ACTIVITY_DATA, ACTIVITY_ACK, CLOSING };
 Phase phase_=Phase::SLEEP_ACK; bool queued_alarm_=false,activity_attempted_=false,sleep_encrypted_=true,close_requested_=false;
 uint32_t activity_attempt_at_=0,activity_deadline_=0,activity_requested_=0;
 activity_data::Transfer activity_transfer_; std::vector<uint8_t> activity_header_,sent,recorded;
 Clock clock; Clock *clock_=&clock;
 std::string status;
 bool activity_phase_() const; void activity_stop_(const char*,bool failed=true); bool begin_activity_();
 void activity_control_(const std::vector<uint8_t>&); void activity_bulk_(const uint8_t*,size_t);
 void close_(){phase_=Phase::CLOSING;}
 void send_(uint16_t endpoint,const std::vector<uint8_t>& p,bool) { assert(endpoint==0x4b); sent=p; }
 void diagnostic_text_(int k,const char* p) { assert(k==10); status=p; }
 void diagnostic_append_(int k,const uint8_t*p,size_t n) { if(k==9)recorded.assign(p,p+n); }
};
}
'''
(build/'mock.h').write_text(mock)
s=(root/'helio_activity.cpp').read_text().replace('#include "helio_bridge.h"','#include "mock.h"').replace('#include "esphome/core/log.h"','')
(build/'production.cpp').write_text(s)
(build/'test.cpp').write_text(r'''#include "production.cpp"
#include <iostream>
using namespace esphome::helio_bridge;
int main() {
 HelioBridge a; a.queued_alarm_=true; assert(!a.begin_activity_()); assert(a.sent.empty());
 a.queued_alarm_=false; assert(a.begin_activity_()); assert(a.sent[1]==1);
 std::vector<uint8_t> header={16,1,1,1,0,0,0,0xea,7,10,2,11,0,0,0};
 a.activity_control_(header); assert(a.phase_==HelioBridge::Phase::ACTIVITY_DATA && a.sent==std::vector<uint8_t>{2});
 uint8_t data[]={0,120,2,0,60,0,0,0,0}; a.activity_bulk_(data,9);
 a.activity_control_({16,2,1}); assert(a.recorded.empty() && a.sent==std::vector<uint8_t>({3,9}));
 a.activity_control_({16,3,1}); assert(a.recorded.size()==25 && a.close_requested_ && a.model_calls==1 && a.score_calls==1);
 assert(a.read_health_.activity.seen && !a.read_health_.activity.failed);
 assert(!a.begin_activity_()); // At most every four minutes, even if sleep polls faster.
 a.smart_enabled_=true;a.smart_session_.onset=a.clock_->utcnow().timestamp-8*3600;a.smart_session_.settings.early_minutes=30;
 fake_ms+=61000;assert(a.begin_activity_()); // Model reads refresh every minute in the early window.
 a.smart_enabled_=false;a.following=true;fake_ms+=61000;assert(a.begin_activity_()); // Post-alarm worn monitoring also refreshes every minute.
 HelioBridge b; assert(b.begin_activity_()); b.activity_control_(header);
 data[0]=1; b.activity_bulk_(data,9); assert(b.recorded.empty() && b.model_calls==0 && b.score_calls==0 && b.phase_==HelioBridge::Phase::CLOSING);
 assert(b.read_health_.activity.failed && !b.read_health_.activity.seen);
 HelioBridge c; assert(c.begin_activity_()); c.activity_control_(header);
 data[0]=0; c.activity_bulk_(data,9); c.activity_control_({16,2,1,0,0,0,0}); assert(c.recorded.empty() && c.model_calls==0 && c.score_calls==0);
 HelioBridge d; assert(d.begin_activity_()); d.activity_control_(header); d.activity_bulk_(data,9);
 d.activity_control_({16,2,1}); d.activity_control_({16,3,0}); assert(d.recorded.empty() && d.model_calls==0 && d.score_calls==0);
 // Intentional alarm preemption is not a failed Bluetooth read.
 a.activity_stop_("Activity read preempted for alarm",false);
 assert(!a.read_health_.activity.failed);
 a.activity_stop_("Timeout"); assert(a.read_health_.activity.failed);
 fake_ms+=300000; assert(a.begin_activity_()); a.activity_control_(header);
 a.activity_bulk_(data,9); a.activity_control_({16,2,1}); a.activity_control_({16,3,1});
 assert(!a.read_health_.activity.failed && a.read_health_.activity.received_at==fake_ms);
 std::cout<<"Production activity controller: alarm skip, rate limit, keep-data ACK, rejected transfer/ACK and publication checks passed\n";
}
''')
subprocess.run(['c++','-std=c++17','-I'+str(root),str(build/'test.cpp'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)

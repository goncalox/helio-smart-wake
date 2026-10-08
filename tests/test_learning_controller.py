"""Exercise the production on-device learning lifecycle against clock/NVS mocks."""
from pathlib import Path
import subprocess
root=Path('components/helio_bridge');build=Path('.test-build/learning-tests');build.mkdir(parents=True,exist_ok=True)
h=(root/'helio_bridge.h').read_text();members=h[h.index('  adaptive::Learner *learning_'):h.index('  stage_model::Prediction model_prediction_')]
prefix=Path('tests/transport-harness/fake_bridge.h').read_text().split('namespace helio_bridge {')[0]
mock=r'''#pragma once
#include "adaptive_model.h"
#include <cstdlib>
#include <new>
#include <map>
#include <string>
#include <vector>
using nvs_handle_t=unsigned;
constexpr int ESP_OK=0,NVS_READWRITE=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
inline bool fail_write=false,fail_commit=false,no_memory=false;
inline unsigned next_handle=1;
inline size_t free_entries=8000;
inline std::map<std::string,unsigned> handles;
inline std::map<unsigned,std::map<std::string,std::vector<uint8_t>>> flash,pending;
inline int nvs_open(const char *name,int,unsigned *h){auto &value=handles[name];if(!value)value=next_handle++;*h=value;return 0;}
inline int nvs_get_blob(unsigned h,const char *key,void *out,size_t *n) {
 auto i=flash[h].find(key);if(i==flash[h].end())return 1;
 if(out){if(*n<i->second.size())return 1;memcpy(out,i->second.data(),i->second.size());}*n=i->second.size();return 0;
}
inline int nvs_set_blob(unsigned h,const char *key,const void *p,size_t n){if(fail_write)return 1;pending[h][key]={(const uint8_t*)p,(const uint8_t*)p+n};return 0;}
inline int nvs_commit(unsigned h){if(fail_commit)return 1;for(auto &entry:pending[h])flash[h][entry.first]=entry.second;pending[h].clear();return 0;}
struct nvs_stats_t{size_t free_entries;};
inline int nvs_get_stats(const char*,nvs_stats_t *s){s->free_entries=free_entries;return 0;}
inline void *heap_caps_malloc(size_t n,int){return no_memory?nullptr:std::malloc(n);}
inline size_t heap_caps_get_free_size(int){return no_memory?0:1000000;}
inline uint32_t fake_ms=1000000;
inline uint32_t millis(){return fake_ms;}
'''+prefix+r'''
namespace helio_bridge {
struct HelioBridge {
 struct Remote {bool owner=false;} remote_;
 enum class Phase {IDLE,BUSY}; Phase phase_=Phase::IDLE;bool queued_alarm_=false,smart_enabled_=true;
 static constexpr size_t DIAGNOSTIC_SLOTS=384;
 Clock clock;Clock *clock_=&clock;
 sleep_data::Snapshot smart_snapshot_{};smart_wake::Session smart_session_{};
 int evictions{},audit_records{},vote_records{};
 std::vector<uint8_t> audit;
 void diagnostic_text_(int,const char*){}
 void diagnostic_append_(int kind,const uint8_t *p,size_t n){if(kind==18){audit_records++;audit.assign(p,p+n);}if(kind==16)vote_records++;}
 bool diagnostic_evict_(){evictions++;free_entries+=1000;return evictions<384;}
'''+members+r'''
};
}
}
'''
(build/'mock.h').write_text(mock)
source=(root/'helio_learning.cpp').read_text().replace('#include "helio_bridge.h"','#include "mock.h"')
for include in ('esphome/core/log.h','esphome/core/hal.h','esp_heap_caps.h'):source=source.replace(f'#include "{include}"','').replace(f'#include <{include}>','')
(build/'production.cpp').write_text(source)
helpers=Path('tests/test_adaptive.cpp').read_text();helpers=helpers[helpers.index('constexpr uint32_t BASE'):helpers.index('int main()')]
(build/'test.cpp').write_text(r'''#include "production.cpp"
#include <iostream>
using namespace esphome;using namespace esphome::helio_bridge;
'''+helpers+r'''
int main() {
 setenv("TZ","Europe/Lisbon",1);tzset();
 HelioBridge b;b.clock.now=BASE+6*3600+adaptive::LABEL_DELAY+adaptive::LABEL_STABILITY;b.setup_learning_();
 assert(b.learning_ && b.learning_handle_ && b.learning_->state.champion_version==0);
 for(int i=0;i<100;++i)b.learning_observe_(observation(BASE+i*180,i/40));
 auto raw=reference(BASE);b.learning_reference_(raw.data(),raw.size(),b.clock.now-adaptive::LABEL_STABILITY);
 b.learning_reference_(raw.data(),raw.size(),b.clock.now);
 assert(b.save_learning_());const auto committed=flash;
 // BLE work and queued alarms always take precedence over training and flash writes.
 fake_ms+=3600001;b.phase_=HelioBridge::Phase::BUSY;b.learning_tick_();assert(!b.learning_->training && flash==committed);
 b.phase_=HelioBridge::Phase::IDLE;b.queued_alarm_=true;b.learning_tick_();assert(!b.learning_->training);
 b.queued_alarm_=false;b.smart_snapshot_.valid=true;b.smart_snapshot_.through=b.clock.now;b.learning_tick_();assert(!b.learning_->training);
 b.smart_snapshot_={};b.learning_tick_();assert(b.learning_->training);
 for(int i=0;i<200&&b.learning_->training;++i)b.learning_tick_();
 assert(b.learning_->state.candidate_version==1 && b.learning_->state.champion_version==0 && b.audit_records==1);
 HelioBridge reboot;reboot.clock.now=b.clock.now;reboot.setup_learning_();
 assert(reboot.learning_->state.candidate_version==1 && reboot.learning_->state.champion_version==0);
 size_t collected=0;for(auto &night:reboot.learning_->nights)collected+=night.size;assert(collected==100);
 // One whole database blob prevents partial score/night writes on reset.
 const auto before=flash;fail_write=true;assert(!b.save_learning_());assert(flash==before);fail_write=false;
 fail_commit=true;assert(!b.save_learning_());assert(flash==before);fail_commit=false;
 // Qualify a frozen candidate; a failed commit must not activate its proposed champion.
 b.learning_->state.evaluated_nights=3;
 auto &a=b.learning_->state.candidate_test;auto &c=b.learning_->state.champion_test;
 c={};a={};c[0][0]=90;c[0][1]=30;c[1][0]=30;c[1][1]=30;c[2][0]=30;c[2][2]=30;
 a[0][0]=110;a[0][1]=10;a[1][0]=10;a[1][1]=50;a[2][0]=10;a[2][2]=50;
 assert(b.save_learning_());fail_write=true;fake_ms+=60001;b.learning_tick_();assert(b.learning_->state.champion_version==0);fail_write=false;
 fake_ms+=60001;b.learning_tick_();assert(b.learning_->state.champion_version==1);
 HelioBridge promoted;promoted.clock.now=b.clock.now;promoted.setup_learning_();assert(promoted.learning_->state.champion_version==1);
 // Learning controls do not reset the active model; turning off collection retains its coefficients.
 promoted.learning_enabled_=false;promoted.learning_publish_();assert(promoted.learning_message_.find("Learning off")!=std::string::npos);
 auto version=promoted.learning_->state.champion_version;promoted.learning_observe_(observation(BASE+86400));assert(promoted.learning_->state.champion_version==version);
 // Make room by evicting rolling log batches without changing partition layout.
 free_entries=500;assert(promoted.save_learning_() && promoted.evictions>=2);free_entries=8000;
 // Corrupt blobs fall back to the original model, never arbitrary coefficients.
 flash[promoted.learning_handle_]["database"].back()^=1;
 HelioBridge corrupt;corrupt.clock.now=b.clock.now;corrupt.setup_learning_();assert(!corrupt.learning_->state.champion_version);
 no_memory=true;HelioBridge unavailable;unavailable.setup_learning_();assert(!unavailable.learning_ && unavailable.learning_message_.find("insufficient memory")!=std::string::npos);
 std::cout<<"Production learning: NVS restart, atomic failure, activation rollback, BLE/live-sleep priority, controls, storage reserve, CRC rejection and memory fallback passed\n";
}
''')
subprocess.run(['c++','-std=c++17','-I'+str(root),'-Itests/transport-harness',str(build/'test.cpp'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)

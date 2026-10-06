"""Production personal-score lifecycle against clock, memory, flash and API mocks."""
from pathlib import Path
import subprocess
root=Path('components/helio_bridge');build=Path('.test-build/score-tests');build.mkdir(parents=True,exist_ok=True)
h=(root/'helio_bridge.h').read_text();members=h[h.index('  sleep_score::Model *quality_'):h.index('  adaptive::Learner *learning_')]
prefix=Path('tests/smart-harness/fake_bridge.h').read_text().split('namespace helio_bridge {')[0]
learning=Path('tests/test_learning_controller.py').read_text()
mock=learning[learning.index("mock=r'''")+len("mock=r'''"):learning.index("'''+prefix")]
mock=mock.replace('#include "adaptive_model.h"','#include "sleep_score_model.h"').replace('inline bool fail_write=', 'inline bool fail_open=false;\ninline bool fail_write=').replace('inline int nvs_open(const char *name,int,unsigned *h){','inline int nvs_open(const char *name,int,unsigned *h){if(fail_open)return 1;')
mock+=prefix+r'''
namespace helio_bridge {
struct HelioBridge {
 enum class Phase {IDLE,BUSY};Phase phase_=Phase::IDLE;bool queued_alarm_=false,smart_enabled_=false,following=false;
 static constexpr size_t DIAGNOSTIC_SLOTS=384;
 Clock clock;Clock *clock_=&clock;smart_wake::Session smart_session_{};
 int evictions{},audit_records{};std::vector<uint8_t> audit;
 void diagnostic_text_(int,const char*){}
 void diagnostic_append_(int k,const uint8_t *p,size_t n){assert(k==19);audit_records++;audit.assign(p,p+n);}
 bool diagnostic_evict_(){evictions++;free_entries+=1000;return evictions<384;}
 bool follow_monitoring_(uint32_t) const{return following;}
 void set_score_target(float);
'''+members+r'''
};
}
}
'''
(build/'mock.h').write_text(mock)
source=(root/'helio_score.cpp').read_text().replace('#include "helio_bridge.h"','#include "mock.h"')
for include in ('esphome/core/log.h','esphome/core/hal.h','esp_heap_caps.h'):source=source.replace(f'#include "{include}"','').replace(f'#include <{include}>','')
(build/'production.cpp').write_text(source)
helpers=Path('tests/test_sleep_score.cpp').read_text();helpers=helpers[helpers.index('using Record='):helpers.index('int main()')]
(build/'test.cpp').write_text(r'''#include "production.cpp"
#include <iostream>
using namespace esphome;using namespace esphome::helio_bridge;
'''+helpers+r'''
int main(){
 setenv("TZ","Europe/Lisbon",1);tzset();
 sensor::Sensor score,coverage;text_sensor::TextSensor status,details;
 HelioBridge b;b.personal_score_sensor_=&score;b.personal_score_coverage_sensor_=&coverage;
 b.personal_score_status_sensor_=&status;b.personal_score_details_sensor_=&details;
 b.setup_score_();assert(b.quality_ && b.quality_handle_ && std::isnan(score.value));
 assert(status.value.find("waiting for a completed night")!=std::string::npos);
 b.set_score_target(9);assert(b.quality_->target_minutes==540);b.set_score_target(NAN);b.set_score_target(0);assert(b.quality_->target_minutes==540);
 auto r=night();uint32_t lo=onset(r),end=lo+480*60;b.clock.now=end+3600;
 activity(*b.quality_,lo,480);b.score_reference_(r.data(),r.size(),b.clock.now);
 assert(std::isfinite(score.value) && coverage.value==70 && b.audit_records==1 && b.audit.size()==108);
 assert(protocol::read32(b.audit.data())==1 && protocol::read32(b.audit.data()+4)==lo);
 assert(b.quality_->latest()->local_onset==ESPTime::from_epoch_local(lo).hour*60+ESPTime::from_epoch_local(lo).minute);
 b.score_reference_(r.data(),r.size(),b.clock.now+300);assert(b.audit_records==1);
 auto before=flash;fake_ms+=60001;b.phase_=HelioBridge::Phase::BUSY;b.score_tick_();assert(flash==before);
 b.phase_=HelioBridge::Phase::IDLE;b.queued_alarm_=true;b.score_tick_();assert(flash==before);
 b.queued_alarm_=false;b.following=true;b.score_tick_();assert(flash==before);
 b.following=false;b.score_tick_();assert(flash!=before && !b.quality_->dirty);
 HelioBridge reboot;reboot.personal_score_sensor_=&score;reboot.setup_score_();
 assert(reboot.quality_->latest()->first_score==b.quality_->latest()->first_score && reboot.quality_->target_minutes==540);
 before=flash;fail_write=true;assert(!b.save_score_());assert(flash==before);assert(status.value.find("save failed")!=std::string::npos);
 fail_write=false;fail_commit=true;assert(!b.save_score_());assert(flash==before);fail_commit=false;
 b.quality_->dirty=true;b.score_tick_();assert(flash==before);fake_ms+=300001;b.score_tick_();assert(!b.quality_->dirty);
 b.clock.now=end+13*3600;b.score_reference_(r.data(),r.size(),b.clock.now);
 assert(b.quality_->latest()->mature(b.clock.now) && b.audit_records==2 && protocol::read32(b.audit.data()+104)==1);
 b.set_score_target(8.5);assert(b.quality_->latest()->target_minutes==540);
 free_entries=1;assert(b.save_score_() && b.evictions>0);
 auto &image=flash[b.quality_handle_]["database"];image.back()^=1;
 HelioBridge damaged;damaged.personal_score_sensor_=&score;damaged.setup_score_();assert(!damaged.quality_->latest());
 no_memory=true;HelioBridge no_ram;no_ram.personal_score_sensor_=&score;no_ram.personal_score_status_sensor_=&status;
 no_ram.setup_score_();assert(!no_ram.quality_ && status.value.find("insufficient memory")!=std::string::npos);no_memory=false;
 fail_open=true;HelioBridge no_store;no_store.personal_score_sensor_=&score;no_store.setup_score_();assert(!no_store.quality_handle_);fail_open=false;
 HelioBridge optional;optional.setup_score_();assert(!optional.quality_);
 std::cout<<"Personal score controller: optional config, local time, API, audits, flash/reboot/CRC, failures and alarm priority passed\n";
}
''')
subprocess.run(['c++','-std=c++17','-I'+str(root),str(build/'test.cpp'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)

# The public downloader preserves the versioned score audit and both first scores.
import sys, struct, zlib
sys.path.insert(0,'diagnostics')
from download import decode_blob
values=(1,1700000000,1700028800,1700030000,1700030000,1700030000,123,7,510,31,78,72)
floats=(81.5,80.5,100.,.95,62.,2.,90.,95.,85.,80.,70.)
payload=struct.pack('<12I11f6HI',*values,*floats,480,0,0,456,456,1320,1)
raw=struct.pack('<BIIH',19,1700030000,90000,len(payload))+payload
packed=bytearray()
for v in raw:
 packed.extend(bytes((v,)) if v else b'\0\1')
blob=struct.pack('<4sIII',b'HLG2',1,len(raw),zlib.crc32(raw))+packed
item=decode_blob(blob,1)[0]
assert item['kind']=='personal_sleep_score' and item['score']['score']==81.5
assert item['score']['first_score']==80.5 and item['score']['helio_score']==78 and item['score']['settled']
assert item['score']['asleep_minutes']==480 and item['score']['component_coverage']==100
try:
 damaged=bytearray(blob);damaged[-1]^=1;decode_blob(damaged,1)
except ValueError: pass
else: raise AssertionError('damaged score audit accepted')
from sleep_score_report import summarize
summary=summarize([item,item])
assert len(summary['nights'])==1 and summary['nights'][0]['overnight_reading_percent']==95
assert 'does not establish' in summary['interpretation']
print('Personal score audit decoder: version, comparison, first estimate, components and corruption passed')

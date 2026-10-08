"""Production model switching and versioned diagnostics remain independently inspectable."""
from pathlib import Path
import struct,subprocess,sys,zlib
root=Path('components/helio_bridge');build=Path('.test-build/adaptive-model-tests');build.mkdir(parents=True,exist_ok=True)
mock=r'''#pragma once
#include "adaptive_model.h"
#include "smart_wake.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#define ESP_LOGI(...) ((void)0)
namespace esphome::helio_bridge {
namespace text_sensor{struct TextSensor{std::string value;void publish_state(const char *s){value=s;}};}
struct Clock{struct T{uint32_t timestamp=1791162300;};T utcnow(){return {};}};
struct HelioBridge {
 adaptive::Learner *learning_=nullptr;unsigned learning_handle_=1;
 Clock clock;Clock *clock_=&clock;
 stage_model::Prediction model_prediction_{};
 text_sensor::TextSensor *model_stage_sensor_=nullptr;
 std::map<int,std::vector<uint8_t>> records;
 int observed=0;
 void learning_observe_(const stage_model::Prediction &){observed++;}
 void diagnostic_append_(int k,const uint8_t *p,size_t n){records[k]={p,p+n};}
 void update_model_(const uint8_t*,size_t,uint32_t);
};
}
'''
(build/'mock.h').write_text(mock)
s=(root/'helio_model.cpp').read_text();s=s[s.index('void HelioBridge::update_model_'):s.rindex('}  // namespace')]
(build/'production.cpp').write_text('#include "mock.h"\nnamespace esphome::helio_bridge {\n'+s+'}\n')
(build/'test.cpp').write_text(r'''#include "production.cpp"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
int main(){
 HelioBridge b;uint8_t raw[40]{};for(int i=0;i<5;++i){raw[i*8]=120;raw[i*8+3]=52;}
 b.update_model_(raw,40,1791162000);assert(b.records[11].size()==44 && b.model_prediction_.valid && b.observed==1);
 adaptive::Learner learner;b.learning_=&learner;learner.state.champion_version=1;learner.state.next_version=2;
 for(auto &row:learner.state.champion)row.fill(0);learner.state.champion[3][0]=2;
 b.update_model_(raw,40,1791162000);assert(b.model_prediction_.stage==7 && b.records[17].size()==48);
 assert(protocol::read32(b.records[17].data()+44)==1 && b.records[17][0]==2);
 assert(b.model_prediction_.sample_time==1791162240 && b.model_prediction_.read_at==1791162300);
 raw[32]=115;b.update_model_(raw,40,1791162000);assert(!b.model_prediction_.valid); // Removal remains invalid for staging.
 std::cout<<"Production model: fixed-to-adaptive inference, versioned logs, timestamp preservation and removed-strap rejection passed\n";
}
''')
subprocess.run(['c++','-std=c++17','-I'+str(root),str(build/'test.cpp'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)
sys.path.insert(0,'diagnostics')
from download import decode_blob
prediction=bytes([2,1,7,0])+struct.pack('<I9fI',1000,52,51,1,0,0,1,0,0,2,3)
vote=bytes([1,7,4,1])+struct.pack('<4I',1000,3,4,1070)
audit=struct.pack('<9I48d32I',1,3,4,500,600,2,1,0,900,*([.1]*48),*range(32))
raw=b''.join(struct.pack('<BIIH',kind,1070,123,len(data))+data for kind,data in ((17,prediction),(16,vote),(18,audit)))
packed=bytearray()
for byte in raw:packed.extend(bytes([byte]) if byte else bytes([0,1]))
blob=struct.pack('<4sIII',b'HLG2',80,len(raw),zlib.crc32(raw))+packed
items=decode_blob(blob,80)
assert items[0]['model_version']=='adaptive-3' and items[0]['stage']=='Awake'
assert items[1]['champion_version']==3 and items[1]['candidate_version']==4
assert items[2]['state']['champion_weights'][0]==[.1]*6
assert items[2]['state']['candidate_test'][-1]==[28,29,30,31]
print('Adaptive prediction, frozen-candidate vote and coefficient/evaluation audits decode correctly.')

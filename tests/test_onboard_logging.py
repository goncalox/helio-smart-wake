from pathlib import Path
import subprocess,sys,struct,zlib,random
root=Path('components/helio_bridge'); build=Path('.test-build/log-tests'); build.mkdir(parents=True, exist_ok=True)
h=(root/'helio_bridge.h').read_text(); a=h.index('  std::vector<uint8_t> diagnostic_sleep_'); b=h.index('  uint16_t bulk_handle_',a)
members=h[a:b]
mock='''#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <map>
#include <string>
#include <cassert>
#include <iostream>
#include "protocol.h"
#include "smart_wake.h"
using nvs_handle_t = unsigned;
constexpr int ESP_OK=0;
constexpr int NVS_READWRITE=1;
struct nvs_stats_t { size_t free_entries; };
inline std::map<std::string,std::vector<uint8_t>> flash;
inline bool fail_write=false;
inline uint32_t fake_millis=0;
inline uint32_t millis() { return fake_millis; }
inline int nvs_open(const char*,int,nvs_handle_t *h) { *h=1; return 0; }
inline int nvs_get_blob(nvs_handle_t,const char *key,void *out,size_t *n) {
  auto i=flash.find(key); if(i==flash.end()) return 1;
  if(out) { if(*n<i->second.size()) return 1; memcpy(out,i->second.data(),i->second.size()); }
  *n=i->second.size(); return 0;
}
inline int nvs_set_blob(nvs_handle_t,const char *key,const void *data,size_t n) {
  if(fail_write) return 1;
  flash[key]=std::vector<uint8_t>((const uint8_t*)data,(const uint8_t*)data+n); return 0;
}
inline int nvs_commit(nvs_handle_t) { return 0; }
inline int nvs_erase_key(nvs_handle_t,const char *key) { return flash.erase(key) ? 0 : 1; }
inline int nvs_get_stats(const char*,nvs_stats_t *s) {
  s->free_entries=14000; for(auto &p:flash) s->free_entries-=(p.second.size()+31)/32+3; return 0;
}
#define ESP_LOGI(...) ((void)0)
namespace esphome::helio_bridge {
struct Clock { struct T { uint32_t timestamp=1790852400; bool is_valid() const { return true; } }; T utcnow() { return {}; } };
namespace text_sensor { struct TextSensor { void publish_state(const char*) {} }; }
class HelioBridge { public:
 void controller_snapshot_() {}
 void controller_export_(const std::vector<uint8_t>&,const char*,uint32_t) {}
  enum class Phase { IDLE, SLEEP_DATA }; Phase phase_=Phase::IDLE; bool queued_alarm_=false;
  Clock *clock_=nullptr;
  void download_diagnostics(int);
  void diagnostic_event(const char *);
'''+members+'};\n}\n'
(build/'mock.h').write_text(mock)
source=(root/'helio_diagnostics.cpp').read_text().replace('#include "helio_bridge.h"','#include "mock.h"').replace('#include "esphome/core/log.h"','').replace('#include "esphome/core/hal.h"','')
(build/'production.cpp').write_text(source)
(build/'test.cpp').write_text('''#include "production.cpp"
#include <random>
using namespace esphome::helio_bridge;
int main() {
 std::mt19937 rng(42);
 for(size_t n: {size_t(0),size_t(1),size_t(594),size_t(19008),size_t(24576)}) {
  std::vector<uint8_t> raw(n),decoded;
  for(int pattern=0;pattern<3;pattern++) {
   for(auto &v:raw) v=pattern==0 ? 0 : pattern==1 ? rng()%256 : rng()%5 ? 0 : rng()%256;
   auto packed=diagnostic::encode(raw.data(),raw.size());
   assert(diagnostic::decode(packed.data(),packed.size(),n,decoded) && decoded==raw);
  }
 }
 std::vector<uint8_t> out;
 uint8_t bad[]={0}; assert(!diagnostic::decode(bad,1,1,out));
 uint8_t overflow[]={0,255}; assert(!diagnostic::decode(overflow,2,1,out));
 assert(diagnostic::crc((const uint8_t*)"123456789",9)==0xcbf43926);
 HelioBridge a; a.diagnostic_setup_();
 std::vector<uint8_t> raw(1188);
 for(int i=0;i<900;i++) {
  for(size_t j=0;j<raw.size();j++) raw[j]=(i<450 || j%3==0) ? rng()%256 : 0;
  a.diagnostic_append_(1,raw.data(),raw.size()); a.diagnostic_text_(4,"Saved alarm verified");
  a.diagnostic_flush_(); assert(a.diagnostic_used_<=256*1024); assert(a.diagnostic_dropped_==0);
 }
 assert(a.diagnostic_next_==901 && flash.size()<384);
 HelioBridge reboot; reboot.diagnostic_setup_();
 assert(reboot.diagnostic_next_==a.diagnostic_next_ && reboot.diagnostic_used_==a.diagnostic_used_);
 assert(reboot.diagnostic_sequences_==a.diagnostic_sequences_);
 // Invalid persisted data must never be exported as a verified batch.
 flash.begin()->second.back()^=1;
 HelioBridge damaged; damaged.diagnostic_setup_(); assert(damaged.diagnostic_dropped_==1);
 fail_write=true; damaged.diagnostic_flush_(); assert(damaged.diagnostic_dropped_==2);
 fail_write=false;
 // No writes occur while Bluetooth work or a queued alarm is active.
 damaged.diagnostic_text_(8,"pending"); auto before=flash;
 fake_millis=100000; damaged.phase_=HelioBridge::Phase::SLEEP_DATA; damaged.diagnostic_tick_(); assert(flash==before);
 damaged.phase_=HelioBridge::Phase::IDLE; damaged.queued_alarm_=true; damaged.diagnostic_tick_(); assert(flash==before);
 damaged.queued_alarm_=false; damaged.diagnostic_tick_(); assert(flash!=before);
 std::cout << "Codec, 900-batch rollover, reboot recovery, corruption, failed writes and BLE deferral passed\\n";
}
''')
subprocess.run(['c++','-std=c++17','-I'+str(root),'-I'+str(build),str(build/'test.cpp'),'-o',str(build/'test')],check=True)
subprocess.run([str(build/'test')],check=True)
sys.path.insert(0,'diagnostics')
from download import decode_blob

def blob(raw,seq=12):
    compressed=bytearray();i=0
    while i<len(raw):
        if raw[i]: compressed.append(raw[i]);i+=1
        else:
            count=1
            while i+count<len(raw) and not raw[i+count] and count<255: count+=1
            compressed.extend([0,count]);i+=count
    return struct.pack('<4sIII',b'HLG2',seq,len(raw),zlib.crc32(raw))+compressed
record=bytearray(594);struct.pack_into('<I',record,4,1790852400);record[0x54]=1;record[0x56+4]=4
frame=lambda data: struct.pack('<BIIH',1,1790900000,1000,len(data))+data
original=blob(frame(record)); first=decode_blob(original,12)
record[0x56+4]=8
second=decode_blob(blob(frame(record)),12)
assert first[0]['records'][0]['night'][0]['stage']=='Light'
assert second[0]['records'][0]['night'][0]['stage']=='REM'
for broken in (original[:-1],original[:12]+b'XXXX'+original[16:],original[:16]+b'\0'):
    try: decode_blob(broken,12)
    except (ValueError,struct.error): pass
    else: raise AssertionError('corrupt batch accepted')
print('Downloader CRC/truncation checks and preserved retrospective Light→REM revision passed')

#include "sleep_score_model.h"
#include <cassert>
#include <iostream>
using namespace esphome::helio_bridge;
using Record=std::array<uint8_t,sleep_data::RECORD_SIZE>;
constexpr uint32_t MID=1700006400U;
void u16(Record &r,unsigned at,unsigned v){r[at]=v;r[at+1]=v>>8;}
void seg(Record &r,unsigned i,unsigned lo,unsigned hi,unsigned stage){u16(r,0x56+i*5,lo);u16(r,0x58+i*5,hi);r[0x5a+i*5]=stage;}
Record night(uint32_t mid=MID,unsigned minutes=480,unsigned awake=0) {
 Record r{};protocol::write32(r.data()+4,mid);u16(r,10,1320);u16(r,12,1320+minutes);r[0x16]=78;
 r[0x54]=awake?3:1;seg(r,0,1320,awake?1439:1319+minutes,4);
 if(awake){seg(r,1,1440,1439+awake,7);seg(r,2,1440+awake,1319+minutes,8);}
 return r;
}
uint32_t onset(const Record &r){return protocol::read32(r.data()+4)-86400+1320*60;}
void activity(sleep_score::Model &m,uint32_t lo,unsigned minutes,unsigned hr=60,unsigned mv=2,unsigned kind=120) {
 for(unsigned at=0;at<minutes;at+=120) {
  unsigned count=std::min(120U,minutes-at);std::vector<uint8_t> rows(count*8);
  for(unsigned i=0;i<count;++i){rows[i*8]=kind;rows[i*8+1]=mv;rows[i*8+3]=hr;rows[i*8+4]=255;}
  m.activity(rows.data(),rows.size(),lo+at*60,lo+minutes*60);
 }
}
int main() {
 sleep_score::Model m;auto r=night();uint32_t lo=onset(r),end=lo+480*60;
 assert(!m.observe(r.data(),r.size(),end-1,1320));
 assert(m.observe(r.data(),r.size(),end+3600,1320));auto initial=*m.latest();
 assert(initial.available==3 && initial.coverage==70 && initial.activity_coverage==0);
 assert(std::abs(initial.ours-(40*(480./510*100)+30*100)/70)<0.001 && !initial.mature(end+86400));
 // Helio's score and its Deep/REM proportions are never targets or inputs.
 r[0x16]=12;seg(r,0,1320,1799,5);assert(m.observe(r.data(),r.size(),end+4000,1320));
 assert(m.latest()->ours==initial.ours && m.latest()->first_helio==78 && m.latest()->helio_score==12);
 seg(r,0,1320,1799,8);assert(m.observe(r.data(),r.size(),end+4100,1320));assert(m.latest()->ours==initial.ours);
 // Invalid/open/gapped/nightless records cannot produce quality scores.
 auto bad=r;u16(bad,12,65535);assert(!m.observe(bad.data(),bad.size(),end+5000,1320));
 bad=r;seg(bad,0,1321,1799,4);assert(!m.observe(bad.data(),bad.size(),end+5000,1320));
 assert(!m.observe(r.data(),r.size()-1,end+5000,1320));bad=r;bad[0x54]=0;assert(!m.observe(bad.data(),bad.size(),end+5000,1320));
 // Night-only metrics ignore samples outside the night; first-arrival minutes deduplicate.
 activity(m,lo-120*60,120,190,250);activity(m,lo,480);activity(m,lo,480,180,200);
 assert(m.observe(r.data(),r.size(),end+6000,1320));assert(m.latest()->mean_hr==60 && m.latest()->mean_movement==2);
 assert(m.latest()->activity_minutes==480 && m.latest()->activity_coverage==1);
 auto aged=m;aged.days={};assert(aged.observe(r.data(),r.size(),end+6100,1320));
 assert(aged.latest()->activity_minutes==480 && aged.latest()->mean_hr==60);
 auto revised=night(MID,480,30);assert(m.observe(revised.data(),revised.size(),end+7000,1320));
 assert(m.latest()->asleep==450 && m.latest()->awake==30 && m.latest()->ours<initial.ours);
 assert(m.latest()->first_score==initial.first_score && m.latest()->first_at==initial.first_at);
 assert(m.latest()->changed==end+7000);
 // Target stays fixed per dated night, including later strap revisions.
 m.target_minutes=600;assert(m.observe(revised.data(),revised.size(),end+8000,1320));assert(m.latest()->target_minutes==510);
 sleep_score::Model unknown;activity(unknown,lo,480,0);assert(unknown.observe(r.data(),r.size(),end+4000,1320));assert(unknown.latest()->activity_coverage==0);
 sleep_score::Model worn;activity(worn,lo,480,60,2,115);assert(worn.observe(r.data(),r.size(),end+4000,1320));assert(worn.latest()->activity_coverage==0);
 // Baselines require seven genuinely prior, freshly reread, settled nights.
 sleep_score::Model baseline;
 for(unsigned day=0;day<7;++day){auto n=night(MID+day*86400);uint32_t start=onset(n),finish=start+480*60;activity(baseline,start,480);
  assert(baseline.observe(n.data(),n.size(),finish+600,1320));
  assert(baseline.observe(n.data(),n.size(),finish+12*3600+3600,1320));assert(baseline.latest()->mature(finish+13*3600));
  assert(baseline.latest()->available==3);
 }
 auto eighth=night(MID+7*86400);lo=onset(eighth);end=lo+480*60;activity(baseline,lo,480,80,20);
 assert(baseline.observe(eighth.data(),eighth.size(),end+3600,1320));assert(baseline.latest()->baseline_nights==7);
 assert(baseline.latest()->available==31 && baseline.latest()->coverage==100);
 assert(baseline.latest()->components[3]<100 && baseline.latest()->components[4]<100);
 // A circadian baseline around midnight uses circular time, not a 24-hour difference.
 for(auto &n:baseline.nights)if(n.onset<lo && n.onset)n.local_onset=1435;
 assert(baseline.observe(eighth.data(),eighth.size(),end+3700,5));assert(baseline.latest()->components[2]==100);
 // Bad persistence fields rejected; rounded bounds stay within 0..100.
 assert(baseline.valid());auto invalid=baseline;invalid.nights[0].ours=NAN;assert(!invalid.valid());
 invalid=baseline;invalid.target_minutes=0;assert(!invalid.valid());
 auto longnight=night(MID+8*86400,600);lo=onset(longnight);assert(baseline.observe(longnight.data(),longnight.size(),lo+601*60,1320));
 assert(baseline.latest()->ours>=0 && baseline.latest()->ours<=100 && baseline.latest()->components[0]==100);
 std::cout<<"Personal score: independent inputs, night boundaries, no gaps, duplicate protection, revisions, target freeze and seven-night baselines passed\n";
}

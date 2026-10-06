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
 assert(initial.available==3 && initial.coverage==60 && initial.activity_coverage==0);
 assert(std::abs(initial.ours-(30*(480./510*100)+30*100)/60)<0.001 && !initial.mature(end+86400));
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
  assert(baseline.latest()->available==(day>=3?35:3));
 }
 auto eighth=night(MID+7*86400);lo=onset(eighth);end=lo+480*60;activity(baseline,lo,480,80,20);
 assert(baseline.observe(eighth.data(),eighth.size(),end+3600,1320));assert(baseline.latest()->baseline_nights==7);
 assert(baseline.latest()->available==63 && baseline.latest()->coverage==100);
 assert(baseline.latest()->components[3]<100 && baseline.latest()->components[4]<100);
 // A circadian baseline around midnight uses circular time, not a 24-hour difference.
 for(auto &n:baseline.nights)if(n.onset<lo && n.onset)n.local_onset=1435;
 assert(baseline.observe(eighth.data(),eighth.size(),end+3700,5));assert(baseline.latest()->components[2]==100);
 // Bad persistence fields rejected; rounded bounds stay within 0..100.
 assert(baseline.valid());auto invalid=baseline;invalid.nights[0].ours=NAN;assert(!invalid.valid());
 invalid=baseline;invalid.target_minutes=0;assert(!invalid.valid());
 auto longnight=night(MID+8*86400,600);lo=onset(longnight);assert(baseline.observe(longnight.data(),longnight.size(),lo+601*60,1320));
 assert(baseline.latest()->ours>=0 && baseline.latest()->ours<=100 && baseline.latest()->components[0]==100);

 // Equal awake totals should distinguish one prolonged interruption from brief awakenings.
 auto make_wakes=[&](unsigned count,unsigned length,unsigned spacing){
   auto q=night(MID,480);q[0x54]=0;unsigned cursor=1320;
   for(unsigned i=0;i<count;++i){unsigned start=1320+10+i*spacing;
     if(start>cursor)seg(q,q[0x54]++,cursor,start-1,4);
     seg(q,q[0x54]++,start,start+length-1,7);cursor=start+length;
   }
   if(cursor<1800)seg(q,q[0x54]++,cursor,1799,4);return q;
 };
 sleep_score::Model one,brief,clustered,spread;
 auto prolonged=make_wakes(1,30,60),short_wakes=make_wakes(6,5,70);
 uint32_t stamp=onset(prolonged)+480*60+3600;
 assert(one.observe(prolonged.data(),prolonged.size(),stamp,1320));assert(brief.observe(short_wakes.data(),short_wakes.size(),stamp,1320));
 assert(one.latest()->awake==brief.latest()->awake && one.latest()->longest_awake==30 && brief.latest()->longest_awake==5);
 assert(one.latest()->components[1]<brief.latest()->components[1]);
 auto tight=make_wakes(6,1,4),loose=make_wakes(6,1,70);
 assert(clustered.observe(tight.data(),tight.size(),stamp,1320));assert(spread.observe(loose.data(),loose.size(),stamp,1320));
 assert(clustered.latest()->wake_cluster==6 && spread.latest()->wake_cluster==1);
 assert(clustered.latest()->components[1]<spread.latest()->components[1]);
 // Splitting one Awake interval cannot create extra awakenings or reset stability.
 auto split=prolonged;split[0x54]=4;seg(split,0,1320,1329,4);seg(split,1,1330,1344,7);seg(split,2,1345,1359,7);seg(split,3,1360,1799,4);
 auto signature=one.latest()->signature;assert(one.observe(split.data(),split.size(),stamp+300,1320));
 assert(one.latest()->awakenings==1 && one.latest()->signature==signature);
 // Personal HR patterns detect fluctuations even with an unchanged nightly mean.
 sleep_score::Model calm;
 for(unsigned d=0;d<7;++d){auto q=night(MID+d*86400);uint32_t start=onset(q),finish=start+480*60;
   activity(calm,start,480,60,0);assert(calm.observe(q.data(),q.size(),finish+600,1320));
   assert(calm.observe(q.data(),q.size(),finish+13*3600,1320));
 }
 auto q=night(MID+7*86400);uint32_t start=onset(q),finish=start+480*60;
 for(unsigned i=0;i<480;++i){uint8_t row[]={120,0,0,uint8_t(i%2?70:50),0,0,0,0};calm.activity(row,8,start+i*60,finish);}
 assert(calm.observe(q.data(),q.size(),finish+3600,1320));
 assert(calm.latest()->mean_hr==60 && calm.latest()->hr_change==20 && calm.latest()->hr_sd==10);
 assert(calm.latest()->components[3]<100 && (calm.latest()->available&8));
 // Trend measures the last third against the first third, independently of average HR.
 auto rising=calm;for(auto &d:rising.days)if(d.start)for(unsigned i=0;i<1440;++i){uint32_t t=d.start+i*60;if(t>=start&&t<finish)d.minutes[i].hr=(t-start)/60<160?50:(t-start)/60>=320?70:60;}
 assert(rising.observe(q.data(),q.size(),finish+3700,1320));assert(rising.latest()->hr_trend==20 && rising.latest()->mean_hr==60);
 // Same movement mean/fraction can contain different burst shapes.
 auto continuous=calm,scattered=calm;
 for(auto *model:{&continuous,&scattered})for(auto &d:model->days)if(d.start)for(unsigned i=0;i<1440;++i)d.minutes[i].movement=0;
 for(unsigned i=0;i<10;++i) {
   for(auto &d:continuous.days)if(d.start==(start+100*60)/86400*86400)d.minutes[(start+(100+i)*60-d.start)/60].movement=6;
   for(auto &d:scattered.days)if(d.start==(start+i*40*60)/86400*86400)d.minutes[(start+i*40*60-d.start)/60].movement=6;
 }
 assert(continuous.observe(q.data(),q.size(),finish+3700,1320));assert(scattered.observe(q.data(),q.size(),finish+3700,1320));
 assert(continuous.latest()->mean_movement==scattered.latest()->mean_movement);
 assert(continuous.latest()->restless_minutes==10 && scattered.latest()->restless_minutes==10);
 assert(continuous.latest()->longest_movement==10 && scattered.latest()->longest_movement==1);
 assert(continuous.latest()->movement_bursts==1 && scattered.latest()->movement_bursts==10);
 assert(continuous.latest()->components[4]!=scattered.latest()->components[4]);
 // Missing minutes break adjacent-HR comparisons and movement runs.
 sleep_score::Model gaps;activity(gaps,start,480,60,2);
 for(unsigned i=0;i<480;i+=2){auto day=(start+i*60)/86400*86400;for(auto &d:gaps.days)if(d.start==day)d.minutes[(start+i*60-day)/60]={};}
 assert(gaps.observe(q.data(),q.size(),finish+3600,1320));assert(gaps.latest()->hr_pairs==0 && gaps.latest()->longest_movement==1);
 assert(!(gaps.latest()->patterns&3) && !(gaps.latest()->available&24));
 // Repeated short nights contribute context without treating missing nights as short sleep.
 sleep_score::Model recent;
 for(unsigned d=0;d<3;++d){auto row=night(MID+d*86400,360);uint32_t e=onset(row)+360*60;
   assert(recent.observe(row.data(),row.size(),e+600,1320));assert(recent.observe(row.data(),row.size(),e+13*3600,1320));}
 auto full=night(MID+3*86400,510);uint32_t e=onset(full)+510*60;
 assert(recent.observe(full.data(),full.size(),e+3600,1320));assert(recent.latest()->history_nights==3 && (recent.latest()->available&32));
 assert(recent.latest()->components[5]<100 && recent.latest()->components[0]==100);
 // Onset corrections from the current night cannot become a prior-night baseline.
 auto corrected=q;u16(corrected,10,1321);
 assert(calm.observe(q.data(),q.size(),finish+13*3600,1320));
 assert(calm.observe(corrected.data(),corrected.size(),finish+13*3600+300,1321));assert(calm.latest()->baseline_nights==7);
 // Preserve v1 results and first estimates while giving v2 its own first estimate.
 sleep_score_v1::Model legacy;auto &old=legacy.nights[0];old.onset=onset(r);old.end=old.onset+480*60;old.observed=old.end+3600;
 old.changed=old.end+600;old.first_at=old.end+600;old.asleep=480;old.local_onset=1320;old.ours=75;old.first_score=74;
 old.coverage=70;old.available=3;old.helio_score=78;old.first_helio=76;
 sleep_score::Model upgraded;assert(upgraded.migrate(legacy));assert(upgraded.latest()->version==1 && upgraded.latest()->first_score==74);
 assert(upgraded.observe(r.data(),r.size(),old.end+7200,1320));assert(upgraded.latest()->version==2 && upgraded.latest()->previous_score==75);
 assert(upgraded.latest()->previous_first_score==74 && upgraded.latest()->previous_first_at==old.first_at);
 assert(upgraded.latest()->previous_first_helio==76 && upgraded.latest()->first_at==old.end+7200 && upgraded.valid());
 sleep_score::Night evening,morning;evening.onset=MID+22*3600+55*60;evening.local_onset=1435;
 morning.onset=MID+86400+5*60;morning.local_onset=65;
 assert(sleep_score::Model::night_key(evening)==sleep_score::Model::night_key(morning));
 evening.onset=MID+23*3600+55*60;morning.local_onset=5;
 assert(sleep_score::Model::night_key(evening)==sleep_score::Model::night_key(morning));
 std::cout<<"Personal score: independent inputs, night boundaries, no gaps, duplicate protection, revisions, target freeze and seven-night baselines passed\n";
}

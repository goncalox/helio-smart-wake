#pragma once
#include "sleep_data.h"
#include "sleep_score_v1.h"
#include <cmath>
#include <array>
#include <algorithm>

namespace esphome::helio_bridge::sleep_score {
constexpr uint32_t VERSION=2;
constexpr size_t HISTORY=90,DAYS=3,MINUTES=1440;
constexpr unsigned BASELINE_NIGHTS=7;
using Minute=sleep_score_v1::Minute;
using Day=sleep_score_v1::Day;
struct Night {
  uint32_t onset{},end{},observed{},changed{},signature{},first_at{},baseline_nights{},version{VERSION};
  uint16_t asleep{},awake{},awakenings{},activity_minutes{},hr_minutes{},local_onset{},target_minutes{510};
  float mean_hr{},mean_movement{},activity_coverage{},ours{},first_score{},coverage{};
  // Duration, continuity, timing, HR patterns, movement patterns, recent shortfall.
  std::array<float,6> components{};
  uint8_t available{},helio_score{255},first_helio{255},patterns{};
  uint16_t longest_awake{},wake_cluster{},late_awake{},restless_minutes{},movement_bursts{},longest_movement{},hr_pairs{},history_nights{};
  float hr_change{},hr_trend{},hr_sd{},shortfall{};
  uint32_t previous_version{},previous_first_at{};
  float previous_score{},previous_first_score{},previous_coverage{};
  uint8_t previous_helio{255},previous_first_helio{255};
  bool mature(uint32_t now) const {
    return onset && end>onset && uint64_t(end)+12*3600<=now &&
      uint64_t(end)+12*3600<=observed && uint64_t(changed)+3600<=now;
  }
};
struct Model {
  uint32_t format{VERSION};uint16_t target_minutes{510};
  std::array<Day,DAYS> days{};
  std::array<Night,HISTORY> nights{};
  bool dirty{};uint8_t replay_done{};
  bool valid() const {
    if(format!=VERSION || target_minutes<360 || target_minutes>600 || replay_done>1)return false;
    for(const auto &d:days) {
      if(d.start && (d.start<1577836800U || d.start%86400))return false;
      for(const auto &m:d.minutes)if(m.flags>3 || (m.hr && (m.hr<30 || m.hr>220)))return false;
    }
    for(const auto &n:nights)if(n.onset) {
      const unsigned span=(n.end-n.onset)/60;
      if((n.version!=1 && n.version!=VERSION) || n.end<=n.onset || span>960 || span<120 || n.onset<1577836800U ||
         n.observed<n.end || n.changed>n.observed || n.first_at>n.observed || n.asleep+n.awake!=span ||
         n.activity_minutes>span || n.hr_minutes>n.activity_minutes || n.longest_awake>n.awake || n.late_awake>n.awake ||
         n.restless_minutes>n.activity_minutes || n.longest_movement>n.restless_minutes || n.movement_bursts>n.restless_minutes ||
         n.hr_pairs>n.hr_minutes || n.history_nights>7 || n.patterns>7 ||
         n.target_minutes<360 || n.target_minutes>600 || n.local_onset>=1440 || n.available>63 ||
         (n.helio_score!=255 && n.helio_score>100) || (n.first_helio!=255 && n.first_helio>100))return false;
      for(float f:n.components)if(!std::isfinite(f)||f<0||f>100)return false;
      for(float f:{n.mean_hr,n.mean_movement,n.activity_coverage,n.ours,n.first_score,n.coverage,n.hr_change,n.hr_sd,n.shortfall,
                   n.previous_score,n.previous_first_score,n.previous_coverage})if(!std::isfinite(f)||f<0)return false;
      if(!std::isfinite(n.hr_trend) || std::abs(n.hr_trend)>190 || n.ours>100 || n.first_score>100 ||
         n.previous_score>100 || n.previous_first_score>100 || n.previous_coverage>100 || n.coverage>100 ||
         n.activity_coverage>1.01 || n.shortfall>1.01)return false;
    }
    return true;
  }
  bool migrate(const sleep_score_v1::Model &old) {
    if(!old.valid())return false;
    target_minutes=old.target_minutes;days=old.days;
    for(size_t i=0;i<HISTORY;++i) {
      const auto &a=old.nights[i];auto &n=nights[i];if(!a.onset)continue;
      n.onset=a.onset;n.end=a.end;n.observed=a.observed;n.changed=a.changed;n.signature=a.signature;
      n.first_at=a.first_at;n.baseline_nights=a.baseline_nights;n.version=a.version;
      n.asleep=a.asleep;n.awake=a.awake;n.awakenings=a.awakenings;n.activity_minutes=a.activity_minutes;
      n.hr_minutes=a.hr_minutes;n.local_onset=a.local_onset;n.target_minutes=a.target_minutes;
      n.mean_hr=a.mean_hr;n.mean_movement=a.mean_movement;n.activity_coverage=a.activity_coverage;
      n.ours=a.ours;n.first_score=a.first_score;n.coverage=a.coverage;
      std::copy(a.components.begin(),a.components.end(),n.components.begin());
      n.available=a.available;n.helio_score=a.helio_score;n.first_helio=a.first_helio;
    }
    dirty=true;return valid();
  }
  void activity(const uint8_t *raw,size_t size,uint32_t first,uint32_t now) {
    if(!raw || first<1577836800U || size%8 || size>120*8)return;
    for(size_t at=0;at<size;at+=8) {
      const uint64_t stamp=uint64_t(first)+(at/8)*60;
      if(stamp>now || stamp+2*86400<now)continue;
      const uint32_t epoch=stamp,day=epoch/86400*86400;size_t slot=DAYS;
      for(size_t i=0;i<DAYS;++i)if(days[i].start==day){slot=i;break;}
      if(slot==DAYS) {
        slot=0;for(size_t i=1;i<DAYS;++i)if(days[i].start<days[slot].start)slot=i;
        if(days[slot].start>day)continue;
        days[slot]={};days[slot].start=day;
      }
      auto &m=days[slot].minutes[(epoch-day)/60];if(m.flags)continue;
      const bool removed=raw[at]==115 || raw[at]==118 || raw[at]==255;
      const bool heart=raw[at+3]>=30 && raw[at+3]<=220;
      m.flags=removed?2:heart?1:3;m.movement=raw[at+1];m.hr=heart && !removed?raw[at+3]:0;dirty=true;
    }
  }
  const Minute *minute(uint32_t epoch) const {
    const uint32_t day=epoch/86400*86400;
    for(const auto &d:days)if(d.start==day)return &d.minutes[(epoch-day)/60];
    return nullptr;
  }
  static double median(std::array<double,HISTORY> values,size_t count) {
    if(!count || count>HISTORY)return 0;
    std::sort(values.begin(),values.begin()+count);
    return count%2?values[count/2]:(values[count/2-1]+values[count/2])/2;
  }
  static double upper_penalty(double value,const std::array<double,HISTORY> &values,size_t count,double floor) {
    if(!count || count>HISTORY || floor<=0)return 100;
    const double centre=median(values,count);std::array<double,HISTORY> deviations{};
    for(size_t i=0;i<count;++i)deviations[i]=std::abs(values[i]-centre);
    const double spread=std::max(floor,1.4826*median(deviations,count));
    return std::clamp(100.-15.*std::max(0.,(value-centre)/spread),0.,100.);
  }
  void physiology(Night &n) const {
    const unsigned span=(n.end-n.onset)/60;double hr=0,hr2=0,movement=0,changes=0,early=0,late=0;
    unsigned early_count=0,late_count=0,movement_run=0,previous_hr=0;
    for(unsigned i=0;i<span;++i) {
      const auto *m=minute(n.onset+i*60U);
      if(!m || m->flags!=1){previous_hr=0;movement_run=0;continue;}
      ++n.activity_minutes;movement+=m->movement;
      if(m->movement) {
        ++n.restless_minutes;if(!movement_run)++n.movement_bursts;
        n.longest_movement=std::max<unsigned>(n.longest_movement,++movement_run);
      } else movement_run=0;
      if(m->hr) {
        ++n.hr_minutes;hr+=m->hr;hr2+=double(m->hr)*m->hr;
        if(previous_hr){++n.hr_pairs;changes+=std::abs(int(m->hr)-int(previous_hr));}
        previous_hr=m->hr;
        if(i<span/3){early+=m->hr;++early_count;}
        if(i>=span-span/3){late+=m->hr;++late_count;}
      } else previous_hr=0;
    }
    n.activity_coverage=double(n.activity_minutes)/span;
    n.mean_hr=n.hr_minutes?hr/n.hr_minutes:0;n.mean_movement=n.activity_minutes?movement/n.activity_minutes:0;
    n.hr_change=n.hr_pairs?changes/n.hr_pairs:0;
    n.hr_sd=n.hr_minutes?std::sqrt(std::max(0.,hr2/n.hr_minutes-double(n.mean_hr)*n.mean_hr)):0;
    const bool trend=early_count>=30 && late_count>=30 && early_count>=span/3.*0.6 && late_count>=span/3.*0.6;
    n.hr_trend=trend?late/late_count-early/early_count:0;
    if(n.activity_coverage>=0.7)n.patterns|=1;
    if(n.activity_coverage>=0.7 && n.hr_minutes>=120 && n.hr_pairs>=span*0.5 && trend)n.patterns|=2;
    n.patterns|=4; // Full pattern metrics measured, even when coverage is insufficient.
  }
  static uint32_t night_key(const Night &n) {
    int offset=int(n.local_onset)-int(n.onset/60%1440);
    if(offset>720)offset-=1440;else if(offset<-720)offset+=1440;
    const uint32_t local_day=(int64_t(n.onset)+offset*60)/86400;
    return local_day-(n.local_onset<18*60?1:0);
  }
  size_t previous_nights(const Night &n,std::array<const Night*,HISTORY> &previous) const {
    size_t count=0;
    for(const auto &p:nights)if(p.onset && p.onset<n.onset && n.onset-p.onset<=28*86400 && night_key(p)<night_key(n)) {
      size_t i=0;while(i<count && night_key(*previous[i])!=night_key(p))++i;
      if(i==count)previous[count++]=&p;
      else if(previous[i]->onset<p.onset)previous[i]=&p;
    }
    size_t eligible=0;
    for(size_t i=0;i<count;++i)if(previous[i]->mature(n.observed))previous[eligible++]=previous[i];
    return eligible;
  }
  double baseline(const Night &n,unsigned feature,size_t &count) const {
    std::array<double,HISTORY> values{};count=0;
    std::array<const Night*,HISTORY> previous{};const size_t total=previous_nights(n,previous);
    for(size_t i=0;i<total;++i) {
      const auto &p=*previous[i];
      if(feature<3 && !(p.patterns&2))continue;
      if(feature>=3 && !(p.patterns&1))continue;
      double value=0;
      switch(feature) {
        case 0:value=p.mean_hr;break;
        case 1:value=p.hr_change;break;
        case 2:value=p.hr_trend;break;
        case 3:value=std::log1p(p.mean_movement);break;
        case 4:value=double(p.restless_minutes)/std::max<unsigned>(1,p.activity_minutes);break;
        case 5:value=std::log1p(p.longest_movement);break;
        default:value=p.movement_bursts*480./(p.end-p.onset)*60.;break;
      }
      values[count++]=value;
    }
    double value=0,floor=1;
    switch(feature) {
      case 0:value=n.mean_hr;floor=2;break;
      case 1:value=n.hr_change;floor=1;break;
      case 2:value=n.hr_trend;floor=3;break;
      case 3:value=std::log1p(n.mean_movement);floor=0.15;break;
      case 4:value=double(n.restless_minutes)/std::max<unsigned>(1,n.activity_minutes);floor=0.02;break;
      case 5:value=std::log1p(n.longest_movement);floor=0.25;break;
      default:value=n.movement_bursts*480./(n.end-n.onset)*60.;floor=0.5;break;
    }
    return upper_penalty(value,values,count,floor);
  }
  void calculate(Night &n) const {
    constexpr double weights[]={30,30,10,15,10,5};
    const double span=(n.end-n.onset)/60.;n.components[0]=100*std::min(1.,double(n.asleep)/n.target_minutes);
    const double fraction=100*n.asleep/span;
    const double longwake=std::clamp(100.-3.*std::max(0.,double(n.longest_awake)-5),0.,100.);
    const double frequency=std::clamp(100.-5.*std::max(0.,n.awakenings*480./span-3),0.,100.);
    const double clusters=std::clamp(100.-15.*std::max(0.,double(n.wake_cluster)-2),0.,100.);
    n.components[1]=.55*fraction+.20*longwake+.15*frequency+.10*clusters;n.available=3;
    double sin_onset=0,cos_onset=0,shortfall=0;unsigned timing_count=0,history_count=0;
    std::array<const Night*,HISTORY> recent{};const size_t total=previous_nights(n,recent);unsigned nr=0;
    for(size_t i=0;i<total;++i) {
      const auto &p=*recent[i];
      ++timing_count;const double phase=p.local_onset*6.283185307179586/1440.;sin_onset+=std::sin(phase);cos_onset+=std::cos(phase);
      if(n.onset-p.onset<=8*86400)recent[nr++]=&p;
    }
    n.baseline_nights=timing_count;
    if(timing_count>=BASELINE_NIGHTS && std::hypot(sin_onset,cos_onset)>0.1) {
      const double expected=std::atan2(sin_onset,cos_onset),phase=n.local_onset*6.283185307179586/1440.;
      const double delta=std::abs(std::atan2(std::sin(phase-expected),std::cos(phase-expected)))*1440/6.283185307179586;
      n.components[2]=std::clamp(100.-std::max(0.,delta-30.)/6.,0.,100.);n.available|=4;
    }
    if(n.patterns&2) {
      size_t a,b,c;const double level=baseline(n,0,a),change=baseline(n,1,b),trend=baseline(n,2,c);
      if(std::min({a,b,c})>=BASELINE_NIGHTS){n.components[3]=.4*level+.4*change+.2*trend;n.available|=8;}
    }
    if(n.patterns&1) {
      size_t a,b,c,d;const double mean=baseline(n,3,a),active=baseline(n,4,b),run=baseline(n,5,c),bursts=baseline(n,6,d);
      if(std::min({a,b,c,d})>=BASELINE_NIGHTS){n.components[4]=.3*mean+.3*active+.2*run+.2*bursts;n.available|=16;}
    }
    std::sort(recent.begin(),recent.begin()+nr,[](const Night *a,const Night *b){return a->onset>b->onset;});
    // At most one main night per local-night date, even after an onset revision.
    for(unsigned i=0;i<nr && history_count<7;++i) {
      const auto &p=*recent[i];
      shortfall+=std::max(0.,1.-double(p.asleep)/p.target_minutes);++history_count;
    }
    n.history_nights=history_count;n.shortfall=history_count?shortfall/history_count:0;
    if(history_count>=3){n.components[5]=std::clamp(100.-75*n.shortfall,0.,100.);n.available|=32;}
    double sum=0,weight=0;for(size_t i=0;i<6;++i)if(n.available&(1<<i)){sum+=weights[i]*n.components[i];weight+=weights[i];}
    n.ours=sum/weight;n.coverage=weight;
  }
  bool observe(const uint8_t *p,size_t size,uint32_t now,unsigned local_onset) {
    sleep_data::Snapshot check;
    if(!sleep_data::decode(p,size,now,check) || !p[0x54] || local_onset>=1440)return false;
    const uint32_t base=protocol::read32(p+4)-86400;
    const uint64_t start=uint64_t(base)+protocol::read16(p+10)*60U,finish=uint64_t(base)+protocol::read16(p+12)*60U;
    if(start<1577836800U || finish<=start || finish-start<2*3600 || finish-start>16*3600 || finish>now || finish+48*3600<now)return false;
    Night n;n.onset=start;n.end=finish;n.local_onset=local_onset;n.observed=now;
    uint32_t cursor=n.onset;unsigned awake=0,asleep=0,run=0,starts_count=0;
    std::array<uint32_t,50> starts{},ends{};
    for(size_t i=0;i<p[0x54];++i) {
      const auto *s=p+0x56+i*5;const uint32_t lo=std::max(n.onset,base+protocol::read16(s)*60U),hi=std::min(n.end,base+(protocol::read16(s+2)+1U)*60U);
      if(hi<=lo)continue;
      if(lo!=cursor)return false;
      if(s[4]==7) {
        awake+=hi-lo;if(!run){starts[starts_count++]=lo;++n.awakenings;}run+=(hi-lo)/60;ends[starts_count-1]=hi;
        n.longest_awake=std::max<unsigned>(n.longest_awake,run);
        const uint32_t late_start=n.end-(n.end-n.onset)/3;
        if(hi>late_start)n.late_awake+=(hi-std::max(lo,late_start))/60;
      } else {asleep+=hi-lo;run=0;}
      cursor=hi;
    }
    if(cursor!=n.end)return false;
    for(unsigned i=0;i<starts_count;++i) {unsigned count=0;for(unsigned j=i;j<starts_count && starts[j]-starts[i]<1800;++j)++count;n.wake_cluster=std::max<unsigned>(n.wake_cluster,count);}
    n.asleep=asleep/60;n.awake=awake/60;n.helio_score=p[0x16]<=100?p[0x16]:255;
    size_t slot=HISTORY;for(size_t i=0;i<HISTORY;++i)if(nights[i].onset==n.onset){slot=i;break;}
    if(slot==HISTORY){slot=0;for(size_t i=1;i<HISTORY;++i)if(nights[i].onset<nights[slot].onset)slot=i;}
    if(nights[slot].onset>n.onset)return false;
    const Night old=nights[slot];
    n.target_minutes=old.onset==n.onset?old.target_minutes:target_minutes;
    // Awake timing now affects the index; hashing each awake interval detects rearrangements.
    uint32_t semantic=0x535132U;for(size_t i=0;i<starts_count;++i) {
      uint32_t interval[]={starts[i],ends[i]};semantic^=protocol::crc32(reinterpret_cast<const uint8_t*>(interval),8)+0x9e3779b9U+(semantic<<6)+(semantic>>2);}
    uint32_t fields[]={n.onset,n.end,n.asleep,n.awake,n.awakenings,n.longest_awake,n.wake_cluster,semantic};
    n.signature=protocol::crc32(reinterpret_cast<const uint8_t*>(fields),sizeof(fields));
    n.changed=old.onset==n.onset && old.version==VERSION && old.signature==n.signature?old.changed:now;
    physiology(n);
    if(old.onset==n.onset && old.end==n.end && old.version==VERSION && (old.patterns&4) && old.activity_minutes>n.activity_minutes) {
      n.activity_minutes=old.activity_minutes;n.hr_minutes=old.hr_minutes;n.mean_hr=old.mean_hr;n.mean_movement=old.mean_movement;
      n.activity_coverage=old.activity_coverage;n.patterns=old.patterns;n.restless_minutes=old.restless_minutes;
      n.movement_bursts=old.movement_bursts;n.longest_movement=old.longest_movement;n.hr_pairs=old.hr_pairs;
      n.hr_change=old.hr_change;n.hr_trend=old.hr_trend;n.hr_sd=old.hr_sd;
    }
    calculate(n);
    const bool same_version=old.onset==n.onset && old.version==VERSION;
    n.first_score=same_version?old.first_score:n.ours;n.first_at=same_version?old.first_at:now;n.first_helio=same_version?old.first_helio:n.helio_score;
    if(old.onset==n.onset) {
      n.previous_version=same_version?old.previous_version:old.version;
      n.previous_score=same_version?old.previous_score:old.ours;
      n.previous_first_score=same_version?old.previous_first_score:old.first_score;
      n.previous_first_at=same_version?old.previous_first_at:old.first_at;
      n.previous_coverage=same_version?old.previous_coverage:old.coverage;
      n.previous_helio=same_version?old.previous_helio:old.helio_score;
      n.previous_first_helio=same_version?old.previous_first_helio:old.first_helio;
    }
    nights[slot]=n;dirty=true;return true;
  }
  const Night *latest() const {
    const Night *result=nullptr;for(const auto &n:nights)if(n.onset && (!result||n.onset>result->onset))result=&n;return result;
  }
};
} // namespace esphome::helio_bridge::sleep_score

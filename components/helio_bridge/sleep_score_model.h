#pragma once
#include "sleep_data.h"
#include <cmath>
#include <array>
#include <algorithm>

namespace esphome::helio_bridge::sleep_score {
constexpr uint32_t VERSION=1;
constexpr size_t HISTORY=90,DAYS=3,MINUTES=1440;
constexpr unsigned BASELINE_NIGHTS=7;
struct Minute {uint8_t hr{},movement{},flags{};};
struct Day {uint32_t start{};std::array<Minute,MINUTES> minutes{};};
struct Night {
  uint32_t onset{},end{},observed{},changed{},signature{},first_at{},baseline_nights{},version{VERSION};
  uint16_t asleep{},awake{},awakenings{},activity_minutes{},hr_minutes{},local_onset{},target_minutes{510};
  float mean_hr{},mean_movement{},activity_coverage{},ours{},first_score{},coverage{};
  // Components: duration, continuity, timing, overnight HR, overnight movement.
  std::array<float,5> components{};
  uint8_t available{},helio_score{255},first_helio{255};
  bool mature(uint32_t now) const {
    return onset && end>onset && uint64_t(end)+12*3600<=now &&
      uint64_t(end)+12*3600<=observed && uint64_t(changed)+3600<=now;
  }
};
struct Model {
  uint32_t format{1};uint16_t target_minutes{510};
  std::array<Day,DAYS> days{};
  std::array<Night,HISTORY> nights{};
  bool dirty{};
  bool valid() const {
    if(format!=1 || target_minutes<360 || target_minutes>600)return false;
    for(const auto &d:days) {
      if(d.start && (d.start<1577836800U || d.start%86400))return false;
      for(const auto &m:d.minutes)if(m.flags>3 || (m.hr && (m.hr<30 || m.hr>220)))return false;
    }
    for(const auto &n:nights)if(n.onset) {
      if(n.version!=VERSION || n.end<=n.onset || n.end-n.onset>16*3600 || n.end-n.onset<2*3600 || n.onset<1577836800U ||
          n.observed<n.end || n.changed>n.observed || n.first_at>n.observed ||
          n.asleep+n.awake!=(n.end-n.onset)/60 || n.activity_minutes>(n.end-n.onset)/60 || n.hr_minutes>n.activity_minutes ||
          n.target_minutes<360 || n.target_minutes>600 || n.local_onset>=1440 || n.available>31 || (n.helio_score!=255 && n.helio_score>100) ||
          (n.first_helio!=255 && n.first_helio>100))return false;
      for(float f:n.components)if(!std::isfinite(f)||f<0||f>100)return false;
      for(float f:{n.mean_hr,n.mean_movement,n.activity_coverage,n.ours,n.first_score,n.coverage})
        if(!std::isfinite(f)||f<0)return false;
      if(n.ours>100 || n.first_score>100 || n.coverage>100 || n.activity_coverage>1.01)return false;
    }
    return true;
  }
  void activity(const uint8_t *raw,size_t size,uint32_t first,uint32_t now) {
    if(!raw || first<1577836800U || size%8 || size>120*8)return;
    for(size_t at=0;at<size;at+=8) {
      const uint64_t stamp=uint64_t(first)+(at/8)*60;
      if(stamp>now || stamp+2*86400<now)continue;
      const uint32_t epoch=stamp,day=epoch/86400*86400;
      size_t slot=DAYS;
      for(size_t i=0;i<DAYS;++i)if(days[i].start==day){slot=i;break;}
      if(slot==DAYS) {
        slot=0;for(size_t i=1;i<DAYS;++i)if(days[i].start<days[slot].start)slot=i;
        if(days[slot].start>day)continue;
        days[slot]={};days[slot].start=day;
      }
      auto &m=days[slot].minutes[(epoch-day)/60];
      if(m.flags)continue; // Overlapping polls cannot multiply observations or revise first-arrival data.
      const bool removed=raw[at]==115 || raw[at]==118 || raw[at]==255;
      const bool heart=raw[at+3]>=30 && raw[at+3]<=220;
      m.flags=removed?2:heart?1:3;m.movement=raw[at+1];
      m.hr=heart && !removed?raw[at+3]:0;
      dirty=true;
    }
  }
  static double median(std::array<double,HISTORY> values,size_t n) {
    if(!n || n>HISTORY)return 0;
    std::sort(values.begin(),values.begin()+n);
    return n%2?values[n/2]:(values[n/2-1]+values[n/2])/2;
  }
  static double upper_penalty(double value,const std::array<double,HISTORY> &values,size_t n,double floor) {
    const double centre=median(values,n);std::array<double,HISTORY> deviations{};
    for(size_t i=0;i<n;++i)deviations[i]=std::abs(values[i]-centre);
    const double spread=std::max(floor,1.4826*median(deviations,n));
    return std::clamp(100.-15.*std::max(0.,(value-centre)/spread),0.,100.);
  }
  bool observe(const uint8_t *p,size_t size,uint32_t now,unsigned local_onset) {
    sleep_data::Snapshot check;
    if(!sleep_data::decode(p,size,now,check) || !p[0x54] || local_onset>=1440)return false;
    const uint32_t base=protocol::read32(p+4)-86400;
    const uint64_t start=uint64_t(base)+protocol::read16(p+10)*60U,finish=uint64_t(base)+protocol::read16(p+12)*60U;
    if(start<1577836800U || finish<=start || finish-start<2*3600 || finish-start>16*3600 ||
        finish>now || finish+48*3600<now)return false;
    const uint32_t onset=start,end=finish;
    uint32_t cursor=onset,awake=0,asleep=0;unsigned bouts=0;bool was_awake=false;
    for(size_t i=0;i<p[0x54];++i) {
      const auto *s=p+0x56+i*5;
      const uint32_t lo=std::max(onset,base+protocol::read16(s)*60U),hi=std::min(end,base+(protocol::read16(s+2)+1U)*60U);
      if(hi<=lo)continue;
      if(lo!=cursor)return false;
      const bool waking=s[4]==7;
      if(waking){awake+=hi-lo;if(!was_awake)++bouts;}else asleep+=hi-lo;
      was_awake=waking;cursor=hi;
    }
    if(cursor!=end)return false; // Unknown intervals cannot count as restorative sleep.
    size_t slot=HISTORY;
    for(size_t i=0;i<HISTORY;++i)if(nights[i].onset==onset){slot=i;break;}
    if(slot==HISTORY){slot=0;for(size_t i=1;i<HISTORY;++i)if(nights[i].onset<nights[slot].onset)slot=i;}
    if(nights[slot].onset>onset)return false;
    const Night old=nights[slot];Night n;n.onset=onset;n.end=end;n.local_onset=local_onset;
    n.target_minutes=old.onset==onset?old.target_minutes:target_minutes;
    n.asleep=asleep/60;n.awake=awake/60;n.awakenings=bouts;n.observed=now;
    n.helio_score=p[0x16]<=100?p[0x16]:255;
    // Semantic signature deliberately excludes Helio's score and Light/Deep/REM distinctions.
    uint32_t semantic[5]={onset,end,n.asleep,n.awake,bouts};
    n.signature=protocol::crc32(reinterpret_cast<const uint8_t*>(semantic),sizeof(semantic));
    n.changed=old.onset==onset && old.signature==n.signature?old.changed:now;
    double hr=0,movement=0;
    for(uint32_t epoch=onset;epoch<end;epoch+=60) {
      const uint32_t day=epoch/86400*86400;
      for(const auto &d:days)if(d.start==day) {
        const auto &m=d.minutes[(epoch-day)/60];
        if(m.flags==1){++n.activity_minutes;movement+=m.movement;if(m.hr){++n.hr_minutes;hr+=m.hr;}}
        break;
      }
    }
    // Retired minute-cache pages must not erase a completed night's aggregate.
    if(old.onset==onset && old.end==end && old.activity_minutes>n.activity_minutes) {
      n.activity_minutes=old.activity_minutes;n.hr_minutes=old.hr_minutes;
      hr=double(old.mean_hr)*old.hr_minutes;movement=double(old.mean_movement)*old.activity_minutes;
    }
    const double minutes=(end-onset)/60.;n.activity_coverage=n.activity_minutes/minutes;
    n.mean_hr=n.hr_minutes?hr/n.hr_minutes:0;n.mean_movement=n.activity_minutes?movement/n.activity_minutes:0;
    const bool physiology=n.activity_coverage>=0.7 && n.hr_minutes>=120 && n.hr_minutes>=minutes*0.5;
    std::array<double,HISTORY> heart_rates{},movements{};size_t nh=0,nm=0,nt=0;
    double sin_onset=0,cos_onset=0;
    for(const auto &previous:nights)if(previous.onset && previous.onset<onset && onset-previous.onset<=28*86400 && previous.mature(now)) {
      ++n.baseline_nights;
      const double phase=previous.local_onset*6.283185307179586/1440.;
      sin_onset+=std::sin(phase);cos_onset+=std::cos(phase);++nt;
      if(previous.activity_coverage>=0.7 && previous.hr_minutes>=120 &&
          previous.hr_minutes>=double(previous.end-previous.onset)/120.)heart_rates[nh++]=previous.mean_hr;
      if(previous.activity_coverage>=0.7)movements[nm++]=std::log1p(previous.mean_movement);
    }
    // Fixed transparent heuristic weights; adapting baselines is not learning a quality ground truth.
    constexpr double weights[5]={40,30,10,10,10};
    n.components[0]=100*std::min(1.,double(n.asleep)/n.target_minutes);
    const double asleep_fraction=asleep/double(end-onset);
    const double fragmentation=std::clamp(100.-5.*std::max(0.,bouts*8./std::max(1.,minutes/60.)-3.),0.,100.);
    n.components[1]=0.75*100*asleep_fraction+0.25*fragmentation;n.available=3;
    if(nt>=BASELINE_NIGHTS && std::hypot(sin_onset,cos_onset)>0.1) {
      const double expected=std::atan2(sin_onset,cos_onset),phase=local_onset*6.283185307179586/1440.;
      const double delta=std::abs(std::atan2(std::sin(phase-expected),std::cos(phase-expected)))*1440/6.283185307179586;
      n.components[2]=std::clamp(100.-std::max(0.,delta-30.)/6.,0.,100.);n.available|=4;
    }
    if(physiology && nh>=BASELINE_NIGHTS){n.components[3]=upper_penalty(n.mean_hr,heart_rates,nh,2.);n.available|=8;}
    if(n.activity_coverage>=0.7 && nm>=BASELINE_NIGHTS){n.components[4]=upper_penalty(std::log1p(n.mean_movement),movements,nm,0.15);n.available|=16;}
    double sum=0,weight=0;for(size_t i=0;i<5;++i)if(n.available&(1<<i)){sum+=weights[i]*n.components[i];weight+=weights[i];}
    n.ours=sum/weight;n.coverage=weight;
    n.first_score=old.onset==onset?old.first_score:n.ours;
    n.first_helio=old.onset==onset?old.first_helio:n.helio_score;
    n.first_at=old.onset==onset?old.first_at:now;
    nights[slot]=n;dirty=true;return true;
  }
  const Night *latest() const {
    const Night *result=nullptr;for(const auto &n:nights)if(n.onset && (!result||n.onset>result->onset))result=&n;return result;
  }
};
} // namespace esphome::helio_bridge::sleep_score

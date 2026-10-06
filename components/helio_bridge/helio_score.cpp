#include "helio_bridge.h"
#include "diagnostic_codec.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <esp_heap_caps.h>
#include <new>
#include <cstring>

namespace esphome::helio_bridge {
namespace { constexpr uint32_t SCORE_MAGIC=0x31535148U; }
float HelioBridge::displayed_personal_score() const {
  const auto *night=quality_?quality_->latest():nullptr;
  const auto &helio=sleep_transfer_.night_score();
  // A retained personal score must not be paired with another night's Helio score.
  return night && helio.valid && night->onset==helio.onset && night->end==helio.end &&
      std::isfinite(night->ours) ? night->ours : NAN;
}
void HelioBridge::set_score_target(float hours) {
  if(!quality_ || !std::isfinite(hours) || hours<6 || hours>10)return;
  const uint16_t target=std::lround(hours*60);
  if(target!=quality_->target_minutes){quality_->target_minutes=target;quality_->dirty=true;}
}
void HelioBridge::setup_score_() {
  // Optional module: a board without sufficient memory keeps its alarm controller.
  if(!personal_score_sensor_)return;
  void *memory=heap_caps_malloc(sizeof(sleep_score::Model),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!memory && heap_caps_get_free_size(MALLOC_CAP_8BIT)>sizeof(sleep_score::Model)+65536)
    memory=heap_caps_malloc(sizeof(sleep_score::Model),MALLOC_CAP_8BIT);
  if(!memory){quality_error_="Unavailable: insufficient memory";score_publish_();return;}
  quality_=new(memory) sleep_score::Model();
  if(nvs_open("helio_score_v1",NVS_READWRITE,&quality_handle_)!=ESP_OK) {
    quality_handle_=0;quality_error_="Unavailable: score storage could not open";score_publish_();return;
  }
  size_t size=0;bool migrated=false;
  if(nvs_get_blob(quality_handle_,"database",nullptr,&size)==ESP_OK) {
    const bool current=size==sizeof(sleep_score::Model)+16,legacy=size==sizeof(sleep_score_v1::Model)+16;
    bool good=current||legacy;
    std::vector<uint8_t> blob(good?size:0);
    if(legacy)new(blob.data()+16) sleep_score_v1::Model();
    if(good)good=nvs_get_blob(quality_handle_,"database",blob.data(),&size)==ESP_OK &&
      protocol::read32(blob.data())==SCORE_MAGIC &&
      protocol::read32(blob.data()+8)==size-16 &&
      protocol::read32(blob.data()+12)==protocol::crc32(blob.data()+16,size-16);
    if(good && current && protocol::read32(blob.data()+4)==sleep_score::VERSION) {
      memcpy(quality_,blob.data()+16,sizeof(*quality_));good=quality_->valid();
    } else if(good && legacy && protocol::read32(blob.data()+4)==1) {
      good=quality_->migrate(*reinterpret_cast<const sleep_score_v1::Model*>(blob.data()+16));migrated=good;
    } else good=false;
    if(!good){quality_->~Model();new(quality_) sleep_score::Model();diagnostic_text_(8,"Personal score storage rejected; collecting fresh nights");}
  }
  quality_->dirty=migrated;quality_flush_at_=millis()+(migrated?60000:3600000);quality_publish_at_=millis()+60000;
  if(!quality_->replay_done && diagnostic_handle_) {
    quality_replay_end_=diagnostic_next_;quality_replay_seq_=1;quality_replay_at_=millis();
  }
  score_publish_();
}
bool HelioBridge::save_score_() {
  if(!quality_||!quality_handle_)return false;
  std::vector<uint8_t> blob(sizeof(*quality_)+16);
  protocol::write32(blob.data(),SCORE_MAGIC);protocol::write32(blob.data()+4,sleep_score::VERSION);
  protocol::write32(blob.data()+8,sizeof(*quality_));
  memcpy(blob.data()+16,quality_,sizeof(*quality_));
  protocol::write32(blob.data()+12,protocol::crc32(blob.data()+16,sizeof(*quality_)));
  nvs_stats_t stats{};const size_t need=(blob.size()+31)/32+16;
  bool space=false;
  for(size_t i=0;i<=DIAGNOSTIC_SLOTS;++i) {
    if(nvs_get_stats(nullptr,&stats)!=ESP_OK)break;
    if(stats.free_entries>need+1024){space=true;break;}
    if(!diagnostic_evict_())break;
  }
  if(!space || nvs_set_blob(quality_handle_,"database",blob.data(),blob.size())!=ESP_OK || nvs_commit(quality_handle_)!=ESP_OK) {
    quality_error_="Storage save failed; latest score is not yet saved";
    quality_flush_at_=millis()+300000;quality_urgent_=false;score_publish_();return false;
  }
  quality_->dirty=false;quality_urgent_=false;quality_flush_at_=millis()+3600000;quality_error_.clear();
  return true;
}
void HelioBridge::score_activity_(const uint8_t *raw,size_t size,uint32_t start) {
  if(quality_ && quality_handle_ && clock_ && clock_->utcnow().is_valid())
    quality_->activity(raw,size,start,clock_->utcnow().timestamp);
}
void HelioBridge::score_audit_(const sleep_score::Night &n,uint32_t now) {
  // Explicit little-endian layout, independent of C++ object padding.
  uint8_t audit[176]{};
  const uint32_t values[]={n.version,n.onset,n.end,n.observed,n.changed,n.first_at,n.signature,n.baseline_nights,
    n.target_minutes,n.available,n.helio_score,n.first_helio};
  for(size_t i=0;i<12;++i)protocol::write32(audit+i*4,values[i]);
  const float floats[]={n.ours,n.first_score,n.coverage,n.activity_coverage,n.mean_hr,n.mean_movement,
    n.components[0],n.components[1],n.components[2],n.components[3],n.components[4]};
  for(size_t i=0;i<11;++i)memcpy(audit+48+i*4,floats+i,4);
  const uint16_t counts[]={n.asleep,n.awake,n.awakenings,n.activity_minutes,n.hr_minutes,n.local_onset};
  for(size_t i=0;i<6;++i){audit[92+i*2]=counts[i];audit[93+i*2]=counts[i]>>8;}
  protocol::write32(audit+104,n.mature(now));
  protocol::write32(audit+108,n.previous_version);protocol::write32(audit+112,n.previous_first_at);
  const float extra[]={n.previous_score,n.previous_first_score,n.previous_coverage,n.components[5],n.hr_change,n.hr_trend,n.hr_sd,n.shortfall};
  for(size_t i=0;i<8;++i)memcpy(audit+116+i*4,extra+i,4);
  const uint16_t patterns[]={n.longest_awake,n.wake_cluster,n.late_awake,n.restless_minutes,n.movement_bursts,n.longest_movement,n.hr_pairs,n.history_nights};
  for(size_t i=0;i<8;++i){audit[148+i*2]=patterns[i];audit[149+i*2]=patterns[i]>>8;}
  protocol::write32(audit+164,n.patterns);protocol::write32(audit+168,n.previous_helio);protocol::write32(audit+172,n.previous_first_helio);
  diagnostic_append_(19,audit,sizeof(audit));
}
void HelioBridge::score_reference_(const uint8_t *raw,size_t size,uint32_t now) {
  if(!quality_||!quality_handle_||size%sleep_data::RECORD_SIZE)return;
  bool audited=false;
  for(size_t at=0;at<size;at+=sleep_data::RECORD_SIZE) {
    const auto *p=raw+at;const uint32_t midnight=protocol::read32(p+4);
    if(midnight<1577836800U)continue;
    const uint64_t onset=uint64_t(midnight)-86400+protocol::read16(p+10)*60U;
    if(onset<1577836800U || onset>now)continue;
    sleep_score::Night old;
    for(const auto &n:quality_->nights)if(n.onset==onset){old=n;break;}
    const auto local=ESPTime::from_epoch_local(onset);
    if(!quality_->observe(p,sleep_data::RECORD_SIZE,now,local.hour*60U+local.minute))continue;
    for(const auto &n:quality_->nights)if(n.onset==onset) {
      if(!old.onset || old.signature!=n.signature || old.helio_score!=n.helio_score || old.available!=n.available ||
         std::abs(old.ours-n.ours)>0.005 || std::abs(old.activity_coverage-n.activity_coverage)>0.001 ||
         old.mature(now)!=n.mature(now)) {score_audit_(n,now);audited=true;}
      break;
    }
  }
  // Save new/revised scores after closing BLE, with a one-minute minimum delay.
  if(audited && !quality_urgent_){quality_flush_at_=millis()+60000;quality_urgent_=true;}
  score_publish_();
}
void HelioBridge::score_publish_() {
  if(!personal_score_sensor_)return;
  if(!quality_error_.empty()) {
    if(personal_score_status_sensor_)personal_score_status_sensor_->publish_state(quality_error_.c_str());
    if(!quality_ || !quality_handle_)personal_score_sensor_->publish_state(NAN);
    return;
  }
  const auto *n=quality_?quality_->latest():nullptr;
  if(!n) {
    personal_score_sensor_->publish_state(NAN);
    if(personal_score_coverage_sensor_)personal_score_coverage_sensor_->publish_state(0);
    if(personal_score_status_sensor_)personal_score_status_sensor_->publish_state("Experimental v2; waiting for a completed night");
    return;
  }
  const uint32_t now=clock_ && clock_->utcnow().is_valid()?clock_->utcnow().timestamp:0;
  personal_score_sensor_->publish_state(n->ours);
  if(personal_score_coverage_sensor_)personal_score_coverage_sensor_->publish_state(n->coverage);
  char message[220];
  snprintf(message,sizeof(message),"Experimental v%u; %s; %u prior settled nights; %.0f%% component coverage%s",
    unsigned(n->version),n->mature(now)?"settled record":"provisional record",unsigned(n->baseline_nights),n->coverage,
    now && now>n->end+48*3600?"; old night":"");
  if(personal_score_status_sensor_)personal_score_status_sensor_->publish_state(message);
  snprintf(message,sizeof(message),"Sleep %um; awake %um; longest wake %um; wake cluster %u; readings %.0f%%; HR %s; movement %s; timing %s; recent nights %u%s",
    unsigned(n->asleep),unsigned(n->awake),unsigned(n->longest_awake),unsigned(n->wake_cluster),n->activity_coverage*100,
    n->available&8?"active":"pending",n->available&16?"active":"pending",n->available&4?"active":"pending",
    unsigned(n->history_nights),n->available&32?"":" (pending)");
  if(personal_score_details_sensor_)personal_score_details_sensor_->publish_state(message);
}
void HelioBridge::score_tick_() {
  if(!quality_||!quality_handle_||!clock_||!clock_->utcnow().is_valid()||phase_!=Phase::IDLE||queued_alarm_)return;
  const uint32_t ms=millis(),now=clock_->utcnow().timestamp;
  if(int32_t(ms-quality_publish_at_)>=0){quality_publish_at_=ms+60000;score_publish_();}
  // Flash work stays outside the alarm window and worn follow-up period.
  if((smart_enabled_ && !smart_session_.finished && smart_wake::light_window(smart_session_,now)) || follow_monitoring_(now))return;
  if(quality_replay_seq_){score_replay_();return;}
  if(quality_->dirty && int32_t(ms-quality_flush_at_)>=0)save_score_();
}
void HelioBridge::score_replay_() {
  // Reuse already committed minute observations; one bounded batch per idle slice.
  const uint32_t ms=millis();if(int32_t(ms-quality_replay_at_)<0)return;quality_replay_at_=ms+100;
  uint32_t seq=quality_replay_end_;
  for(uint32_t value:diagnostic_sequences_)if(value>=quality_replay_seq_ && value<seq)seq=value;
  if(seq>=quality_replay_end_) {
    quality_replay_seq_=0;quality_->replay_done=1;quality_->dirty=true;
    quality_flush_at_=ms+60000;quality_urgent_=true;
    diagnostic_text_(8,"Personal score v2: finished recovery of recent committed overnight minute readings");return;
  }
  quality_replay_seq_=seq+1;const size_t slot=seq%DIAGNOSTIC_SLOTS;
  char key[12];snprintf(key,sizeof(key),"b%03u",unsigned(slot));size_t size=0;
  if(nvs_get_blob(diagnostic_handle_,key,nullptr,&size)!=ESP_OK || size<16 || size>49168)return;
  std::vector<uint8_t> blob(size),raw;
  if(nvs_get_blob(diagnostic_handle_,key,blob.data(),&size)!=ESP_OK || memcmp(blob.data(),"HLG2",4) ||
      protocol::read32(blob.data()+4)!=seq || protocol::read32(blob.data()+8)>24576 ||
      !diagnostic::decode(blob.data()+16,size-16,protocol::read32(blob.data()+8),raw) ||
      diagnostic::crc(raw.data(),raw.size())!=protocol::read32(blob.data()+12))return;
  const uint32_t now=clock_->utcnow().timestamp;
  for(size_t at=0;at+11<=raw.size();) {
    const auto *event=raw.data()+at;const uint32_t stamp=protocol::read32(event+1);const unsigned len=protocol::read16(event+9);
    at+=11;if(len>raw.size()-at)return;const auto *p=raw.data()+at;
    if(event[0]==9 && stamp>=1577836800U && stamp<=now && uint64_t(stamp)+2*86400>=now &&
        len>=17 && len<=977 && p[0]==1 && (len-17)%8==0)
      quality_->activity(p+17,len-17,protocol::read32(p+5),now);
    at+=len;
  }
}
} // namespace esphome::helio_bridge

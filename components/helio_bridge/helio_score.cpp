#include "helio_bridge.h"
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
  size_t size=0;
  if(nvs_get_blob(quality_handle_,"database",nullptr,&size)==ESP_OK) {
    bool good=size==sizeof(sleep_score::Model)+16;
    std::vector<uint8_t> blob(good?size:0);
    if(good)good=nvs_get_blob(quality_handle_,"database",blob.data(),&size)==ESP_OK &&
      protocol::read32(blob.data())==SCORE_MAGIC && protocol::read32(blob.data()+4)==sleep_score::VERSION &&
      protocol::read32(blob.data()+8)==sizeof(sleep_score::Model) &&
      protocol::read32(blob.data()+12)==protocol::crc32(blob.data()+16,sizeof(sleep_score::Model));
    if(good){memcpy(quality_,blob.data()+16,sizeof(*quality_));good=quality_->valid();}
    if(!good){quality_->~Model();new(quality_) sleep_score::Model();diagnostic_text_(8,"Personal score storage rejected; collecting fresh nights");}
  }
  quality_->dirty=false;quality_flush_at_=millis()+3600000;quality_publish_at_=millis()+60000;
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
  uint8_t audit[108]{};
  const uint32_t values[]={n.version,n.onset,n.end,n.observed,n.changed,n.first_at,n.signature,n.baseline_nights,
    n.target_minutes,n.available,n.helio_score,n.first_helio};
  for(size_t i=0;i<12;++i)protocol::write32(audit+i*4,values[i]);
  const float floats[]={n.ours,n.first_score,n.coverage,n.activity_coverage,n.mean_hr,n.mean_movement,
    n.components[0],n.components[1],n.components[2],n.components[3],n.components[4]};
  for(size_t i=0;i<11;++i)memcpy(audit+48+i*4,floats+i,4);
  const uint16_t counts[]={n.asleep,n.awake,n.awakenings,n.activity_minutes,n.hr_minutes,n.local_onset};
  for(size_t i=0;i<6;++i){audit[92+i*2]=counts[i];audit[93+i*2]=counts[i]>>8;}
  protocol::write32(audit+104,n.mature(now));diagnostic_append_(19,audit,sizeof(audit));
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
    if(personal_score_status_sensor_)personal_score_status_sensor_->publish_state("Experimental v1; waiting for a completed night");
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
  snprintf(message,sizeof(message),"Sleep %um; awake %um; wake bouts %u; overnight readings %.0f%%; duration %.0f; continuity %.0f; timing %s; HR %s; movement %s",
    unsigned(n->asleep),unsigned(n->awake),unsigned(n->awakenings),n->activity_coverage*100,
    n->components[0],n->components[1],n->available&4?"available":"pending",n->available&8?"available":"pending",n->available&16?"available":"pending");
  if(personal_score_details_sensor_)personal_score_details_sensor_->publish_state(message);
}
void HelioBridge::score_tick_() {
  if(!quality_||!quality_handle_||!clock_||!clock_->utcnow().is_valid()||phase_!=Phase::IDLE||queued_alarm_)return;
  const uint32_t ms=millis(),now=clock_->utcnow().timestamp;
  if(int32_t(ms-quality_publish_at_)>=0){quality_publish_at_=ms+60000;score_publish_();}
  // Flash work stays outside the alarm window and worn follow-up period.
  if((smart_enabled_ && !smart_session_.finished && smart_wake::light_window(smart_session_,now)) || follow_monitoring_(now))return;
  if(quality_->dirty && int32_t(ms-quality_flush_at_)>=0)save_score_();
}
} // namespace esphome::helio_bridge
